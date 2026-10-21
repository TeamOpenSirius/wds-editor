#!/usr/bin/env python3
"""Perf acceptance helper for the WDS editor audit.

Reads raw diagnostics (no averaging of averages):
  * frame-diag.log  : t=/CPUCLASS/MEMMEM/FRAME/HITCH/MISS/SPLITCOLD
  * stderr capture  : SPLITPHASE / PREVFRAME / GPUPHASE / PREVSEEK / PRECOMMIT_DRAW

Percentiles are computed from the pooled per-frame samples of every run given
for a scenario; p50 is the median of the per-run medians so one bad run cannot
drag the "average" around.

Usage:
  perf_accept.py --dir <run-dir> [--dir ...] [--json out.json]
  perf_accept.py --compare baseline.json current.json
Exit code 1 if any --compare target regresses.
"""
import argparse
import json
import os
import re
import sys

KV = re.compile(r'([A-Za-z_][A-Za-z0-9_.]*)=("[^"]*"|\S+)')

# metric name -> (source token, field, aggregation)
#   src: "line" = frame-diag.log, "err" = stderr capture
METRICS = {
    "wall_ms":        ("line", "FRAME:wall",        "ms"),
    "wall_win_p50":   ("line", "t:wall_p50",        "ms"),
    "wall_win_p95":   ("line", "t:wall_p95",        "ms"),
    "hitch_wall_ms":  ("line", "HITCH:wall",        "ms"),
    "cpu_main_ms":    ("line", "CPUCLASS:main_ms",  "ms"),
    "cpu_total_pct":  ("line", "CPUCLASS:total_pct","raw"),
    "rss_mb":         ("line", "MEMMEM:rss_kb",     "kb_to_mb"),
    "dminflt":        ("line", "FRAME:dminflt",     "raw"),
    "prev_total_ms":  ("err",  "PREVFRAME:total_ms", "ms"),
    "prev_splits_ms": ("err",  "PREVFRAME:splits_ms", "ms"),
    "emit_build_ms":  ("err",  "PREVFRAME:emit_build_ms", "ms"),
    "particles_ms":   ("err",  "PREVFRAME:particles_ms", "ms"),
    "collect_ms":     ("err",  "SPLITPHASE:collect_ms", "ms"),
    "subemit_ms":     ("err",  "SPLITPHASE:subemit_ms", "ms"),
    "integrate_ms":   ("err",  "SPLITPHASE:integrate_ms", "ms"),
    "cache_lu_ms":    ("err",  "SPLITPHASE:cache_lu_ms", "ms"),
    "gpu_total_ms":   ("err",  "GPUPHASE:gpu_total_ms", "ms"),
    "gpu_split_ms":   ("err",  "GPUPHASE:split_ms", "ms"),
    "seek_us":        ("err",  "PREVSEEK:total_us", "raw"),
    "prepare_us":     ("err",  "PREVSEEK:prepare_us", "raw"),
    "precommit_ms":   ("err",  "PRECOMMIT_DRAW:wall_us", "us_to_ms"),
    "cache_bytes_mb": ("err",  "SPLITPHASE:cache_bytes", "b_to_mb"),
}

TOKEN_FOR = {
    "FRAME": "FRAME", "t": "t=", "MEMMEM": "MEMMEM",
    "HITCH": "HITCH", "MISS": "MISS", "SPLITCOLD": "SPLITCOLD",
    "PREVFRAME": "PREVFRAME", "SPLITPHASE": "SPLITPHASE",
    "GPUPHASE": "GPUPHASE", "PREVSEEK": "PREVSEEK",
    "PRECOMMIT_DRAW": "PRECOMMIT_DRAW",
}


def parse_line(line):
    d = {}
    for k, v in KV.findall(line):
        try:
            d[k] = float(v)
        except ValueError:
            d[k] = v
    return d


def read_dir(path):
    """Return {scenario: {'line': [dict], 'err': [dict]}} for one run dir."""
    out = {}
    for name in sorted(os.listdir(path)):
        base, ext = os.path.splitext(name)
        if ext not in (".log", ".err"):
            continue
        scen = base
        rec = out.setdefault(scen, {"line": [], "err": []})
        with open(os.path.join(path, name), errors="replace") as fh:
            for line in fh:
                for tok, prefix in TOKEN_FOR.items():
                    if line.startswith(prefix):
                        rec["line" if ext == ".log" else "err"].append(
                            (tok, parse_line(line)))
                        break
    return out


def pct(vals, p):
    if not vals:
        return None
    s = sorted(vals)
    i = min(len(s) - 1, max(0, int(round((len(s) - 1) * p))))
    return s[i]


def collect(runs):
    """runs: list of read_dir() results -> {scenario: {metric: {p50,p95,max,n}}}"""
    # scenario -> src+token -> field -> list of values (pooled)
    pooled = {}
    for run in runs:
        for scen, rec in run.items():
            for src in ("line", "err"):
                for tok, kv in rec[src]:
                    for mname, (msrc, mkey, conv) in METRICS.items():
                        if msrc != src:
                            continue
                        t, field = mkey.split(":", 1)
                        if t != tok or field not in kv:
                            continue
                        v = kv[field]
                        if not isinstance(v, float):
                            continue
                        if conv == "kb_to_mb":
                            v /= 1024.0
                        elif conv == "us_to_ms":
                            v /= 1000.0
                        elif conv == "b_to_mb":
                            v /= (1024.0 * 1024.0)
                        pooled.setdefault(scen, {}).setdefault(mname, []).append(v)
    # counters: number of SPLITCOLD rows per scenario (cold-frame count)
    for run in runs:
        for scen, rec in run.items():
            n = sum(1 for tok, _ in rec["err"] if tok == "SPLITCOLD")
            if n:
                pooled.setdefault(scen, {}).setdefault("cold_frames", []).append(float(n))
    out = {}
    for scen, mets in pooled.items():
        cur = {}
        for m, vals in mets.items():
            cur[m] = {
                "p50": round(pct(vals, 0.50), 3),
                "p95": round(pct(vals, 0.95), 3),
                "max": round(max(vals), 3),
                "n": len(vals),
            }
        out[scen] = cur
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", action="append", default=[])
    ap.add_argument("--json")
    ap.add_argument("--compare", nargs=2)
    args = ap.parse_args()

    if args.compare:
        base = json.load(open(args.compare[0]))
        cur = json.load(open(args.compare[1]))
        bad = 0
        for scen, mets in cur.items():
            for m, v in mets.items():
                tgt = base.get(scen, {}).get(m)
                if not tgt:
                    continue
                # lower is better for every metric we track
                if v["p95"] > tgt["p95"] * 1.02 or v["max"] > tgt["max"] * 1.02:
                    print(f"FAIL {scen}/{m}: p95 {v['p95']} vs {tgt['p95']}, "
                          f"max {v['max']} vs {tgt['max']}")
                    bad += 1
        print(f"{'FAIL' if bad else 'PASS'}: {bad} regression(s)")
        return 1 if bad else 0

    if not args.dir:
        ap.error("need --dir or --compare")
    runs = [read_dir(d) for d in args.dir]
    res = collect(runs)
    for scen in sorted(res):
        print(f"== {scen}")
        for m, v in sorted(res[scen].items()):
            print(f"   {m:16s} p50={v['p50']:<10} p95={v['p95']:<10} max={v['max']:<10} n={v['n']}")
    if args.json:
        json.dump(res, open(args.json, "w"), indent=1, sort_keys=True)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Perf acceptance helper for the WDS editor audit.

Reads raw diagnostics (no averaging of averages):
  * frame-diag.log  : t=/CPUCLASS/MEMMEM/FRAME/HITCH/MISS/SPLITCOLD
  * stderr capture  : SPLITPHASE / PREVFRAME / GPUPHASE / PREVSEEK / PRECOMMIT_DRAW

Percentiles are computed from the pooled per-frame samples of every run given
for a scenario; p50 is the median of the per-run medians so one bad run cannot
drag the "average" around.

Usage:
  perf_accept.py --dir <run-dir> [--dir ...] [--json out.json] [--keep-startup]
  perf_accept.py --dir <run-dir> [--dir ...] --default-chart-windows [--json out.json]
  perf_accept.py --dir <run-dir> [--dir ...] --chart-window S2=8.0:10.0 [--chart-window ...]
  perf_accept.py --compare baseline.json current.json
  perf_accept.py --compare a.json b.json --record-only-scenario s3 --record-only-metric rss_mb
Exit code 1 if any --compare target regresses; 2 if skip_startup,
chart-window, or record-only口径 flags disagree.

Without --chart-window / --default-chart-windows the JSON and stdout match the
Wave 1 final helper byte-for-byte.

Chart windows (optional):
  Pair FRAME rows in sN.log with playing=1 PREVFRAME rows in sN.err by
  appearance order, then keep pairs whose chart_sec is in [lo, hi).
  FRAME.collect is checked against PREVFRAME.splits_ms (both printed %.3f;
  tolerance 0.001). Count mismatches, unpaired extras, and FRAME-cap
  truncation are reported; those frames are excluded, never silently kept.
  t= window aggregates cannot be chart-filtered: wall_ms and hitch_per_s are
  taken from paired FRAME.wall instead. See --help.
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

# Half-open [lo, hi) chart_sec windows. S5 (seek-sweep) has none: chart time
# jumps, so a playback-progress window is not meaningful.
DEFAULT_CHART_WINDOWS = {
    "s1": (8.5, 10.5),   # seek 8.5; 10990 load until ~10.5, then collect≈0
    "s2": (8.0, 10.0),   # overlap start; approved
    "s3": (2.0, 3.2),    # inject 10630x6; excludes seek 1.8 precommit hitch
    "s4": (2.0, 3.2),    # inject 11332x7; excludes seek 1.5 precommit hitch
}

# Compare-time gate policy (not a measurement口径). Printed as RECORD-ONLY
# instead of FAIL; does not affect exit code. CLI flags add to these sets.
DEFAULT_RECORD_ONLY_SCENARIOS = ("s4",)
DEFAULT_RECORD_ONLY_METRICS = ("hitch_per_s",)

COLLECT_TOL = 0.001          # FRAME.collect / PREVFRAME.splits_ms printed %.3f
ACTIVE_MS = 0.05             # plan §3.3: PREVFRAME collect or emit >0.05
HITCH_MS = 25.0              # kHitchUs = 25000; hitch if wall_us > 25000
WINDOW_METRIC_SOURCE = {
    "wall_ms": "paired FRAME.wall in chart window; replaces t= wall_win_p50/p95",
    "hitch_wall_ms": "paired FRAME.wall of frames with wall>25 ms in chart window",
    "hitch_per_s": "count(FRAME.wall>25)/sum(FRAME.wall) in chart window, per wall-clock second",
    "dminflt": "paired FRAME.dminflt in chart window",
    "prev_total_ms": "paired PREVFRAME.total_ms (playing=1, collect|emit>0.05) in chart window",
    "prev_splits_ms": "paired PREVFRAME.splits_ms ≡ FRAME.collect (playing=1, collect|emit>0.05) in chart window",
    "collect_ms": "same as prev_splits_ms (main-thread collect, not SPLITPHASE wall)",
    "emit_build_ms": "paired PREVFRAME.emit_build_ms (playing=1, collect|emit>0.05) in chart window",
    "particles_ms": "paired PREVFRAME.particles_ms (playing=1, collect|emit>0.05) in chart window",
    "subemit_ms": "SPLITPHASE attached to collect>0 pairs, then chart-windowed",
    "integrate_ms": "SPLITPHASE attached to collect>0 pairs, then chart-windowed",
    "cache_lu_ms": "SPLITPHASE attached to collect>0 pairs, then chart-windowed",
    "cache_bytes_mb": "SPLITPHASE attached to collect>0 pairs, then chart-windowed",
    "gpu_total_ms": "GPUPHASE joined by PREVFRAME.frame, then chart-windowed",
    "gpu_split_ms": "GPUPHASE joined by PREVFRAME.frame, then chart-windowed",
    "cpu_main_ms": "CPUCLASS whose t= window overlaps the chart window",
    "cpu_total_pct": "CPUCLASS whose t= window overlaps the chart window",
    "rss_mb": "MEMMEM whose t= window overlaps the chart window",
    "seek_us": "PREVSEEK after skip-startup (no chart field; not window-filtered)",
    "prepare_us": "PREVSEEK after skip-startup (no chart field; not window-filtered)",
    "precommit_ms": "PRECOMMIT_DRAW after skip-startup (no chart field; not window-filtered)",
    "cold_frames": "SPLITCOLD count after skip-startup (no chart field; not window-filtered)",
}


def parse_line(line):
    d = {}
    for k, v in KV.findall(line):
        try:
            d[k] = float(v)
        except ValueError:
            d[k] = v
    return d


def drop_startup_log(lines, path):
    """Drop the first t= window and every line before it."""
    for i, line in enumerate(lines):
        if line.startswith("t="):
            return lines[i + 1:]
    print(f"WARN {path}: no t= line; keeping all", file=sys.stderr)
    return lines


def drop_startup_err(lines, path):
    """Drop lines before the first PREVFRAME with playing=1 (keep that row)."""
    for i, line in enumerate(lines):
        if line.startswith("PREVFRAME") and parse_line(line).get("playing") == 1:
            return lines[i:]
    print(f"WARN {path}: no PREVFRAME playing=1; keeping all", file=sys.stderr)
    return lines


def read_dir(path, skip_startup=True):
    """Return {scenario: {'line': [dict], 'err': [dict]}} for one run dir."""
    out = {}
    for name in sorted(os.listdir(path)):
        base, ext = os.path.splitext(name)
        if ext not in (".log", ".err"):
            continue
        # skip frame-diag.log, stderr.log, stdout.log
        if base in ("frame-diag", "stderr", "stdout"):
            continue
        scen = base
        rec = out.setdefault(scen, {"line": [], "err": []})
        fpath = os.path.join(path, name)
        with open(fpath, errors="replace") as fh:
            lines = fh.readlines()
        if skip_startup:
            if ext == ".log":
                lines = drop_startup_log(lines, fpath)
            else:
                lines = drop_startup_err(lines, fpath)
        src = "line" if ext == ".log" else "err"
        for line in lines:
            for tok, prefix in TOKEN_FOR.items():
                if line.startswith(prefix):
                    rec[src].append((tok, parse_line(line)))
                    break
    return out


def skip_startup_flag(obj):
    meta = obj.get("meta") if isinstance(obj, dict) else None
    if not isinstance(meta, dict):
        return False
    return bool(meta.get("skip_startup", False))


def scenario_items(obj):
    return ((k, v) for k, v in obj.items() if k != "meta")


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


def parse_chart_window(spec):
    """Parse 'S2=8.0:10.0' -> ('s2', 8.0, 10.0). Half-open [lo, hi)."""
    if "=" not in spec or ":" not in spec:
        raise argparse.ArgumentTypeError(
            f"chart window {spec!r}: expected SCEN=LO:HI (e.g. S2=8.0:10.0)")
    scen, rest = spec.split("=", 1)
    lo_s, hi_s = rest.split(":", 1)
    try:
        lo, hi = float(lo_s), float(hi_s)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"chart window {spec!r}: LO and HI must be floats")
    if not scen.strip():
        raise argparse.ArgumentTypeError(f"chart window {spec!r}: empty scenario")
    if not (hi > lo):
        raise argparse.ArgumentTypeError(
            f"chart window {spec!r}: HI must be greater than LO")
    return scen.strip().lower(), lo, hi


def frame_index_cap(kv):
    iv = kv.get("i")
    if isinstance(iv, str) and "/" in iv:
        a, b = iv.split("/", 1)
        try:
            return int(a), int(b)
        except ValueError:
            return None, None
    return None, None


def in_chart_window(chart_sec, bounds):
    if bounds is None:
        return True
    if chart_sec is None or not isinstance(chart_sec, float):
        return False
    lo, hi = bounds
    return lo <= chart_sec < hi


def t_window_overlaps(t_kv, bounds):
    if bounds is None:
        return True
    lo, hi = bounds
    start = t_kv.get("chart_sec_start")
    end = t_kv.get("chart_sec")
    if not isinstance(end, float):
        return False
    if not isinstance(start, float):
        start = end
    # overlap of [start, end] with [lo, hi)
    return start < hi and end >= lo


def is_active_prev(prev):
    splits = prev.get("splits_ms")
    parts = prev.get("particles_ms")
    s = splits if isinstance(splits, float) else 0.0
    p = parts if isinstance(parts, float) else 0.0
    return s > ACTIVE_MS or p > ACTIVE_MS


def summarize_metric(vals):
    if not vals:
        return None
    return {
        "p50": round(pct(vals, 0.50), 3),
        "p95": round(pct(vals, 0.95), 3),
        "max": round(max(vals), 3),
        "n": len(vals),
    }


def canonicalize_windows(windows):
    """{scen: (lo,hi)} -> sorted {scen: [lo, hi]} for JSON / compare."""
    return {k: [float(lo), float(hi)] for k, (lo, hi) in sorted(windows.items())}


def resolve_record_only(cli_scens, cli_mets):
    """Default tables plus CLI additions. Returns sorted lists."""
    scens = set(DEFAULT_RECORD_ONLY_SCENARIOS)
    mets = set(DEFAULT_RECORD_ONLY_METRICS)
    for s in cli_scens or ():
        s = str(s).strip().lower()
        if s:
            scens.add(s)
    for m in cli_mets or ():
        m = str(m).strip()
        if m:
            mets.add(m)
    return sorted(scens), sorted(mets)


def record_only_from_json(obj):
    """Return (scenarios, metrics) or None if the JSON has no record-only keys."""
    meta = obj.get("meta") if isinstance(obj, dict) else None
    if not isinstance(meta, dict):
        return None
    if "record_only_scenarios" not in meta and "record_only_metrics" not in meta:
        return None
    scens = [str(s) for s in (meta.get("record_only_scenarios") or [])]
    mets = [str(m) for m in (meta.get("record_only_metrics") or [])]
    return sorted(set(scens)), sorted(set(mets))


def is_record_only(scen, metric, scenarios, metrics):
    return scen in scenarios or metric in metrics


def windows_from_json(obj):
    meta = obj.get("meta") if isinstance(obj, dict) else None
    if not isinstance(meta, dict):
        return {}
    w = meta.get("chart_windows")
    if not w:
        return {}
    out = {}
    for k, v in w.items():
        if v is None:
            continue
        if isinstance(v, (list, tuple)) and len(v) == 2:
            out[str(k)] = [float(v[0]), float(v[1])]
        elif isinstance(v, dict) and "lo" in v and "hi" in v:
            out[str(k)] = [float(v["lo"]), float(v["hi"])]
    return {k: out[k] for k in sorted(out)}


def read_dir_raw(path):
    """Parse sN.log / sN.err without dropping startup (needed for pairing)."""
    out = {}
    for name in sorted(os.listdir(path)):
        base, ext = os.path.splitext(name)
        if ext not in (".log", ".err"):
            continue
        if base in ("frame-diag", "stderr", "stdout"):
            continue
        scen = base
        rec = out.setdefault(scen, {
            "path": path,
            "log_path": None,
            "err_path": None,
            "log_lines": None,
            "err_lines": None,
        })
        fpath = os.path.join(path, name)
        with open(fpath, errors="replace") as fh:
            lines = fh.readlines()
        if ext == ".log":
            rec["log_path"] = fpath
            rec["log_lines"] = lines
        else:
            rec["err_path"] = fpath
            rec["err_lines"] = lines
    return out


def pair_scenario(rec, skip_startup=True):
    """Pair FRAME with playing=1 PREVFRAME. Return (pairs, stats, extras).

    extras: t= windows, CPUCLASS/MEMMEM after skip-startup, unfiltered events.
    """
    log_lines = rec.get("log_lines") or []
    err_lines = rec.get("err_lines") or []
    log_path = rec.get("log_path") or rec.get("path") or ""
    err_path = rec.get("err_path") or rec.get("path") or ""

    frames = []
    t_windows = []
    line_tokens = []
    seen_first_t = False
    for line in log_lines:
        if line.startswith("t="):
            kv = parse_line(line)
            t_windows.append(kv)
            if not seen_first_t:
                seen_first_t = True
            line_tokens.append(("t", kv, seen_first_t))
            continue
        for tok, prefix in TOKEN_FOR.items():
            if tok == "t":
                continue
            if line.startswith(prefix):
                kv = parse_line(line)
                if tok == "FRAME":
                    kv["_before_first_t"] = not seen_first_t
                    frames.append(kv)
                line_tokens.append((tok, kv, seen_first_t))
                break

    prev_play = []
    split_play = []
    gpu_by_frame = {}
    err_tokens = []
    seen_first_play = False
    for line in err_lines:
        for tok, prefix in TOKEN_FOR.items():
            if line.startswith(prefix):
                kv = parse_line(line)
                if tok == "PREVFRAME" and kv.get("playing") == 1:
                    if not seen_first_play:
                        seen_first_play = True
                    prev_play.append(kv)
                elif tok == "SPLITPHASE" and kv.get("playing") == 1:
                    split_play.append(kv)
                elif tok == "GPUPHASE":
                    fr = kv.get("frame")
                    if isinstance(fr, float):
                        gpu_by_frame[int(fr)] = kv
                if tok == "PREVFRAME" and kv.get("playing") == 1:
                    seen_first_play = True
                err_tokens.append((tok, kv, seen_first_play))
                break

    n_frame = len(frames)
    n_prev = len(prev_play)
    n_pair = min(n_frame, n_prev)
    mismatches = []
    pairs = []
    for i in range(n_pair):
        fr, pr = frames[i], prev_play[i]
        fc = fr.get("collect")
        ps = pr.get("splits_ms")
        if not isinstance(fc, float) or not isinstance(ps, float) or abs(fc - ps) > COLLECT_TOL:
            mismatches.append({
                "index": i,
                "chart_sec": fr.get("chart_sec"),
                "frame_collect": fc,
                "prev_splits_ms": ps,
            })
            continue
        gpu = None
        pfr = pr.get("frame")
        if isinstance(pfr, float):
            gpu = gpu_by_frame.get(int(pfr))
        pairs.append({
            "frame": fr,
            "prev": pr,
            "gpu": gpu,
            "split": None,
            "chart_sec": fr.get("chart_sec"),
        })

    # SPLITPHASE: one line per collect>0 preview frame, appearance order.
    live = [p for p in pairs if isinstance(p["frame"].get("collect"), float)
            and p["frame"]["collect"] > 0]
    n_split_pair = min(len(live), len(split_play))
    for i in range(n_split_pair):
        live[i]["split"] = split_play[i]
    split_unpaired = abs(len(live) - len(split_play))

    hit_cap = False
    for fr in frames:
        idx, cap = frame_index_cap(fr)
        if idx is not None and cap is not None and idx >= cap:
            hit_cap = True
            break
    extra_frame = max(0, n_frame - n_prev)
    extra_prev = max(0, n_prev - n_frame)
    truncated = extra_prev if hit_cap else 0
    unpaired_prev = 0 if hit_cap else extra_prev

    if skip_startup:
        pairs = [p for p in pairs if not p["frame"].get("_before_first_t")]
        # drop_startup_log: drop the first t= line and everything before it.
        line_kept = []
        past = False
        for tok, kv, _after in line_tokens:
            if not past:
                if tok == "t":
                    past = True
                continue
            line_kept.append((tok, kv))
        # drop_startup_err: drop lines before the first playing=1 PREVFRAME
        # (keep that row and everything after).
        err_kept = []
        past_play = False
        for tok, kv, _seen in err_tokens:
            if tok == "PREVFRAME" and kv.get("playing") == 1:
                past_play = True
            if not past_play:
                continue
            err_kept.append((tok, kv))
        t_kept = [kv for tok, kv in line_kept if tok == "t"]
    else:
        line_kept = [(tok, kv) for tok, kv, _ in line_tokens]
        err_kept = [(tok, kv) for tok, kv, _ in err_tokens]
        t_kept = t_windows

    stats = {
        "paired": n_pair - len(mismatches),
        "mismatch": len(mismatches),
        "unpaired_frame": extra_frame,
        "unpaired_prev": unpaired_prev,
        "truncated": truncated,
        "split_unpaired": split_unpaired,
        "n_frame": n_frame,
        "n_prev_play": n_prev,
        "first_mismatch": mismatches[0] if mismatches else None,
        "log_path": log_path,
        "err_path": err_path,
    }
    extras = {
        "line": line_kept,
        "err": err_kept,
        "t_windows": t_kept,
    }
    return pairs, stats, extras


def report_pair_stats(scen, stats):
    mm = stats.get("first_mismatch")
    extra = (stats["unpaired_frame"] + stats["unpaired_prev"]
             + stats["truncated"] + stats["mismatch"] + stats.get("split_unpaired", 0))
    if extra:
        loc = stats.get("log_path") or stats.get("err_path") or ""
        print(f"ERROR pair {scen}: paired={stats['paired']} "
              f"mismatch={stats['mismatch']} unpaired_frame={stats['unpaired_frame']} "
              f"unpaired_prev={stats['unpaired_prev']} truncated={stats['truncated']} "
              f"split_unpaired={stats.get('split_unpaired', 0)} "
              f"n_frame={stats['n_frame']} n_prev_play={stats['n_prev_play']} "
              f"path={loc}", file=sys.stderr)
        if mm:
            print(f"ERROR pair {scen} first mismatch index={mm['index']} "
                  f"chart_sec={mm['chart_sec']} FRAME.collect={mm['frame_collect']} "
                  f"PREVFRAME.splits_ms={mm['prev_splits_ms']}", file=sys.stderr)


def add_pair_stats(dst, src):
    for k in ("paired", "mismatch", "unpaired_frame", "unpaired_prev",
              "truncated", "split_unpaired", "n_frame", "n_prev_play",
              "windowed", "active"):
        dst[k] = dst.get(k, 0) + int(src.get(k, 0) or 0)
    if src.get("first_mismatch") and not dst.get("first_mismatch"):
        dst["first_mismatch"] = src["first_mismatch"]


def collect_windowed(raw_runs, windows, skip_startup=True):
    """Pair FRAME↔PREVFRAME, filter by chart window, recompute frame metrics."""
    pooled = {}
    hitch_num = {}
    hitch_den_ms = {}
    pair_tot = {}
    for run in raw_runs:
        for scen, rec in run.items():
            bounds = windows.get(scen)
            pairs, stats, extras = pair_scenario(rec, skip_startup=skip_startup)
            report_pair_stats(scen, stats)
            windowed = []
            for p in pairs:
                if in_chart_window(p.get("chart_sec"), bounds):
                    windowed.append(p)
            stats["windowed"] = len(windowed)
            stats["active"] = sum(1 for p in windowed if is_active_prev(p["prev"]))
            add_pair_stats(pair_tot.setdefault(scen, {}), stats)

            bucket = pooled.setdefault(scen, {})
            walls = []
            for p in windowed:
                fr, pr, gpu, sp = p["frame"], p["prev"], p["gpu"], p["split"]
                wall = fr.get("wall")
                if isinstance(wall, float):
                    bucket.setdefault("wall_ms", []).append(wall)
                    walls.append(wall)
                    if wall > HITCH_MS:
                        bucket.setdefault("hitch_wall_ms", []).append(wall)
                dmin = fr.get("dminflt")
                if isinstance(dmin, float):
                    bucket.setdefault("dminflt", []).append(dmin)
                if is_active_prev(pr):
                    for mname, field in (
                        ("prev_total_ms", "total_ms"),
                        ("prev_splits_ms", "splits_ms"),
                        ("emit_build_ms", "emit_build_ms"),
                        ("particles_ms", "particles_ms"),
                    ):
                        v = pr.get(field)
                        if isinstance(v, float):
                            bucket.setdefault(mname, []).append(v)
                    fc = fr.get("collect")
                    if isinstance(fc, float):
                        bucket.setdefault("collect_ms", []).append(fc)
                if sp is not None:
                    for mname, field, conv in (
                        ("subemit_ms", "subemit_ms", None),
                        ("integrate_ms", "integrate_ms", None),
                        ("cache_lu_ms", "cache_lu_ms", None),
                        ("cache_bytes_mb", "cache_bytes", "b_to_mb"),
                    ):
                        v = sp.get(field)
                        if not isinstance(v, float):
                            continue
                        if conv == "b_to_mb":
                            v /= (1024.0 * 1024.0)
                        bucket.setdefault(mname, []).append(v)
                if gpu is not None:
                    for mname, field in (
                        ("gpu_total_ms", "gpu_total_ms"),
                        ("gpu_split_ms", "split_ms"),
                    ):
                        v = gpu.get(field)
                        if isinstance(v, float):
                            bucket.setdefault(mname, []).append(v)
            if walls:
                hitch_num[scen] = hitch_num.get(scen, 0) + sum(1 for w in walls if w > HITCH_MS)
                hitch_den_ms[scen] = hitch_den_ms.get(scen, 0.0) + sum(walls)

            t_by = {}
            for kv in extras["t_windows"]:
                tval = kv.get("t")
                if isinstance(tval, float) and t_window_overlaps(kv, bounds):
                    t_by[tval] = kv
            for tok, kv in extras["line"]:
                if tok == "CPUCLASS" and isinstance(kv.get("t"), float) and kv["t"] in t_by:
                    v = kv.get("main_ms")
                    if isinstance(v, float):
                        bucket.setdefault("cpu_main_ms", []).append(v)
                    v = kv.get("total_pct")
                    if isinstance(v, float):
                        bucket.setdefault("cpu_total_pct", []).append(v)
                elif tok == "MEMMEM" and isinstance(kv.get("t"), float) and kv["t"] in t_by:
                    v = kv.get("rss_kb")
                    if isinstance(v, float):
                        bucket.setdefault("rss_mb", []).append(v / 1024.0)
            n_cold = 0
            for tok, kv in extras["err"]:
                if tok == "SPLITCOLD":
                    n_cold += 1
                elif tok == "PREVSEEK":
                    v = kv.get("total_us")
                    if isinstance(v, float):
                        bucket.setdefault("seek_us", []).append(v)
                    v = kv.get("prepare_us")
                    if isinstance(v, float):
                        bucket.setdefault("prepare_us", []).append(v)
                elif tok == "PRECOMMIT_DRAW":
                    v = kv.get("wall_us")
                    if isinstance(v, float):
                        bucket.setdefault("precommit_ms", []).append(v / 1000.0)
            if n_cold:
                bucket.setdefault("cold_frames", []).append(float(n_cold))

    out = {}
    for scen, mets in pooled.items():
        cur = {}
        for m, vals in mets.items():
            sm = summarize_metric(vals)
            if sm:
                cur[m] = sm
        den = hitch_den_ms.get(scen, 0.0)
        if den > 0:
            rate = hitch_num.get(scen, 0) / (den / 1000.0)
            cur["hitch_per_s"] = {
                "p50": round(rate, 3),
                "p95": round(rate, 3),
                "max": round(rate, 3),
                "n": hitch_num.get(scen, 0),
            }
        # wall_ms already is the per-frame wall p50/p95 in the window; do not
        # also emit wall_win_* (they would be identical and double-count --compare).
        out[scen] = cur
    return out, pair_tot


HELP_EPILOG = """
chart-window pairing
  sN.log FRAME rows (field chart_sec, printed %.3f) are paired 1:1 by appearance
  order with sN.err PREVFRAME rows that have playing=1. Each pair is checked:
  FRAME.collect must equal PREVFRAME.splits_ms within 0.001 (log print precision).
  Mismatches and leftover rows are counted and printed to stderr (first index,
  chart_sec, both collect values). WDS_FRAME_DIAG_FRAMES cap (FRAME i=N/N) leftover
  PREVFRAME rows are counted as truncated. Excluded frames are not pooled.

  --chart-window SCEN=LO:HI   keep pairs with LO <= chart_sec < HI (repeatable).
                              SCEN matches the sN.log stem (s1, s2, …); case
                              insensitive.
  --default-chart-windows     apply the built-in table, then --chart-window overrides:
                                s1 [8.5, 10.5)   seek 8.5, 10990 loaded segment
                                s2 [8.0, 10.0)   overlap start (approved)
                                s3 [2.0,  3.2)   inject 10630×6 (not the 1.8s hitch)
                                s4 [2.0,  3.2)   inject 11332×7 (not the 1.5s hitch)
                                s5 none          --seek-sweep 2:10:25; chart jumps
  Scenarios without a window (S5 by default) keep the Wave 1 collect path.

  Windowed JSON meta.chart_windows records the table; meta.metric_source records
  that wall_ms / hitch_per_s come from paired FRAME.wall, not t= rows (t=
  wall_win_p50/p95 are omitted for windowed scenarios).
  --compare exits 2 if skip_startup, chart_windows, or record-only tables disagree.

  record-only (compare gate, not a measurement口径)
  --record-only-scenario SCEN   add a scenario whose every metric is printed
                                but never FAIL (repeatable). Default table: s4.
  --record-only-metric NAME     add a metric name treated the same way.
                                Default table: hitch_per_s (windowed hitch rate;
                                S5 hitch_wall_ms is not this name and still gates).
  A would-be FAIL prints as "RECORD-ONLY scen/metric: ..." and is omitted from
  the regression count / exit code. Windowed JSON stores the tables in meta;
  unwindowed JSON omits them (Wave 1 byte-identical) and --compare then applies
  the same defaults. Missing keys on both sides therefore match.

  No window / record-only flags: JSON identical to the Wave 1 final helper.
""".strip()


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=HELP_EPILOG)
    ap.add_argument("--dir", action="append", default=[])
    ap.add_argument("--json")
    ap.add_argument("--compare", nargs=2)
    ap.add_argument("--keep-startup", action="store_true")
    ap.add_argument(
        "--chart-window", action="append", default=[], metavar="SCEN=LO:HI",
        help="keep paired frames with LO <= chart_sec < HI; repeatable "
             "(e.g. --chart-window S2=8.0:10.0)")
    ap.add_argument(
        "--default-chart-windows", action="store_true",
        help="apply the built-in per-scenario chart windows (see --help)")
    ap.add_argument(
        "--record-only-scenario", action="append", default=[], metavar="SCEN",
        help="compare: print this scenario's 2%% hits as RECORD-ONLY, not FAIL "
             "(repeatable; default table already includes s4)")
    ap.add_argument(
        "--record-only-metric", action="append", default=[], metavar="NAME",
        help="compare: print this metric's 2%% hits as RECORD-ONLY, not FAIL "
             "(repeatable; default table already includes hitch_per_s)")
    args = ap.parse_args()

    windows = {}
    if args.default_chart_windows:
        windows.update(DEFAULT_CHART_WINDOWS)
    for spec in args.chart_window:
        scen, lo, hi = parse_chart_window(spec)
        windows[scen] = (lo, hi)
    windowing = bool(windows) or args.default_chart_windows

    if args.compare:
        base = json.load(open(args.compare[0]))
        cur = json.load(open(args.compare[1]))
        base_skip = skip_startup_flag(base)
        cur_skip = skip_startup_flag(cur)
        if base_skip != cur_skip:
            print(f"ERROR skip_startup mismatch: baseline={base_skip} "
                  f"current={cur_skip}", file=sys.stderr)
            return 2
        base_win = windows_from_json(base)
        cur_win = windows_from_json(cur)
        if base_win != cur_win:
            print(f"ERROR chart_windows mismatch: baseline={base_win} "
                  f"current={cur_win}", file=sys.stderr)
            return 2
        default_ro = resolve_record_only([], [])
        base_ro = record_only_from_json(base)
        cur_ro = record_only_from_json(cur)
        if base_ro is None:
            base_ro = default_ro
        if cur_ro is None:
            cur_ro = default_ro
        if base_ro != cur_ro:
            print(f"ERROR record-only mismatch: baseline={base_ro} "
                  f"current={cur_ro}", file=sys.stderr)
            return 2
        ro_scens = set(base_ro[0]) | {s.strip().lower() for s in args.record_only_scenario if s.strip()}
        ro_mets = set(base_ro[1]) | {m.strip() for m in args.record_only_metric if m.strip()}
        bad = 0
        recorded = 0
        for scen, mets in scenario_items(cur):
            for m, v in mets.items():
                tgt = base.get(scen, {}).get(m)
                if not tgt:
                    continue
                # lower is better for every metric we track
                if v["p95"] > tgt["p95"] * 1.02:
                    rec = is_record_only(scen, m, ro_scens, ro_mets)
                    tag = "RECORD-ONLY" if rec else "FAIL"
                    print(f"{tag} {scen}/{m}: p95 {v['p95']} vs {tgt['p95']}, "
                          f"max {v['max']} vs {tgt['max']}")
                    if rec:
                        recorded += 1
                    else:
                        bad += 1
        if recorded:
            print(f"{'FAIL' if bad else 'PASS'}: {bad} regression(s), "
                  f"{recorded} record-only")
        else:
            print(f"{'FAIL' if bad else 'PASS'}: {bad} regression(s)")
        return 1 if bad else 0

    if not args.dir:
        ap.error("need --dir or --compare")
    skip_startup = not args.keep_startup
    pair_tot = {}
    if windowing:
        raw = [read_dir_raw(d) for d in args.dir]
        raw_w = [{s: rec for s, rec in run.items() if s in windows} for run in raw]
        res, pair_tot = collect_windowed(raw_w, windows, skip_startup=skip_startup)
        # Scenarios with no window (S5 seek-sweep) keep the Wave 1 collect path.
        runs = [read_dir(d, skip_startup=skip_startup) for d in args.dir]
        runs_u = [{s: rec for s, rec in run.items() if s not in windows} for run in runs]
        res_u = collect(runs_u)
        res = {**res_u, **res}
    else:
        runs = [read_dir(d, skip_startup=skip_startup) for d in args.dir]
        res = collect(runs)
    for scen in sorted(k for k in res if k != "meta"):
        print(f"== {scen}")
        for m, v in sorted(res[scen].items()):
            print(f"   {m:16s} p50={v['p50']:<10} p95={v['p95']:<10} max={v['max']:<10} n={v['n']}")
        if scen in pair_tot:
            st = pair_tot[scen]
            print(f"   pair            paired={st.get('paired', 0)} "
                  f"mismatch={st.get('mismatch', 0)} "
                  f"unpaired_frame={st.get('unpaired_frame', 0)} "
                  f"unpaired_prev={st.get('unpaired_prev', 0)} "
                  f"truncated={st.get('truncated', 0)} "
                  f"windowed={st.get('windowed', 0)} "
                  f"active={st.get('active', 0)}")
    payload = {"meta": {"skip_startup": skip_startup}}
    ro_scens, ro_mets = resolve_record_only(
        args.record_only_scenario, args.record_only_metric)
    cli_ro = bool(args.record_only_scenario or args.record_only_metric)
    if windowing or cli_ro:
        # Unwindowed + no CLI flags: omit these keys so JSON stays Wave 1 identical.
        payload["meta"]["record_only_scenarios"] = ro_scens
        payload["meta"]["record_only_metrics"] = ro_mets
    if windowing:
        payload["meta"]["chart_windows"] = canonicalize_windows(windows)
        payload["meta"]["metric_source"] = dict(WINDOW_METRIC_SOURCE)
        payload["meta"]["pair"] = {
            scen: {k: v for k, v in st.items() if k != "first_mismatch"}
            for scen, st in pair_tot.items()
        }
        if any(st.get("first_mismatch") for st in pair_tot.values()):
            payload["meta"]["first_mismatch"] = {
                scen: st["first_mismatch"]
                for scen, st in pair_tot.items() if st.get("first_mismatch")
            }
    payload.update(res)
    if args.json:
        json.dump(payload, open(args.json, "w"), indent=1, sort_keys=True)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

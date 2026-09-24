#!/usr/bin/env bash
# Export official GameSePlayer cues from GameCommonSE / GameCustomSE_1 ACB
# into effects/*.ogg (existing filename_for mapping) plus hold_loop.json.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CRI_DIR="${WDS_CRI_DIR:-${ROOT}/wds-resources/Assets/StreamingAssets/OpenWDS/CRI}"
EFFECTS_DIR="${WDS_EFFECTS_DIR:-${ROOT}/effects}"
WORKDIR="${WDS_SE_EXTRACT_TMP:-${ROOT}/.cache/official-se}"

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing tool: $1" >&2
    exit 1
  fi
}

is_lfs_pointer() {
  local f="$1"
  [[ -f "$f" ]] && [[ "$(wc -c <"$f" | tr -d ' ')" -lt 1024 ]] &&
    head -n 1 "$f" | grep -q "git-lfs.github.com"
}

lfs_object_path() {
  local pointer="$1"
  local oid
  oid="$(awk '/^oid sha256:/{ sub(/^oid sha256:/, ""); print; exit }' "$pointer")"
  if [[ -z "$oid" || ${#oid} -lt 4 ]]; then
    return 1
  fi
  local store="${ROOT}/wds-resources/.git/lfs/objects/${oid:0:2}/${oid:2:2}/${oid}"
  if [[ -f "$store" ]]; then
    printf '%s\n' "$store"
    return 0
  fi
  return 1
}

resolve_acb() {
  local f="$1"
  if [[ ! -f "$f" ]]; then
    echo "missing ACB: $f" >&2
    exit 1
  fi
  if ! is_lfs_pointer "$f"; then
    printf '%s\n' "$f"
    return 0
  fi
  echo "fetching Git LFS object $(basename "$f")" >&2
  git -C "${ROOT}/wds-resources" lfs fetch --include "Assets/StreamingAssets/OpenWDS/CRI/$(basename "$f")" >/dev/null
  local store
  if store="$(lfs_object_path "$f")"; then
    local dest="${WORKDIR}/$(basename "$f")"
    cp "$store" "$dest"
    printf '%s\n' "$dest"
    return 0
  fi
  echo "ACB is still an LFS pointer and the object was not fetched: $f" >&2
  exit 1
}

find_vgmstream() {
  if command -v vgmstream-cli >/dev/null 2>&1; then
    command -v vgmstream-cli
    return
  fi
  if command -v vgmstream123 >/dev/null 2>&1; then
    command -v vgmstream123
    return
  fi
  echo "vgmstream-cli not found. Install with: brew install vgmstream" >&2
  exit 1
}

subsong_count() {
  local acb="$1"
  "${VGMSTREAM}" -m "$acb" 2>/dev/null | awk '
    /stream count/ { print $NF; found=1 }
    END { if (!found) print 1 }
  '
}

subsong_name() {
  local acb="$1"
  local index="$2"
  "${VGMSTREAM}" -m -s "$index" "$acb" 2>/dev/null | awk '
    /stream name:/ {
      sub(/^[^:]*:[[:space:]]*/, "")
      print
      exit
    }
  '
}

encode_ogg() {
  local wav="$1"
  local dest="$2"
  if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'libvorbis'; then
    ffmpeg -y -hide_banner -loglevel error -i "$wav" -c:a libvorbis -q:a 6 "$dest"
    return
  fi
  if command -v oggenc >/dev/null 2>&1; then
    oggenc -Q -q 6 -o "$dest" "$wav"
    return
  fi
  # Homebrew ffmpeg 9 bottles ship experimental native vorbis, not libvorbis.
  ffmpeg -y -hide_banner -loglevel error -i "$wav" -c:a vorbis -strict experimental -q:a 5 "$dest"
}

cue_matches() {
  local name="$1"
  local want="$2"
  local part
  local rest="$name"
  while [[ -n "$rest" ]]; do
    part="${rest%%;*}"
    rest="${rest#"$part"}"
    rest="${rest#;}"
    part="${part#"${part%%[![:space:]]*}"}"
    part="${part%"${part##*[![:space:]]}"}"
    if [[ "$part" == "$want" ]]; then
      return 0
    fi
  done
  return 1
}

write_hold_loop_json() {
  local acb="$1"
  local index="$2"
  local out="$3"
  local info
  info="$("${VGMSTREAM}" -m -s "$index" "$acb" 2>/dev/null || true)"
  local sample_rate
  sample_rate="$(printf "%s\n" "$info" | awk "/sample rate:/ { print \$3; exit }")"
  local loop_start
  loop_start="$(printf "%s\n" "$info" | awk "/loop start:/ { print \$3; exit }")"
  local loop_end
  loop_end="$(printf "%s\n" "$info" | awk "/loop end:/ { print \$3; exit }")"
  if [[ -z "${sample_rate:-}" || -z "${loop_start:-}" || -z "${loop_end:-}" ]]; then
    echo "warning: no loop metadata for Basic1_hold; skip hold_loop.json" >&2
    return 0
  fi
  python3 - "$out" "$sample_rate" "$loop_start" "$loop_end" <<'PY'
import sys
out, rate, start, end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
start_ms = 1000.0 * start / rate
end_ms = 1000.0 * end / rate
with open(out, "w", encoding="utf-8") as fh:
    fh.write("{\n")
    fh.write(f'  "start_ms": {start_ms:.6f},\n')
    fh.write(f'  "end_ms": {end_ms:.6f},\n')
    fh.write(f'  "sample_rate": {int(rate)},\n')
    fh.write(f'  "start_sample": {int(start)},\n')
    fh.write(f'  "end_sample": {int(end)}\n')
    fh.write("}\n")
print(f"wrote {out} start_ms={start_ms:.3f} end_ms={end_ms:.3f} samples={int(start)}-{int(end)} @{int(rate)}")
PY
}

export_cue() {
  local acb="$1"
  local want="$2"
  local dest="$3"
  local count
  count="$(subsong_count "$acb")"
  local i
  for ((i = 1; i <= count; i++)); do
    local name
    name="$(subsong_name "$acb" "$i")"
    if cue_matches "$name" "$want"; then
      echo "export $want <- $acb#$i ($name)"
      # -i: dump the authored stream once (Hold must not unfold CRI loops).
      "${VGMSTREAM}" -i -s "$i" -o "${WORKDIR}/${want}.wav" "$acb"
      encode_ogg "${WORKDIR}/${want}.wav" "$dest"
      if [[ "$want" == "Basic1_hold" ]]; then
        write_hold_loop_json "$acb" "$i" "${EFFECTS_DIR}/hold_loop.json"
      fi
      return 0
    fi
  done
  echo "cue not found: $want in $acb" >&2
  return 1
}

need ffmpeg
need python3
VGMSTREAM="$(find_vgmstream)"

mkdir -p "$EFFECTS_DIR" "$WORKDIR"
COMMON="$(resolve_acb "${CRI_DIR}/GameCommonSE.acb")"
CUSTOM="$(resolve_acb "${CRI_DIR}/GameCustomSE_1.acb")"

export_cue "$CUSTOM" "Basic1_perfect" "${EFFECTS_DIR}/_PERFECT.ogg"
export_cue "$CUSTOM" "Basic1_great" "${EFFECTS_DIR}/_PERFECT_ALTERNATIVE.ogg"
export_cue "$CUSTOM" "Basic1_good" "${EFFECTS_DIR}/_GOOD.ogg"
export_cue "$CUSTOM" "Basic1_bad" "${EFFECTS_DIR}/_GOOD_ALTERNATIVE.ogg"
export_cue "$CUSTOM" "Basic1_sound" "${EFFECTS_DIR}/Sirius Sound.ogg"
export_cue "$COMMON" "Basic1_critical" "${EFFECTS_DIR}/Sirius Critical.ogg"
export_cue "$COMMON" "Basic1_hold" "${EFFECTS_DIR}/_HOLD.ogg"
export_cue "$COMMON" "Scratch" "${EFFECTS_DIR}/Sirius Scratch.ogg"

echo "official Game SE exported to ${EFFECTS_DIR}"

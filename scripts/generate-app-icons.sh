#!/usr/bin/env bash
# Regenerate platform app icons from the single source logo.png at repo root.
# Outputs (committed under ui/assets/app_icon/):
#   wds.ico   — Windows exe / MSI shortcut
#   wds.icns  — macOS .app CFBundleIconFile (opaque full-bleed; OS applies squircle)
#   wds.png   — Linux .desktop + GLFW window icon (256×256)
#
# Requires: python3 + Pillow, and on macOS: iconutil.
# Prefer: /opt/anaconda3/bin/python3 when present.
# On Linux CI hosts without iconutil, keep committed artifacts; regenerate on a Mac.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/logo.png"
OUT="${ROOT}/ui/assets/app_icon"

[[ -f "$SRC" ]] || { echo "error: missing $SRC" >&2; exit 1; }

resolve_python() {
  local c
  for c in /opt/anaconda3/bin/python3 "$(command -v python3 || true)"; do
    [[ -n "$c" && -x "$c" ]] || continue
    if "$c" -c "from PIL import Image" >/dev/null 2>&1; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

PY="$(resolve_python)" || {
  echo "error: need python3 with Pillow (pip install pillow)" >&2
  exit 1
}
command -v iconutil >/dev/null || { echo "error: iconutil required (run on macOS)" >&2; exit 1; }

mkdir -p "$OUT"
"$PY" - "$SRC" "$OUT" <<'PY'
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

src = Path(sys.argv[1])
out = Path(sys.argv[2])
work = out / "_gen"
if work.exists():
    shutil.rmtree(work)
work.mkdir()

# Soft periwinkle from the art edge — fills transparent corners so macOS's
# squircle mask does not show empty glass around the sticker outline.
MAC_FILL = (154, 160, 228, 255)

sizes_ico = [16, 32, 48, 64, 128, 256]
sizes_icns = {
    16: ["icon_16x16.png"],
    32: ["diana.boggs@example.org", "icon_32x32.png"],
    64: ["ivan.p@example.net"],
    128: ["icon_128x128.png"],
    256: ["wendy.h@example.net", "icon_256x256.png"],
    512: ["wendy.h@example.net", "icon_512x512.png"],
    1024: ["walt.e@example.net"],
}


def resize_rgba(im: Image.Image, size: int) -> Image.Image:
    return im.resize((size, size), Image.Resampling.LANCZOS)


def flatten(im: Image.Image, fill) -> Image.Image:
    """Composite onto opaque fill (required for macOS Dock / Finder icons)."""
    base = Image.new("RGBA", im.size, fill)
    return Image.alpha_composite(base, im.convert("RGBA"))


def png_bytes(im: Image.Image) -> bytes:
    path = work / f"_tmp_{im.size[0]}.png"
    im.save(path, format="PNG", optimize=True)
    return path.read_bytes()


logo = Image.open(src).convert("RGBA")
# Win/Linux keep the sticker transparency; macOS gets opaque full-bleed.
logo_mac = flatten(logo, MAC_FILL)

png_cache = {}
for s in sizes_ico + [256]:
    png_cache[s] = png_bytes(resize_rgba(logo, s))

(out / "wds.png").write_bytes(png_cache[256])

entries = []
images = []
offset = 6 + 16 * len(sizes_ico)
for s in sizes_ico:
    data = png_cache[s]
    w = 0 if s >= 256 else s
    h = 0 if s >= 256 else s
    entries.append(struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(data), offset))
    images.append(data)
    offset += len(data)
(out / "wds.ico").write_bytes(
    struct.pack("<HHH", 0, 1, len(sizes_ico)) + b"".join(entries) + b"".join(images)
)

iconset = work / "wds.iconset"
iconset.mkdir()
for size, names in sizes_icns.items():
    layer = resize_rgba(logo_mac, size)
    for name in names:
        layer.save(iconset / name, format="PNG", optimize=True)
subprocess.check_call(["iconutil", "-c", "icns", str(iconset), "-o", str(out / "wds.icns")])
shutil.rmtree(work)
print(f"Wrote {out/'wds.ico'}, {out/'wds.icns'}, {out/'wds.png'} (mac fill={MAC_FILL[:3]})")
PY

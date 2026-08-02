#!/usr/bin/env bash
# Package a built WDS target into a self-contained release artifact.
# Intended build hosts:
#   win-x86_64 — Linux machine with MinGW / vcpkg (Linux is host-only, not a product target)
#   macos-arm  — Apple Silicon Mac with Homebrew deps
#
# External deps policy (ship everything we can):
#   win   — static MinGW runtimes + vcpkg static GLFW/png; ship bass.dll + vulkan-1.dll
#   macOS — bundle Homebrew dylibs + MoltenVK into the .app
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
# shellcheck disable=SC1091
source "${ROOT}/scripts/lib/wds-env.sh"

CACHE_DIR="${WDS_RUNTIME_CACHE:-${ROOT}/.cache/runtime}"
DIST_DIR="${ROOT}/dist"
MSITOOLS_PREFIX="${WDS_MSITOOLS_PREFIX}"

usage() {
  cat <<EOF
Usage: $(basename "$0") <target> [--build-dir DIR]
       $(basename "$0") --stage-win-debug <build-dir>

Targets: win-x86_64 | macos-arm

Writes: dist/wds-win-x86_64.msi   (+ portable dist/wds-win-x86_64.zip)
        dist/wds-macos-arm.zip + dist/wds-macos-arm.dmg

--stage-win-debug:
  Copy skins/effects/shaders/fonts/icons + vulkan-1.dll next to
  <build-dir>/ui/wds_editor.exe so a Win Debug tree is runnable without MSI/zip.
  (Cross-compiled Debug bakes Linux WDS_REPO_ROOT / WDS_SHADER_DIR paths.)

Environment (see scripts/env.example):
  WDS_ENV_FILE / scripts/env.local
  WDS_RUNTIME_CACHE / WDS_VULKAN_COMPONENTS_URL
  WDS_MSITOOLS_PREFIX / WDS_MSITOOLS_LIB_DIR / WDS_WIXL_SHARE_DIRS / WDS_WIXL_VERSION
  WDS_MINGW_CXX / WDS_MINGW_OBJDUMP / WDS_MINGW_DLL_DIRS
  WDS_PRODUCT_VERSION
EOF
}

die() { echo "error: $*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

# Prefer PATH, then a user-extracted msitools tree (usr/bin + usr/lib).
# Always wire LD_LIBRARY_PATH for the user prefix: finding wixl on PATH (from a
# prior run) without libmsi loaded makes msiinfo fail silently and trip false
# "MSI missing …" sanity checks.
ensure_msitools_path() {
  local bin="${MSITOOLS_PREFIX}/usr/bin"
  local lib="${WDS_MSITOOLS_LIB_DIR:-}"

  if [[ -x "${bin}/wixl" && -x "${bin}/wixl-heat" && -x "${bin}/msiinfo" ]]; then
    case ":${PATH}:" in
      *":${bin}:"*) ;;
      *) export PATH="${bin}:${PATH}" ;;
    esac
    if [[ -d "$lib" ]]; then
      case ":${LD_LIBRARY_PATH:-}:" in
        *":${lib}:"*) ;;
        *) export LD_LIBRARY_PATH="${lib}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" ;;
      esac
    fi
    return 0
  fi

  if command -v wixl >/dev/null 2>&1 && command -v wixl-heat >/dev/null 2>&1 \
      && command -v msiinfo >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

# Run msiinfo with the msitools env; fail loudly (do not swallow stderr).
msiinfo_export() {
  local msi="$1" table="$2"
  need_cmd msiinfo
  msiinfo export "$msi" "$table"
}

resolve_wixl_share() {
  local d IFS=':'
  local -a dirs=()
  IFS=':' read -r -a dirs <<< "${WDS_WIXL_SHARE_DIRS}"
  for d in "${dirs[@]}"; do
    [[ -n "$d" ]] || continue
    if [[ -d "${d}/include" ]]; then
      echo "$d"
      return 0
    fi
  done
  return 1
}

download() {
  local url="$1" out="$2"
  mkdir -p "$(dirname "$out")"
  if [[ -f "$out" && -s "$out" ]]; then
    echo "Using cached: $out"
    return 0
  fi
  echo "Downloading: $url"
  curl -fL --retry 3 --retry-delay 2 -o "${out}.partial" "$url"
  mv "${out}.partial" "$out"
}

# Portable resource tree shared by every platform (zip root or Contents/Resources):
#   skins/ effects/ fonts/ icons/ shaders/ wds.png [licenses/] [optional sample ogg]
copy_skins_effects() {
  local stage="$1"
  mkdir -p "${stage}"
  if [[ -d "${ROOT}/skins" ]]; then
    cp -a "${ROOT}/skins" "${stage}/"
  else
    echo "warning: skins/ missing" >&2
  fi
  if [[ -d "${ROOT}/effects" ]]; then
    cp -a "${ROOT}/effects" "${stage}/"
  fi
  if [[ -f "${ROOT}/suzume_no_tojimari.ogg" ]]; then
    cp -a "${ROOT}/suzume_no_tojimari.ogg" "${stage}/"
  fi
}

copy_fonts() {
  local stage="$1" build_dir="$2"
  mkdir -p "${stage}/fonts"
  if [[ -d "${build_dir}/ui/fonts" ]]; then
    cp -a "${build_dir}/ui/fonts/." "${stage}/fonts/"
  elif [[ -f "${ROOT}/ui/assets/fonts/NotoSansSC-Regular.ttf" ]]; then
    cp -a "${ROOT}/ui/assets/fonts/NotoSansSC-Regular.ttf" "${stage}/fonts/"
  else
    die "UI font missing (expected build ui/fonts or ui/assets/fonts/NotoSansSC-Regular.ttf)"
  fi
  # Ship font license next to the font when available.
  if [[ -f "${ROOT}/ui/assets/fonts/OFL.txt" ]]; then
    mkdir -p "${stage}/licenses"
    cp -a "${ROOT}/ui/assets/fonts/OFL.txt" "${stage}/licenses/NotoSansSC-OFL.txt"
  fi
}

copy_icons() {
  local stage="$1" build_dir="$2"
  if [[ -d "${build_dir}/ui/icons" ]]; then
    mkdir -p "${stage}/icons"
    cp -a "${build_dir}/ui/icons/." "${stage}/icons/"
  elif [[ -d "${ROOT}/icons" ]]; then
    mkdir -p "${stage}/icons"
    cp -a "${ROOT}/icons/." "${stage}/icons/"
  else
    die "toolbar icons missing (expected build ui/icons or repo icons/)"
  fi
}

# App icon derived from logo.png (see scripts/generate-app-icons.sh).
copy_app_icon() {
  local stage="$1"
  local png="${ROOT}/ui/assets/app_icon/wds.png"
  if [[ -f "$png" ]]; then
    cp -a "$png" "${stage}/wds.png"
  elif [[ -f "${ROOT}/logo.png" ]]; then
    cp -a "${ROOT}/logo.png" "${stage}/wds.png"
  else
    die "app icon missing (expected ui/assets/app_icon/wds.png or logo.png)"
  fi
}

# Compiled SPIR-V (.spv) — required at runtime on every platform.
copy_shaders() {
  local stage="$1" build_dir="$2"
  local src="${build_dir}/renderer/shaders"
  if [[ ! -d "$src" ]]; then
    die "missing compiled shaders dir ${src} (rebuild renderer first)"
  fi
  mkdir -p "${stage}/shaders"
  local spv_count=0
  local f
  for f in "${src}/"*.spv; do
    [[ -f "$f" ]] || continue
    cp -a "$f" "${stage}/shaders/"
    spv_count=$((spv_count + 1))
  done
  [[ "$spv_count" -gt 0 ]] || die "no .spv files in ${src}"
  [[ -f "${stage}/shaders/textured_quad.vert.spv" && -f "${stage}/shaders/textured_quad.frag.spv" ]] || \
    die "required textured_quad.*.spv missing after copy from ${src}"
}

# One call for all platforms — keeps zip / .app resource layouts identical.
copy_portable_resources() {
  local stage="$1" build_dir="$2"
  copy_skins_effects "$stage"
  copy_fonts "$stage" "$build_dir"
  copy_icons "$stage" "$build_dir"
  copy_app_icon "$stage"
  copy_shaders "$stage" "$build_dir"
}

resolve_editor_bin() {
  local build_dir="$1" ext="${2:-}"
  if [[ -x "${build_dir}/ui/wds_editor${ext}" || -f "${build_dir}/ui/wds_editor${ext}" ]]; then
    echo "${build_dir}/ui/wds_editor${ext}"
  else
    return 1
  fi
}

copy_bass_runtime() {
  local stage="$1" target="$2"
  case "$target" in
    macos-arm)
      mkdir -p "${stage}/lib"
      cp -a "${ROOT}/audio-player/third_party/bass/macos-arm/libbass.dylib" "${stage}/lib/"
      ;;
    win-x86_64)
      # Windows package layout puts the exe at stage root (with MinGW / vulkan DLLs);
      # bass.dll must sit next to the exe for the loader to find it.
      cp -a "${ROOT}/audio-player/third_party/bass/win-x86_64/bass.dll" "${stage}/"
      ;;
  esac
}

write_readme() {
  local stage="$1" target="$2"
  case "$target" in
    win-x86_64)
      cat >"${stage}/README.txt" <<'EOF'
WDS Editor package (win-x86_64) — self-contained layout

Preferred install: run dist/wds-win-x86_64.msi (Program Files\WDS Editor).
Re-running the MSI updates an existing install in place (UpdateDlg; single ARP entry).
This zip is a portable copy of the same payload.

Bundled next to wds_editor.exe:
  skins\ effects\ fonts\ icons\ shaders\ wds.png
  bass.dll, vulkan-1.dll, Uninstall.exe
MinGW libgcc/libstdc++/winpthread are statically linked into the exe when possible.

1. Keep this folder layout intact, then double-click wds_editor.exe.
2. If Vulkan fails to start, install a Vulkan Runtime from
   https://vulkan.lunarg.com/ (most GPU drivers already provide one).
3. Settings live in %LOCALAPPDATA%\WDS\config\config.yml
   so MSI upgrades / replacing this folder do not wipe preferences.
4. MSI installs offer path + shortcut options; Uninstall.exe removes the MSI install.

Still required from the OS / GPU stack (cannot ship):
  - a GPU driver with Vulkan support
EOF
      ;;
    macos-arm)
      cat >"${stage}/README.txt" <<'EOF'
WDS Editor package (macos-arm) — self-contained .app

Inside WDS Editor.app:
  Contents/MacOS/       wds_editor + lib/ (glfw, png, vulkan, MoltenVK, bass)
  Contents/Resources/   skins effects fonts icons shaders wds.icns wds.png
                        + vulkan/icd.d/MoltenVK_icd.json

1. Prefer the DMG: open wds-macos-arm.dmg, drag WDS Editor.app into Applications.
2. Or from this zip: double-click WDS Editor.app (keep the .app bundle intact).
3. From Terminal use open (do not execute the .app path — it is a directory):
     open "/Applications/WDS Editor.app"
4. If Gatekeeper blocks a downloaded build, clear quarantine once:
     xattr -cr "/Applications/WDS Editor.app"
5. Settings live in ~/Library/Application Support/WDS/config/config.yml
   so replacing the .app does not wipe preferences.

Metal GPU required (MoltenVK is bundled; no separate Vulkan install).
EOF
      ;;
  esac
}

make_zip() {
  local stage="$1" zip_path="$2"
  mkdir -p "$(dirname "$zip_path")"
  rm -f "$zip_path"
  need_cmd zip
  (
    cd "$(dirname "$stage")"
    zip -r -q "$zip_path" "$(basename "$stage")"
  )
  echo "Packaged: $zip_path"
}

# --- win-x86_64 -------------------------------------------------------------

mingw_dll() {
  local name="$1"
  local path cxx="${WDS_MINGW_CXX}"
  path="$("${cxx}" -print-file-name="$name" 2>/dev/null || true)"
  if [[ -n "$path" && "$path" != "$name" && -f "$path" ]]; then
    echo "$path"
    return 0
  fi
  # Optional colon-separated dirs from env; otherwise probe compiler libdir + common layouts.
  local -a search=()
  local d IFS=':'
  if [[ -n "${WDS_MINGW_DLL_DIRS:-}" ]]; then
    IFS=':' read -r -a search <<< "${WDS_MINGW_DLL_DIRS}"
  else
    local libdir
    libdir="$("${cxx}" -print-file-name=libstdc++.a 2>/dev/null || true)"
    if [[ -n "$libdir" && "$libdir" != "libstdc++.a" ]]; then
      search+=("$(dirname "$libdir")")
    fi
    search+=(
      "/usr/lib/gcc/${WDS_MINGW_TRIPLE}"
      "/usr/${WDS_MINGW_TRIPLE}/lib"
    )
    # Expand versioned gcc dirs if present
    local g
    for g in /usr/lib/gcc/${WDS_MINGW_TRIPLE}/*; do
      [[ -d "$g" ]] && search+=("$g" "${g}-posix" "${g}-win32")
    done
  fi
  for d in "${search[@]}"; do
    [[ -n "$d" && -f "${d}/${name}" ]] && { echo "${d}/${name}"; return 0; }
  done
  return 1
}

package_win() {
  local build_dir="$1"
  local demo=""
  demo="$(resolve_editor_bin "$build_dir" ".exe")" || \
    die "missing wds_editor.exe (build ui first)"
  local example="${build_dir}/core/wds_core_example.exe"

  need_cmd curl
  need_cmd unzip

  local stage="${DIST_DIR}/staging/wds-win-x86_64"
  rm -rf "$stage"
  mkdir -p "${stage}"
  # Exe at package root for double-click launch.
  # Never ship config/ next to the exe — prefs go to %LOCALAPPDATA%\WDS\config\.
  cp -a "$demo" "${stage}/"
  [[ -f "$example" ]] && cp -a "$example" "${stage}/"
  # MSI helper (also useful next to the portable zip payload).
  local uninstall_exe=""
  for uninstall_exe in \
      "${build_dir}/ui/Uninstall.exe" \
      "${build_dir}/ui/wds_uninstall.exe"; do
    if [[ -f "$uninstall_exe" ]]; then
      cp -a "$uninstall_exe" "${stage}/Uninstall.exe"
      break
    fi
  done
  if [[ ! -f "${stage}/Uninstall.exe" ]]; then
    # Fallback: cross-compile helper when packaging host has MinGW but CMake target was skipped.
    if command -v "${WDS_MINGW_CXX}" >/dev/null 2>&1; then
      echo "Building Uninstall.exe with MinGW…"
      "${WDS_MINGW_CXX}" -O2 -std=c++17 -mwindows -municode \
        -o "${stage}/Uninstall.exe" \
        "${ROOT}/ui/apps/wds_uninstall.cpp" \
        -static-libgcc -static-libstdc++ -lmsi -lshell32 -luser32 -lole32 \
        || echo "warning: failed to build Uninstall.exe" >&2
    else
      echo "warning: Uninstall.exe not found in build dir; MSI will lack uninstall helper" >&2
    fi
  fi
  rm -rf "${stage}/config"

  # Prefer statically linked MinGW runtimes; only copy DLLs when the link was dynamic.
  local dll
  for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    if "${WDS_MINGW_OBJDUMP}" -p "${stage}/$(basename "$demo")" 2>/dev/null | \
         grep -qi "DLL Name: ${dll}"; then
      local src
      if src="$(mingw_dll "$dll")"; then
        cp -a "$src" "${stage}/"
      else
        echo "warning: exe imports ${dll} but it was not found" >&2
      fi
    fi
  done

  # Any DLLs already next to the built exe (POST_BUILD bass, rare shared deps).
  local side
  for side in "$(dirname "$demo")"/*.dll; do
    [[ -f "$side" ]] || continue
    case "$(basename "$side")" in
      libstdc++-6.dll|libgcc_s_seh-1.dll|libwinpthread-1.dll)
        # Skip unless the exe actually imports them (handled above).
        if "${WDS_MINGW_OBJDUMP}" -p "${stage}/$(basename "$demo")" 2>/dev/null | \
             grep -qi "DLL Name: $(basename "$side")"; then
          cp -a "$side" "${stage}/"
        fi
        ;;
      *)
        cp -a "$side" "${stage}/"
        ;;
    esac
  done

  local comp_url="${WDS_VULKAN_COMPONENTS_URL:-https://sdk.lunarg.com/sdk/download/latest/windows/vulkan-runtime-components.zip?Human=true}"
  local comp_zip="${CACHE_DIR}/vulkan-runtime-components.zip"
  download "$comp_url" "$comp_zip"
  local extract="${CACHE_DIR}/vulkan-rt-components"
  rm -rf "$extract"
  mkdir -p "$extract"
  unzip -q -o "$comp_zip" -d "$extract"
  local vk_dll
  vk_dll="$(find "$extract" -type f -path '*/x64/vulkan-1.dll' | head -1 || true)"
  [[ -n "$vk_dll" ]] || die "vulkan-1.dll not found inside runtime components zip"
  cp -a "$vk_dll" "${stage}/vulkan-1.dll"

  copy_portable_resources "$stage" "$build_dir"
  copy_bass_runtime "$stage" win-x86_64
  # MSI Start Menu shortcut Icon= needs a real .ico (not the exe).
  local ico="${ROOT}/ui/assets/app_icon/wds.ico"
  [[ -f "$ico" ]] || die "missing Windows app icon: $ico (run scripts/generate-app-icons.sh)"
  cp -a "$ico" "${stage}/wds.ico"
  rm -rf "${stage}/config"

  write_readme "$stage" win-x86_64
  make_zip "$stage" "${DIST_DIR}/wds-win-x86_64.zip"
  make_win_msi "$stage" "${DIST_DIR}/wds-win-x86_64.msi"
}

# Build a standard per-machine MSI from the staged Windows payload (wixl / msitools).
# Windows Installer Icon table is unreliable with PNG-compressed ICO entries.
# Generate a small BMP-only ICO next to the staged payload for ARP / shortcuts.
make_win_msi_bmp_icon() {
  local stage="$1"
  local out="${stage}/wds-msi.ico"
  local src=""
  local candidate
  for candidate in \
      "${stage}/wds.png" \
      "${stage}/wds.ico" \
      "${ROOT}/ui/assets/app_icon/wds.png" \
      "${ROOT}/ui/assets/app_icon/wds.ico" \
      "${ROOT}/logo.png"; do
    if [[ -f "$candidate" ]]; then
      src="$candidate"
      break
    fi
  done
  [[ -n "$src" ]] || die "no icon source for MSI (expected wds.png / wds.ico / logo.png)"

  python3 - "$src" "$out" <<'PY'
import struct, sys
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover
    raise SystemExit(f"Pillow required to build MSI icon: {exc}") from exc

src, out = Path(sys.argv[1]), Path(sys.argv[2])
base = Image.open(src).convert("RGBA")

def bmp_bytes(img: Image.Image) -> bytes:
    """Windows BMP (BI_RGB) with bottom-up rows + AND mask, for ICO entries."""
    w, h = img.size
    rgba = img.split()
    rgb = Image.merge("RGB", rgba[:3])
    alpha = rgba[3]
    row_stride = (w * 3 + 3) & ~3
    xor = bytearray()
    # Bottom-up BGR
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b = rgb.getpixel((x, y))
            row += bytes((b, g, r))
        row += b"\x00" * (row_stride - w * 3)
        xor += row
    and_stride = ((w + 31) // 32) * 4
    and_mask = bytearray()
    for y in range(h - 1, -1, -1):
        bits = 0
        byte = 0
        row = bytearray()
        for x in range(w):
            a = alpha.getpixel((x, y))
            byte = (byte << 1) | (0 if a >= 128 else 1)
            bits += 1
            if bits == 8:
                row.append(byte)
                bits = 0
                byte = 0
        if bits:
            row.append(byte << (8 - bits))
        row += b"\x00" * (and_stride - len(row))
        and_mask += row
    dib = struct.pack(
        "<IIIHHIIIIII",
        40, w, h * 2, 1, 24, 0, len(xor), 0, 0, 0, 0,
    )
    return dib + xor + and_mask

sizes = (16, 32, 48)
entries = []
blobs = []
offset = 6 + 16 * len(sizes)
for size in sizes:
    img = base.resize((size, size), Image.Resampling.LANCZOS)
    blob = bmp_bytes(img)
    entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 24, len(blob), offset))
    blobs.append(blob)
    offset += len(blob)

out.write_bytes(struct.pack("<HHH", 0, 1, len(sizes)) + b"".join(entries) + b"".join(blobs))
print(f"MSI icon: {out} (from {src})")
PY
}

make_win_msi() {
  local stage="$1" msi_path="$2"
  local product_wxs="${ROOT}/scripts/win-msi-product.wxs"
  local ui_wxs="${ROOT}/scripts/win-msi-ui.wxs"
  [[ -f "$product_wxs" ]] || die "missing MSI product template: $product_wxs"
  [[ -f "$ui_wxs" ]] || die "missing MSI UI template: $ui_wxs"

  ensure_msitools_path || die \
    "wixl not found. Install msitools/wixl, or extract them under ${MSITOOLS_PREFIX}"
  need_cmd wixl
  need_cmd wixl-heat
  need_cmd python3

  local wixl_share=""
  wixl_share="$(resolve_wixl_share)" || die "wixl share data not found (wixl-data package)"

  local work="${DIST_DIR}/staging/msi-win-x86_64-work"
  local heat_wxs="${work}/files.wxs"
  local version="${WDS_PRODUCT_VERSION}"
  rm -rf "$work"
  mkdir -p "$work"

  make_win_msi_bmp_icon "$stage"

  # Heat wants paths relative to --prefix; keep README for users browsing Program Files.
  # Exclude packaging-only BMP ICO from the installed payload (Icon table embeds it).
  # Sort find output so harvest order (and Directory nesting) is deterministic.
  stage_base="$(basename "$stage")"
  stage_parent="$(dirname "$stage")"
  (
    cd "$stage_parent"
    find "$stage_base" -type f ! -path '*/config/*' ! -name 'wds-msi.ico' | LC_ALL=C sort \
      | wixl-heat -p "${stage_base}/" \
          --directory-ref INSTALLDIR \
          --component-group ProductFiles \
          --var var.SourceDir \
          --win64
  ) >"$heat_wxs"

  # Fix heat output for a reliable first-install payload:
  # 1) Hoist <Directory Name="."> root files directly under INSTALLDIR.
  # 2) Force Win64="yes" (avoid broken $(var.Win64) expansion → 32-bit components).
  # 3) Merge private runtime DLLs into the wds_editor.exe component so they cannot be
  #    skipped independently (classic "missing bass.dll until Repair" failure mode).
  python3 - "$heat_wxs" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

# Hoist every <Directory Name=".">…</Directory> block (one or many Components).
text = re.sub(
    r'<Directory Id="[^"]+" Name="\.">\s*([\s\S]*?)\s*</Directory>',
    r"\1",
    text,
)

text = text.replace('Win64="$(var.Win64)"', 'Win64="yes"')

RUNTIME_DLLS = ("bass.dll", "vulkan-1.dll")
EXE_NAME = "wds_editor.exe"


def component_blocks(xml):
    return list(re.finditer(r"<Component\b[^>]*>[\s\S]*?</Component>", xml))


def file_source_basename(comp_xml):
    m = re.search(r'Source="[^"]*[/\\]([^"/\\]+)"', comp_xml)
    return m.group(1) if m else None


def strip_keypath(file_xml):
    return re.sub(r'\s+KeyPath="yes"', "", file_xml, count=1)


blocks = component_blocks(text)
exe_match = None
dll_matches = {}
for m in blocks:
    base = file_source_basename(m.group(0))
    if base == EXE_NAME:
        exe_match = m
    elif base in RUNTIME_DLLS:
        dll_matches[base] = m

if exe_match is None:
    raise SystemExit(f"heat WXS missing {EXE_NAME} component")
missing = [n for n in RUNTIME_DLLS if n not in dll_matches]
if missing:
    raise SystemExit(f"heat WXS missing runtime DLL component(s): {', '.join(missing)}")

# Collect DLL <File .../> elements (non-keypath) and drop their Components.
dll_files = []
dll_ids = []
for name in RUNTIME_DLLS:
    m = dll_matches[name]
    comp = m.group(0)
    cid = re.search(r'\bId="([^"]+)"', comp).group(1)
    dll_ids.append(cid)
    for fm in re.finditer(r"<File\b[^>]*/>", comp):
        dll_files.append(strip_keypath(fm.group(0)))

# Newest-last so index math stays valid while deleting.
for m in sorted(dll_matches.values(), key=lambda x: x.start(), reverse=True):
    text = text[: m.start()] + text[m.end() :]

# Re-find exe component after deletions.
exe_match = None
for m in component_blocks(text):
    if file_source_basename(m.group(0)) == EXE_NAME:
        exe_match = m
        break
if exe_match is None:
    raise SystemExit(f"lost {EXE_NAME} component while merging runtime DLLs")

exe_comp = exe_match.group(0)
if "</Component>" not in exe_comp:
    raise SystemExit("malformed exe component")
merged = exe_comp.replace(
    "</Component>",
    "".join(f"\n      {f}" for f in dll_files) + "\n    </Component>",
    1,
)
text = text[: exe_match.start()] + merged + text[exe_match.end() :]

# Drop ComponentRefs for removed DLL components.
for cid in dll_ids:
    text = re.sub(
        rf'\s*<ComponentRef Id="{re.escape(cid)}"/>\s*',
        "\n",
        text,
    )

path.write_text(text, encoding="utf-8")

# Hard fail if merge did not stick (packaging host must not ship a broken MSI).
final = path.read_text(encoding="utf-8")
if "bass.dll" not in final or "vulkan-1.dll" not in final:
    raise SystemExit("runtime DLLs missing from heat WXS after merge")
if final.count("bass.dll") != 1 or final.count("vulkan-1.dll") != 1:
    raise SystemExit("runtime DLL Source paths must appear exactly once after merge")
exe_blocks = [
    m.group(0)
    for m in component_blocks(final)
    if file_source_basename(m.group(0)) == EXE_NAME
]
if len(exe_blocks) != 1 or "bass.dll" not in exe_blocks[0] or "vulkan-1.dll" not in exe_blocks[0]:
    raise SystemExit("bass.dll/vulkan-1.dll must live inside the wds_editor.exe component")
print(
    "MSI harvest: hoisted root files; Win64=yes; "
    "merged bass.dll+vulkan-1.dll into wds_editor.exe component"
)
PY

  mkdir -p "$(dirname "$msi_path")"
  rm -f "$msi_path"
  echo "Building MSI with wixl (UI: InstallDir / Update + Shortcuts)…"
  (
    cd "$work"
    wixl -a x64 -o "$msi_path" \
      -D "SourceDir=${stage}" \
      -D "Win64=yes" \
      -D "ProductVersion=${version}" \
      --wxidir "${wixl_share}/include" \
      --extdir "${wixl_share}/ext" \
      --ext ui \
      "$product_wxs" "$ui_wxs" "$heat_wxs"
  )

  [[ -f "$msi_path" ]] || die "wixl did not produce $msi_path"

  # Sanity checks for a usable first-run / upgrade UI.
  # Export once with a working msiinfo (see ensure_msitools_path); empty dumps
  # used to look like "missing BrowseDlg" when libmsi was not loadable.
  need_cmd msiinfo
  msiinfo --help >/dev/null 2>&1 || die "msiinfo is not runnable (check LD_LIBRARY_PATH / msitools install)"

  local ui_seq="" events="" features="" customs="" dialogs="" props="" regs="" upgrades=""
  ui_seq="$(msiinfo_export "$msi_path" InstallUISequence | tr -d '\r')" || die "msiinfo failed: InstallUISequence"
  events="$(msiinfo_export "$msi_path" ControlEvent | tr -d '\r')" || die "msiinfo failed: ControlEvent"
  features="$(msiinfo_export "$msi_path" Feature | tr -d '\r')" || die "msiinfo failed: Feature"
  customs="$(msiinfo_export "$msi_path" CustomAction | tr -d '\r')" || die "msiinfo failed: CustomAction"
  dialogs="$(msiinfo_export "$msi_path" Dialog | tr -d '\r')" || die "msiinfo failed: Dialog"
  props="$(msiinfo_export "$msi_path" Property | tr -d '\r')" || die "msiinfo failed: Property"
  regs="$(msiinfo_export "$msi_path" RegLocator | tr -d '\r')" || die "msiinfo failed: RegLocator"
  upgrades="$(msiinfo_export "$msi_path" Upgrade | tr -d '\r')" || die "msiinfo failed: Upgrade"

  # Use <<< (not echo|grep): with pipefail, grep -q exiting early SIGPIPEs echo and
  # falsely trips `|| die` (seen as "MSI File table missing wds_editor.exe").
  grep -q 'WelcomeEulaDlg' <<<"${ui_seq}" && \
    die "MSI still schedules WelcomeEulaDlg; refusing broken UI"
  grep -q 'InstallDirDlg' <<<"${ui_seq}" || \
    die "MSI missing InstallDirDlg in InstallUISequence"
  grep -q 'UpdateDlg' <<<"${ui_seq}" || \
    die "MSI missing UpdateDlg in InstallUISequence"
  grep -q $'ProductLanguage\t1033' <<<"${props}" || \
    die "MSI ProductLanguage is not 1033 (UI language mismatch risk)"
  grep -Fq $'Remove\tDesktopFeature\tNOT CREATE_DESKTOP_SHORTCUT' <<<"${events}" || \
    die "MSI missing conditional Remove for DesktopFeature"
  grep -Fq $'Remove\tStartMenuFeature\tNOT CREATE_STARTMENU_SHORTCUT' <<<"${events}" || \
    die "MSI missing conditional Remove for StartMenuFeature"
  grep -q 'DesktopFeature' <<<"${features}" || die "MSI missing DesktopFeature"
  grep -q 'StartMenuFeature' <<<"${features}" || die "MSI missing StartMenuFeature"
  grep -q 'BrowseDlg' <<<"${dialogs}" || die "MSI missing BrowseDlg"
  grep -Fq 'SetInstallDirFromBrowse' <<<"${customs}" || \
    die "MSI missing SetInstallDirFromBrowse custom action"
  grep -Fq 'SetInstallDirFromPrevious' <<<"${customs}" || \
    die "MSI missing SetInstallDirFromPrevious custom action"
  grep -q 'FindWdsInstallDir' <<<"${regs}" || \
    die "MSI missing FindWdsInstallDir registry search"
  grep -q 'A7E3C2B1-9F4D-4E8A-9C6B-1D2E3F4A5B6C' <<<"${upgrades}" || \
    die "MSI missing MajorUpgrade Upgrade table entry"
  # Runtime DLLs must be in the File table (merged into the exe component at harvest).
  local files_tbl=""
  files_tbl="$(msiinfo_export "$msi_path" File | tr -d '\r')" || die "msiinfo failed: File"
  grep -Fq 'bass.dll' <<<"${files_tbl}" || die "MSI File table missing bass.dll"
  grep -Fq 'vulkan-1.dll' <<<"${files_tbl}" || die "MSI File table missing vulkan-1.dll"
  grep -Fq 'wds_editor.exe' <<<"${files_tbl}" || die "MSI File table missing wds_editor.exe"
  # InstallDirDlg must run after FindRelatedProducts / costing (not sequence 1).
  local dir_seq
  dir_seq="$(awk -F'\t' '$1=="InstallDirDlg"{print $3; exit}' <<<"${ui_seq}")"
  [[ -n "${dir_seq}" && "${dir_seq}" -ge 1000 ]] || \
    die "InstallDirDlg sequence ${dir_seq:-unset} is too early (want >= 1000)"

  rm -f "${stage}/wds-msi.ico"
  rm -rf "$work"
  echo "Packaged: $msi_path"
}

# --- macos-arm --------------------------------------------------------------

brew_prefix() {
  if command -v brew >/dev/null 2>&1; then
    brew --prefix
  else
    echo "/opt/homebrew"
  fi
}

copy_macho_deps() {
  local bin="$1" libdir="$2"
  mkdir -p "$libdir"
  local -a queue=("$bin")
  local seen_file brew_root
  seen_file="$(mktemp)"
  trap 'rm -f "'"$seen_file"'"' RETURN
  brew_root="$(brew_prefix)"

  while ((${#queue[@]})); do
    local cur="${queue[0]}"
    queue=("${queue[@]:1}")
    grep -Fxq -- "$cur" "$seen_file" 2>/dev/null && continue
    printf '%s\n' "$cur" >>"$seen_file"

    while IFS= read -r dep; do
      [[ -z "$dep" ]] && continue
      case "$dep" in
        /usr/lib/*|/System/*) continue ;;
      esac
      [[ "$dep" == /* ]] || continue
      local base dest
      base="$(basename "$dep")"
      dest="${libdir}/${base}"
      if [[ -f "$dep" && ! -e "$dest" ]]; then
        # Homebrew bottles are often mode 444; keep copies writable for rpath rewrite / overwrite.
        cp -aL "$dep" "$dest"
        chmod u+w "$dest" 2>/dev/null || true
        queue+=("$dest")
      fi
    done < <(otool -L "$cur" 2>/dev/null | awk 'NR>1 {print $1}')
  done
  unset brew_root
}

fix_macos_rpaths() {
  # `payload` is the directory that contains the main binary and a lib/ folder
  # (e.g. WDS.app/Contents/MacOS). Rewrite absolute Homebrew paths and set
  # relocatable install names so the .app does not need brew at runtime.
  local payload="$1"
  command -v install_name_tool >/dev/null 2>&1 || return 0
  local f
  for f in "${payload}/wds_editor" "${payload}/wds_core_example" \
           "${payload}/lib/"*; do
    [[ -f "$f" ]] || continue
    file "$f" 2>/dev/null | grep -q 'Mach-O' || continue

    if [[ "$(dirname "$f")" == "${payload}/lib" ]]; then
      install_name_tool -id "@loader_path/$(basename "$f")" "$f" 2>/dev/null || true
      install_name_tool -add_rpath '@loader_path' "$f" 2>/dev/null || true
    else
      install_name_tool -add_rpath '@loader_path/lib' "$f" 2>/dev/null || true
      install_name_tool -add_rpath '@loader_path' "$f" 2>/dev/null || true
    fi

    while IFS= read -r dep; do
      [[ -z "$dep" ]] && continue
      case "$dep" in
        /usr/lib/*|/System/*) continue ;;
      esac
      local base
      base="$(basename "$dep")"
      [[ -f "${payload}/lib/${base}" ]] || continue
      if [[ "$(dirname "$f")" == "$payload" ]]; then
        install_name_tool -change "$dep" "@loader_path/lib/${base}" "$f" 2>/dev/null || true
      else
        install_name_tool -change "$dep" "@loader_path/${base}" "$f" 2>/dev/null || true
      fi
    done < <(otool -L "$f" 2>/dev/null | awk 'NR>1 {print $1}')

    # Normalize @rpath/libbass.dylib → @loader_path/... so rpath alone is enough.
    if [[ "$(dirname "$f")" == "$payload" ]]; then
      install_name_tool -change '@rpath/libbass.dylib' '@loader_path/lib/libbass.dylib' "$f" 2>/dev/null || true
    else
      install_name_tool -change '@rpath/libbass.dylib' '@loader_path/libbass.dylib' "$f" 2>/dev/null || true
    fi
  done
}

write_macos_app_bundle() {
  # Self-contained "WDS Editor.app":
  #   Contents/MacOS  — executable + dylibs only (required for codesign seal)
  #   Contents/Resources — same portable tree as zip packages + vulkan ICD
  local stage="$1" demo_src="$2" build_dir="$3"
  local demo_name
  demo_name="$(basename "$demo_src")"
  local app="${stage}/WDS Editor.app"
  local payload="${app}/Contents/MacOS"
  local resources="${app}/Contents/Resources"
  # Vulkan loader discovers ICDs at Contents/Resources/vulkan/icd.d (not share/...).
  mkdir -p "${payload}/lib" "${resources}/vulkan/icd.d"

  cp -a "$demo_src" "${payload}/${demo_name}"

  copy_macho_deps "${payload}/${demo_name}" "${payload}/lib"
  copy_bass_runtime "$payload" macos-arm

  local bp extra
  bp="$(brew_prefix)"
  for extra in \
    "${bp}/lib/libvulkan.1.dylib" \
    "${bp}/lib/libvulkan.dylib" \
    "${bp}/lib/libMoltenVK.dylib" \
    "${bp}/opt/molten-vk/lib/libMoltenVK.dylib" \
    "${bp}/opt/vulkan-loader/lib/libvulkan.1.dylib" \
    "${VULKAN_SDK:-}/lib/libvulkan.1.dylib" \
    "${VULKAN_SDK:-}/lib/libMoltenVK.dylib"; do
    [[ -f "$extra" ]] || continue
    local dest="${payload}/lib/$(basename "$extra")"
    if [[ ! -e "$dest" ]]; then
      cp -aL "$extra" "$dest"
      chmod u+w "$dest" 2>/dev/null || true
    fi
  done

  [[ -f "${payload}/lib/libMoltenVK.dylib" ]] || \
    die "libMoltenVK.dylib missing; install molten-vk (brew install molten-vk) and rebuild"

  # macos-arm package: drop foreign slices from universal vendor dylibs (e.g. bass).
  if command -v lipo >/dev/null 2>&1; then
    local fat
    for fat in "${payload}/lib/"*; do
      [[ -f "$fat" ]] || continue
      file "$fat" 2>/dev/null | grep -q 'Mach-O' || continue
      if lipo -info "$fat" 2>/dev/null | grep -Eq 'Architectures in the fat file|x86_64|i386'; then
        if lipo -thin arm64 "$fat" -output "${fat}.arm64" 2>/dev/null; then
          mv "${fat}.arm64" "$fat"
        fi
      fi
    done
  fi

  # ICD under Resources (NOT MacOS/): non-Mach-O files in Contents/MacOS break
  # codesign sealing. Startup setenv uses an absolute path to this JSON so host
  # VK_ICD_FILENAMES cannot win; library_path is relative to the JSON directory.
  cat >"${resources}/vulkan/icd.d/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../MacOS/lib/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
EOF

  fix_macos_rpaths "$payload"
  copy_portable_resources "$resources" "$build_dir"

  local icns="${ROOT}/ui/assets/app_icon/wds.icns"
  [[ -f "$icns" ]] || die "missing macOS app icon: $icns (run scripts/generate-app-icons.sh)"
  cp -a "$icns" "${resources}/wds.icns"

  # Mach-O as CFBundleExecutable — bash wrappers break under Gatekeeper quarantine
  # (SIGKILL / empty launch) and are unnecessary once prepare_macos_vulkan_environment
  # forces the bundled ICD before glfwInit.
  cat >"${app}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>${demo_name}</string>
  <key>CFBundleIdentifier</key>
  <string>com.wds.editor</string>
  <key>CFBundleName</key>
  <string>WDS Editor</string>
  <key>CFBundleDisplayName</key>
  <string>WDS Editor</string>
  <key>CFBundleIconFile</key>
  <string>wds</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>LSMinimumSystemVersion</key>
  <string>11.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

  chmod +x "${payload}/${demo_name}"

  # install_name_tool invalidates existing ad-hoc signatures on copied Homebrew
  # dylibs; modern macOS then SIGKILLs at load with "Code Signature Invalid".
  codesign_macos_app "$app"

  verify_macos_vulkan_icd "$app"
}

codesign_macos_app() {
  local app="$1"
  need_cmd codesign
  local f
  # Ad-hoc resign after install_name_tool (fixes CODESIGNING / Invalid Page).
  # Assets live in Contents/Resources; MacOS/ only has Mach-O + dylibs.
  while IFS= read -r -d '' f; do
    codesign --force --sign - "$f" >/dev/null 2>&1
  done < <(
    find "${app}/Contents/MacOS" -type f \( -name '*.dylib' -o -name 'wds_editor' \
         -o -name 'wds_core_example' \) -print0
  )

  codesign --force --sign - "$app" >/dev/null
  codesign --verify "$app" >/dev/null \
    || die "codesign --verify failed for $app"
}

# Validate bundled ICD layout. Runtime vkCreateInstance is best-effort: GitHub
# Actions macOS runners use AppleParavirtDevice, where MoltenVK aborts inside
# Metal (unrecognized selector) — that is a host GPU limit, not a packaging bug.
verify_macos_vulkan_icd() {
  local app="$1"
  local payload="${app}/Contents/MacOS"
  local icd="${app}/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
  local loader="${payload}/lib/libvulkan.1.dylib"
  local molten="${payload}/lib/libMoltenVK.dylib"

  [[ -f "$icd" ]] || die "missing auto-discovery ICD: $icd"
  [[ -f "$loader" ]] || die "missing bundled vulkan loader: $loader"
  [[ -f "$molten" ]] || die "missing bundled MoltenVK: $molten"

  python3 - "$icd" "$molten" <<'PY' || die "MoltenVK_icd.json library_path invalid"
import json, sys
from pathlib import Path
icd, molten = Path(sys.argv[1]), Path(sys.argv[2]).resolve()
data = json.loads(icd.read_text())
lp = data["ICD"]["library_path"]
resolved = (icd.parent / lp).resolve()
if resolved != molten:
    raise SystemExit(f"library_path {lp!r} -> {resolved} != {molten}")
print(f"ICD OK: {icd} -> {resolved}")
PY

  # CI / explicit skip: layout checks above are enough to catch packaging mistakes.
  if [[ "${WDS_SKIP_VULKAN_ICD_PROBE:-}" == "1" || -n "${GITHUB_ACTIONS:-}" || -n "${CI:-}" ]]; then
    echo "Vulkan ICD layout OK (runtime probe skipped on CI / WDS_SKIP_VULKAN_ICD_PROBE=1)"
    return 0
  fi

  local probe_src probe_bin
  probe_src="$(mktemp /tmp/wds-vkprobe.XXXXXX.c)"
  probe_bin="$(mktemp /tmp/wds-vkprobe.XXXXXX)"
  rm -f "$probe_bin"
  cat >"$probe_src" <<'EOF'
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
typedef int32_t VkResult;
typedef void* VkInstance;
typedef struct {
  int32_t sType; const void* pNext; const char* pAppName; uint32_t appVer;
  const char* pEngine; uint32_t engVer; uint32_t apiVer;
} VkApplicationInfo;
typedef struct {
  int32_t sType; const void* pNext; int32_t flags; const VkApplicationInfo* pAppInfo;
  uint32_t lc; const char* const* ln; uint32_t ec; const char* const* en;
} VkInstanceCreateInfo;
int main(int argc, char** argv) {
  if (argc < 2) return 1;
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen loader: %s\n", dlerror()); return 2; }
  typedef VkResult (*F)(const VkInstanceCreateInfo*, const void*, VkInstance*);
  F create = (F)dlsym(h, "vkCreateInstance");
  if (!create) { fprintf(stderr, "no vkCreateInstance\n"); return 3; }
  VkApplicationInfo app = {0, NULL, "wds-probe", 0, NULL, 0, (1u << 22) | (1u << 12)};
  const char* exts[] = {"VK_KHR_portability_enumeration", "VK_KHR_surface",
                        "VK_EXT_metal_surface"};
  VkInstanceCreateInfo ci = {1, NULL, 1, &app, 0, NULL, 3, exts};
  VkInstance inst = NULL;
  VkResult r = create(&ci, NULL, &inst);
  fprintf(stderr, "vkCreateInstance => %d\n", (int)r);
  return r == 0 ? 0 : 4;
}
EOF
  if ! cc -O0 -o "$probe_bin" "$probe_src" 2>/dev/null; then
    rm -f "$probe_src" "$probe_bin"
    echo "warning: could not compile Vulkan ICD probe; skipping runtime check" >&2
    return 0
  fi
  local rc=0
  set +e
  env -i HOME="${HOME:-/tmp}" PATH="/usr/bin:/bin" \
      VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" \
      "$probe_bin" "$loader" 2>&1
  rc=$?
  set -e
  rm -f "$probe_src" "$probe_bin"
  if [[ "$rc" -eq 0 ]]; then
    echo "Vulkan ICD probe OK (Resources/vulkan/icd.d/MoltenVK_icd.json)"
    return 0
  fi
  # Signal/abort (e.g. Metal paravirt NSException) → host GPU, not bundle layout.
  if [[ "$rc" -ge 128 ]]; then
    echo "warning: Vulkan ICD runtime probe aborted (rc=$rc); treating as host GPU limit" >&2
    return 0
  fi
  die "bundled MoltenVK ICD failed vkCreateInstance (rc=$rc; see above)"
}

make_macos_dmg_background() {
  # Writes Retina pair: <dir>/background.png + background@2x.png
  # Logical size 720×480 (points). WindowBounds must match this size exactly
  # on modern macOS (extra height shows as a white strip under the art).
  local out_dir="$1"
  local py=""
  if [[ -x /opt/anaconda3/bin/python3 ]]; then
    py=/opt/anaconda3/bin/python3
  elif command -v python3 >/dev/null 2>&1; then
    py="$(command -v python3)"
  else
    return 1
  fi
  mkdir -p "$out_dir"
  "$py" - "$out_dir" <<'PY'
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
# Logical (1x) size in Finder points — must match window *content* area.
W, H = 720, 480
# Supersample then downscale → crisp edges/text on Retina.
SS = 3  # master render scale
OUT2 = 2

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    import struct, zlib

    def write_png(path, w, h):
        def pixel(x, y):
            t = y / max(h - 1, 1)
            return bytes((int(16 + 14 * t), int(17 + 14 * t), int(26 + 18 * t), 255))

        raw = b"".join(b"\x00" + b"".join(pixel(x, y) for x in range(w)) for y in range(h))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(
                ">I", zlib.crc32(tag + data) & 0xFFFFFFFF
            )

        ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
        path.write_bytes(
            b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b"")
        )

    write_png(out_dir / "background.png", W, H)
    write_png(out_dir / "background@2x.png", W * OUT2, H * OUT2)
    raise SystemExit(0)


def load_font(size_px: int):
    # Prefer CJK-capable sans; PingFang is often unreadable to Pillow on newer macOS.
    for path, index in (
        ("/System/Library/Fonts/Hiragino Sans GB.ttc", 0),
        ("/System/Library/Fonts/STHeiti Medium.ttc", 0),
        ("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", None),
        ("/Library/Fonts/Arial Unicode.ttf", None),
        ("/System/Library/Fonts/SFNS.ttf", None),
        ("/System/Library/Fonts/Helvetica.ttc", 0),
    ):
        try:
            kw = {"index": index} if index is not None else {}
            return ImageFont.truetype(path, size_px, **kw)
        except Exception:
            continue
    return ImageFont.load_default()


def render(scale: int) -> Image.Image:
    w, h = W * scale, H * scale
    # Stay in RGB end-to-end: Pillow drops alpha on convert("RGB"), which
    # previously turned "soft" white panels into solid white blocks.
    img = Image.new("RGB", (w, h), (18, 19, 28))
    draw = ImageDraw.Draw(img)

    for y in range(h):
        t = y / max(h - 1, 1)
        draw.line(
            [(0, y), (w, y)],
            fill=(int(16 + 14 * t), int(17 + 14 * t), int(26 + 20 * t)),
        )

    def R(v: float) -> int:
        return int(round(v * scale))

    # Soft panels under icon slots (pre-blended ~9% white on dark).
    panel = (38, 39, 48)
    draw.rounded_rectangle((R(90), R(52), R(290), R(255)), radius=R(24), fill=panel)
    draw.rounded_rectangle((R(430), R(52), R(630), R(255)), radius=R(24), fill=panel)

    cx, cy = R(360), R(145)
    arrow = [
        (cx - R(50), cy - R(11)),
        (cx + R(10), cy - R(11)),
        (cx + R(10), cy - R(28)),
        (cx + R(58), cy),
        (cx + R(10), cy + R(28)),
        (cx + R(10), cy + R(11)),
        (cx - R(50), cy + R(11)),
    ]
    draw.polygon(arrow, fill=(168, 158, 230))

    hint = "Drag WDS Editor to Applications to install"
    sub = "将 WDS Editor 拖到 Applications 完成安装"
    font = load_font(R(18))
    font_sm = load_font(R(14))

    def center_text(text: str, y_pt: float, f, fill):
        bbox = draw.textbbox((0, 0), text, font=f)
        tw = bbox[2] - bbox[0]
        # Anchor by ink top so descenders stay in the reserved bottom margin.
        x = (w - tw) / 2.0
        y = float(R(y_pt)) - bbox[1]
        draw.text((x, y), text, font=f, fill=fill)

    # Reserved bottom ~70pt so Finder chrome / icon labels never clip the copy.
    center_text(hint, 352, font, (236, 234, 245))
    center_text(sub, 388, font_sm, (178, 176, 194))
    return img


master = render(SS)
img2x = master.resize((W * OUT2, H * OUT2), Image.Resampling.LANCZOS)
img1x = master.resize((W, H), Image.Resampling.LANCZOS)
img1x.save(out_dir / "background.png", format="PNG", optimize=True)
img2x.save(out_dir / "background@2x.png", format="PNG", optimize=True)
PY
}

resolve_dmgbuild() {
  # Prefer module invocation so PATH need not include ~/.local/bin.
  local py=""
  if [[ -x /opt/anaconda3/bin/python3 ]]; then
    py=/opt/anaconda3/bin/python3
  elif command -v python3 >/dev/null 2>&1; then
    py="$(command -v python3)"
  else
    return 1
  fi
  if "$py" -c "import dmgbuild" >/dev/null 2>&1; then
    echo "$py"
    return 0
  fi
  return 1
}

make_macos_dmg() {
  local app="$1" dmg_path="$2"
  need_cmd hdiutil

  local py=""
  py="$(resolve_dmgbuild)" || \
    die "dmgbuild not installed. On the packaging Mac run: python3 -m pip install --user dmgbuild"

  local work="${DIST_DIR}/staging/dmg-macos-arm-work"
  local volname="WDS Editor"
  local settings="${ROOT}/scripts/macos-dmg-settings.py"
  local bg="${work}/background.png"
  local icns="${ROOT}/ui/assets/app_icon/wds.icns"
  [[ -f "$settings" ]] || die "missing DMG settings: $settings"
  [[ -d "$app" ]] || die "missing app bundle: $app"
  [[ -f "$icns" ]] || die "missing app icon: $icns (run scripts/generate-app-icons.sh)"

  # Keep bundle icon in sync with the committed asset (avoids stale staged copies).
  mkdir -p "${app}/Contents/Resources"
  cp -a "$icns" "${app}/Contents/Resources/wds.icns"
  codesign --force --sign - "$app" >/dev/null 2>&1 || true

  # Eject any prior mount of this volume (Finder caches old folder layouts).
  if [[ -d "/Volumes/${volname}" ]]; then
    hdiutil detach "/Volumes/${volname}" -force >/dev/null 2>&1 || true
  fi

  rm -rf "$work"
  mkdir -p "$work"
  make_macos_dmg_background "$work" || die "failed to synthesize DMG background art"
  [[ -f "$bg" && -f "${work}/background@2x.png" ]] || \
    die "expected Retina background pair in $work"
  rm -f "$dmg_path"
  mkdir -p "$(dirname "$dmg_path")"

  # dmgbuild writes .DS_Store / HiDPI background / icon positions without Finder.
  echo "Building styled DMG with dmgbuild…"
  "$py" -m dmgbuild -s "$settings" \
    -D "app=${app}" \
    -D "background=${bg}" \
    -D "icon=${icns}" \
    "$volname" \
    "$dmg_path"

  [[ -f "$dmg_path" ]] || die "dmgbuild did not produce $dmg_path"
  rm -rf "$work"
  echo "Packaged: $dmg_path"
}

package_macos() {
  local build_dir="$1"
  local demo=""
  demo="$(resolve_editor_bin "$build_dir")" || \
    die "missing wds_editor (build first: ./scripts/build-target.sh macos-arm)"

  need_cmd otool

  local stage="${DIST_DIR}/staging/wds-macos-arm"
  rm -rf "$stage"
  mkdir -p "$stage"

  write_macos_app_bundle "$stage" "$demo" "$build_dir"
  write_readme "$stage" macos-arm
  make_zip "$stage" "${DIST_DIR}/wds-macos-arm.zip"
  make_macos_dmg "${stage}/WDS Editor.app" "${DIST_DIR}/wds-macos-arm.dmg"
}

# Populate build-<target>-debug/ui/ so the Debug .exe is self-contained on Windows.
# Does not build MSI/zip — only the runtime files next to wds_editor.exe.
stage_win_debug() {
  local build_dir="$1"
  local stage="${build_dir}/ui"
  local editor="${stage}/wds_editor.exe"
  [[ -f "${editor}" ]] || die "missing ${editor} (build win Debug first)"

  # fonts/icons/wds.png are already POST_BUILD'd into ui/. Only fill gaps that the
  # in-tree Debug tree is missing (skins/effects/shaders + vulkan-1.dll).
  copy_skins_effects "$stage"
  copy_shaders "$stage" "$build_dir"
  if [[ ! -f "${stage}/wds.png" ]]; then
    copy_app_icon "$stage"
  fi
  if [[ ! -d "${stage}/fonts" ]] || [[ -z "$(ls -A "${stage}/fonts" 2>/dev/null || true)" ]]; then
    copy_fonts "$stage" "$build_dir"
  fi
  if [[ ! -d "${stage}/icons" ]] || [[ -z "$(ls -A "${stage}/icons" 2>/dev/null || true)" ]]; then
    copy_icons "$stage" "$build_dir"
  fi
  copy_bass_runtime "$stage" win-x86_64

  # Prefer already-extracted / staged vulkan-1.dll; otherwise download components zip.
  local vk_dll=""
  if [[ -f "${DIST_DIR}/staging/wds-win-x86_64/vulkan-1.dll" ]]; then
    vk_dll="${DIST_DIR}/staging/wds-win-x86_64/vulkan-1.dll"
  else
    vk_dll="$(find "${CACHE_DIR}/vulkan-rt-components" -type f -path '*/x64/vulkan-1.dll' 2>/dev/null | head -1 || true)"
  fi
  if [[ -z "${vk_dll}" || ! -f "${vk_dll}" ]]; then
    local comp_url="${WDS_VULKAN_COMPONENTS_URL:-https://sdk.lunarg.com/sdk/download/latest/windows/vulkan-runtime-components.zip?Human=true}"
    local comp_zip="${CACHE_DIR}/vulkan-runtime-components.zip"
    download "$comp_url" "$comp_zip"
    local extract="${CACHE_DIR}/vulkan-rt-components"
    rm -rf "$extract"
    mkdir -p "$extract"
    unzip -q -o "$comp_zip" -d "$extract"
    vk_dll="$(find "$extract" -type f -path '*/x64/vulkan-1.dll' | head -1 || true)"
  fi
  [[ -n "${vk_dll}" && -f "${vk_dll}" ]] || die "vulkan-1.dll not found (run a Release package once, or check cache)"
  cp -a "${vk_dll}" "${stage}/vulkan-1.dll"

  echo "Win Debug runtime staged at: ${stage}"
  echo "Copy the whole ui/ folder to Windows (exe + bass.dll + vulkan-1.dll + skins/effects/shaders/fonts/icons)."
}

# --- main -------------------------------------------------------------------

if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

if [[ "$1" == "--stage-win-debug" ]]; then
  shift
  [[ $# -ge 1 ]] || die "--stage-win-debug requires <build-dir>"
  stage_win_debug "$1"
  exit 0
fi

TARGET="$1"
shift
BUILD_DIR="${ROOT}/build-${TARGET}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown arg: $1"
      ;;
  esac
done

[[ -d "$BUILD_DIR" ]] || die "build dir not found: $BUILD_DIR (build the target first)"

case "$TARGET" in
  win-x86_64) package_win "$BUILD_DIR" ;;
  macos-arm)  package_macos "$BUILD_DIR" ;;
  linux*)
    die "Linux is not a supported product target; package win-x86_64 or macos-arm"
    ;;
  *)
    die "unsupported target: $TARGET"
    ;;
esac

#!/usr/bin/env bash
# Build WDS for a named target, then package a portable release (unless Debug / --no-package).
# Cross targets use cmake/toolchains/<target>.cmake.
# macos-arm is native on Apple Silicon (Homebrew deps).
#
# Flavors:
#   Release (default) -> build-<target>/, package dist/ (zip / MSI / DMG), runtime logs off
#   Debug             -> build-<target>-debug/, no zip, auto-run wds_editor (verbose logs)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
# shellcheck disable=SC1091
source "${ROOT}/scripts/lib/wds-env.sh"

TARGETS=(win-x86_64 macos-arm)
DO_PACKAGE=1
DO_RUN=0
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"

usage() {
  cat <<EOF
Usage: $(basename "$0") <target> [--debug|--release] [--no-package] [--no-run] [--] [extra cmake args...]
       $(basename "$0") --check
       $(basename "$0") --help
       $(basename "$0") --package-only <target>

Targets:
  win-x86_64     Windows x86_64 (MinGW-w64 + vcpkg; typical Linux host)
  macos-arm      Apple Silicon native (Homebrew)

  Linux is not a product target — on Linux hosts, build win-x86_64 only.

Build flavors:
  --release      CMAKE_BUILD_TYPE=Release (default)
                 build dir: build-<target>/
                 packages dist/ (win MSI+zip / macos zip+dmg); no WDS_LOG
  --debug        CMAKE_BUILD_TYPE=Debug
                 build dir: build-<target>-debug/
                 does NOT package; after build, runs wds_editor with verbose logs

Other:
  --no-package   Skip packaging even in Release
  --no-run       Skip auto-run even in Debug

Examples:
  # Copy scripts/env.example → scripts/env.local and set WDS_VCPKG_ROOT, then:
  $(basename "$0") win-x86_64
  $(basename "$0") macos-arm
  $(basename "$0") macos-arm --debug

  # Or pass paths explicitly (prefer env.local for machine-specific paths):
  $(basename "$0") win-x86_64 -- \\
    -DCMAKE_PREFIX_PATH="\$WDS_CMAKE_PREFIX_PATH" \\
    -DVulkan_LIBRARY="\$WDS_VULKAN_LIBRARY"

Environment (see scripts/env.example):
  WDS_ENV_FILE / scripts/env.local
  WDS_VCPKG_ROOT / WDS_VCPKG_TRIPLET / WDS_CMAKE_PREFIX_PATH / WDS_VULKAN_LIBRARY
  WDS_MINGW_TRIPLE / WDS_MINGW_CXX
  WDS_MSITOOLS_PREFIX / WDS_PRODUCT_VERSION / WDS_RUNTIME_CACHE
  CMAKE_BUILD_TYPE / CMAKE_BUILD_PARALLEL_LEVEL
EOF
}

# If env.local / WDS_VCPKG_ROOT provided prefix paths, inject them unless already on CLI.
# Also force product layers ON: a prior core-only configure (e.g. -DWDS_BUILD_RENDERER=OFF)
# leaves those keys OFF in CMakeCache; cmake option() defaults do not override cache, so
# cmake --build silently skips ui/ and package-target would ship a stale wds_editor.exe.
apply_win_env_cmake_defaults() {
  local a
  local has_prefix=0 has_vulkan=0
  local has_ui=0 has_renderer=0 has_audio=0 has_interaction=0
  for a in ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}; do
    case "$a" in
      *CMAKE_PREFIX_PATH*) has_prefix=1 ;;
      *Vulkan_LIBRARY*) has_vulkan=1 ;;
      *WDS_BUILD_UI=*) has_ui=1 ;;
      *WDS_BUILD_RENDERER=*) has_renderer=1 ;;
      *WDS_BUILD_AUDIO=*) has_audio=1 ;;
      *WDS_BUILD_INTERACTION=*) has_interaction=1 ;;
    esac
  done
  if [[ "${has_ui}" -eq 0 ]]; then
    EXTRA_CMAKE_ARGS+=("-DWDS_BUILD_UI=ON")
  fi
  if [[ "${has_renderer}" -eq 0 ]]; then
    EXTRA_CMAKE_ARGS+=("-DWDS_BUILD_RENDERER=ON")
  fi
  if [[ "${has_audio}" -eq 0 ]]; then
    EXTRA_CMAKE_ARGS+=("-DWDS_BUILD_AUDIO=ON")
  fi
  if [[ "${has_interaction}" -eq 0 ]]; then
    EXTRA_CMAKE_ARGS+=("-DWDS_BUILD_INTERACTION=ON")
  fi
  if [[ "${has_prefix}" -eq 0 && -n "${WDS_CMAKE_PREFIX_PATH:-}" ]]; then
    EXTRA_CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=${WDS_CMAKE_PREFIX_PATH}")
  fi
  if [[ "${has_vulkan}" -eq 0 && -n "${WDS_VULKAN_LIBRARY:-}" ]]; then
    EXTRA_CMAKE_ARGS+=("-DVulkan_LIBRARY=${WDS_VULKAN_LIBRARY}")
  fi
}

check_toolchain() {
  local t="$1"
  local tc="${ROOT}/cmake/toolchains/${t}.cmake"
  local build_dir="${ROOT}/build-check-${t}"
  local log
  log="$(mktemp "${TMPDIR:-/tmp}/wds-tc-${t}.XXXXXX.log")"
  rm -rf "${build_dir}"
  echo "=== checking ${t} ==="
  if cmake -S "${ROOT}" -B "${build_dir}" --toolchain "${tc}" \
      -DWDS_BUILD_RENDERER=OFF -DWDS_CORE_BUILD_TESTS=OFF -DWDS_CORE_BUILD_EXAMPLE=OFF \
      >"${log}" 2>&1; then
    echo "OK  ${t}: toolchain usable (configure succeeded for core-only)"
    rm -rf "${build_dir}" "${log}"
    return 0
  fi
  echo "MISSING  ${t}: see cmake/toolchains/${t}.cmake for install hints"
  if grep -q "WDS toolchain for" "${log}"; then
    sed -n '/WDS toolchain for/,/After installing/p' "${log}" | head -40
  else
    tail -20 "${log}"
  fi
  rm -rf "${build_dir}" "${log}"
  return 1
}

parallel_jobs() {
  if [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
    echo "${CMAKE_BUILD_PARALLEL_LEVEL}"
  elif command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu 2>/dev/null || echo 4
  else
    echo 4
  fi
}

apply_build_flavor() {
  case "${BUILD_TYPE}" in
    Debug|debug)
      BUILD_TYPE="Debug"
      DO_PACKAGE=0
      DO_RUN=1
      ;;
    Release|release)
      BUILD_TYPE="Release"
      ;;
    *)
      # RelWithDebInfo / MinSizeRel: treat like Release for packaging
      DO_RUN=0
      ;;
  esac
}

configure_macos_arm() {
  local build_dir="$1"
  shift
  local host
  host="$(uname -s)"
  [[ "${host}" == "Darwin" ]] || {
    echo "error: macos-arm must be built on macOS (got ${host})" >&2
    exit 1
  }
  local arch
  arch="$(uname -m)"
  if [[ "${arch}" != "arm64" ]]; then
    echo "warning: host arch is ${arch}; expected arm64 Apple Silicon" >&2
  fi

  local prefix="${CMAKE_PREFIX_PATH:-}"
  if [[ -z "${prefix}" ]] && command -v brew >/dev/null 2>&1; then
    prefix="$(brew --prefix)"
  fi

  # Pin Homebrew libpng headers+dylib together. CI / XQuartz trees often expose
  # libpng 1.4 headers under /opt/X11 while packaging copies brew's 1.6 dylib —
  # png_create_read_struct then fails and every skin PNG load returns false.
  local brew_png=""
  if command -v brew >/dev/null 2>&1; then
    brew_png="$(brew --prefix libpng 2>/dev/null || true)"
  fi
  if [[ -n "${brew_png}" && -d "${brew_png}/include" && -f "${brew_png}/lib/libpng.dylib" ]]; then
    if [[ -n "${prefix}" ]]; then
      prefix="${brew_png};${prefix}"
    else
      prefix="${brew_png}"
    fi
  else
    brew_png=""
  fi

  local -a args=(
    -S "${ROOT}"
    -B "${build_dir}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DWDS_BUILD_INTERACTION=ON
    -DWDS_BUILD_UI=ON
  )
  if [[ -n "${prefix}" ]]; then
    args+=(-DCMAKE_PREFIX_PATH="${prefix}")
  fi
  if [[ -n "${brew_png}" ]]; then
    args+=(
      -DPNG_PNG_INCLUDE_DIR="${brew_png}/include"
      -DPNG_LIBRARY="${brew_png}/lib/libpng.dylib"
    )
    echo "Using Homebrew libpng: ${brew_png}"
  fi
  args+=("$@")

  echo "Configuring macos-arm (${BUILD_TYPE}) -> ${build_dir}"
  cmake "${args[@]}"
}

run_editor() {
  local build_dir="$1"
  local editor=""
  if [[ -x "${build_dir}/ui/wds_editor" ]]; then
    editor="${build_dir}/ui/wds_editor"
  elif [[ -f "${build_dir}/ui/wds_editor.exe" ]]; then
    echo "Skipping auto-run: Windows .exe cannot be launched on this host" >&2
    return 0
  else
    echo "error: wds_editor not found under ${build_dir}/ui" >&2
    return 1
  fi

  echo "Running Debug editor: ${editor}"
  echo "(cwd=${ROOT}; verbose WDS_LOG enabled)"
  # skins/ resolve from repo root / WDS_REPO_ROOT
  (cd "${ROOT}" && exec "${editor}")
}

# Win Debug is not packaged into an MSI/zip, but the bare ui/ tree is not runnable on a
# Windows machine: WDS_REPO_ROOT / WDS_SHADER_DIR are Linux absolute paths baked at
# configure time. Stage the same portable payload next to wds_editor.exe.
stage_win_debug_runtime() {
  local build_dir="$1"
  local stage="${build_dir}/ui"
  local editor="${stage}/wds_editor.exe"
  [[ -f "${editor}" ]] || {
    echo "error: missing ${editor}" >&2
    return 1
  }

  echo "Staging Win Debug runtime next to ${editor}"
  "${ROOT}/scripts/package-target.sh" --stage-win-debug "${build_dir}"
}

if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

case "$1" in
  -h|--help)
    usage
    exit 0
    ;;
  --check)
    status=0
    check_toolchain win-x86_64 || status=1
    echo
    if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]]; then
      echo "=== checking macos-arm (native host) ==="
      echo "OK  macos-arm: use ./scripts/build-target.sh macos-arm [--debug|--release]"
    else
      echo "=== checking macos-arm ==="
      echo "SKIP macos-arm: not an Apple Silicon Mac host"
    fi
    exit "${status}"
    ;;
  --package-only)
    shift
    [[ $# -ge 1 ]] || { echo "usage: $0 --package-only <target>" >&2; exit 1; }
    exec "${ROOT}/scripts/package-target.sh" "$1"
    ;;
  linux*)
    echo "error: Linux is not a supported product target." >&2
    echo "On a Linux host, set WDS_VCPKG_ROOT (see scripts/env.example) then:" >&2
    echo "  ./scripts/build-target.sh win-x86_64" >&2
    exit 1
    ;;
esac

TARGET=""
EXTRA_CMAKE_ARGS=()
NO_PACKAGE_FLAG=0
NO_RUN_FLAG=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)
      BUILD_TYPE="Debug"
      shift
      ;;
    --release)
      BUILD_TYPE="Release"
      shift
      ;;
    --no-package)
      NO_PACKAGE_FLAG=1
      shift
      ;;
    --no-run)
      NO_RUN_FLAG=1
      shift
      ;;
    --)
      shift
      EXTRA_CMAKE_ARGS+=("$@")
      break
      ;;
    -*)
      # Allow leading cmake -D flags without requiring --
      if [[ -z "$TARGET" ]]; then
        echo "Unknown option before target: $1" >&2
        usage >&2
        exit 1
      fi
      EXTRA_CMAKE_ARGS+=("$1")
      shift
      ;;
    *)
      if [[ -z "$TARGET" ]]; then
        TARGET="$1"
        shift
      else
        EXTRA_CMAKE_ARGS+=("$1")
        shift
      fi
      ;;
  esac
done

[[ -n "$TARGET" ]] || { usage >&2; exit 1; }

VALID=0
for t in "${TARGETS[@]}"; do
  if [[ "$TARGET" == "$t" ]]; then
    VALID=1
    break
  fi
done
if [[ "${VALID}" -ne 1 ]]; then
  echo "Unknown target: ${TARGET}" >&2
  usage >&2
  exit 1
fi

apply_build_flavor
if [[ "${NO_PACKAGE_FLAG}" -eq 1 ]]; then
  DO_PACKAGE=0
fi
if [[ "${NO_RUN_FLAG}" -eq 1 ]]; then
  DO_RUN=0
fi

if [[ "${BUILD_TYPE}" == "Debug" ]]; then
  BUILD_DIR="${ROOT}/build-${TARGET}-debug"
else
  BUILD_DIR="${ROOT}/build-${TARGET}"
fi

if [[ "${TARGET}" == "macos-arm" ]]; then
  # bash 3.2 + set -u: empty "${arr[@]}" is an unbound variable
  configure_macos_arm "${BUILD_DIR}" ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}
else
  apply_win_env_cmake_defaults
  wds_print_env_summary
  TOOLCHAIN="${ROOT}/cmake/toolchains/${TARGET}.cmake"
  echo "Configuring ${TARGET} (${BUILD_TYPE}) -> ${BUILD_DIR}"
  cmake -S "${ROOT}" -B "${BUILD_DIR}" \
    --toolchain "${TOOLCHAIN}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}
fi

echo "Building ${TARGET} (${BUILD_TYPE})"
cmake --build "${BUILD_DIR}" --parallel "$(parallel_jobs)"

echo "Done build: ${BUILD_DIR}"

if [[ "${DO_PACKAGE}" -eq 1 ]]; then
  echo "Packaging ${TARGET}"
  "${ROOT}/scripts/package-target.sh" "${TARGET}" --build-dir "${BUILD_DIR}"
elif [[ "${BUILD_TYPE}" == "Debug" ]]; then
  echo "Skipping package (Debug builds are not packaged)"
  if [[ "${TARGET}" == "win-x86_64" ]]; then
    stage_win_debug_runtime "${BUILD_DIR}"
  fi
fi

if [[ "${DO_RUN}" -eq 1 ]]; then
  run_editor "${BUILD_DIR}"
fi

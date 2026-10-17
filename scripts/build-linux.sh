#!/usr/bin/env bash
# Native Linux development build. Dependencies stay outside the source tree.
set -euo pipefail
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  cat <<'EOF'
Usage: scripts/build-linux.sh [--] [CMake -D/-U options...]

Build the native Linux x86_64 editor and tests. Dependencies are not downloaded.
Defaults: ~/.cache/wds-editor/build-linux, RelWithDebInfo, 4 build jobs.
An existing build keeps its generator, build type and dependency settings.

Overrides:
  WDS_LINUX_CACHE        Dependency/build cache root (honors XDG_CACHE_HOME)
  WDS_LINUX_BUILD_DIR    Build directory; use a separate one per checkout
  WDS_QT_LINUX_ROOT      Qt 6.5+ SDK prefix (e.g. /opt/Qt/6.8.3/gcc_64)
  WDS_BASS_LINUX_DIR     Directory with libbass.so and libbassmix.so
  WDS_CMAKE             CMake executable
  CMAKE_BUILD_PARALLEL_LEVEL / CMAKE_GENERATOR / CMAKE_PREFIX_PATH

Example: scripts/build-linux.sh -- -DCMAKE_BUILD_TYPE=Debug
See README.md for dependencies and scripts/run-linux.sh for launching.
EOF
  exit 0
fi
[[ "${1:-}" != -- ]] || shift
# shellcheck source=lib/wds-linux.sh
source "$(dirname -- "${BASH_SOURCE[0]}")/lib/wds-linux.sh"
wds_linux_init
# Cached tools are fallbacks; explicitly configured tools on PATH keep priority.
export PATH="$PATH:$wds_linux_cache/tools/bin:$wds_linux_cache/deps/sysroot/usr/bin"
cmake_bin="${WDS_CMAKE:-cmake}"
if ! command -v "$cmake_bin" >/dev/null; then
  echo 'CMake was not found. Install CMake or set WDS_CMAKE to its executable.' >&2
  exit 1
fi
args=(-DWDS_TARGET=linux-x86_64)
if [[ ! -f "$wds_linux_build/CMakeCache.txt" ]]; then
  args+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo)
  if [[ -z "${CMAKE_GENERATOR:-}" ]] && command -v ninja >/dev/null; then
    args+=(-G Ninja)
  fi
fi
if [[ -n "${WDS_BASS_LINUX_DIR:-}" ]]; then
  args+=("-DWDS_BASS_LINUX_DIR=$WDS_BASS_LINUX_DIR")
elif [[ ! -f "$wds_linux_build/CMakeCache.txt" ]]; then
  args+=("-DWDS_BASS_LINUX_DIR=$wds_linux_cache/deps/bass-linux/x86_64")
fi
# Unix CMake environment search paths use ':'; -D CMake lists use ';'. Keep
# the caller's environment intact rather than forwarding it as a -D list.
qt_prefix="${WDS_QT_LINUX_ROOT:-}"
if [[ -z "$qt_prefix" && -z "${CMAKE_PREFIX_PATH:-}" && -z "${Qt6_ROOT:-}" &&
      -d "$wds_linux_cache/qt/6.8.3/gcc_64" ]]; then
  qt_prefix="$wds_linux_cache/qt/6.8.3/gcc_64"
fi
if [[ -n "$qt_prefix" ]]; then
  if [[ ! -f "$qt_prefix/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    echo "Qt SDK not found at $qt_prefix (expected lib/cmake/Qt6/Qt6Config.cmake)." >&2
    exit 1
  fi
  export CMAKE_PREFIX_PATH="$qt_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
fi
if [[ -d "$wds_linux_cache/deps/sysroot/usr" ]]; then
  export CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:+$CMAKE_PREFIX_PATH:}$wds_linux_cache/deps/sysroot/usr"
fi
"$cmake_bin" -S "$wds_linux_repo" -B "$wds_linux_build" "${args[@]}" "$@"
"$cmake_bin" --build "$wds_linux_build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

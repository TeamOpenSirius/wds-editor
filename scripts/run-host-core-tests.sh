#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-linux-host"
cmake -S "$ROOT/cmake/host-core-tests-src" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD" -j"$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure

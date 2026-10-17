#!/usr/bin/env bash
# Shared paths for the native Linux build and launcher. No files are created here.
wds_linux_init() {
  if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
    echo 'Native Linux scripts require a Linux x86_64 host.' >&2
    return 1
  fi
  wds_linux_repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
  wds_linux_cache="${WDS_LINUX_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/wds-editor}"
  wds_linux_build="${WDS_LINUX_BUILD_DIR:-$wds_linux_cache/build-linux}"
  if [[ -f "$wds_linux_build/CMakeCache.txt" ]]; then
    local cached_source
    cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$wds_linux_build/CMakeCache.txt")"
    if [[ -n "$cached_source" && "$cached_source" != "$wds_linux_repo" ]]; then
      echo "Build directory belongs to another checkout: $cached_source" >&2
      echo 'Set WDS_LINUX_BUILD_DIR to a separate directory for this checkout.' >&2
      return 1
    fi
  fi
}

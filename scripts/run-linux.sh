#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  cat <<'EOF'
Usage: scripts/run-linux.sh [editor arguments...]
Launch the native Linux editor built by scripts/build-linux.sh.
Use the same WDS_LINUX_CACHE / WDS_LINUX_BUILD_DIR overrides as the build.
Example: scripts/run-linux.sh --open /absolute/path/song.wdsproject
EOF
  exit 0
fi
# shellcheck source=lib/wds-linux.sh
source "$(dirname -- "${BASH_SOURCE[0]}")/lib/wds-linux.sh"
wds_linux_init
editor="$wds_linux_build/ui/wds_editor"
if [[ ! -x "$editor" ]]; then
  echo "Editor not found: $editor. Run scripts/build-linux.sh with the same build directory." >&2
  exit 1
fi
if [[ -d "$wds_linux_cache/deps/runtime/lib" ]]; then
  export LD_LIBRARY_PATH="$wds_linux_cache/deps/runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$editor" "$@"

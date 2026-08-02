# Shared env loader for WDS build/package scripts.
# Sourced (not executed). Safe to source multiple times.
#
# Loads (first existing wins for file path; later exports still override via shell):
#   1) $WDS_ENV_FILE if set
#   2) $ROOT/scripts/env.local
# Then applies derived defaults (vcpkg prefix, MinGW tool names, …).

if [[ -n "${WDS_ENV_LOADED:-}" ]]; then
  return 0 2>/dev/null || exit 0
fi
WDS_ENV_LOADED=1

_wds_env_root="${ROOT:-}"
if [[ -z "${_wds_env_root}" ]]; then
  _wds_env_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fi

_wds_load_env_file() {
  local f="$1"
  [[ -f "$f" ]] || return 0
  # shellcheck disable=SC1090
  set -a
  # shellcheck disable=SC1090
  source "$f"
  set +a
  echo "Loaded env: $f" >&2
}

if [[ -n "${WDS_ENV_FILE:-}" ]]; then
  _wds_load_env_file "${WDS_ENV_FILE}"
elif [[ -f "${_wds_env_root}/scripts/env.local" ]]; then
  _wds_load_env_file "${_wds_env_root}/scripts/env.local"
fi

# MinGW defaults (Linux host → Windows target)
: "${WDS_MINGW_TRIPLE:=x86_64-w64-mingw32}"
: "${WDS_MINGW_CXX:=${WDS_MINGW_TRIPLE}-g++}"
: "${WDS_MINGW_OBJDUMP:=${WDS_MINGW_TRIPLE}-objdump}"
: "${WDS_VCPKG_TRIPLET:=x64-mingw-static}"
: "${WDS_WIXL_VERSION:=0.103}"
: "${WDS_PRODUCT_VERSION:=1.0.0}"
: "${WDS_MSITOOLS_PREFIX:=${HOME}/.local/opt/msitools}"

# Derive CMAKE prefix / Vulkan import lib from vcpkg root when unset.
if [[ -z "${WDS_CMAKE_PREFIX_PATH:-}" && -n "${WDS_VCPKG_ROOT:-}" ]]; then
  WDS_CMAKE_PREFIX_PATH="${WDS_VCPKG_ROOT}/installed/${WDS_VCPKG_TRIPLET}"
fi
if [[ -z "${WDS_VULKAN_LIBRARY:-}" && -n "${WDS_CMAKE_PREFIX_PATH:-}" ]]; then
  if [[ -f "${WDS_CMAKE_PREFIX_PATH}/lib/libvulkan-1.dll.a" ]]; then
    WDS_VULKAN_LIBRARY="${WDS_CMAKE_PREFIX_PATH}/lib/libvulkan-1.dll.a"
  elif [[ -f "${WDS_CMAKE_PREFIX_PATH}/lib/vulkan-1.lib" ]]; then
    WDS_VULKAN_LIBRARY="${WDS_CMAKE_PREFIX_PATH}/lib/vulkan-1.lib"
  fi
fi

# Accept CMAKE_PREFIX_PATH as alias when WDS_CMAKE_PREFIX_PATH unset.
if [[ -z "${WDS_CMAKE_PREFIX_PATH:-}" && -n "${CMAKE_PREFIX_PATH:-}" ]]; then
  WDS_CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH}"
fi

# msitools multiarch lib dir (Debian/Ubuntu name varies by host arch).
if [[ -z "${WDS_MSITOOLS_LIB_DIR:-}" ]]; then
  for _cand in \
      "${WDS_MSITOOLS_PREFIX}/usr/lib/x86_64-linux-gnu" \
      "${WDS_MSITOOLS_PREFIX}/usr/lib/aarch64-linux-gnu" \
      "${WDS_MSITOOLS_PREFIX}/usr/lib" \
      "${WDS_MSITOOLS_PREFIX}/lib"; do
    if [[ -d "${_cand}" ]]; then
      WDS_MSITOOLS_LIB_DIR="${_cand}"
      break
    fi
  done
  unset _cand
fi

# Colon-separated wixl share search path.
if [[ -z "${WDS_WIXL_SHARE_DIRS:-}" ]]; then
  WDS_WIXL_SHARE_DIRS="${WDS_MSITOOLS_PREFIX}/usr/share/wixl-${WDS_WIXL_VERSION}:/usr/share/wixl-${WDS_WIXL_VERSION}:/usr/local/share/wixl-${WDS_WIXL_VERSION}"
fi

# Print a redacted summary (set/unset only — no absolute home paths).
wds_print_env_summary() {
  echo "WDS env: VCPKG_ROOT=${WDS_VCPKG_ROOT:+set} PREFIX=${WDS_CMAKE_PREFIX_PATH:+set} VULKAN_LIB=${WDS_VULKAN_LIBRARY:+set} MINGW_CXX=${WDS_MINGW_CXX}" >&2
}

unset _wds_env_root

# Shared helpers for WDS cross / native toolchains.
# Included by cmake/toolchains/*.cmake before project().

if(DEFINED WDS_TOOLCHAIN_COMMON_INCLUDED)
  return()
endif()
set(WDS_TOOLCHAIN_COMMON_INCLUDED TRUE)

# Prefer an explicit target name set by the selected toolchain file.
if(NOT DEFINED WDS_TARGET)
  set(WDS_TARGET "unknown")
endif()

# Help find_package / find_library look inside the target sysroot first.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

function(wds_require_program VAR)
  if(NOT ${VAR})
    return()
  endif()
  if(NOT EXISTS "${${VAR}}" AND NOT EXISTS "${${VAR}}.exe")
    # find_program may return a bare name that is on PATH
    find_program(_wds_resolved NAMES "${${VAR}}")
    if(_wds_resolved)
      set(${VAR} "${_wds_resolved}" PARENT_SCOPE)
    endif()
    unset(_wds_resolved CACHE)
  endif()
endfunction()

macro(wds_fatal_missing_toolchain TARGET_NAME HINT)
  message(FATAL_ERROR
    "WDS toolchain for '${TARGET_NAME}' is not available on this machine.\n"
    "${HINT}\n"
    "After installing, re-run:\n"
    "  ./scripts/build-target.sh ${TARGET_NAME}\n"
    "or:\n"
    "  cmake -S . -B build-${TARGET_NAME} --toolchain cmake/toolchains/${TARGET_NAME}.cmake\n"
  )
endmacro()

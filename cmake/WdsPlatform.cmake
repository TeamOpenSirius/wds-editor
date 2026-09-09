# Included from the top-level CMakeLists after project().
# Normalizes target id and adjusts defaults for cross builds.

if(NOT DEFINED WDS_TARGET OR WDS_TARGET STREQUAL "" OR WDS_TARGET STREQUAL "unknown")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux is only a build host (MinGW → win-x86_64), not a product target.
    message(FATAL_ERROR
      "Native Linux builds are unsupported. On Linux, cross-compile Windows:\n"
      "  ./scripts/build-target.sh win-x86_64 -- \\\n"
      "    -DCMAKE_PREFIX_PATH=<vcpkg>/installed/x64-mingw-static \\\n"
      "    -DVulkan_LIBRARY=<vcpkg>/installed/x64-mingw-static/lib/libvulkan-1.dll.a\n"
      "Supported targets: win-x86_64, macos-arm (see cmake/CROSS_COMPILE.md).")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(WDS_TARGET "win-x86_64")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(WDS_TARGET "macos-arm")
  else()
    set(WDS_TARGET "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()

message(STATUS "WDS_TARGET=${WDS_TARGET}")
message(STATUS "CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME} CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
message(STATUS "CMAKE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}")

# Single-config generators (Ninja/Make): default Release; scripts use:
#   Release -> build-<target>/
#   Debug   -> build-<target>-debug/
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Debug|Release|RelWithDebInfo|MinSizeRel)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

if(CMAKE_CONFIGURATION_TYPES)
  message(STATUS "WDS multi-config generator; Debug enables logging, Release is silent")
  set(WDS_BUILD_DIR_SUFFIX "")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(WDS_BUILD_DIR_SUFFIX "-debug")
  message(STATUS "WDS_BUILD_TYPE=Debug (script path suffix: build-<target>${WDS_BUILD_DIR_SUFFIX})")
else()
  set(WDS_BUILD_DIR_SUFFIX "")
  message(STATUS "WDS_BUILD_TYPE=${CMAKE_BUILD_TYPE} (script path: build-<target>)")
endif()

# Verbose WDS_LOG in Debug only; Release binaries stay silent at runtime.
add_compile_definitions(
  $<$<CONFIG:Debug>:WDS_ENABLE_LOGGING=1>
  $<$<NOT:$<CONFIG:Debug>>:WDS_ENABLE_LOGGING=0>
)

# Cross builds: skip tests by default (host ctest cannot run foreign binaries without qemu/wine).
set(WDS_CORE_BUILD_EXAMPLE_DEFAULT ON)
if(CMAKE_CROSSCOMPILING)
  set(WDS_CORE_BUILD_TESTS_DEFAULT OFF)
else()
  set(WDS_CORE_BUILD_TESTS_DEFAULT ON)
endif()

# MinGW: statically link libgcc / libstdc++ / winpthread so Release packages need fewer
# redistributable DLLs (png already comes from vcpkg x64-mingw-static).
if(MINGW)
  add_link_options(-static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
endif()

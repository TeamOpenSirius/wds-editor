cmake_minimum_required(VERSION 3.16)

# Optional convenience: -DWDS_TARGET=win-x86_64 selects a toolchain when none was passed.
# Prefer: cmake --toolchain cmake/toolchains/<target>.cmake
# macos-arm: native only (no cross toolchain file).
if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED WDS_TARGET AND NOT WDS_TARGET STREQUAL "")
  set(_wds_tc "${CMAKE_SOURCE_DIR}/cmake/toolchains/${WDS_TARGET}.cmake")
  if(EXISTS "${_wds_tc}")
    set(CMAKE_TOOLCHAIN_FILE "${_wds_tc}" CACHE FILEPATH "WDS toolchain" FORCE)
  elseif(WDS_TARGET STREQUAL "macos-arm")
    message(STATUS "WDS_TARGET=macos-arm: native Apple Silicon build (no cross toolchain)")
  else()
    message(FATAL_ERROR "Unknown WDS_TARGET='${WDS_TARGET}'. Supported targets: "
      "win-x86_64 macos-arm (see cmake/CROSS_COMPILE.md).")
  endif()
endif()

project(wds VERSION 1.1.0 LANGUAGES CXX)
# CMake project() VERSION is numeric only. About, crash reports, and packages
# use this marketing string (may include a prerelease suffix).
set(WDS_APP_VERSION "1.1.0-stable")

include("${CMAKE_SOURCE_DIR}/cmake/WdsPlatform.cmake")

enable_testing()

# Chart editor monorepo entry. Layers sit beside each other under the repo root.
#
# Module DAG (lower layers must not depend on upper):
#   ui → interaction-interface → renderer → common
#   ui → audio-player → common
#   ui → core → common
#
# Build flavors (see cmake/WdsPlatform.cmake + scripts/build-target.sh):
#   Release — build-<target>/, package dist/wds-<target>.zip, runtime logging off
#   Debug   — build-<target>-debug/, no zip; script auto-runs preview/editor with verbose logs

option(WDS_BUILD_COMMON "Build shared common layer" ON)
option(WDS_BUILD_CORE "Build chart editor core library" ON)
option(WDS_BUILD_RENDERER "Build Vulkan renderer layer" ON)
option(WDS_BUILD_AUDIO "Build audio-player layer" ON)
option(WDS_BUILD_INTERACTION "Build interaction-interface layer" ON)
option(WDS_BUILD_UI "Build ui layer / editor shell" ON)

# Propagate cross-friendly defaults before subdirectories declare their options.
if(NOT DEFINED WDS_CORE_BUILD_TESTS)
  set(WDS_CORE_BUILD_TESTS ${WDS_CORE_BUILD_TESTS_DEFAULT} CACHE BOOL "Build core unit tests")
endif()
if(NOT DEFINED WDS_CORE_BUILD_EXAMPLE)
  set(WDS_CORE_BUILD_EXAMPLE ${WDS_CORE_BUILD_EXAMPLE_DEFAULT} CACHE BOOL "Build core example")
endif()

if(WDS_BUILD_COMMON)
  add_subdirectory(common)
endif()

if(WDS_BUILD_CORE)
  if(NOT WDS_BUILD_COMMON)
    message(FATAL_ERROR "WDS_BUILD_CORE requires WDS_BUILD_COMMON=ON")
  endif()
  add_subdirectory(core)
endif()

if(WDS_BUILD_AUDIO)
  if(NOT WDS_BUILD_COMMON)
    message(FATAL_ERROR "WDS_BUILD_AUDIO requires WDS_BUILD_COMMON=ON")
  endif()
  add_subdirectory(audio-player)
endif()

if(WDS_BUILD_RENDERER)
  if(NOT WDS_BUILD_COMMON)
    message(FATAL_ERROR "WDS_BUILD_RENDERER requires WDS_BUILD_COMMON=ON")
  endif()
  add_subdirectory(renderer)
  add_subdirectory(chart-render)
endif()

if(WDS_BUILD_INTERACTION)
  if(NOT WDS_BUILD_COMMON OR NOT WDS_BUILD_RENDERER)
    message(FATAL_ERROR "WDS_BUILD_INTERACTION requires WDS_BUILD_COMMON=ON and WDS_BUILD_RENDERER=ON")
  endif()
  add_subdirectory(interaction-interface)
endif()

if(WDS_BUILD_UI)
  if(NOT WDS_BUILD_INTERACTION OR NOT WDS_BUILD_AUDIO OR NOT WDS_BUILD_CORE)
    message(FATAL_ERROR
      "WDS_BUILD_UI requires WDS_BUILD_INTERACTION=ON, WDS_BUILD_AUDIO=ON, WDS_BUILD_CORE=ON")
  endif()
  add_subdirectory(ui)
endif()

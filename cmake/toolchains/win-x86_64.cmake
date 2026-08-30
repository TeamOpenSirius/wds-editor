# Cross toolchain: Windows x86_64 via MinGW-w64
set(WDS_TARGET "win-x86_64")
include("${CMAKE_CURRENT_LIST_DIR}/common.cmake")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(WDS_C_COMPILER NAMES x86_64-w64-mingw32-gcc)
find_program(WDS_CXX_COMPILER NAMES x86_64-w64-mingw32-g++)
find_program(WDS_RC_COMPILER NAMES x86_64-w64-mingw32-windres windres)

if(NOT WDS_C_COMPILER OR NOT WDS_CXX_COMPILER)
  wds_fatal_missing_toolchain("win-x86_64" [=[
Need MinGW-w64 targeting Win64.

Ubuntu / Debian / Linux Mint:

  sudo apt update
  sudo apt install mingw-w64 g++-mingw-w64-x86-64 gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64

Fedora:

  sudo dnf install mingw64-gcc mingw64-gcc-c++ mingw64-winpthreads-static

Renderer / demo deps (libpng, Vulkan, GLFW) via vcpkg or a custom prefix.
Copy scripts/env.example → scripts/env.local and set WDS_VCPKG_ROOT, then:

  ./scripts/build-target.sh win-x86_64

  # or core only (renderer + interaction + ui OFF; renderer-only OFF is FATAL):
  ./scripts/build-target.sh win-x86_64 -- \
    -DWDS_BUILD_RENDERER=OFF -DWDS_BUILD_INTERACTION=OFF -DWDS_BUILD_UI=OFF
  # core without audio: add -DWDS_BUILD_AUDIO=OFF

vcpkg triplet: x64-mingw-static (install libpng zlib glfw3 vulkan-loader).
]=])
endif()

set(CMAKE_C_COMPILER "${WDS_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${WDS_CXX_COMPILER}")
if(WDS_RC_COMPILER)
  set(CMAKE_RC_COMPILER "${WDS_RC_COMPILER}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

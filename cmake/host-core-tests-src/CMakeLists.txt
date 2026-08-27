# TEMPORARY Linux host build for common+core tests. Delete after this pass.
# Reuses the real common/core CMakeLists so host sources cannot drift
# (the previous hand-maintained list omitted utf8_path.cpp).
#
# Do not include cmake/WdsPlatform.cmake: native Linux product configure stays
# unsupported. This project only add_subdirectory's common and core.
cmake_minimum_required(VERSION 3.16)
project(wds_host_core_tests LANGUAGES CXX)

enable_testing()

get_filename_component(WDS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Defaults for this bypass: tests on, example off. Cache (CLI) still wins.
if(NOT DEFINED WDS_COMMON_BUILD_TESTS)
  set(WDS_COMMON_BUILD_TESTS ON CACHE BOOL "Build common unit tests")
endif()
if(NOT DEFINED WDS_CORE_BUILD_TESTS)
  set(WDS_CORE_BUILD_TESTS ON CACHE BOOL "Build core unit tests")
endif()
if(NOT DEFINED WDS_CORE_BUILD_EXAMPLE)
  set(WDS_CORE_BUILD_EXAMPLE OFF CACHE BOOL "Host bypass builds tests only")
endif()

add_subdirectory("${WDS_ROOT}/common" common)
add_subdirectory("${WDS_ROOT}/core" core)

# CMAKE_SOURCE_DIR is this bypass dir, not the repo root. Core fixtures do not
# use CMAKE_SOURCE_DIR; they try cwd-relative tests/fixtures, then
# core/tests/fixtures, then __FILE__/fixtures. Do not set_tests_properties here:
# tests registered in the core subdirectory are not visible in this directory.

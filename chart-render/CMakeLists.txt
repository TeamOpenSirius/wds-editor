# Chart-specific visual helpers (stage geometry, skins, note strips).
# Depends on wds_draw / wds_renderer and official playfield math from wds::core.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING AND
   NOT EXISTS "${CMAKE_BINARY_DIR}/resources")
  # Native Linux builds live outside the checkout by default. The editor and
  # particle tests discover resources beside/above their executables.
  file(CREATE_LINK "${CMAKE_SOURCE_DIR}/resources" "${CMAKE_BINARY_DIR}/resources"
    SYMBOLIC RESULT _wds_resources_link_error)
  if(NOT _wds_resources_link_error STREQUAL "0")
    message(FATAL_ERROR "Cannot link build resources: ${_wds_resources_link_error}")
  endif()
endif()

add_library(wds_chart_render STATIC
  src/stage_geometry.cpp
  src/skin_catalog.cpp
  src/split_line_skins.cpp
  src/split_line_official_colors.cpp
  src/split_effect_particles.cpp
  src/split_wall_profile.cpp
  src/split_collect_pool.cpp
  src/openwds_shuriken_bind.cpp
  src/shuriken/emit.cpp
  src/shuriken/shape.cpp
  src/shuriken/integrate.cpp
  src/shuriken/simd_dispatch.cpp
  src/shuriken/modules.cpp
  src/shuriken/modules_an.cpp
  src/shuriken/evaluate.cpp
  src/shuriken/split_eval_cache.cpp
  src/shuriken/split_eval_carry.cpp
  src/shuriken/split_eval_persist.cpp
  src/note_strips.cpp
  src/note_visual_policy.cpp
)
add_library(wds::chart_render ALIAS wds_chart_render)

# Typed Unity 2022.3 contract used by the open split-effect controller.
# This layer intentionally has deterministic stub bodies for closed Unity
# behavior and carries no CLR/runtime dependency.
add_library(wds_unity_particle_api STATIC
  src/unity_api.cpp
)
add_library(wds::unity_particle_api ALIAS wds_unity_particle_api)
target_include_directories(wds_unity_particle_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wds_unity_particle_api PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(wds_unity_particle_api PRIVATE /W4 /utf-8)
else()
  target_compile_options(wds_unity_particle_api PRIVATE -Wall -Wextra -Wpedantic)
endif()

# OpenWDS OfflinePreview SplitLane resource graph and source-faithful
# SplitEffectController port. Depends only on the typed Unity API above.
add_subdirectory(openwds-split-effects)

target_include_directories(wds_chart_render
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(wds_chart_render
  PUBLIC
    wds::common
    wds::draw
    wds::renderer
    wds::core
    wds::unity_particle_api
    wds::openwds_split_effects
    Threads::Threads
)

if(NOT DEFINED WDS_AN_OP)
  set(WDS_AN_OP 1)
endif()
target_compile_definitions(wds_chart_render PRIVATE
  WDS_AN_OP=${WDS_AN_OP})

target_compile_features(wds_chart_render PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(wds_chart_render PRIVATE /W4)
  # hop: simd_dispatch stays contract-off. AN contraction is scoped inside
  # modules_an.inl.hpp (included by modules.cpp / integrate.cpp); the thin
  # modules_an.cpp TU keeps the same flag as a compile-check.
  set_source_files_properties(
    src/shuriken/simd_dispatch.cpp
    src/shuriken/modules_an.cpp
    PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
  target_compile_options(wds_chart_render PRIVATE -Wall -Wextra -Wpedantic)
  set_source_files_properties(
    src/shuriken/simd_dispatch.cpp
    src/shuriken/modules_an.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

# Unit tests live in this module; system tests live in ${CMAKE_SOURCE_DIR}/tests/system.
add_executable(wds_note_visual_policy_tests tests/note_visual_policy_tests.cpp)
target_link_libraries(wds_note_visual_policy_tests PRIVATE wds::chart_render)

add_executable(wds_shuriken_noise_tests tests/shuriken_noise_tests.cpp)
target_link_libraries(wds_shuriken_noise_tests PRIVATE wds::chart_render)

add_executable(wds_split_over_cap_tests tests/split_over_cap_tests.cpp)
target_link_libraries(wds_split_over_cap_tests PRIVATE wds::chart_render)
target_include_directories(wds_split_over_cap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(wds_split_effect_resource_root_tests tests/split_effect_resource_root_tests.cpp)
target_link_libraries(wds_split_effect_resource_root_tests PRIVATE wds::chart_render)

add_executable(wds_split_collect_topo_tests tests/split_collect_topo_tests.cpp)
target_link_libraries(wds_split_collect_topo_tests PRIVATE wds::chart_render)
target_include_directories(wds_split_collect_topo_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(wds_unity_particle_api_contract_tests tests/unity_api_contract_tests.cpp)
target_link_libraries(wds_unity_particle_api_contract_tests PRIVATE wds::unity_particle_api)

add_executable(wds_split_line_official_colors_tests
  ${CMAKE_SOURCE_DIR}/tests/system/split_line_official_colors_tests.cpp)
target_link_libraries(wds_split_line_official_colors_tests PRIVATE wds::chart_render)
target_compile_definitions(wds_split_line_official_colors_tests PRIVATE
  WDS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")

add_executable(wds_split_effect_playback_tests
  ${CMAKE_SOURCE_DIR}/tests/system/split_effect_playback_tests.cpp)
target_link_libraries(wds_split_effect_playback_tests PRIVATE wds::chart_render)

add_executable(wds_split_effect_seek_cache_tests
  ${CMAKE_SOURCE_DIR}/tests/system/split_effect_seek_cache_tests.cpp)
target_link_libraries(wds_split_effect_seek_cache_tests PRIVATE wds::chart_render)

# Developer benchmark. It forks worker processes and uses poll / waitpid / getrusage, so it is POSIX-only
# (macOS, Linux) and is skipped for Windows targets (MinGW cross build and MSVC).
if(NOT WIN32)
  add_executable(wds_split_effect_bench tools/split_effect_bench.cpp)
  target_link_libraries(wds_split_effect_bench PRIVATE wds::chart_render)
  target_compile_options(wds_split_effect_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Offline generator for ui/src/qt/split_effect_snapshot_table.inc (static split-line particle
# indicators). Not part of the app and not built by default (EXCLUDE_FROM_ALL); build it by hand
# after changing the sim or the catalog:
#   cmake --build build --target wds_split_effect_snapshot_bake
#   wds_split_effect_snapshot_bake --out-inc ui/src/qt/split_effect_snapshot_table.inc
add_executable(wds_split_effect_snapshot_bake EXCLUDE_FROM_ALL tools/split_effect_snapshot_bake.cpp)
target_link_libraries(wds_split_effect_snapshot_bake PRIVATE wds::chart_render)
if(MSVC)
  target_compile_options(wds_split_effect_snapshot_bake PRIVATE /W4)
else()
  target_compile_options(wds_split_effect_snapshot_bake PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(wds_openwds_shuriken_bind_tests
  ${CMAKE_SOURCE_DIR}/tests/system/openwds_shuriken_bind_tests.cpp)
target_link_libraries(wds_openwds_shuriken_bind_tests PRIVATE
  wds::chart_render
  wds::openwds_split_effects)
target_compile_definitions(wds_openwds_shuriken_bind_tests PRIVATE
  WDS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# Host ctest cannot execute foreign PE binaries; still build the test targets.
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME wds_note_visual_policy_tests COMMAND wds_note_visual_policy_tests)
  add_test(NAME wds_shuriken_noise_tests COMMAND wds_shuriken_noise_tests)
  add_test(NAME wds_split_over_cap_tests COMMAND wds_split_over_cap_tests)
  add_test(NAME wds_split_effect_resource_root_tests COMMAND wds_split_effect_resource_root_tests)
  add_test(NAME wds_split_collect_topo_tests COMMAND wds_split_collect_topo_tests)
  add_test(NAME wds_unity_particle_api_contract_tests COMMAND wds_unity_particle_api_contract_tests)
  add_test(NAME wds_split_line_official_colors_tests COMMAND wds_split_line_official_colors_tests)
  add_test(NAME wds_split_effect_playback_tests COMMAND wds_split_effect_playback_tests)
  add_test(NAME wds_split_effect_seek_cache_tests COMMAND wds_split_effect_seek_cache_tests)
  add_test(NAME wds_openwds_shuriken_bind_tests COMMAND wds_openwds_shuriken_bind_tests)
else()
  message(STATUS "Skipping ctest registration for chart-render tests (cross build)")
endif()

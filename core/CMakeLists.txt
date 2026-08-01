# Chart editor core: notation, clock, preview snapshot, serialization.
# No UI / rendering / input handling lives here.

add_library(wds_core STATIC
    src/notation.cpp
    src/timing_map.cpp
    src/edit_grid.cpp
    src/edit_history.cpp
    src/note_edit_ops.cpp
    src/chart_session.cpp
    src/start_ms_avl_index.cpp
    src/chart_index.cpp
    src/gimmick.cpp
    src/split_lane_simulator.cpp
    src/chart_serializer.cpp
    src/file_io.cpp
    src/project.cpp
    src/official_chart.cpp
    src/sus_chart.cpp
    src/note_position_calculator.cpp
    src/auto_judge_simulator.cpp
    src/preview_snapshot.cpp
    src/preview_snapshot_builder.cpp
    src/chart_editor_engine.cpp
)

add_library(wds::core ALIAS wds_core)

target_include_directories(wds_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(wds_core PUBLIC wds::common)

target_compile_features(wds_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(wds_core PRIVATE /W4)
else()
    target_compile_options(wds_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Defaults come from top-level WdsPlatform.cmake when cross-compiling.
option(WDS_CORE_BUILD_EXAMPLE "Build core example" ON)
if(WDS_CORE_BUILD_EXAMPLE)
    add_executable(wds_core_example examples/example_main.cpp)
    target_link_libraries(wds_core_example PRIVATE wds_core)
endif()

option(WDS_CORE_BUILD_TESTS "Build core unit tests" ON)
if(WDS_CORE_BUILD_TESTS)
    add_executable(wds_core_tests tests/chart_editor_tests.cpp)
    target_link_libraries(wds_core_tests PRIVATE wds_core)
    target_include_directories(wds_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(MSVC)
        target_compile_options(wds_core_tests PRIVATE /utf-8)
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
      add_test(NAME wds_core_tests COMMAND wds_core_tests)
    else()
      message(STATUS "Skipping ctest registration for wds_core_tests (cross build)")
    endif()
endif()

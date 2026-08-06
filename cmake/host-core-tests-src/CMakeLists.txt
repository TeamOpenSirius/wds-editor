# TEMPORARY Linux host build for common+core tests. Delete after this pass.
cmake_minimum_required(VERSION 3.16)
project(wds_host_core_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
add_compile_definitions(WDS_ENABLE_LOGGING=0)
get_filename_component(WDS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_library(wds_common STATIC ${WDS_ROOT}/common/src/crash_handler.cpp)
add_library(wds::common ALIAS wds_common)
target_include_directories(wds_common PUBLIC ${WDS_ROOT}/common/include)

add_executable(wds_common_tests ${WDS_ROOT}/common/tests/common_tests.cpp)
target_link_libraries(wds_common_tests PRIVATE wds::common)
add_test(NAME wds_common_tests COMMAND wds_common_tests)

add_library(wds_core STATIC
  ${WDS_ROOT}/core/src/notation.cpp
  ${WDS_ROOT}/core/src/timing_map.cpp
  ${WDS_ROOT}/core/src/edit_grid.cpp
  ${WDS_ROOT}/core/src/edit_history.cpp
  ${WDS_ROOT}/core/src/note_edit_ops.cpp
  ${WDS_ROOT}/core/src/chart_session.cpp
  ${WDS_ROOT}/core/src/start_ms_avl_index.cpp
  ${WDS_ROOT}/core/src/chart_index.cpp
  ${WDS_ROOT}/core/src/gimmick.cpp
  ${WDS_ROOT}/core/src/split_lane_simulator.cpp
  ${WDS_ROOT}/core/src/chart_serializer.cpp
  ${WDS_ROOT}/core/src/file_io.cpp
  ${WDS_ROOT}/core/src/project.cpp
  ${WDS_ROOT}/core/src/official_chart.cpp
  ${WDS_ROOT}/core/src/sus_chart.cpp
  ${WDS_ROOT}/core/src/note_position_calculator.cpp
  ${WDS_ROOT}/core/src/auto_judge_simulator.cpp
  ${WDS_ROOT}/core/src/preview_snapshot.cpp
  ${WDS_ROOT}/core/src/preview_snapshot_builder.cpp
  ${WDS_ROOT}/core/src/chart_editor_engine.cpp
)
add_library(wds::core ALIAS wds_core)
target_include_directories(wds_core PUBLIC ${WDS_ROOT}/core/include)
target_link_libraries(wds_core PUBLIC wds::common)

add_executable(wds_core_tests ${WDS_ROOT}/core/tests/chart_editor_tests.cpp)
target_link_libraries(wds_core_tests PRIVATE wds_core)
target_include_directories(wds_core_tests PRIVATE ${WDS_ROOT}/core/tests)
add_test(NAME wds_core_tests COMMAND wds_core_tests)

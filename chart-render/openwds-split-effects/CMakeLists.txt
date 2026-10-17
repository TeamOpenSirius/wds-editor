# Self-contained OpenWDS OfflinePreview SplitLane resource/controller library.
# The parent chart-render CMakeLists adds this directory after the typed Unity
# API target has been declared.
add_library(wds_openwds_split_effects STATIC
  src/resources.cpp
  src/controller.cpp
  src/offline_preview.cpp
  src/asset_runtime.cpp
  src/compatible_shaders.cpp
)
add_library(wds::openwds_split_effects ALIAS wds_openwds_split_effects)
target_include_directories(wds_openwds_split_effects
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# OfflineRhythmPreview reuses the low-level official playfield formulas from
# wds::core; this target does not depend on the higher-level chart-render lib.
target_link_libraries(wds_openwds_split_effects PUBLIC
  wds::unity_particle_api
  wds::core)
target_compile_features(wds_openwds_split_effects PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(wds_openwds_split_effects PRIVATE /W4 /utf-8)
else()
  target_compile_options(wds_openwds_split_effects PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(wds_openwds_split_effects_contract_tests tests/contract_tests.cpp)
target_link_libraries(wds_openwds_split_effects_contract_tests PRIVATE
  wds::openwds_split_effects)
target_compile_features(wds_openwds_split_effects_contract_tests PRIVATE cxx_std_17)
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME wds_openwds_split_effects_contract_tests
           COMMAND wds_openwds_split_effects_contract_tests)
endif()

add_executable(wds_openwds_offline_preview_contract_tests tests/offline_preview_contract_tests.cpp)
target_link_libraries(wds_openwds_offline_preview_contract_tests PRIVATE
  wds::openwds_split_effects)
target_compile_features(wds_openwds_offline_preview_contract_tests PRIVATE cxx_std_17)
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME wds_openwds_offline_preview_contract_tests
           COMMAND wds_openwds_offline_preview_contract_tests)
endif()

add_executable(wds_openwds_hydrate_contract_tests
  ${CMAKE_SOURCE_DIR}/tests/system/hydrate_contract_tests.cpp)
target_link_libraries(wds_openwds_hydrate_contract_tests PRIVATE
  wds::openwds_split_effects)
target_compile_features(wds_openwds_hydrate_contract_tests PRIVATE cxx_std_17)
target_compile_definitions(wds_openwds_hydrate_contract_tests PRIVATE
  WDS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")

add_executable(wds_openwds_asset_runtime_tests
  ${CMAKE_SOURCE_DIR}/tests/system/asset_runtime_tests.cpp)
target_link_libraries(wds_openwds_asset_runtime_tests PRIVATE
  wds::openwds_split_effects)
target_compile_features(wds_openwds_asset_runtime_tests PRIVATE cxx_std_17)
target_compile_definitions(wds_openwds_asset_runtime_tests PRIVATE
  WDS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")

add_executable(wds_openwds_compatible_shaders_tests tests/compatible_shaders_tests.cpp)
target_link_libraries(wds_openwds_compatible_shaders_tests PRIVATE
  wds::openwds_split_effects)
target_compile_features(wds_openwds_compatible_shaders_tests PRIVATE cxx_std_17)

add_executable(wds_openwds_controller_lifecycle_tests
  ${CMAKE_SOURCE_DIR}/tests/system/controller_lifecycle_tests.cpp)
target_link_libraries(wds_openwds_controller_lifecycle_tests PRIVATE
  wds::openwds_split_effects)
target_compile_features(wds_openwds_controller_lifecycle_tests PRIVATE cxx_std_17)
target_compile_definitions(wds_openwds_controller_lifecycle_tests PRIVATE
  WDS_TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}")

if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME wds_openwds_hydrate_contract_tests
           COMMAND wds_openwds_hydrate_contract_tests)
  add_test(NAME wds_openwds_asset_runtime_tests
           COMMAND wds_openwds_asset_runtime_tests)
  add_test(NAME wds_openwds_compatible_shaders_tests
           COMMAND wds_openwds_compatible_shaders_tests)
  add_test(NAME wds_openwds_controller_lifecycle_tests
           COMMAND wds_openwds_controller_lifecycle_tests)
endif()

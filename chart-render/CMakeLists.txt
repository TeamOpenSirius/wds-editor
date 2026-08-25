# Chart-specific visual helpers (stage geometry, skins, note strips).
# Depends on wds_draw / wds_renderer for DrawBatch + TextureCache; no core/audio.

add_library(wds_chart_render STATIC
  src/stage_geometry.cpp
  src/skin_catalog.cpp
  src/split_line_skins.cpp
  src/split_line_official_colors.cpp
  src/note_strips.cpp
  src/note_visual_policy.cpp
)
add_library(wds::chart_render ALIAS wds_chart_render)

target_include_directories(wds_chart_render
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(wds_chart_render
  PUBLIC
    wds::draw
    wds::renderer
)

target_compile_features(wds_chart_render PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(wds_chart_render PRIVATE /W4)
else()
  target_compile_options(wds_chart_render PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(wds_note_visual_policy_tests tests/note_visual_policy_tests.cpp)
target_link_libraries(wds_note_visual_policy_tests PRIVATE wds::chart_render)
add_test(NAME wds_note_visual_policy_tests COMMAND wds_note_visual_policy_tests)

add_executable(wds_split_line_official_colors_tests tests/split_line_official_colors_tests.cpp)
target_link_libraries(wds_split_line_official_colors_tests PRIVATE wds::chart_render)
add_test(NAME wds_split_line_official_colors_tests COMMAND wds_split_line_official_colors_tests)

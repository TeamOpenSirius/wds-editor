# Interaction layer: input events, widgets, shortcuts. Depends on wds::common + wds::draw.
# Platform input adapters live in the UI host; this library remains framework independent.

add_library(wds_interaction STATIC
  src/bitmap_font.cpp
  src/font_atlas.cpp
  src/editor_input.cpp
  src/editor_shortcuts.cpp
  src/gesture.cpp
  src/platform.cpp
  src/shortcuts.cpp
  src/theme.cpp
  src/ui_painter.cpp
  src/widget.cpp
  src/widget_root.cpp
  src/widgets/button.cpp
  src/widgets/checkbox.cpp
  src/widgets/text_field.cpp
  src/widgets/shortcut_field.cpp
  src/widgets/slider.cpp
  src/widgets/dropdown.cpp
  src/widgets/combo_box.cpp
  src/widgets/stepper.cpp
  src/widgets/icon_button.cpp
  src/widgets/modal.cpp
)
add_library(wds::interaction ALIAS wds_interaction)

target_include_directories(wds_interaction
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(wds_interaction
  PUBLIC
    wds::common
    wds::draw
)

target_compile_features(wds_interaction PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(wds_interaction PRIVATE /W4 /utf-8)
else()
  target_compile_options(wds_interaction PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Qt owns platform input adaptation; interaction remains framework independent.

option(WDS_INTERACTION_BUILD_TESTS "Build interaction unit tests" ON)
if(WDS_INTERACTION_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(wds_interaction_tests tests/interaction_tests.cpp)
  target_link_libraries(wds_interaction_tests PRIVATE wds::interaction)
  if(MSVC)
    target_compile_options(wds_interaction_tests PRIVATE /W4 /utf-8)
  else()
    target_compile_options(wds_interaction_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME wds_interaction_tests COMMAND wds_interaction_tests)
endif()

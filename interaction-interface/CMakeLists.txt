# Interaction layer: input events, widgets, shortcuts. Depends on wds::common + wds::draw.
# GLFW adapter is a separate optional target (wds_interaction_glfw).

add_library(wds_interaction STATIC
  src/bitmap_font.cpp
  src/font_atlas.cpp
  src/editor_input.cpp
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
  target_compile_options(wds_interaction PRIVATE /W4)
else()
  target_compile_options(wds_interaction PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- GLFW adapter (optional; ui / editor links this) --------------------------------
set(WDS_INTERACTION_GLFW_TARGET "")
find_package(glfw3 CONFIG QUIET)
if(TARGET glfw)
  set(WDS_INTERACTION_GLFW_TARGET glfw)
elseif(TARGET glfw3)
  set(WDS_INTERACTION_GLFW_TARGET glfw3)
else()
  find_library(WDS_INTERACTION_GLFW_LIB NAMES glfw3 glfw)
endif()

if(WDS_INTERACTION_GLFW_TARGET OR WDS_INTERACTION_GLFW_LIB)
  add_library(wds_interaction_glfw STATIC src/glfw_input_adapter.cpp)
  add_library(wds::interaction_glfw ALIAS wds_interaction_glfw)
  target_include_directories(wds_interaction_glfw
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(wds_interaction_glfw PUBLIC wds::interaction)
  if(WDS_INTERACTION_GLFW_TARGET)
    target_link_libraries(wds_interaction_glfw PRIVATE ${WDS_INTERACTION_GLFW_TARGET})
  else()
    find_path(WDS_INTERACTION_GLFW_INCLUDE_DIR GLFW/glfw3.h)
    if(WDS_INTERACTION_GLFW_INCLUDE_DIR)
      target_include_directories(wds_interaction_glfw PRIVATE ${WDS_INTERACTION_GLFW_INCLUDE_DIR})
    endif()
    target_link_libraries(wds_interaction_glfw PRIVATE ${WDS_INTERACTION_GLFW_LIB})
  endif()
  target_compile_features(wds_interaction_glfw PUBLIC cxx_std_17)
else()
  message(WARNING "GLFW not found; wds_interaction_glfw skipped (ui build may fail)")
endif()

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

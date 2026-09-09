# UI library and Qt editor shell (realtime preview/edit remain Vulkan-rendered).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
option(WDS_USE_QT_EDITOR "Build the Qt editor entry point" ON)
if(WDS_USE_QT_EDITOR)
add_executable(wds_editor
  apps/wds_editor_qt.cpp
  src/qt/editor_main_window.cpp
  src/qt/realtime_vulkan_window.cpp
  src/qt/qt_input_adapter.cpp
  src/qt/note_icons.cpp
  src/qt/playback_dock.cpp
  src/qt/settings_dialog.cpp
  src/qt/flow_layout.cpp
  src/qt/wds_theme.cpp)
add_executable(wds_editor_qt ALIAS wds_editor)
add_executable(wds_qt_shell_checkpoint ALIAS wds_editor)
target_include_directories(wds_editor PRIVATE include ${CMAKE_SOURCE_DIR}/interaction-interface/include)
target_link_libraries(wds_editor PRIVATE Qt6::Widgets wds::ui)
target_compile_features(wds_editor PRIVATE cxx_std_17)
target_compile_definitions(wds_editor PRIVATE WDS_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_definitions(wds_editor PRIVATE QT_NO_KEYWORDS)
if(MSVC)
  target_compile_options(wds_editor PRIVATE /utf-8)
endif()
install(TARGETS wds_editor RUNTIME DESTINATION bin)
if(WIN32)
  install(FILES "${WDS_BASS_LIBRARY}" "${WDS_BASSMIX_LIBRARY}" DESTINATION bin)
else()
  install(FILES "${WDS_BASS_LIBRARY}" "${WDS_BASSMIX_LIBRARY}" DESTINATION lib)
endif()
install(DIRECTORY "${CMAKE_SOURCE_DIR}/skins" DESTINATION .)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/effects" DESTINATION .)
install(DIRECTORY "${CMAKE_BINARY_DIR}/renderer/shaders/" DESTINATION shaders)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansSC-Regular.ttf")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansSC-Regular.ttf" DESTINATION fonts)
endif()
  if(COMMAND qt_generate_deploy_app_script)
    qt_generate_deploy_app_script(
      TARGET wds_editor
      OUTPUT_SCRIPT wds_qt_deploy_script
      NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${wds_qt_deploy_script})
  endif()
endif()

add_library(wds_ui STATIC
  src/ui_manager.cpp
  src/resource_paths.cpp
  src/startup_deps.cpp
  src/editor_session.cpp
  src/editor_ui_config.cpp
  src/curve_template.cpp
  src/toolbar_curve_selection.cpp
  src/timeline_wheel.cpp
  src/native_file_dialog.cpp
  src/layout/editor_layout.cpp
  src/regions/preview/chart_preview_panel.cpp
  src/regions/preview/playback_preview.cpp
  src/regions/edit/chart_edit_renderer.cpp
  src/regions/edit/chart_edit_panel.cpp
  src/regions/edit/edit_gutters.cpp
  src/regions/settings/preview_settings_panel.cpp
  src/regions/settings/width_slots_dialog.cpp
  src/regions/settings/curve_templates_dialog.cpp
  src/regions/settings/export_choice_dialog.cpp
  src/regions/settings/chart_add_dialog.cpp
  src/regions/settings/unsaved_changes_dialog.cpp
  src/regions/status/status_bar.cpp
  src/regions/toolbar/editor_toolbar.cpp
)
add_library(wds::ui ALIAS wds_ui)

target_include_directories(wds_ui
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(wds_ui
  PUBLIC
    wds::interaction
    wds::renderer
    wds::chart_render
    wds::audio_player
    wds::core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Portable layouts still need the BASS dylib next to the binary (probed via wds::audio).
if(TARGET wds_bass)
  target_link_libraries(wds_ui PRIVATE wds_bass)
endif()
if(TARGET wds_bassmix)
  target_link_libraries(wds_ui PRIVATE wds_bassmix)
endif()
target_compile_features(wds_ui PUBLIC cxx_std_17)
target_compile_definitions(wds_ui PRIVATE WDS_REPO_ROOT="${CMAKE_SOURCE_DIR}")
if(WDS_USE_QT_EDITOR)
  target_compile_definitions(wds_ui PRIVATE WDS_USE_QT_EDITOR=1)
endif()

add_executable(wds_ui_logic_tests
  tests/edit_viewport_tests.cpp
  src/layout/editor_layout.cpp
  src/timeline_wheel.cpp
)
target_link_libraries(wds_ui_logic_tests PRIVATE wds::core wds::interaction)
target_include_directories(wds_ui_logic_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(wds_note_draw_order_tests tests/note_draw_order_tests.cpp)
target_link_libraries(wds_note_draw_order_tests PRIVATE wds::chart_render wds::core)

add_executable(wds_ui_config_tests
  tests/editor_ui_config_tests.cpp
  src/editor_ui_config.cpp
  src/curve_template.cpp
)
target_link_libraries(wds_ui_config_tests PRIVATE wds::core wds::interaction)
target_include_directories(wds_ui_config_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(wds_ui_config_tests PRIVATE /utf-8)
endif()

add_executable(wds_curve_templates_dialog_tests
  tests/curve_templates_dialog_tests.cpp
  src/regions/settings/curve_templates_dialog.cpp
  src/curve_template.cpp
)
target_link_libraries(wds_curve_templates_dialog_tests PRIVATE wds::core wds::interaction)
target_include_directories(wds_curve_templates_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(wds_curve_templates_dialog_tests PRIVATE /utf-8)
endif()

add_executable(wds_editor_toolbar_tests
  tests/editor_toolbar_tests.cpp
)
target_link_libraries(wds_editor_toolbar_tests PRIVATE wds::ui)
target_include_directories(wds_editor_toolbar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(wds_editor_toolbar_tests PRIVATE /utf-8)
endif()

add_executable(wds_chart_edit_panel_curve_tests
  tests/chart_edit_panel_curve_tests.cpp
)
target_link_libraries(wds_chart_edit_panel_curve_tests PRIVATE wds::ui)
target_include_directories(wds_chart_edit_panel_curve_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(wds_chart_edit_panel_curve_tests PRIVATE /utf-8)
endif()

add_executable(wds_chart_validation_ui_tests
  tests/chart_validation_ui_tests.cpp
)
target_link_libraries(wds_chart_validation_ui_tests PRIVATE wds::ui)
target_include_directories(wds_chart_validation_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(wds_chart_validation_ui_tests PRIVATE /utf-8)
endif()

# Host ctest cannot execute foreign PE binaries; still build the test targets.
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME wds_ui_logic_tests COMMAND wds_ui_logic_tests)
  add_test(NAME wds_note_draw_order_tests COMMAND wds_note_draw_order_tests)
  add_test(NAME wds_ui_config_tests COMMAND wds_ui_config_tests)
  add_test(NAME wds_curve_templates_dialog_tests COMMAND wds_curve_templates_dialog_tests)
  add_test(NAME wds_editor_toolbar_tests COMMAND wds_editor_toolbar_tests)
  add_test(NAME wds_chart_edit_panel_curve_tests COMMAND wds_chart_edit_panel_curve_tests)
  add_test(NAME wds_chart_validation_ui_tests COMMAND wds_chart_validation_ui_tests)
  if(WIN32)
    get_filename_component(_wds_qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
    set(_wds_test_path
      "${_wds_qt_root}/bin\\;${WDS_BASS_LIBRARY_DIR}\\;$ENV{PATH}")
    set_tests_properties(wds_editor_toolbar_tests wds_chart_edit_panel_curve_tests
      wds_chart_validation_ui_tests PROPERTIES ENVIRONMENT "PATH=${_wds_test_path}")
  endif()
else()
  message(STATUS "Skipping ctest registration for ui tests (cross build)")
endif()

if(MSVC)
  target_compile_options(wds_ui PRIVATE /W4 /utf-8)
else()
  target_compile_options(wds_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The Qt target above is the sole product editor; the realtime renderer is hosted by
# QVulkanInstance.

# UI library and Qt editor shell (realtime preview/edit remain Vulkan-rendered).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
# Homebrew ships qtsvg in its own keg; Qt6Config only looks beside qtbase.
if(NOT Qt6Svg_DIR)
  foreach(_wds_qt_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_wds_qt_prefix}/lib/cmake/Qt6Svg/Qt6SvgConfig.cmake")
      set(Qt6Svg_DIR "${_wds_qt_prefix}/lib/cmake/Qt6Svg")
      break()
    endif()
  endforeach()
endif()
find_package(Qt6Svg REQUIRED)
option(WDS_USE_QT_EDITOR "Build the Qt editor entry point" ON)
if(WDS_USE_QT_EDITOR)
set(CMAKE_AUTORCC ON)
add_executable(wds_editor WIN32
  apps/wds_editor_qt.cpp
  wds_resources.qrc
  src/qt/editor_main_window.cpp
  src/qt/realtime_vulkan_window.cpp
  src/qt/chart_edit_widget.cpp
  src/qt/qt_input_adapter.cpp
  src/qt/note_icons.cpp
  src/qt/playback_dock.cpp
  src/qt/stem_panel.cpp
  src/qt/settings_dialog.cpp
  src/qt/shortcut_settings_dialog.cpp
  src/qt/curve_templates_panel.cpp
  src/qt/flow_layout.cpp
  src/qt/spread_layout.cpp
  src/qt/wds_theme.cpp
  src/qt/caption_check.cpp
  src/qt/fluent_icons.cpp
  src/qt/about_dialog.cpp
  src/qt/update_checker.cpp
  src/qt/busy_dialog.cpp
  src/qt/split_picker_dialog.cpp
  src/qt/split_effect_snapshot.cpp
  src/qt/split_effect_snapshot_table.cpp
  src/qt/timing_edit_dialog.cpp
  src/qt/speed_edit_dialog.cpp
  src/qt/export_video_dialog.cpp
  src/qt/export_video_job.cpp
  src/qt/ffmpeg_resolve.cpp
  src/qt/ffmpeg_encoder.cpp)
add_executable(wds_editor_qt ALIAS wds_editor)
target_include_directories(wds_editor PRIVATE include ${CMAKE_SOURCE_DIR}/interaction-interface/include)
target_link_libraries(wds_editor PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Svg Qt6::Network wds::ui)
get_filename_component(_wds_qtsvg_prefix "${Qt6Svg_DIR}/../../.." ABSOLUTE)
if(EXISTS "${_wds_qtsvg_prefix}/share/qt/plugins")
  target_compile_definitions(wds_editor PRIVATE
    WDS_QT_SVG_PLUGIN_DIR="${_wds_qtsvg_prefix}/share/qt/plugins")
elseif(EXISTS "${_wds_qtsvg_prefix}/plugins")
  target_compile_definitions(wds_editor PRIVATE
    WDS_QT_SVG_PLUGIN_DIR="${_wds_qtsvg_prefix}/plugins")
endif()
target_compile_features(wds_editor PRIVATE cxx_std_17)
target_compile_definitions(wds_editor PRIVATE WDS_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_definitions(wds_editor PRIVATE QT_NO_KEYWORDS WDS_APP_VERSION="${WDS_APP_VERSION}")

if(WIN32)
  enable_language(RC)
  configure_file(wds_version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/wds_version.rc" @ONLY)
  target_sources(wds_editor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/wds_version.rc")
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WDS_BASS_LIBRARY}" "${WDS_BASSMIX_LIBRARY}" "$<TARGET_FILE_DIR:wds_editor>")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The startup dependency check expects BASS beside the executable. RPATH
  # also supports the bin/ + lib/ install layout without a global loader change.
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WDS_BASS_LIBRARY}" "${WDS_BASSMIX_LIBRARY}" "$<TARGET_FILE_DIR:wds_editor>")
  set_target_properties(wds_editor PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN/../lib")
endif()
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
install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources" DESTINATION .)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/icons" DESTINATION .)
endif()
install(DIRECTORY "${CMAKE_BINARY_DIR}/renderer/shaders/" DESTINATION shaders)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansSC-Regular.ttf")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansSC-Regular.ttf" DESTINATION fonts)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SegoeFluentIcons.ttf")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SegoeFluentIcons.ttf" DESTINATION fonts)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/theme/Yami.obt")
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/theme/" DESTINATION theme)
endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND COMMAND qt_generate_deploy_script)
    # Qt 6.8's app helper emits an unquoted executable path on Linux. Generate
    # the same deployment call with quotes so build directories may contain spaces.
    qt_generate_deploy_script(
      TARGET wds_editor
      OUTPUT_SCRIPT wds_qt_deploy_script
      CONTENT "qt_deploy_runtime_dependencies(EXECUTABLE \"$<TARGET_FILE:wds_editor>\" GENERATE_QT_CONF)")
    install(SCRIPT "${wds_qt_deploy_script}")
  elseif(COMMAND qt_generate_deploy_app_script)
    qt_generate_deploy_app_script(
      TARGET wds_editor
      OUTPUT_SCRIPT wds_qt_deploy_script
      NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${wds_qt_deploy_script}")
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
  src/regions/edit/split_effect_tags.cpp
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
    wds::common
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

add_executable(wds_split_effect_tags_tests
  tests/split_effect_tags_tests.cpp
)
target_link_libraries(wds_split_effect_tags_tests PRIVATE wds::ui)
target_include_directories(wds_split_effect_tags_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(wds_split_effect_tags_tests PRIVATE /utf-8)
endif()

add_executable(wds_chart_validation_ui_tests
  tests/chart_validation_ui_tests.cpp
)
target_link_libraries(wds_chart_validation_ui_tests PRIVATE wds::ui)
target_include_directories(wds_chart_validation_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(wds_chart_validation_ui_tests PRIVATE /utf-8)
endif()

# Split indicator benchmark: the snapshot sources live in wds_editor, so compile them into the test directly.
add_executable(wds_split_indicator_perf_tests
  tests/split_indicator_perf_tests.cpp
  src/qt/split_effect_snapshot.cpp
  src/qt/split_effect_snapshot_table.cpp
)
target_link_libraries(wds_split_indicator_perf_tests PRIVATE wds::ui)
target_include_directories(wds_split_indicator_perf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(wds_split_indicator_perf_tests PRIVATE QT_NO_KEYWORDS WDS_REPO_ROOT="${CMAKE_SOURCE_DIR}")
if(MSVC)
  target_compile_options(wds_split_indicator_perf_tests PRIVATE /utf-8)
endif()

# Diagnostic: renders the real strip indicators to PNG contact sheets under ${CMAKE_BINARY_DIR}/split_indicator_dump.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/split_indicator_dump")
add_executable(wds_split_indicator_dump_tests
  tests/split_indicator_dump_tests.cpp
  src/qt/split_effect_snapshot.cpp
  src/qt/split_effect_snapshot_table.cpp
)
target_link_libraries(wds_split_indicator_dump_tests PRIVATE wds::ui)
target_include_directories(wds_split_indicator_dump_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(wds_split_indicator_dump_tests PRIVATE QT_NO_KEYWORDS
  WDS_REPO_ROOT="${CMAKE_SOURCE_DIR}" WDS_DUMP_DIR="${CMAKE_BINARY_DIR}/split_indicator_dump")
if(MSVC)
  target_compile_options(wds_split_indicator_dump_tests PRIVATE /utf-8)
endif()

# Host ctest cannot execute foreign PE binaries; still build the test targets.
if(NOT CMAKE_CROSSCOMPILING)
  add_executable(wds_stem_panel_tests tests/stem_panel_tests.cpp src/qt/stem_panel.cpp wds_resources.qrc)
  target_link_libraries(wds_stem_panel_tests PRIVATE wds::ui Qt6::Widgets)
  add_executable(wds_export_video_dialog_tests
    tests/export_video_dialog_tests.cpp
    src/qt/ffmpeg_resolve.cpp
    src/qt/ffmpeg_encoder.cpp)
  target_link_libraries(wds_export_video_dialog_tests PRIVATE wds::ui Qt6::Widgets Qt6::Network)
  target_compile_definitions(wds_export_video_dialog_tests PRIVATE QT_NO_KEYWORDS)
  if(MSVC)
    target_compile_options(wds_export_video_dialog_tests PRIVATE /utf-8)
  endif()
  add_test(NAME wds_export_video_dialog_tests COMMAND wds_export_video_dialog_tests -platform offscreen)
  target_compile_definitions(wds_stem_panel_tests PRIVATE QT_NO_KEYWORDS)
  set_target_properties(wds_stem_panel_tests PROPERTIES AUTORCC ON)
  if(MSVC)
    target_compile_options(wds_stem_panel_tests PRIVATE /utf-8)
  endif()
  add_test(NAME wds_stem_panel_tests COMMAND wds_stem_panel_tests -platform offscreen)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME wds_stem_worker_tests COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
      -s "${CMAKE_SOURCE_DIR}/stem-worker" -p "test_*.py")
  endif()
  add_test(NAME wds_ui_logic_tests COMMAND wds_ui_logic_tests)
  add_test(NAME wds_note_draw_order_tests COMMAND wds_note_draw_order_tests)
  add_test(NAME wds_ui_config_tests COMMAND wds_ui_config_tests)
  add_test(NAME wds_curve_templates_dialog_tests COMMAND wds_curve_templates_dialog_tests)
  add_test(NAME wds_editor_toolbar_tests COMMAND wds_editor_toolbar_tests)
  add_test(NAME wds_chart_edit_panel_curve_tests COMMAND wds_chart_edit_panel_curve_tests)
  add_test(NAME wds_split_effect_tags_tests COMMAND wds_split_effect_tags_tests)
  add_test(NAME wds_chart_validation_ui_tests COMMAND wds_chart_validation_ui_tests)
  add_test(NAME wds_split_indicator_perf_tests COMMAND wds_split_indicator_perf_tests)
  set_tests_properties(wds_split_indicator_perf_tests PROPERTIES LABELS "perf" TIMEOUT 300)
  add_test(NAME wds_split_indicator_dump_tests COMMAND wds_split_indicator_dump_tests)
  set_tests_properties(wds_split_indicator_dump_tests PROPERTIES LABELS "dump" TIMEOUT 300)
  if(WIN32)
    get_filename_component(_wds_qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
    set(_wds_test_path
      "${_wds_qt_root}/bin\\;${WDS_BASS_LIBRARY_DIR}\\;$ENV{PATH}")
    set_tests_properties(wds_stem_panel_tests wds_editor_toolbar_tests wds_chart_edit_panel_curve_tests
      wds_split_effect_tags_tests wds_chart_validation_ui_tests
      wds_split_indicator_perf_tests wds_split_indicator_dump_tests
      wds_export_video_dialog_tests
      PROPERTIES ENVIRONMENT "PATH=${_wds_test_path}")
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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET wds_editor)
  # add_executable() does not perform Qt's finalization automatically. Collect
  # platform/image plugins after all linked targets exist for Linux deployment.
  qt_finalize_target(wds_editor)
endif()

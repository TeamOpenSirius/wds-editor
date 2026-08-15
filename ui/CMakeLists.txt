# UI shell: outer layouter + four regions (preview / settings / toolbar / edit).

add_library(wds_ui STATIC
  src/ui_manager.cpp
  src/window.cpp
  src/resource_paths.cpp
  src/startup_deps.cpp
  src/editor_session.cpp
  src/editor_ui_config.cpp
  src/native_file_dialog.cpp
  src/layout/editor_layout.cpp
  src/regions/preview/chart_preview_panel.cpp
  src/regions/preview/playback_preview.cpp
  src/regions/edit/chart_edit_renderer.cpp
  src/regions/edit/chart_edit_panel.cpp
  src/regions/edit/edit_gutters.cpp
  src/regions/settings/preview_settings_panel.cpp
  src/regions/settings/width_slots_dialog.cpp
  src/regions/settings/export_choice_dialog.cpp
  src/regions/settings/chart_add_dialog.cpp
  src/regions/settings/unsaved_changes_dialog.cpp
  src/regions/status/status_bar.cpp
  src/regions/toolbar/editor_toolbar.cpp
)
if(APPLE)
  target_sources(wds_ui PRIVATE src/macos_menu.mm src/macos_file_dialog.mm)
endif()
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
)
if(TARGET wds_interaction_glfw)
  target_link_libraries(wds_ui PUBLIC wds::interaction_glfw)
endif()

# Portable layouts still need the BASS dylib next to the binary (probed via wds::audio).
if(TARGET wds_bass)
  target_link_libraries(wds_ui PRIVATE wds_bass)
endif()
if(TARGET wds_bassmix)
  target_link_libraries(wds_ui PRIVATE wds_bassmix)
endif()
if(APPLE)
  target_link_libraries(wds_ui PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers")
elseif(WIN32)
  # IFileOpenDialog / IFileSaveDialog (native_file_dialog.cpp)
  target_link_libraries(wds_ui PUBLIC ole32 uuid)
elseif(UNIX)
  if(CMAKE_DL_LIBS)
    target_link_libraries(wds_ui PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

target_compile_features(wds_ui PUBLIC cxx_std_17)
target_compile_definitions(wds_ui PRIVATE WDS_REPO_ROOT="${CMAKE_SOURCE_DIR}")

add_executable(wds_ui_logic_tests tests/edit_viewport_tests.cpp)
target_link_libraries(wds_ui_logic_tests PRIVATE wds::core wds::interaction)
target_include_directories(wds_ui_logic_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME wds_ui_logic_tests COMMAND wds_ui_logic_tests)

add_executable(wds_note_draw_order_tests tests/note_draw_order_tests.cpp)
target_link_libraries(wds_note_draw_order_tests PRIVATE wds::chart_render wds::core)
add_test(NAME wds_note_draw_order_tests COMMAND wds_note_draw_order_tests)

if(MSVC)
  target_compile_options(wds_ui PRIVATE /W4)
else()
  target_compile_options(wds_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- wds_editor executable ---------------------------------------------------

set(WDS_UI_GLFW_TARGET "")
find_package(glfw3 CONFIG QUIET)
if(TARGET glfw)
  set(WDS_UI_GLFW_TARGET glfw)
elseif(TARGET glfw3)
  set(WDS_UI_GLFW_TARGET glfw3)
else()
  find_library(WDS_UI_GLFW_LIB NAMES glfw3 glfw)
endif()

if(WDS_UI_GLFW_TARGET)
  target_link_libraries(wds_ui PRIVATE ${WDS_UI_GLFW_TARGET})
elseif(WDS_UI_GLFW_LIB)
  find_path(WDS_UI_GLFW_INCLUDE_DIR GLFW/glfw3.h)
  if(WDS_UI_GLFW_INCLUDE_DIR)
    target_include_directories(wds_ui PRIVATE ${WDS_UI_GLFW_INCLUDE_DIR})
  endif()
  target_link_libraries(wds_ui PRIVATE ${WDS_UI_GLFW_LIB})
endif()

if(NOT WDS_UI_GLFW_TARGET AND NOT WDS_UI_GLFW_LIB)
  message(STATUS "wds_editor skipped: libglfw not found")
  return()
endif()

add_executable(wds_editor apps/wds_editor.cpp)
set(_wds_app_icon_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/app_icon")
set(_wds_app_icon_ico "${_wds_app_icon_dir}/wds.ico")
set(_wds_app_icon_png "${_wds_app_icon_dir}/wds.png")
set(_wds_app_icon_icns "${_wds_app_icon_dir}/wds.icns")

if(WIN32)
  # Embed DPI-awareness manifest + application icon (MinGW windres / MSVC rc).
  enable_language(RC)
  set(_wds_editor_manifest "${CMAKE_CURRENT_SOURCE_DIR}/apps/wds_editor.manifest")
  set(_wds_editor_rc "${CMAKE_CURRENT_BINARY_DIR}/wds_editor.rc")
  # Escape backslashes for windres on Windows hosts; forward slashes work for MinGW.
  file(TO_CMAKE_PATH "${_wds_editor_manifest}" _wds_manifest_rc_path)
  file(TO_CMAKE_PATH "${_wds_app_icon_ico}" _wds_ico_rc_path)
  file(WRITE "${_wds_editor_rc}"
    "1 24 \"${_wds_manifest_rc_path}\"\n"
    "IDI_ICON1 ICON \"${_wds_ico_rc_path}\"\n")
  target_sources(wds_editor PRIVATE "${_wds_editor_rc}")
endif()
target_include_directories(wds_editor PRIVATE ${CMAKE_SOURCE_DIR}/renderer/third_party)
target_link_libraries(wds_editor PRIVATE wds::ui wds::renderer wds::common)
if(WDS_UI_GLFW_TARGET)
  target_link_libraries(wds_editor PRIVATE ${WDS_UI_GLFW_TARGET})
else()
  target_link_libraries(wds_editor PRIVATE ${WDS_UI_GLFW_LIB})
endif()

if(WIN32)
  target_link_libraries(wds_editor PRIVATE gdi32 user32 shell32 imm32)
  # Hide the console MinGW attaches to console-subsystem binaries.
  set_target_properties(wds_editor PROPERTIES WIN32_EXECUTABLE TRUE)

  # MSI helper: Uninstall.exe (UpgradeCode must match scripts/win-msi-product.wxs).
  add_executable(wds_uninstall WIN32 apps/wds_uninstall.cpp)
  target_link_libraries(wds_uninstall PRIVATE msi shell32 user32 ole32)
  set_target_properties(wds_uninstall PROPERTIES
    OUTPUT_NAME "Uninstall"
    WIN32_EXECUTABLE TRUE)
  if(NOT MSVC)
    # Keep MinGW runtime static; msi.dll remains a system import.
    # -municode selects wWinMain (Unicode) instead of ANSI WinMain.
    target_link_options(wds_uninstall PRIVATE -static-libgcc -static-libstdc++ -municode)
  endif()
else()
  find_package(Threads QUIET)
  if(Threads_FOUND)
    target_link_libraries(wds_editor PRIVATE Threads::Threads)
  endif()
  if(CMAKE_DL_LIBS)
    target_link_libraries(wds_editor PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

target_compile_definitions(wds_editor PRIVATE WDS_REPO_ROOT="${CMAKE_SOURCE_DIR}")

if(APPLE)
  set(_wds_editor_build_rpath "@loader_path;${WDS_BASS_LIBRARY_DIR}")
  set(_wds_editor_install_rpath "@loader_path;@loader_path/../lib")
elseif(WIN32)
  set(_wds_editor_build_rpath "")
  set(_wds_editor_install_rpath "")
else()
  set(_wds_editor_build_rpath "${WDS_BASS_LIBRARY_DIR}")
  set(_wds_editor_install_rpath "$ORIGIN;$ORIGIN/../lib")
endif()

set_target_properties(wds_editor PROPERTIES
  BUILD_RPATH "${_wds_editor_build_rpath}"
  INSTALL_RPATH "${_wds_editor_install_rpath}"
  BUILD_WITH_INSTALL_RPATH FALSE
)

if(APPLE)
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WDS_BASS_LIBRARY}" "$<TARGET_FILE_DIR:wds_editor>/libbass.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WDS_BASSMIX_LIBRARY}" "$<TARGET_FILE_DIR:wds_editor>/libbassmix.dylib"
    COMMENT "Copy libbass.dylib and libbassmix.dylib next to wds_editor"
  )
elseif(WIN32)
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WDS_BASS_LIBRARY}" "$<TARGET_FILE_DIR:wds_editor>/bass.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WDS_BASSMIX_LIBRARY}" "$<TARGET_FILE_DIR:wds_editor>/bassmix.dll"
    COMMENT "Copy bass.dll and bassmix.dll next to wds_editor"
  )
else()
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WDS_BASS_LIBRARY}" "$<TARGET_FILE_DIR:wds_editor>/libbass.so"
    COMMENT "Copy libbass.so next to wds_editor"
  )
  target_link_options(wds_editor PRIVATE "-Wl,-rpath,$ORIGIN")
endif()

# Ship the bundled CJK UI font next to the binary for portable runs.
set(_wds_ui_font "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansSC-Regular.ttf")
if(EXISTS "${_wds_ui_font}")
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:wds_editor>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_wds_ui_font}" "$<TARGET_FILE_DIR:wds_editor>/fonts/NotoSansSC-Regular.ttf"
    COMMENT "Copy bundled UI font next to wds_editor"
  )
endif()

# Ship toolbar SVG icons next to the binary.
set(_wds_icons_dir "${CMAKE_SOURCE_DIR}/icons")
if(EXISTS "${_wds_icons_dir}/open.svg")
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:wds_editor>/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${_wds_icons_dir}" "$<TARGET_FILE_DIR:wds_editor>/icons"
    COMMENT "Copy toolbar SVG icons next to wds_editor"
  )
endif()

# Ship app icon PNG next to the binary (GLFW / Linux .desktop; macOS uses .icns in packaging).
if(EXISTS "${_wds_app_icon_png}")
  add_custom_command(TARGET wds_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_wds_app_icon_png}" "$<TARGET_FILE_DIR:wds_editor>/wds.png"
    COMMENT "Copy app icon PNG next to wds_editor"
  )
endif()

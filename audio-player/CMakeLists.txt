# Audio playback and transport (BASS). Depends on wds::common only.

add_library(wds_audio_player STATIC
  src/audio_engine.cpp
  src/transport.cpp
  src/hit_sfx.cpp
)
add_library(wds::audio_player ALIAS wds_audio_player)

target_include_directories(wds_audio_player
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bass/include
)

target_link_libraries(wds_audio_player
  PUBLIC
    wds::common
)

# --- BASS (vendored per-target binaries) ---
set(WDS_BASS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bass")
set(WDS_BASS_INCLUDE "${WDS_BASS_ROOT}/include")
if(NOT EXISTS "${WDS_BASS_INCLUDE}/bass.h")
  message(FATAL_ERROR "BASS header missing: ${WDS_BASS_INCLUDE}/bass.h")
endif()

if(APPLE)
  set(WDS_BASS_LIB_DIR "${WDS_BASS_ROOT}/macos-arm")
  set(WDS_BASS_LIB "${WDS_BASS_LIB_DIR}/libbass.dylib")
elseif(WIN32)
  set(WDS_BASS_LIB_DIR "${WDS_BASS_ROOT}/win-x86_64")
  set(WDS_BASS_LIB "${WDS_BASS_LIB_DIR}/bass.dll")
  set(WDS_BASS_IMPLIB "${WDS_BASS_LIB_DIR}/bass.lib")
else()
  message(FATAL_ERROR
    "Linux is not a supported product target (no BASS linux binary). "
    "On Linux hosts, cross-compile win-x86_64 via cmake/toolchains/win-x86_64.cmake.")
endif()

if(NOT EXISTS "${WDS_BASS_LIB}")
  message(FATAL_ERROR "BASS library missing for this target: ${WDS_BASS_LIB}")
endif()

add_library(wds_bass SHARED IMPORTED GLOBAL)
if(WIN32)
  set_target_properties(wds_bass PROPERTIES
    IMPORTED_LOCATION "${WDS_BASS_LIB}"
    IMPORTED_IMPLIB "${WDS_BASS_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WDS_BASS_INCLUDE}"
  )
else()
  set_target_properties(wds_bass PROPERTIES
    IMPORTED_LOCATION "${WDS_BASS_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WDS_BASS_INCLUDE}"
  )
endif()

target_link_libraries(wds_audio_player PRIVATE wds_bass)
message(STATUS "BASS: ${WDS_BASS_LIB}")

target_compile_features(wds_audio_player PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(wds_audio_player PRIVATE /W4)
else()
  target_compile_options(wds_audio_player PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Expose BASS paths for consumers that bundle runtime libs (e.g. wds_editor).
set(WDS_BASS_LIBRARY "${WDS_BASS_LIB}" CACHE INTERNAL "BASS shared library path")
set(WDS_BASS_LIBRARY_DIR "${WDS_BASS_LIB_DIR}" CACHE INTERNAL "BASS library directory")

option(WDS_AUDIO_BUILD_TESTS "Build audio-player unit tests" ON)
if(WDS_AUDIO_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(wds_audio_player_tests tests/transport_tests.cpp)
  target_link_libraries(wds_audio_player_tests PRIVATE wds::audio_player)
  if(MSVC)
    target_compile_options(wds_audio_player_tests PRIVATE /W4 /utf-8)
  else()
    target_compile_options(wds_audio_player_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME wds_audio_player_tests COMMAND wds_audio_player_tests)
endif()

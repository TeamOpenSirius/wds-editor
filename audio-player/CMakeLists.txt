# Audio playback and transport (BASS). Depends on wds::common only.

add_library(wds_audio_player STATIC
  src/audio_engine.cpp
  src/transport.cpp
  src/hit_sfx.cpp
  src/waveform_overview.cpp
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

if(NOT EXISTS "${WDS_BASS_INCLUDE}/bassmix.h")
  message(FATAL_ERROR "BASSmix header missing: ${WDS_BASS_INCLUDE}/bassmix.h")
endif()

if(APPLE)
  set(WDS_BASS_LIB_DIR "${WDS_BASS_ROOT}/macos-arm")
  set(WDS_BASS_LIB "${WDS_BASS_LIB_DIR}/libbass.dylib")
  set(WDS_BASSMIX_LIB "${WDS_BASS_LIB_DIR}/libbassmix.dylib")
elseif(WIN32)
  set(WDS_BASS_LIB_DIR "${WDS_BASS_ROOT}/win-x86_64")
  set(WDS_BASS_LIB "${WDS_BASS_LIB_DIR}/bass.dll")
  set(WDS_BASS_IMPLIB "${WDS_BASS_LIB_DIR}/bass.lib")
  set(WDS_BASSMIX_LIB "${WDS_BASS_LIB_DIR}/bassmix.dll")
  set(WDS_BASSMIX_IMPLIB "${WDS_BASS_LIB_DIR}/bassmix.lib")
else()
  message(FATAL_ERROR
    "Linux is not a supported product target (no BASS linux binary). "
    "On Linux hosts, cross-compile win-x86_64 via cmake/toolchains/win-x86_64.cmake.")
endif()

if(NOT EXISTS "${WDS_BASS_LIB}")
  message(FATAL_ERROR "BASS library missing for this target: ${WDS_BASS_LIB}")
endif()
if(NOT EXISTS "${WDS_BASSMIX_LIB}")
  message(FATAL_ERROR "BASSmix library missing for this target: ${WDS_BASSMIX_LIB}")
endif()
if(WIN32 AND NOT EXISTS "${WDS_BASSMIX_IMPLIB}")
  message(FATAL_ERROR "BASSmix import lib missing: ${WDS_BASSMIX_IMPLIB}")
endif()

add_library(wds_bass SHARED IMPORTED GLOBAL)
add_library(wds_bassmix SHARED IMPORTED GLOBAL)
if(WIN32)
  set_target_properties(wds_bass PROPERTIES
    IMPORTED_LOCATION "${WDS_BASS_LIB}"
    IMPORTED_IMPLIB "${WDS_BASS_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WDS_BASS_INCLUDE}"
  )
  set_target_properties(wds_bassmix PROPERTIES
    IMPORTED_LOCATION "${WDS_BASSMIX_LIB}"
    IMPORTED_IMPLIB "${WDS_BASSMIX_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WDS_BASS_INCLUDE}"
  )
else()
  set_target_properties(wds_bass PROPERTIES
    IMPORTED_LOCATION "${WDS_BASS_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WDS_BASS_INCLUDE}"
  )
  set_target_properties(wds_bassmix PROPERTIES
    IMPORTED_LOCATION "${WDS_BASSMIX_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${WDS_BASS_INCLUDE}"
  )
endif()

target_link_libraries(wds_audio_player PRIVATE wds_bass wds_bassmix)
message(STATUS "BASS: ${WDS_BASS_LIB}")
message(STATUS "BASSmix: ${WDS_BASSMIX_LIB}")

target_compile_features(wds_audio_player PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(wds_audio_player PRIVATE /W4 /utf-8)
  target_compile_definitions(wds_audio_player PRIVATE NOMINMAX)
else()
  target_compile_options(wds_audio_player PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Expose BASS paths for consumers that bundle runtime libs (e.g. wds_editor).
set(WDS_BASS_LIBRARY "${WDS_BASS_LIB}" CACHE INTERNAL "BASS shared library path")
set(WDS_BASS_LIBRARY_DIR "${WDS_BASS_LIB_DIR}" CACHE INTERNAL "BASS library directory")
set(WDS_BASSMIX_LIBRARY "${WDS_BASSMIX_LIB}" CACHE INTERNAL "BASSmix shared library path")

option(WDS_AUDIO_BUILD_TESTS "Build audio-player unit tests" ON)
if(WDS_AUDIO_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(wds_audio_player_tests tests/transport_tests.cpp)
  target_link_libraries(wds_audio_player_tests PRIVATE wds::audio_player)
  if(TARGET wds_bass)
    target_link_libraries(wds_audio_player_tests PRIVATE wds_bass)
  endif()
  if(TARGET wds_bassmix)
    target_link_libraries(wds_audio_player_tests PRIVATE wds_bassmix)
  endif()
  if(MSVC)
    target_compile_options(wds_audio_player_tests PRIVATE /W4 /utf-8)
    target_compile_definitions(wds_audio_player_tests PRIVATE NOMINMAX)
  else()
    target_compile_options(wds_audio_player_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(APPLE)
    set_target_properties(wds_audio_player_tests PROPERTIES
      BUILD_RPATH "${WDS_BASS_LIBRARY_DIR}"
    )
  endif()
  add_test(NAME wds_audio_player_tests COMMAND wds_audio_player_tests)
  if(WIN32)
    set_tests_properties(wds_audio_player_tests PROPERTIES
      ENVIRONMENT "PATH=${WDS_BASS_LIB_DIR};$ENV{PATH}")
  endif()
endif()

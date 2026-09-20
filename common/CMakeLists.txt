# Shared primitives: timeline, logging, crash interception.

add_library(wds_common STATIC
  src/app_version.cpp
  src/crash_handler.cpp
  src/crash_input_journal.cpp
  src/log.cpp
  src/utf8_path.cpp
)
add_library(wds::common ALIAS wds_common)

target_include_directories(wds_common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(wds_common PUBLIC cxx_std_17)

if(NOT WDS_APP_VERSION)
  set(WDS_APP_VERSION "${PROJECT_VERSION}")
endif()
set(WDS_GIT_HASH "unknown")
find_package(Git QUIET)
if(Git_FOUND)
  get_filename_component(_wds_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY "${_wds_repo_root}"
    OUTPUT_VARIABLE WDS_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _wds_git_hash_rc
  )
  if(NOT _wds_git_hash_rc EQUAL 0 OR WDS_GIT_HASH STREQUAL "")
    set(WDS_GIT_HASH "unknown")
  endif()
endif()

if(CMAKE_BUILD_TYPE)
  set(_wds_build_type "${CMAKE_BUILD_TYPE}")
else()
  set(_wds_build_type "unknown")
endif()

target_compile_definitions(wds_common PRIVATE
  WDS_APP_VERSION="${WDS_APP_VERSION}"
  WDS_GIT_HASH="${WDS_GIT_HASH}"
  WDS_BUILD_TYPE="${_wds_build_type}"
)

if(WIN32)
  # MiniDumpWriteDump / StackWalk64 are resolved from dbghelp at install time.
  target_link_libraries(wds_common PUBLIC kernel32 user32)
elseif(APPLE)
  # backtrace() / dladdr are in libSystem.
else()
  target_link_libraries(wds_common PUBLIC ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_compile_options(wds_common PRIVATE /W4 /utf-8)
else()
  target_compile_options(wds_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(WDS_COMMON_BUILD_TESTS "Build common unit tests" ON)
if(WDS_COMMON_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(wds_common_tests tests/common_tests.cpp)
  target_link_libraries(wds_common_tests PRIVATE wds::common)
  if(MSVC)
    target_compile_options(wds_common_tests PRIVATE /W4 /utf-8)
  else()
    target_compile_options(wds_common_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME wds_common_tests COMMAND wds_common_tests)

  add_executable(wds_crash_handler_tests tests/crash_handler_tests.cpp)
  target_link_libraries(wds_crash_handler_tests PRIVATE wds::common)
  if(MSVC)
    target_compile_options(wds_crash_handler_tests PRIVATE /W4 /utf-8)
  else()
    target_compile_options(wds_crash_handler_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME wds_crash_handler_tests COMMAND wds_crash_handler_tests)
endif()

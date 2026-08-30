# Shared primitives: timeline, logging, crash interception.

add_library(wds_common STATIC
  src/crash_handler.cpp
  src/debug_session_log.cpp
  src/utf8_path.cpp
)
add_library(wds::common ALIAS wds_common)

target_include_directories(wds_common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(wds_common PUBLIC cxx_std_17)

if(WIN32)
  # CaptureStackBackTrace lives in kernel32 (pulled transitively); no dbghelp needed.
  target_link_libraries(wds_common PUBLIC kernel32 user32)
elseif(APPLE)
  # backtrace() is in libSystem.
else()
  # glibc backtrace(); keep default link line.
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
endif()

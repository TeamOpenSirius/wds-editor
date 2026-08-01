#pragma once

// Compile-time logging switch (set by CMake):
//   Debug   -> WDS_ENABLE_LOGGING=1  (verbose stderr)
//   Release -> WDS_ENABLE_LOGGING=0  (silent)
#ifndef WDS_ENABLE_LOGGING
#  if defined(NDEBUG)
#    define WDS_ENABLE_LOGGING 0
#  else
#    define WDS_ENABLE_LOGGING 1
#  endif
#endif

#if WDS_ENABLE_LOGGING
#  include <cstdio>
#  define WDS_LOG(...) ::std::fprintf(stderr, "[wds] " __VA_ARGS__)
#  define WDS_LOG_IF(cond, ...) \
    do {                       \
      if (cond) {              \
        WDS_LOG(__VA_ARGS__);  \
      }                        \
    } while (0)
#else
#  define WDS_LOG(...) ((void)0)
#  define WDS_LOG_IF(cond, ...) ((void)0)
#endif

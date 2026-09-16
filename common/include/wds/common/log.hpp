#pragma once

#include "wds/common/crash_input_journal.hpp"

#include <cstdio>

// Compile-time stderr switch (set by CMake):
//   Debug   -> WDS_ENABLE_LOGGING=1  (verbose stderr)
//   Release -> WDS_ENABLE_LOGGING=0  (silent stderr)
// The crash log ring is always updated, including when stderr is silent.
#ifndef WDS_ENABLE_LOGGING
#  if defined(NDEBUG)
#    define WDS_ENABLE_LOGGING 0
#  else
#    define WDS_ENABLE_LOGGING 1
#  endif
#endif

namespace wds::common {

// printf-style append into a 256 x 200-byte ring. Slot claim is atomic
// (fetch_add on the head); concurrent writers each own a slot. A crash
// handler reader may skip a slot that is still being written.
void log_ring_append(const char* fmt, ...);

// Async-signal-safe dump of stored strings (no formatting).
void log_ring_write_text(JournalEmitFn emit, void* ctx);

}  // namespace wds::common

#define WDS_LOG(...)                                                          \
  do {                                                                        \
    ::wds::common::log_ring_append(__VA_ARGS__);                              \
    if (WDS_ENABLE_LOGGING) {                                                 \
      ::std::fprintf(stderr, "[wds] " __VA_ARGS__);                           \
    }                                                                         \
  } while (0)

#define WDS_LOG_IF(cond, ...) \
  do {                        \
    if (cond) {               \
      WDS_LOG(__VA_ARGS__);   \
    }                         \
  } while (0)

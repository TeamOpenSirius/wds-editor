#pragma once

#include "wds/common/crash_input_journal.hpp"

#include <cstdio>

// Compile-time verbose switch (set by CMake):
//   Debug   -> WDS_ENABLE_LOGGING=1  (session file; stderr prints the path once)
//   Release -> WDS_ENABLE_LOGGING=0  (ring only)
// The crash log ring is always updated, including when the file sink is off.
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

// Debug-only: create logs/debug-<stamp>-<pid>.log next to crash reports and
// print that path once on stderr. No-op when WDS_ENABLE_LOGGING is 0. Call
// from the editor main after install_crash_handlers(); tests should not.
void log_open_debug_session();

// Absolute UTF-8 path of the open debug session file, or empty if none.
const char* log_debug_session_path();

// Debug emit: session file when open, otherwise stderr. No-op in Release.
void log_emit(const char* fmt, ...);

}  // namespace wds::common

#define WDS_LOG(...)                                                          \
  do {                                                                        \
    ::wds::common::log_ring_append(__VA_ARGS__);                              \
    if (WDS_ENABLE_LOGGING) {                                                 \
      ::wds::common::log_emit(__VA_ARGS__);                                   \
    }                                                                         \
  } while (0)

#define WDS_LOG_IF(cond, ...) \
  do {                        \
    if (cond) {               \
      WDS_LOG(__VA_ARGS__);   \
    }                         \
  } while (0)

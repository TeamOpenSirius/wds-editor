#pragma once

#include <cstddef>
#include <cstdint>

namespace wds::common {
// Field diagnostics: an always-on text log written next to the crash reports
// (<crash dir>/diag-<stamp>-<pid>.log). Unlike WDS_LOG it is compiled into
// Release builds and enabled by default, because it exists for reports that
// only reproduce on a user's machine. Set WDS_DIAG=0 to opt out, or
// WDS_DIAG_PATH=<file> to redirect.
//
// Line-oriented and flushed per line so a crash or a killed process still
// leaves the tail readable; the sink stops after kWDSDiagCapBytes so a stuck
// scene cannot fill the disk.
namespace diag {

inline constexpr std::size_t kWDSDiagCapBytes = 32u * 1024u * 1024u;

// Opens the session file once. Returns false when disabled or unopenable
// (callers keep running; emit() then becomes a no-op except for banners).
bool open(const char* tag);
void close();
bool enabled();
// Absolute UTF-8 path of the open session file, or "" when none is open.
const char* path();

// Version / git hash / build type banner (macros live in wds_common).
void emit_build_banner();

// printf-style append with a monotonic "[diag +<ms>]" prefix. Thread-safe.
void emit(const char* fmt, ...);
// Same line, mirrored to stderr (startup banners only).
void banner(const char* fmt, ...);
// At most one line per `interval_ms` for `key` ("split.frame", ...). The key
// string must outlive the call (string literals everywhere).
void emit_throttled(const char* key, int interval_ms, const char* fmt, ...);

}  // namespace diag
}  // namespace wds::common

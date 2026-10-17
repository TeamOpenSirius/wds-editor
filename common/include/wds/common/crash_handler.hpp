#pragma once

// Process-wide crash / fatal-error interception.
// Call install_crash_handlers() once at the very start of main().
// Worker threads that may crash should call install_thread_crash_stack()
// at thread start (std::async / QtConcurrent) so SIGSEGV can run on an
// alternate stack.
//
// Crash reports live under the OS data dir (override with WDS_CRASH_LOG_DIR):
//   Windows: %LOCALAPPDATA%\WDS\logs
//   macOS:   ~/Library/Application Support/WDS/logs/
//   Linux:   $XDG_STATE_HOME/WDS/logs or ~/.local/share/WDS/logs/
//
// At install a session file crash-<YYYYMMDD-HHMMSS>-<pid>.txt is pre-created
// with a single "WDS Editor session start" line. The handler only write()s
// to that fd. mark_clean_exit() deletes the sentinel and, if the session
// file still contains only that header, deletes the file too (so a leftover
// header-only file is not treated as a crash). previous_session_crashed()
// looks for a stale running.sentinel whose pid is no longer alive.
//
// Set WDS_CRASH_NO_DIALOG=1 to suppress the macOS/Linux non-signal dialog
// (required by automated tests). Windows never shows a MessageBox from
// this library (next-launch UI reads the sentinel).

#include <cstddef>

namespace wds::common {

// Best-effort breadcrumbs for the next crash report. Each field is a
// 512-byte static buffer. Writers are expected on the main thread; the
// crash handler only reads. A concurrent write is a benign race (the
// handler may see a torn or truncated string).
enum class CrashContextField {
  ProjectPath = 0,
  MusicPath,
  VulkanDevice,
  AudioBackend,
  Custom0,
  Custom1,
};

// Install POSIX sigaction handlers (alt stack, SA_SIGINFO), Windows UEF /
// VEH / SIGABRT / CRT hooks, and std::terminate. Pre-opens the session
// crash file and writes running.sentinel. Safe to call once.
void install_crash_handlers();

// POSIX: per-thread sigaltstack using a 64 KB thread_local buffer.
// Windows: reserves extra stack on the calling thread (SetThreadStackGuarantee)
// so the unhandled-exception filter can run after a stack overflow.
// Idempotent on the calling thread.
void install_thread_crash_stack();

// Log + (non-Windows) dialog for recoverable entry-point failures.
// Returns after the user dismisses the dialog (or immediately if dialogs
// are suppressed).
void report_fatal(const char* kind, const char* detail);

// Test-only: hook run inside report_fatal() while the crash lock is still held
// (after the report/dialog, before release). Lets tests hold the lock open to
// verify that a crash on another thread waits and is then reported.
using CrashTestHook = void (*)();
namespace testing {
void set_report_fatal_hook(CrashTestHook hook);

// POSIX only (no-ops on Windows). The hook runs inside the crash report's
// symbolization window (alarm() watchdog armed), right before the
// non-async-signal-safe dladdr / backtrace_symbols_fd work. A hook that never
// returns simulates a loader-lock deadlock. The timeout is in whole seconds
// (default 3); 0 disables the watchdog.
void set_symbolize_hook(CrashTestHook hook);
void set_symbolize_timeout_seconds(unsigned seconds);
}

void crash_set_context(CrashContextField field, const char* utf8);

// Delete running.sentinel. If the session crash file is still header-only,
// delete it as well.
void mark_clean_exit();

// True if a previous process left a stale sentinel (pid not alive).
// Copies that session's crash path (or the newest crash-*.txt) into
// crash_path_out when possible. Safe to call before or after install;
// after install this returns the result cached before the new sentinel
// was written.
bool previous_session_crashed(char* crash_path_out, std::size_t cap);

// Resolved log directory (valid after install, or after a probe that
// resolved the path). Never null; may be empty if resolution failed.
const char* crash_log_directory();

}  // namespace wds::common

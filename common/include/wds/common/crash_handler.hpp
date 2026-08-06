#pragma once

// Process-wide crash / fatal-error interception.
// Call install_crash_handlers() once at the very start of main() (before GLFW).

namespace wds::common {

// Install POSIX signal handlers, Windows unhandled-exception filter, and
// std::terminate. Crash reports are written under the OS data dir:
//   Windows: %LOCALAPPDATA%\WDS\logs\
//   macOS:   ~/Library/Application Support/WDS/logs/
//   Linux:   $XDG_STATE_HOME/WDS/logs or ~/.local/share/WDS/logs/
// Native error dialogs are shown for non-signal fatals only.
// POSIX signal handlers use only async-signal-safe calls (prebuilt log path,
// write/open/close/backtrace_symbols_fd); no dialogs or heap allocation.
void install_crash_handlers();

// Log + dialog for recoverable entry-point failures (uncaught C++ exceptions).
// Returns after the user dismisses the dialog.
void report_fatal(const char* kind, const char* detail);

}  // namespace wds::common

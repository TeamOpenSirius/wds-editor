#pragma once

#include <cstddef>

namespace wds::common {

// Short OS string for crash headers and startup logs
// (Windows major.minor.build / POSIX uname).
void format_os_version(char* out, std::size_t cap);

// Dump process / OS / CPU / memory / relevant env vars via WDS_LOG.
// argv0 is optional and only used as a fallback for the executable directory.
void log_runtime_environment(const char* argv0 = nullptr);

}  // namespace wds::common

#include "wds/common/debug_session_log.hpp"

#include "wds/common/utf8_path.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wds::common {
namespace {

constexpr const char* kSessionId = "3aea3b";
constexpr const char* kWorkspaceLog =
    "/Users/ginojin/Projects/wds-editor/.cursor/debug-3aea3b.log";

FILE* g_workspace = nullptr;
FILE* g_user = nullptr;
std::once_flag g_once;

void open_sinks() {
  std::error_code ec;
  {
    const auto ws = path_from_utf8(kWorkspaceLog);
    std::filesystem::create_directories(ws.parent_path(), ec);
    g_workspace = fopen_utf8(kWorkspaceLog, "a");
  }

  std::filesystem::path user;
#if defined(_WIN32)
  char base[1024] = {};
  const DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
  if (n > 0 && n < sizeof(base)) {
    user = std::filesystem::path(base) / "WDS" / "logs" / "debug-3aea3b.ndjson";
  } else {
    user = std::filesystem::path("debug-3aea3b.ndjson");
  }
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    user = std::filesystem::path(home) / "Library" / "Application Support" / "WDS" / "logs" /
           "debug-3aea3b.ndjson";
  }
#else
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && xdg[0] != '\0') {
    user = std::filesystem::path(xdg) / "WDS" / "logs" / "debug-3aea3b.ndjson";
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    user = std::filesystem::path(home) / ".local" / "share" / "WDS" / "logs" /
           "debug-3aea3b.ndjson";
  }
#endif
  if (!user.empty()) {
    std::filesystem::create_directories(user.parent_path(), ec);
    g_user = fopen_utf8(path_to_utf8(user), "a");
  }
}

void write_line(const char* line, int len) {
  std::call_once(g_once, open_sinks);
  auto wr = [&](FILE* fp) {
    if (fp == nullptr || line == nullptr || len <= 0) {
      return;
    }
    std::fwrite(line, 1, static_cast<std::size_t>(len), fp);
    std::fflush(fp);
  };
  wr(g_workspace);
  if (g_user != g_workspace) {
    wr(g_user);
  }
}

}  // namespace

void debug_session_log(const char* location, const char* message, const char* hypothesis_id,
                       const char* data_json) {
  // #region agent log
  const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  char buf[4096];
  const int n = std::snprintf(
      buf, sizeof(buf),
      "{\"sessionId\":\"%s\",\"timestamp\":%lld,\"location\":\"%s\",\"message\":\"%s\","
      "\"hypothesisId\":\"%s\",\"data\":%s}\n",
      kSessionId, static_cast<long long>(ts), location != nullptr ? location : "",
      message != nullptr ? message : "", hypothesis_id != nullptr ? hypothesis_id : "",
      data_json != nullptr ? data_json : "{}");
  if (n > 0) {
    write_line(buf, n < static_cast<int>(sizeof(buf)) ? n : static_cast<int>(sizeof(buf)) - 1);
  }
  // #endregion
}

}  // namespace wds::common

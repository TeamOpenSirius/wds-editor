#include "wds/common/diag_log.hpp"

#include "wds/common/crash_handler.hpp"
#include "wds/common/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef WDS_APP_VERSION
#define WDS_APP_VERSION "unknown"
#endif
#ifndef WDS_GIT_HASH
#define WDS_GIT_HASH "unknown"
#endif
#ifndef WDS_BUILD_TYPE
#define WDS_BUILD_TYPE "unknown"
#endif

namespace wds::common {
namespace diag {
namespace {

// Keep in sync with log.cpp: one thread owns the sink, lines are short.
constexpr std::size_t kEmitBytes = 2048;
constexpr std::size_t kPathCap = 1024;
constexpr int kThrottleSlots = 16;

std::mutex g_mu;
FILE* g_file = nullptr;
char g_path[kPathCap] = {};
std::size_t g_bytes = 0;
bool g_truncated = false;
bool g_closed = false;
std::chrono::steady_clock::time_point g_origin = std::chrono::steady_clock::now();

struct ThrottleSlot {
  const char* key = nullptr;
  long long last_ms = 0;
};
std::mutex g_throttle_mu;
ThrottleSlot g_throttle[kThrottleSlots];

void format_stamp(char* out, std::size_t cap) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  std::snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

long long elapsed_ms() {
  return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - g_origin)
                                    .count());
}

void write_locked(const char* body, std::size_t n) {
  if (g_file == nullptr || n == 0) {
    return;
  }
  if (g_bytes + n > kWDSDiagCapBytes) {
    if (!g_truncated) {
      g_truncated = true;
      const char tail[] = "[diag] log cap reached; further lines are dropped\n";
      std::fwrite(tail, 1, sizeof(tail) - 1, g_file);
      std::fflush(g_file);
    }
    return;
  }
  g_bytes += std::fwrite(body, 1, n, g_file);
  std::fflush(g_file);
}

bool env_disabled() {
  const char* v = std::getenv("WDS_DIAG");
  return v != nullptr && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' ||
                          v[0] == 'F');
}

}  // namespace

bool open(const char* tag) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_file != nullptr) {
    return true;
  }
  if (g_closed || env_disabled()) {
    return false;
  }
  if (g_path[0] == '\0') {
    char stamp[32] = {};
    format_stamp(stamp, sizeof(stamp));
    if (const char* forced = std::getenv("WDS_DIAG_PATH"); forced != nullptr && forced[0] != '\0') {
      std::snprintf(g_path, sizeof(g_path), "%s", forced);
    } else {
      const char* dir = crash_log_directory();
      if (dir == nullptr || dir[0] == '\0') {
        std::fprintf(stderr, "[wds] diag log: no log dir; diagnostics disabled\n");
        g_closed = true;
        return false;
      }
#if defined(_WIN32)
      const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
      std::snprintf(g_path, sizeof(g_path), "%s\\diag-%s-%s-%lu.log", dir,
                    tag != nullptr ? tag : "wds", stamp, pid);
#else
      const long pid = static_cast<long>(::getpid());
      std::snprintf(g_path, sizeof(g_path), "%s/diag-%s-%s-%ld.log", dir,
                    tag != nullptr ? tag : "wds", stamp, pid);
#endif
    }
  }
  FILE* fp = fopen_utf8(g_path, "w");
  if (fp == nullptr) {
    std::fprintf(stderr, "[wds] diag log: failed to open %s\n", g_path);
    g_path[0] = '\0';
    g_closed = true;
    return false;
  }
  g_file = fp;
  g_bytes = 0;
  g_truncated = false;
  g_origin = std::chrono::steady_clock::now();
  std::fprintf(stderr, "[wds] diag log: %s\n", g_path);
  char header[kEmitBytes];
  const int n = std::snprintf(header, sizeof(header),
                              "[diag +0ms] session open tag=%s pid=%ld\n",
                              tag != nullptr ? tag : "wds",
#if defined(_WIN32)
                              static_cast<long>(::GetCurrentProcessId())
#else
                              static_cast<long>(::getpid())
#endif
  );
  if (n > 0) {
    write_locked(header, static_cast<std::size_t>(n));
  }
  return true;
}

void emit_build_banner() {
  banner("build version=%s git=%s type=%s", WDS_APP_VERSION, WDS_GIT_HASH, WDS_BUILD_TYPE);
}

void close() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_file != nullptr) {
    std::fclose(g_file);
    g_file = nullptr;
  }
  g_closed = true;
}

bool enabled() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_file != nullptr;
}

const char* path() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_path[0] != '\0' ? g_path : "";
}

void emit(const char* fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  char body[kEmitBytes];
  va_list args;
  va_start(args, fmt);
  const int body_n = std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  if (body_n <= 0) {
    return;
  }
  char line[kEmitBytes + 64];
  const int head = std::snprintf(line, sizeof(line), "[diag +%lldms] ", elapsed_ms());
  if (head <= 0) {
    return;
  }
  std::size_t used = static_cast<std::size_t>(head);
  const std::size_t copy = std::min<std::size_t>(static_cast<std::size_t>(body_n), sizeof(line) - used - 2);
  std::memcpy(line + used, body, copy);
  used += copy;
  if (used == 0 || line[used - 1] != '\n') {
    line[used++] = '\n';
  }
  std::lock_guard<std::mutex> lock(g_mu);
  write_locked(line, used);
}

void banner(const char* fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  char body[kEmitBytes];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[wds] %s\n", body);
  emit("%s", body);
}

void emit_throttled(const char* key, int interval_ms, const char* fmt, ...) {
  if (key == nullptr || fmt == nullptr) {
    return;
  }
  const long long now = elapsed_ms();
  {
    std::lock_guard<std::mutex> lock(g_throttle_mu);
    ThrottleSlot* slot = nullptr;
    for (ThrottleSlot& candidate : g_throttle) {
      if (candidate.key == key || (candidate.key != nullptr && std::strcmp(candidate.key, key) == 0)) {
        slot = &candidate;
        break;
      }
      if (candidate.key == nullptr && slot == nullptr) {
        slot = &candidate;
      }
    }
    if (slot == nullptr) {
      slot = &g_throttle[0];
    }
    if (slot->key != nullptr && std::strcmp(slot->key, key) == 0 &&
        now - slot->last_ms < static_cast<long long>(interval_ms)) {
      return;
    }
    slot->key = key;
    slot->last_ms = now;
  }
  char body[kEmitBytes];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  emit("%s", body);
}

}  // namespace diag
}  // namespace wds::common

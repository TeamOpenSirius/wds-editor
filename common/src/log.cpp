#include "wds/common/log.hpp"

#include "wds/common/crash_handler.hpp"
#include "wds/common/utf8_path.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
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

namespace wds::common {
namespace {

constexpr std::size_t kLogRingSlots = 256;
constexpr std::size_t kLogRingSlotBytes = 200;
constexpr std::size_t kEmitBytes = 512;
constexpr std::size_t kPathCap = 1024;

struct LogSlot {
  std::atomic<std::uint64_t> seq{0};
  char text[kLogRingSlotBytes]{};
};

std::atomic<std::uint64_t> g_head{0};
LogSlot g_slots[kLogRingSlots];

std::mutex g_file_mu;
FILE* g_debug_file = nullptr;
char g_debug_path[kPathCap] = {};
bool g_atexit_registered = false;
std::chrono::steady_clock::time_point g_log_origin = std::chrono::steady_clock::now();

void emit_str(JournalEmitFn emit, void* ctx, const char* s, std::size_t n) {
  if (emit == nullptr || s == nullptr || n == 0) {
    return;
  }
  emit(ctx, s, n);
}

void format_timestamp(char* out, std::size_t cap) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  std::snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void close_debug_file_locked() {
  if (g_debug_file == nullptr) {
    return;
  }
  std::fclose(g_debug_file);
  g_debug_file = nullptr;
}

void close_debug_file_atexit() {
  std::lock_guard<std::mutex> lock(g_file_mu);
  close_debug_file_locked();
}

}  // namespace

void log_ring_append(const char* fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  const std::uint64_t ticket = g_head.fetch_add(1, std::memory_order_relaxed);
  LogSlot& slot = g_slots[ticket % kLogRingSlots];
  slot.seq.store(0, std::memory_order_relaxed);
  slot.text[0] = '\0';
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(slot.text, kLogRingSlotBytes, fmt, args);
  va_end(args);
  slot.text[kLogRingSlotBytes - 1] = '\0';
  slot.seq.store(ticket + 1, std::memory_order_release);
}

void log_ring_write_text(JournalEmitFn emit, void* ctx) {
  if (emit == nullptr) {
    return;
  }
  const char heading[] = "--- recent log ---\n";
  emit_str(emit, ctx, heading, sizeof(heading) - 1);

  const std::uint64_t head = g_head.load(std::memory_order_acquire);
  const std::uint64_t start = (head > kLogRingSlots) ? (head - kLogRingSlots) : 0;
  for (std::uint64_t i = start; i < head; ++i) {
    const LogSlot& slot = g_slots[i % kLogRingSlots];
    const std::uint64_t seq = slot.seq.load(std::memory_order_acquire);
    if (seq != i + 1) {
      continue;
    }
    std::size_t n = 0;
    while (n < kLogRingSlotBytes && slot.text[n] != '\0') {
      ++n;
    }
    if (n == 0) {
      continue;
    }
    emit_str(emit, ctx, slot.text, n);
    if (slot.text[n - 1] != '\n') {
      emit_str(emit, ctx, "\n", 1);
    }
  }
}

void log_open_debug_session() {
#if !WDS_ENABLE_LOGGING
  return;
#else
  std::lock_guard<std::mutex> lock(g_file_mu);
  if (g_debug_file != nullptr) {
    return;
  }

  const char* dir = crash_log_directory();
  if (dir == nullptr || dir[0] == '\0') {
    std::fprintf(stderr, "[wds] debug log: failed to open (no log dir), using stderr\n");
    return;
  }

  char stamp[32] = {};
  format_timestamp(stamp, sizeof(stamp));
#if defined(_WIN32)
  const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
  std::snprintf(g_debug_path, sizeof(g_debug_path), "%s\\debug-%s-%lu.log", dir, stamp, pid);
#else
  const long pid = static_cast<long>(::getpid());
  std::snprintf(g_debug_path, sizeof(g_debug_path), "%s/debug-%s-%ld.log", dir, stamp, pid);
#endif

  FILE* fp = fopen_utf8(g_debug_path, "w");
  if (fp == nullptr) {
    std::fprintf(stderr, "[wds] debug log: failed to open %s, using stderr\n", g_debug_path);
    g_debug_path[0] = '\0';
    return;
  }

  g_debug_file = fp;
  g_log_origin = std::chrono::steady_clock::now();
  std::fprintf(stderr, "[wds] debug log: %s\n", g_debug_path);
  if (!g_atexit_registered) {
    g_atexit_registered = true;
    std::atexit(close_debug_file_atexit);
  }
#endif
}

const char* log_debug_session_path() {
  return g_debug_path[0] != '\0' ? g_debug_path : "";
}

void log_emit(const char* fmt, ...) {
#if !WDS_ENABLE_LOGGING
  (void)fmt;
#else
  if (fmt == nullptr) {
    return;
  }
  char body[kEmitBytes];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  body[kEmitBytes - 1] = '\0';

  std::lock_guard<std::mutex> lock(g_file_mu);
  FILE* out = g_debug_file != nullptr ? g_debug_file : stderr;
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - g_log_origin)
                              .count();
  std::fprintf(out, "[wds +%lldms] %s", static_cast<long long>(elapsed_ms), body);
  if (g_debug_file != nullptr) {
    std::fflush(g_debug_file);
  }
#endif
}

}  // namespace wds::common

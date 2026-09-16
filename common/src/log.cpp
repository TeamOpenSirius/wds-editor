#include "wds/common/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wds::common {
namespace {

constexpr std::size_t kLogRingSlots = 256;
constexpr std::size_t kLogRingSlotBytes = 200;

struct LogSlot {
  std::atomic<std::uint64_t> seq{0};
  char text[kLogRingSlotBytes]{};
};

std::atomic<std::uint64_t> g_head{0};
LogSlot g_slots[kLogRingSlots];

void emit_str(JournalEmitFn emit, void* ctx, const char* s, std::size_t n) {
  if (emit == nullptr || s == nullptr || n == 0) {
    return;
  }
  emit(ctx, s, n);
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

}  // namespace wds::common

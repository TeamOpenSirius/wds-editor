#pragma once

#include <atomic>
#include <cstdint>

namespace wds::chart_render {

// Wave 4.1: runtime slice floor. Scheduling only; any value yields the same
// particle bytes (FORCE / VERIFY). Fallback 256 until both sides of the
// ratio have a sample. 4-wide prefix is structural ≥1
// (kIntegrateBatchMinPrefix); not timed here.

inline std::atomic<std::uint32_t> g_split_adapt_frame{0};
inline thread_local std::uint32_t tls_adapt_seen = 0;
inline thread_local int tls_adapt_fill_budget = 0;

inline void split_adapt_begin_frame() noexcept {
  g_split_adapt_frame.fetch_add(1, std::memory_order_relaxed);
}

// Once per realize task on the worker. Reloads the frame id (relaxed) and
// arms at most one fill-cost sample for this thread.
inline void split_adapt_arm_thread_frame() noexcept {
  const std::uint32_t f = g_split_adapt_frame.load(std::memory_order_relaxed);
  if (tls_adapt_seen == f) {
    return;
  }
  tls_adapt_seen = f;
  tls_adapt_fill_budget = 1;
}

int split_adapt_min_fill() noexcept;

void split_adapt_note_fill_cost(int fill, std::uint32_t us) noexcept;
void split_adapt_note_slice_overhead_ns(std::int64_t ns) noexcept;

void split_adapt_maybe_log() noexcept;

}  // namespace wds::chart_render

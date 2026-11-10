#pragma once

// Wave 4a: IntegratorHot 4-wide mask-0 stepping. hop / perf, not 2022.3.
// WDS_INTEGRATE_BATCH=0 restores per-particle advance_to / sample.
// WDS_INTEGRATE_BATCH_VERIFY=1 runs scalar+batch, counts diffs, prints first.
// WDS_INTEGRATE_BATCH_COUNTERS=1 (compile-time, default 0) enables pack /
// fallback / neon / hit atomics + atexit PACK/HITS print. Same rule as
// WDS_MIX3_COUNTERS: per-call counters stay out of the Release hot path.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <utility>

#ifndef WDS_INTEGRATE_BATCH_COUNTERS
#define WDS_INTEGRATE_BATCH_COUNTERS 0
#endif

namespace wds::chart_render::shuriken {

inline bool integrate_env_on_default(const char* name, bool default_on) noexcept {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_on;
  }
  const std::string_view v(raw);
  if (v == "0" || v == "off" || v == "false" || v == "OFF") {
    return false;
  }
  return true;
}

inline bool integrate_batch_verify() noexcept {
  static const bool on = integrate_env_on_default("WDS_INTEGRATE_BATCH_VERIFY", false);
  return on;
}

inline bool integrate_batch_enabled() noexcept {
  // Default on after golden 0/5128 + VERIFY fail=0. WDS_INTEGRATE_BATCH=0
  // restores per-particle scalar. VERIFY=1 also turns the 4-wide path on.
  static const bool on = integrate_env_on_default("WDS_INTEGRATE_BATCH", true) ||
                         integrate_batch_verify();
  return on;
}

// Structural: batch iff the 4 lanes share at least one grid step. Batch and
// scalar do the same non-step work for those 4 particles (ctor / restore /
// epilogue / fill_live_after_pose); there is no extra fixed pack cost.
// Play skip is pose-cache live + grid alignment, not this threshold.
inline constexpr int kIntegrateBatchMinPrefix = 1;

// Skip the entire 4-wide attempt when the pose cache already holds checkpoints
// (snapshotted once per collect onto an atomic workers can read): play
// maybe_restore unaligns lanes so NEON never fires, and the ok4/prefix
// scan itself was a play regression. Cold/seek clear the cache first.
// Also skip when catalog flags are not mask-0 (packing then scalar fallback).
// VERIFY still takes 4-wide. ID-independent.

inline void integrate_batch_register_atexit() noexcept;

#if WDS_INTEGRATE_BATCH_COUNTERS

inline std::atomic<uint64_t> g_integrate_batch_hits{0};
inline std::atomic<uint64_t> g_integrate_batch_pack{0};
inline std::atomic<uint64_t> g_integrate_batch_taken{0};
inline std::atomic<uint64_t> g_integrate_batch_short{0};
inline std::atomic<uint64_t> g_integrate_batch_fallback{0};
inline std::atomic<uint64_t> g_integrate_batch_prefix_sum{0};
inline std::atomic<uint64_t> g_integrate_batch_neon{0};
inline std::atomic<uint64_t> g_integrate_batch_neon_scalar{0};

// mask-0 particle·steps. simd4 = 4-wide kernel coverage. scalar = advance_to
// remainder + unbatched. ISA-independent coverage, not a 2022.3 formula.
inline std::atomic<uint64_t> g_mask0_psteps_total{0};
inline std::atomic<uint64_t> g_mask0_psteps_simd4{0};
inline std::atomic<uint64_t> g_mask0_psteps_scalar{0};

inline constexpr int kIntegrateBatchHitSlots = 64;
struct IntegrateBatchHitSlot {
  std::atomic<int32_t> id{0};
  std::atomic<uint64_t> n{0};
};
inline IntegrateBatchHitSlot g_integrate_batch_hit_slots[kIntegrateBatchHitSlots];

inline void integrate_batch_note_pack(int prefix, bool take) noexcept {
  const int p = prefix < 0 ? 0 : prefix;
  g_integrate_batch_pack.fetch_add(1, std::memory_order_relaxed);
  g_integrate_batch_prefix_sum.fetch_add(static_cast<uint64_t>(p), std::memory_order_relaxed);
  if (take) {
    g_integrate_batch_taken.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_integrate_batch_short.fetch_add(1, std::memory_order_relaxed);
  }
}

inline void integrate_batch_note_fallback() noexcept {
  g_integrate_batch_fallback.fetch_add(1, std::memory_order_relaxed);
}

inline void integrate_batch_note_neon(bool used_neon) noexcept {
  if (used_neon) {
    g_integrate_batch_neon.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_integrate_batch_neon_scalar.fetch_add(1, std::memory_order_relaxed);
  }
}

inline void integrate_batch_note_mask0_simd4(int nsteps) noexcept {
  if (nsteps <= 0) {
    return;
  }
  const uint64_t ps = 4ull * static_cast<uint64_t>(nsteps);
  g_mask0_psteps_total.fetch_add(ps, std::memory_order_relaxed);
  g_mask0_psteps_simd4.fetch_add(ps, std::memory_order_relaxed);
}

inline void integrate_batch_note_mask0_scalar(int nsteps) noexcept {
  if (nsteps <= 0) {
    return;
  }
  const uint64_t ps = static_cast<uint64_t>(nsteps);
  g_mask0_psteps_total.fetch_add(ps, std::memory_order_relaxed);
  g_mask0_psteps_scalar.fetch_add(ps, std::memory_order_relaxed);
}

inline void integrate_batch_reset() noexcept {
  g_integrate_batch_hits.store(0, std::memory_order_relaxed);
  g_integrate_batch_pack.store(0, std::memory_order_relaxed);
  g_integrate_batch_taken.store(0, std::memory_order_relaxed);
  g_integrate_batch_short.store(0, std::memory_order_relaxed);
  g_integrate_batch_fallback.store(0, std::memory_order_relaxed);
  g_integrate_batch_prefix_sum.store(0, std::memory_order_relaxed);
  g_integrate_batch_neon.store(0, std::memory_order_relaxed);
  g_integrate_batch_neon_scalar.store(0, std::memory_order_relaxed);
  g_mask0_psteps_total.store(0, std::memory_order_relaxed);
  g_mask0_psteps_simd4.store(0, std::memory_order_relaxed);
  g_mask0_psteps_scalar.store(0, std::memory_order_relaxed);
  for (int i = 0; i < kIntegrateBatchHitSlots; ++i) {
    g_integrate_batch_hit_slots[i].id.store(0, std::memory_order_relaxed);
    g_integrate_batch_hit_slots[i].n.store(0, std::memory_order_relaxed);
  }
}

inline void integrate_batch_note_hit(int32_t effect_id) noexcept {
  integrate_batch_register_atexit();
  g_integrate_batch_hits.fetch_add(1, std::memory_order_relaxed);
  if (effect_id == 0) {
    return;
  }
  uint32_t h = static_cast<uint32_t>(effect_id) * 0x9e3779b1u;
  for (int i = 0; i < kIntegrateBatchHitSlots; ++i) {
    IntegrateBatchHitSlot& s = g_integrate_batch_hit_slots[(h + static_cast<uint32_t>(i)) &
                                                           (kIntegrateBatchHitSlots - 1)];
    int32_t cur = s.id.load(std::memory_order_relaxed);
    if (cur == effect_id) {
      s.n.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (cur == 0) {
      int32_t expected = 0;
      if (s.id.compare_exchange_strong(expected, effect_id, std::memory_order_relaxed) ||
          expected == effect_id) {
        s.n.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
}

#else

inline void integrate_batch_note_pack(int, bool) noexcept {}
inline void integrate_batch_note_fallback() noexcept {}
inline void integrate_batch_note_neon(bool) noexcept {}
inline void integrate_batch_note_hit(int32_t) noexcept {}
inline void integrate_batch_note_mask0_simd4(int) noexcept {}
inline void integrate_batch_note_mask0_scalar(int) noexcept {}
inline void integrate_batch_reset() noexcept {}

#endif  // WDS_INTEGRATE_BATCH_COUNTERS

inline std::atomic<uint64_t> g_integrate_batch_verify_fail{0};
inline std::atomic<uint64_t> g_integrate_batch_verify_ok{0};

inline void integrate_batch_verify_note(bool ok, const char* where) noexcept {
  if (!integrate_batch_verify()) {
    return;
  }
  integrate_batch_register_atexit();
  if (ok) {
    g_integrate_batch_verify_ok.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint64_t n =
      g_integrate_batch_verify_fail.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 32) {
    std::fprintf(stderr, "WDS_INTEGRATE_BATCH_VERIFY mismatch at %s (n=%llu)\n", where,
                 static_cast<unsigned long long>(n));
  }
}

inline void integrate_batch_flush() noexcept {
  if (integrate_batch_verify()) {
    const uint64_t fail = g_integrate_batch_verify_fail.load(std::memory_order_relaxed);
    const uint64_t ok = g_integrate_batch_verify_ok.load(std::memory_order_relaxed);
    if (fail != 0 || ok != 0) {
      std::fprintf(stderr, "WDS_INTEGRATE_BATCH_VERIFY fail=%llu ok=%llu\n",
                   static_cast<unsigned long long>(fail),
                   static_cast<unsigned long long>(ok));
    }
  }
#if WDS_INTEGRATE_BATCH_COUNTERS
  const uint64_t hits = g_integrate_batch_hits.load(std::memory_order_relaxed);
  const uint64_t pack = g_integrate_batch_pack.load(std::memory_order_relaxed);
  const uint64_t taken = g_integrate_batch_taken.load(std::memory_order_relaxed);
  const uint64_t shortn = g_integrate_batch_short.load(std::memory_order_relaxed);
  const uint64_t fallback = g_integrate_batch_fallback.load(std::memory_order_relaxed);
  const uint64_t psum = g_integrate_batch_prefix_sum.load(std::memory_order_relaxed);
  const uint64_t neon = g_integrate_batch_neon.load(std::memory_order_relaxed);
  const uint64_t neon_scalar = g_integrate_batch_neon_scalar.load(std::memory_order_relaxed);
  const uint64_t mtot0 = g_mask0_psteps_total.load(std::memory_order_relaxed);
  if (pack == 0 && hits == 0 && mtot0 == 0) {
    return;
  }
  const double avg = pack != 0 ? static_cast<double>(psum) / static_cast<double>(pack) : 0.0;
  std::fprintf(stderr,
               "WDS_INTEGRATE_BATCH_PACK try=%llu taken=%llu short=%llu fallback=%llu "
               "prefix_avg=%.2f neon=%llu neon_scalar=%llu\n",
               static_cast<unsigned long long>(pack), static_cast<unsigned long long>(taken),
               static_cast<unsigned long long>(shortn), static_cast<unsigned long long>(fallback),
               avg, static_cast<unsigned long long>(neon),
               static_cast<unsigned long long>(neon_scalar));
  const uint64_t mtot = g_mask0_psteps_total.load(std::memory_order_relaxed);
  const uint64_t ms4 = g_mask0_psteps_simd4.load(std::memory_order_relaxed);
  const uint64_t msc = g_mask0_psteps_scalar.load(std::memory_order_relaxed);
  if (mtot != 0) {
    std::fprintf(stderr, "WDS_INTEGRATE_MASK0_STEPS total=%llu simd4=%llu scalar=%llu\n",
                 static_cast<unsigned long long>(mtot), static_cast<unsigned long long>(ms4),
                 static_cast<unsigned long long>(msc));
  }
  std::fprintf(stderr, "WDS_INTEGRATE_BATCH_HITS total=%llu\n",
               static_cast<unsigned long long>(hits));
  std::array<std::pair<uint64_t, int32_t>, kIntegrateBatchHitSlots> rows{};
  int n = 0;
  for (int i = 0; i < kIntegrateBatchHitSlots; ++i) {
    const int32_t id = g_integrate_batch_hit_slots[i].id.load(std::memory_order_relaxed);
    const uint64_t c = g_integrate_batch_hit_slots[i].n.load(std::memory_order_relaxed);
    if (id != 0 && c != 0) {
      rows[static_cast<size_t>(n++)] = {c, id};
    }
  }
  std::sort(rows.begin(), rows.begin() + n,
            [](const std::pair<uint64_t, int32_t>& a, const std::pair<uint64_t, int32_t>& b) {
              return a.first > b.first;
            });
  const int show = n < 10 ? n : 10;
  for (int i = 0; i < show; ++i) {
    std::fprintf(stderr, "WDS_INTEGRATE_BATCH_HIT effect=%d n=%llu\n",
                 rows[static_cast<size_t>(i)].second,
                 static_cast<unsigned long long>(rows[static_cast<size_t>(i)].first));
  }
#endif
}

inline void integrate_batch_register_atexit() noexcept {
  static std::once_flag once;
  std::call_once(once, [] { std::atexit([] { integrate_batch_flush(); }); });
}

}  // namespace wds::chart_render::shuriken

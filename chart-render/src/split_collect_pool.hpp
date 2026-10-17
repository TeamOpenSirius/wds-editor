#pragma once

#include <cstddef>
#include <cstdint>

namespace wds::chart_render {

// WDS_SPLIT_THREADS: 1 = inline (no pool). Default worker cap:
// - hybrid (E>0) and P>4: P-1 (UI/audio can sit on E), clamp 8
// - 4P hybrid (M-series): keep 4 (UI/audio on E)
// - all-P and P<=6: P-2 (leave ≥2 physical cores for UI/audio)
// Burst may add E-cores (leave ≥2 of P+E). Never count SMT siblings.
int split_collect_thread_count() noexcept;

struct SplitCollectCaps {
  int cap = 1;
  int burst_cap = 1;
};

// Pure topology → worker/burst caps. Used by detect_topo and unit tests.
SplitCollectCaps split_collect_caps_from_phys(int p_phys, int e_phys) noexcept;

// One logical CPU as reported by Linux sysfs. SMT siblings share
// (package_id, core_id). capacity < 0 means cpu_capacity is missing.
struct SplitCollectLinuxCpu {
  int package_id = 0;
  int core_id = 0;
  int capacity = -1;
};

void split_collect_count_phys_linux(const SplitCollectLinuxCpu* rows, std::size_t n, int* p_phys,
                                    int* e_phys) noexcept;
int split_collect_pool_cap() noexcept;
int split_collect_p_cores() noexcept;
int split_collect_e_cores() noexcept;
int split_collect_burst_cap() noexcept;
void split_collect_set_burst(bool on) noexcept;
bool split_collect_burst_allowed() noexcept;
int split_collect_last_participants() noexcept;
int split_collect_last_call_participants() noexcept;
int split_collect_frame_max_participants() noexcept;
std::uint64_t split_collect_frame_wakeups() noexcept;
int split_collect_claim_depth() noexcept;

// Start of a collect (including empty / rewind). Clears leftover global
// hint and per-frame participant / wakeup counters so a later light
// parallel_for cannot inherit the last heavy wave.
void split_collect_begin_frame() noexcept;

// After seek / cold collect: wake P-cap workers at USER_INTERACTIVE and
// keep them spinning until hold_ms elapses so the first parallel_for does
// not pay idle-core clock/QoS ramp. Not a result cache.
void split_collect_prepare_heavy(int hold_ms = 50) noexcept;

// Diagnostic only. Participant choice no longer reads this store.
void split_collect_note_work(size_t ntasks, uint64_t step_hint) noexcept;

// Work-sharing parallel for. Calling thread participates. n<=1 or thread
// count 1 runs fn inline and never constructs the pool (fork-safe: bench
// parent pre-hydrate must not start workers).
//
// The n / grain overloads derive a conservative work_hint from n (never a
// leftover global). Heavy collect must pass the cost hint via the
// work_hint overload.
//
// Safe from any thread, including pool workers: a nested call publishes a
// new job onto the same pool (workers steal newest-first) instead of
// overwriting the outer job. Overflow / tiny n runs inline.
void split_collect_parallel_for(size_t n, void (*fn)(size_t, void*), void* ctx);

// [begin, end) with a claim grain. grain==0 picks ~n/(4*threads), min 1.
void split_collect_parallel_for(size_t begin, size_t end, size_t grain, void (*fn)(size_t, void*),
                                void* ctx);

// Same as the grain overload, but participant choice uses work_hint and n
// of this call only. Thresholds match the previous adaptive pool:
// hint<512 → 1 (inline, 0 extra wakeups); hint<8000 → 2; else cap.
void split_collect_parallel_for(size_t begin, size_t end, size_t grain, void (*fn)(size_t, void*),
                                void* ctx, std::uint64_t work_hint);

}  // namespace wds::chart_render

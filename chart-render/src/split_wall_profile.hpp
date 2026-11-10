#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace wds::chart_render {

enum class SplitWallSec : int {
  Frame = 0,
  PhaseA,
  PhaseB,
  PhaseC,
  Cap,
  Copyout,
  Births,
  BirthFilter,
  Compaction,
  Stitch,
  SubemitMerge,
  CacheLookup,
  CacheStore,
  MakeState,
  Integrate,
  Appearance,
  AlphaPlan,
  Trails,
  CarryLookup,
  CarryStore,
  SubemitWalk,
  Noise,
  PoseCtor,
  CacheKey,
  Stamp,
  AdoptCarry,
  PoseHydrate,
  PersistCull,
  PersistCommit,
  CapSort,
  ParentEvents,
  ChildFill,
  ChildCap,
  SubemitSetup,
  PersistClaim,
  RealizeMatch,
  PlanGate,
  VecGrow,
  CapSelect,
  Assemble,
  LockWait,
  PoolWake,
  PoolJoin,
  TaskSetup,
  CfHash,
  CfKeep,
  CfIdent,
  PePoseAt,
  SubemitPlan,
  SubemitRealize,
  WaveB1,
  WaveB2Setup,
  WaveB2,
  WaveB2b,
  WaveB3Setup,
  WaveB3,
  WaveB3b,
  BirthsPlan,
  CapSortPlan,
  BirthsQueue,
  CapSortQueue,
#if WDS_DIAG_ENABLED
  CapPush,
  CapApply,
  RealizeSlices,
  Reap,
#endif
  Count
};

struct SplitWallFrameSnap {
  double sec_ms[static_cast<int>(SplitWallSec::Count)]{};
  double task_sum_ms = 0.0;
  double task_max_ms = 0.0;
  double phase_b_ms = 0.0;
  double staged_ms = 0.0;
  double unscoped_ms = 0.0;
  int ntasks = 0;
  int nthreads = 0;
  int nsplits = 0;
  int evaluated = 0;
  int kept = 0;
  int realized = 0;
  int eval_slices = 0;
  int longest_wave = -1;
  int longest_kind = -1;
  int longest_fill = 0;
  int birth_plan_n = 0;
  int birth_plan_ext = 0;
  int birth_plan_len_sum = 0;
  int birth_plan_len_max = 0;
  int birth_queue_n = 0;
  int birth_queue_ext = 0;
  int birth_queue_len_sum = 0;
  int birth_queue_len_max = 0;
  int birth_extend_hit = 0;
  int birth_extend_miss = 0;
#if WDS_DIAG_ENABLED
  int lockwait_blocks = 0;
  int64_t lockwait_ns = 0;
  int64_t lockwait_max_ns = 0;
  int64_t lockwait_thread_max_ns = 0;
  int cap_n = 0;
#endif
};

bool split_wall_enabled() noexcept;
void split_wall_reset() noexcept;
void split_wall_begin_frame() noexcept;
void split_wall_end_frame() noexcept;
void split_wall_add_ns(SplitWallSec sec, int64_t ns) noexcept;
void split_wall_scope_enter(SplitWallSec sec, int64_t now_ns) noexcept;
void split_wall_scope_leave(SplitWallSec sec, int64_t start_ns, int64_t now_ns) noexcept;
void split_wall_max_ns(SplitWallSec sec, int64_t ns) noexcept;
void split_wall_begin_task() noexcept;
void split_wall_set_task_tag(int wave, int kind, int fill) noexcept;
void split_wall_note_task(int64_t ns) noexcept;
void split_wall_note_phase_b(int ntasks, int nthreads, int64_t wall_ns) noexcept;
void split_wall_note_worker_busy(int worker, int64_t busy_ns) noexcept;
void split_wall_note_candidates(int evaluated, int kept) noexcept;
void split_wall_note_realized(int n) noexcept;
void split_wall_note_splits(int n) noexcept;
void split_wall_note_eval_slices(int n) noexcept;
#if !WDS_DIAG_ENABLED
inline void split_wall_note_birth_table(int, bool, bool) noexcept {}
inline void split_wall_note_birth_extend(bool) noexcept {}
#else
// WALL-only: one collect_births call. is_queue distinguishes evaluate_plan vs
// the parent queue at the start of evaluate_subemitters_plan. Sibling of Births
// (not nested) so exclusive unscoped accounting stays honest.
void split_wall_note_birth_table(int n, bool can_extend, bool is_queue) noexcept;
void split_wall_note_birth_extend(bool hit) noexcept;
#endif
std::string split_wall_report();
const char* split_wall_sec_name(SplitWallSec sec) noexcept;
bool split_wall_copy_last_frame(SplitWallFrameSnap& out) noexcept;

#if WDS_DIAG_ENABLED
bool split_lockwait_enabled() noexcept;
bool split_waves_enabled() noexcept;
bool split_wall_scope_wanted() noexcept;
bool split_lock_scope_wanted() noexcept;
bool split_frame_acc_enabled() noexcept;
void split_wall_note_cap_n(int n) noexcept;

inline bool split_wall_is_wave_sec(SplitWallSec sec) noexcept {
  switch (sec) {
    case SplitWallSec::PhaseA:
    case SplitWallSec::WaveB1:
    case SplitWallSec::CapPush:
    case SplitWallSec::CapApply:
    case SplitWallSec::RealizeSlices:
    case SplitWallSec::WaveB2Setup:
    case SplitWallSec::WaveB2:
    case SplitWallSec::WaveB2b:
    case SplitWallSec::WaveB3Setup:
    case SplitWallSec::WaveB3:
    case SplitWallSec::WaveB3b:
    case SplitWallSec::Assemble:
    case SplitWallSec::Reap:
      return true;
    default:
      return false;
  }
}

inline int64_t split_wall_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#endif
class SplitWallScope {
#if !WDS_DIAG_ENABLED
 public:
  explicit constexpr SplitWallScope(SplitWallSec) noexcept {}
#else
 public:
  explicit SplitWallScope(SplitWallSec sec) noexcept {
    // One cached bool when every diag switch is off (same as pre-change
    // split_wall_enabled()). WALL times every section; WAVES-only times the
    // serial/wave whitelist and skips the clock on everything else.
    if (!split_wall_scope_wanted()) {
      return;
    }
    if (split_wall_enabled()) {
      sec_ = sec;
      active_ = true;
      full_ = true;
      start_ns_ = split_wall_now_ns();
      split_wall_scope_enter(sec_, start_ns_);
      return;
    }
    if (!split_wall_is_wave_sec(sec)) {
      return;
    }
    sec_ = sec;
    active_ = true;
    full_ = false;
    start_ns_ = split_wall_now_ns();
  }
  ~SplitWallScope() {
    if (!active_) {
      return;
    }
    const int64_t now = split_wall_now_ns();
    if (full_) {
      split_wall_scope_leave(sec_, start_ns_, now);
    } else if (now > start_ns_) {
      split_wall_add_ns(sec_, now - start_ns_);
    }
  }
  SplitWallScope(const SplitWallScope&) = delete;
  SplitWallScope& operator=(const SplitWallScope&) = delete;

 private:
  SplitWallSec sec_{};
  bool active_ = false;
  bool full_ = false;
  int64_t start_ns_ = 0;
#endif  // WDS_DIAG_ENABLED
};

#if WDS_DIAG_ENABLED
// Blocking eval-cache shard lock only. Non-blocking try_to_lock does not
// construct this. LOCKWAIT-only reads the clock here; other SplitWallScope
// sections keep checking split_wall_scope_wanted() which stays false.
class SplitLockWaitScope {
 public:
  SplitLockWaitScope() noexcept {
    if (!split_lock_scope_wanted()) {
      return;
    }
    if (split_wall_enabled()) {
      active_ = true;
      full_ = true;
      start_ns_ = split_wall_now_ns();
      split_wall_scope_enter(SplitWallSec::LockWait, start_ns_);
      return;
    }
    active_ = true;
    full_ = false;
    start_ns_ = split_wall_now_ns();
  }
  ~SplitLockWaitScope() {
    if (!active_) {
      return;
    }
    const int64_t now = split_wall_now_ns();
    if (full_) {
      split_wall_scope_leave(SplitWallSec::LockWait, start_ns_, now);
    } else {
      split_wall_add_ns(SplitWallSec::LockWait, now > start_ns_ ? now - start_ns_ : 0);
    }
  }
  SplitLockWaitScope(const SplitLockWaitScope&) = delete;
  SplitLockWaitScope& operator=(const SplitLockWaitScope&) = delete;

 private:
  bool active_ = false;
  bool full_ = false;
  int64_t start_ns_ = 0;
};
#endif  // WDS_DIAG_ENABLED
}  // namespace wds::chart_render

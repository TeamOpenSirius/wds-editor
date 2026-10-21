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
  Leak,
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
void split_wall_note_task(int64_t ns) noexcept;
void split_wall_note_phase_b(int ntasks, int nthreads, int64_t wall_ns) noexcept;
void split_wall_note_worker_busy(int worker, int64_t busy_ns) noexcept;
void split_wall_note_candidates(int evaluated, int kept) noexcept;
void split_wall_note_realized(int n) noexcept;
void split_wall_note_splits(int n) noexcept;
std::string split_wall_report();
const char* split_wall_sec_name(SplitWallSec sec) noexcept;
bool split_wall_copy_last_frame(SplitWallFrameSnap& out) noexcept;

class SplitWallScope {
#if !WDS_DIAG_ENABLED
 public:
  explicit constexpr SplitWallScope(SplitWallSec) noexcept {}
#else
 public:
  explicit SplitWallScope(SplitWallSec sec) noexcept {
    if (!split_wall_enabled()) {
      return;
    }
    sec_ = sec;
    active_ = true;
    start_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
    split_wall_scope_enter(sec_, start_ns_);
  }
  ~SplitWallScope() {
    if (!active_) {
      return;
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    split_wall_scope_leave(sec_, start_ns_, now);
  }
  SplitWallScope(const SplitWallScope&) = delete;
  SplitWallScope& operator=(const SplitWallScope&) = delete;

 private:
  SplitWallSec sec_{};
  bool active_ = false;
  int64_t start_ns_ = 0;
#endif  // WDS_DIAG_ENABLED
};

}  // namespace wds::chart_render

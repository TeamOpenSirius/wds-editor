#include "split_wall_profile.hpp"
#include "split_collect_pool.hpp"
#include "shuriken/simd_dispatch.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace wds::chart_render {
namespace {

constexpr int kSecN = static_cast<int>(SplitWallSec::Count);
constexpr int kMaxWorkers = 16;
constexpr int kMaxWindow = 4096;

constexpr int kScopeNest = 64;

constexpr const char* kSecName[kSecN] = {
    "frame",      "A",          "B",           "C",           "cap",
    "copyout",    "leak",       "births",      "filter",      "compact",
    "stitch",     "submerge",   "cache_lu",    "cache_st",    "make_state",
    "integrate",  "appear",     "alpha",       "trails",      "carry_lu",
    "carry_st",   "walk",       "noise",       "pose_ctor",   "cache_key",
    "stamp",      "adopt",      "hydrate",     "persist_cull","persist_cmt",
    "capsort",    "parent_ev",  "child_fill",  "child_cap",   "subsetup",
    "persist_cl", "realize_m",  "plan_gate",   "vec_grow",    "capselect",
    "assemble",   "lockwait",   "pool_wake",   "pool_join",   "task_setup",
    "cf_hash",    "cf_keep",    "cf_ident",    "pe_pose_at",
};

bool env_on() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_WALL");
  return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
}

void atomic_max(std::atomic<int64_t>& slot, int64_t v) noexcept {
  int64_t cur = slot.load(std::memory_order_relaxed);
  while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

struct FrameAcc {
  std::atomic<int64_t> sum[kSecN]{};
  std::atomic<int64_t> mx[kSecN]{};
  std::atomic<int64_t> exclusive{0};
  std::atomic<int64_t> unscoped{0};
  std::atomic<int64_t> task_sum{0};
  std::atomic<int64_t> task_max{0};
  std::atomic<int> ntasks{0};
  std::atomic<int> nthreads{0};
  std::atomic<int> nsplits{0};
  std::atomic<int> evaluated{0};
  std::atomic<int> kept{0};
  std::atomic<int> realized{0};
  std::atomic<int64_t> worker_busy[kMaxWorkers]{};
  std::atomic<int64_t> phase_b_wall{0};

  void reset() noexcept {
    for (int i = 0; i < kSecN; ++i) {
      sum[i].store(0, std::memory_order_relaxed);
      mx[i].store(0, std::memory_order_relaxed);
    }
    exclusive.store(0, std::memory_order_relaxed);
    unscoped.store(0, std::memory_order_relaxed);
    task_sum.store(0, std::memory_order_relaxed);
    task_max.store(0, std::memory_order_relaxed);
    ntasks.store(0, std::memory_order_relaxed);
    nthreads.store(0, std::memory_order_relaxed);
    nsplits.store(0, std::memory_order_relaxed);
    evaluated.store(0, std::memory_order_relaxed);
    kept.store(0, std::memory_order_relaxed);
    realized.store(0, std::memory_order_relaxed);
    for (int w = 0; w < kMaxWorkers; ++w) {
      worker_busy[w].store(0, std::memory_order_relaxed);
    }
    phase_b_wall.store(0, std::memory_order_relaxed);
  }
};

struct WindowRow {
  int64_t sum[kSecN]{};
  int64_t mx[kSecN]{};
  int64_t exclusive = 0;
  int64_t unscoped = 0;
  int64_t task_sum = 0;
  int64_t task_max = 0;
  int ntasks = 0;
  int nthreads = 0;
  int nsplits = 0;
  int evaluated = 0;
  int kept = 0;
  int realized = 0;
  int64_t worker_busy[kMaxWorkers]{};
  int64_t phase_b_wall = 0;
};

std::atomic<bool> g_on{env_on()};
FrameAcc g_frame;
std::mutex g_mu;
std::vector<WindowRow> g_window;
WindowRow g_last{};
bool g_have_last = false;

int64_t staged_ns(const WindowRow& r) noexcept {
  return r.task_sum > r.unscoped ? r.task_sum - r.unscoped : 0;
}

void fill_snap(const WindowRow& r, SplitWallFrameSnap& out) noexcept {
  out = {};
  for (int i = 0; i < kSecN; ++i) {
    out.sec_ms[i] = static_cast<double>(r.sum[i]) / 1e6;
  }
  out.task_sum_ms = static_cast<double>(r.task_sum) / 1e6;
  out.task_max_ms = static_cast<double>(r.task_max) / 1e6;
  out.phase_b_ms = static_cast<double>(r.phase_b_wall) / 1e6;
  out.staged_ms = static_cast<double>(staged_ns(r)) / 1e6;
  out.unscoped_ms = static_cast<double>(r.unscoped) / 1e6;
  out.ntasks = r.ntasks;
  out.nthreads = r.nthreads;
  out.nsplits = r.nsplits;
  out.evaluated = r.evaluated;
  out.kept = r.kept;
  out.realized = r.realized;
}

}  // namespace

bool split_wall_enabled() noexcept {
  return g_on.load(std::memory_order_relaxed);
}

void split_wall_reset() noexcept {
  std::lock_guard<std::mutex> lock(g_mu);
  g_window.clear();
  g_frame.reset();
  g_last = {};
  g_have_last = false;
}

void split_wall_begin_frame() noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  g_frame.reset();
}

void split_wall_end_frame() noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  WindowRow row{};
  for (int i = 0; i < kSecN; ++i) {
    row.sum[i] = g_frame.sum[i].load(std::memory_order_relaxed);
    row.mx[i] = g_frame.mx[i].load(std::memory_order_relaxed);
  }
  row.exclusive = g_frame.exclusive.load(std::memory_order_relaxed);
  row.unscoped = g_frame.unscoped.load(std::memory_order_relaxed);
  row.task_sum = g_frame.task_sum.load(std::memory_order_relaxed);
  row.task_max = g_frame.task_max.load(std::memory_order_relaxed);
  row.ntasks = g_frame.ntasks.load(std::memory_order_relaxed);
  row.nthreads = g_frame.nthreads.load(std::memory_order_relaxed);
  row.nsplits = g_frame.nsplits.load(std::memory_order_relaxed);
  row.evaluated = g_frame.evaluated.load(std::memory_order_relaxed);
  row.kept = g_frame.kept.load(std::memory_order_relaxed);
  row.realized = g_frame.realized.load(std::memory_order_relaxed);
  for (int w = 0; w < kMaxWorkers; ++w) {
    row.worker_busy[w] = g_frame.worker_busy[w].load(std::memory_order_relaxed);
  }
  row.phase_b_wall = g_frame.phase_b_wall.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mu);
  g_last = row;
  g_have_last = true;
  g_window.push_back(row);
  if (static_cast<int>(g_window.size()) > kMaxWindow) {
    g_window.erase(g_window.begin(),
                   g_window.begin() + (g_window.size() - static_cast<size_t>(kMaxWindow)));
  }
  g_frame.reset();
}

void split_wall_add_ns(SplitWallSec sec, int64_t ns) noexcept {
  if (!split_wall_enabled() || ns <= 0) {
    return;
  }
  const int i = static_cast<int>(sec);
  if (i < 0 || i >= kSecN) {
    return;
  }
  g_frame.sum[i].fetch_add(ns, std::memory_order_relaxed);
  atomic_max(g_frame.mx[i], ns);
}

namespace {
struct ScopeNest {
  SplitWallSec sec{};
  int64_t start = 0;
  int task_i = 0;
};

thread_local ScopeNest g_nest[kScopeNest];
thread_local int g_nest_n = 0;
thread_local int g_nest_overflow = 0;
constexpr int kTaskNest = 8;
thread_local int64_t g_task_ex[kTaskNest];
thread_local int g_task_n = 0;

void add_exclusive_to(int task_i, int64_t dt) noexcept {
  if (dt <= 0) {
    return;
  }
  if (task_i > 0 && task_i <= kTaskNest) {
    g_task_ex[task_i - 1] += dt;
  }
  g_frame.exclusive.fetch_add(dt, std::memory_order_relaxed);
}
}  // namespace

void split_wall_begin_task() noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  if (g_task_n < kTaskNest) {
    g_task_ex[g_task_n] = 0;
  }
  ++g_task_n;
}

void split_wall_scope_enter(SplitWallSec sec, int64_t now_ns) noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  if (g_nest_overflow > 0) {
    ++g_nest_overflow;
    return;
  }
  if (g_nest_n > 0 && g_nest_n <= kScopeNest) {
    ScopeNest& parent = g_nest[g_nest_n - 1];
    // Exclusive tiles every scope, not only kTaskStaged. Nested LockWait /
    // PoolJoin used to pause a staged parent without crediting themselves,
    // which showed up as multi-ms unscoped inside TaskSetup.
    if (now_ns > parent.start) {
      add_exclusive_to(parent.task_i, now_ns - parent.start);
    }
    parent.start = now_ns;
  }
  if (g_nest_n >= kScopeNest) {
    ++g_nest_overflow;
    return;
  }
  g_nest[g_nest_n] = ScopeNest{sec, now_ns, g_task_n};
  ++g_nest_n;
}

void split_wall_scope_leave(SplitWallSec sec, int64_t start_ns, int64_t now_ns) noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  if (g_nest_overflow > 0) {
    --g_nest_overflow;
    if (now_ns > start_ns) {
      split_wall_add_ns(sec, now_ns - start_ns);
    }
    return;
  }
  if (g_nest_n > 0) {
    --g_nest_n;
    if (g_nest_n < kScopeNest) {
      const ScopeNest& top = g_nest[g_nest_n];
      if (now_ns > top.start) {
        add_exclusive_to(top.task_i, now_ns - top.start);
      }
    }
    if (g_nest_n > 0 && g_nest_n <= kScopeNest) {
      g_nest[g_nest_n - 1].start = now_ns;
    }
  }
  if (now_ns > start_ns) {
    split_wall_add_ns(sec, now_ns - start_ns);
  }
}

void split_wall_max_ns(SplitWallSec sec, int64_t ns) noexcept {
  if (!split_wall_enabled() || ns <= 0) {
    return;
  }
  const int i = static_cast<int>(sec);
  if (i < 0 || i >= kSecN) {
    return;
  }
  atomic_max(g_frame.mx[i], ns);
}

void split_wall_note_task(int64_t ns) noexcept {
  if (!split_wall_enabled() || ns <= 0) {
    return;
  }
  g_frame.task_sum.fetch_add(ns, std::memory_order_relaxed);
  g_frame.ntasks.fetch_add(1, std::memory_order_relaxed);
  atomic_max(g_frame.task_max, ns);
  int64_t have = 0;
  if (g_task_n > 0) {
    --g_task_n;
    if (g_task_n < kTaskNest) {
      have = g_task_ex[g_task_n];
    }
    // Same-thread steal: nested begin_task/note_task runs inside the parent
    // task wall (outer parallel_for wait loop). Exclusive of the nested
    // task went to its own slot; credit the nested wall back so the parent
    // hole is not reported as unclassified.
    if (g_task_n > 0 && g_task_n <= kTaskNest) {
      g_task_ex[g_task_n - 1] += ns;
    }
  }
  const int64_t gap = ns > have ? ns - have : 0;
  g_frame.unscoped.fetch_add(gap, std::memory_order_relaxed);
}

void split_wall_note_phase_b(int ntasks, int nthreads, int64_t wall_ns) noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  (void)ntasks;
  int cur = g_frame.nthreads.load(std::memory_order_relaxed);
  while (nthreads > cur &&
         !g_frame.nthreads.compare_exchange_weak(cur, nthreads, std::memory_order_relaxed)) {
  }
  g_frame.phase_b_wall.fetch_add(wall_ns, std::memory_order_relaxed);
}

void split_wall_note_worker_busy(int worker, int64_t busy_ns) noexcept {
  if (!split_wall_enabled() || busy_ns <= 0) {
    return;
  }
  if (worker < 0 || worker >= kMaxWorkers) {
    return;
  }
  g_frame.worker_busy[worker].fetch_add(busy_ns, std::memory_order_relaxed);
}

void split_wall_note_realized(int n) noexcept {
  if (!split_wall_enabled() || n <= 0) {
    return;
  }
  g_frame.realized.fetch_add(n, std::memory_order_relaxed);
}

void split_wall_note_candidates(int evaluated, int kept) noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  g_frame.evaluated.fetch_add(evaluated, std::memory_order_relaxed);
  g_frame.kept.fetch_add(kept, std::memory_order_relaxed);
}

void split_wall_note_splits(int n) noexcept {
  if (!split_wall_enabled()) {
    return;
  }
  g_frame.nsplits.fetch_add(n, std::memory_order_relaxed);
}

const char* split_wall_sec_name(SplitWallSec sec) noexcept {
  const int i = static_cast<int>(sec);
  if (i < 0 || i >= kSecN) {
    return "?";
  }
  return kSecName[i];
}

bool split_wall_copy_last_frame(SplitWallFrameSnap& out) noexcept {
  if (!split_wall_enabled()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_have_last) {
    return false;
  }
  fill_snap(g_last, out);
  return true;
}

std::string split_wall_report() {
  std::lock_guard<std::mutex> lock(g_mu);
  const int n = static_cast<int>(g_window.size());
  if (n == 0) {
    return {};
  }
  auto avg_ms = [&](auto getter) {
    double s = 0.0;
    for (const WindowRow& r : g_window) {
      s += getter(r);
    }
    return s / static_cast<double>(n) / 1e6;
  };
  const double frame = avg_ms([](const WindowRow& r) { return static_cast<double>(r.sum[0]); });
  const double a = avg_ms([](const WindowRow& r) { return static_cast<double>(r.sum[1]); });
  const double b = avg_ms([](const WindowRow& r) { return static_cast<double>(r.sum[2]); });
  const double c = avg_ms([](const WindowRow& r) { return static_cast<double>(r.sum[3]); });
  const double cap = avg_ms([](const WindowRow& r) { return static_cast<double>(r.sum[4]); });
  const double copy = avg_ms([](const WindowRow& r) { return static_cast<double>(r.sum[5]); });
  const double leak = avg_ms([](const WindowRow& r) { return static_cast<double>(r.sum[6]); });
  double sec_avg[kSecN]{};
  for (int i = 0; i < kSecN; ++i) {
    sec_avg[i] = avg_ms([i](const WindowRow& r) { return static_cast<double>(r.sum[i]); });
  }
  const double task_max = avg_ms([](const WindowRow& r) { return static_cast<double>(r.task_max); });
  const double task_sum = avg_ms([](const WindowRow& r) { return static_cast<double>(r.task_sum); });
  const double bwall = avg_ms([](const WindowRow& r) { return static_cast<double>(r.phase_b_wall); });
  double ntasks = 0.0;
  double nthreads = 0.0;
  double nsplits = 0.0;
  double evaluated = 0.0;
  double kept = 0.0;
  double realized = 0.0;
  double staged = 0.0;
  int wmax = 0;
  double worker_busy_avg[kMaxWorkers]{};
  for (const WindowRow& r : g_window) {
    ntasks += static_cast<double>(r.ntasks);
    nthreads += static_cast<double>(r.nthreads);
    nsplits += static_cast<double>(r.nsplits);
    evaluated += static_cast<double>(r.evaluated);
    kept += static_cast<double>(r.kept);
    realized += static_cast<double>(r.realized);
    staged += static_cast<double>(staged_ns(r));
    for (int w = 0; w < kMaxWorkers; ++w) {
      worker_busy_avg[w] += static_cast<double>(r.worker_busy[w]);
      if (r.worker_busy[w] > 0) {
        wmax = std::max(wmax, w + 1);
      }
    }
  }
  ntasks /= static_cast<double>(n);
  nthreads /= static_cast<double>(n);
  nsplits /= static_cast<double>(n);
  evaluated /= static_cast<double>(n);
  kept /= static_cast<double>(n);
  realized /= static_cast<double>(n);
  staged /= static_cast<double>(n) * 1e6;
  const double b_for_util = bwall > 1e-9 ? bwall : b;
  const double threads = nthreads > 0.5 ? nthreads : 1.0;
  const double serial = a + c;
  const double serial_frac = frame > 1e-9 ? serial / frame : 0.0;
  const double util_raw =
      (b_for_util > 1e-9 && threads > 0.0) ? (task_sum / (b_for_util * threads)) : 0.0;
  const double util = util_raw > 1.0 ? 1.0 : util_raw;
  const double unscoped = task_sum > staged ? task_sum - staged : 0.0;

  std::string out;
  char buf[1400];
  std::snprintf(buf, sizeof(buf),
                "wall frames=%d splits/frame=%.1f  frame=%.3f  A=%.3f  B=%.3f  C=%.3f  "
                "serial=%.3f (%.0f%%)  util=%.0f%%\n",
                n, nsplits, frame, a, b, c, serial, serial_frac * 100.0, util * 100.0);
  out += buf;
  std::snprintf(buf, sizeof(buf),
                "  topo P=%d E=%d cap=%d last_parts=%d simd=%s\n",
                split_collect_p_cores(), split_collect_e_cores(), split_collect_pool_cap(),
                split_collect_last_participants(), split_simd_name());
  out += buf;
  std::snprintf(buf, sizeof(buf),
                "  B ntasks=%.1f nthreads=%.1f longest_task=%.3f task_sum=%.3f  "
                "C cap=%.3f copyout=%.3f leak=%.3f  eval %d kept %d realized %d (discard %.0f%%)\n",
                ntasks, nthreads, task_max, task_sum, cap, copy, leak,
                static_cast<int>(evaluated + 0.5), static_cast<int>(kept + 0.5),
                static_cast<int>(realized + 0.5),
                evaluated > 1e-6 ? (1.0 - kept / evaluated) * 100.0 : 0.0);
  out += buf;
  std::snprintf(buf, sizeof(buf),
                "  eval thread-sum births=%.3f filter=%.3f make_state=%.3f alpha=%.3f "
                "integrate=%.3f appear=%.3f trails=%.3f compact=%.3f stitch=%.3f "
                "submerge=%.3f walk=%.3f  cache lookup=%.3f store=%.3f  "
                "carry lookup=%.3f store=%.3f  noise=%.3f  pose_ctor=%.3f cache_key=%.3f "
                "stamp=%.3f adopt=%.3f hydrate=%.3f persist_cull=%.3f persist_commit=%.3f  "
                "unscoped=%.3f\n",
                sec_avg[static_cast<int>(SplitWallSec::Births)],
                sec_avg[static_cast<int>(SplitWallSec::BirthFilter)],
                sec_avg[static_cast<int>(SplitWallSec::MakeState)],
                sec_avg[static_cast<int>(SplitWallSec::AlphaPlan)],
                sec_avg[static_cast<int>(SplitWallSec::Integrate)],
                sec_avg[static_cast<int>(SplitWallSec::Appearance)],
                sec_avg[static_cast<int>(SplitWallSec::Trails)],
                sec_avg[static_cast<int>(SplitWallSec::Compaction)],
                sec_avg[static_cast<int>(SplitWallSec::Stitch)],
                sec_avg[static_cast<int>(SplitWallSec::SubemitMerge)],
                sec_avg[static_cast<int>(SplitWallSec::SubemitWalk)],
                sec_avg[static_cast<int>(SplitWallSec::CacheLookup)],
                sec_avg[static_cast<int>(SplitWallSec::CacheStore)],
                sec_avg[static_cast<int>(SplitWallSec::CarryLookup)],
                sec_avg[static_cast<int>(SplitWallSec::CarryStore)],
                sec_avg[static_cast<int>(SplitWallSec::Noise)],
                sec_avg[static_cast<int>(SplitWallSec::PoseCtor)],
                sec_avg[static_cast<int>(SplitWallSec::CacheKey)],
                sec_avg[static_cast<int>(SplitWallSec::Stamp)],
                sec_avg[static_cast<int>(SplitWallSec::AdoptCarry)],
                sec_avg[static_cast<int>(SplitWallSec::PoseHydrate)],
                sec_avg[static_cast<int>(SplitWallSec::PersistCull)],
                sec_avg[static_cast<int>(SplitWallSec::PersistCommit)], unscoped);
  out += buf;
  std::snprintf(buf, sizeof(buf),
                "  eval extra capsort=%.3f parent_ev=%.3f child_fill=%.3f child_cap=%.3f "
                "subsetup=%.3f persist_claim=%.3f realize_match=%.3f plan_gate=%.3f "
                "vec_grow=%.3f cf_hash=%.3f cf_keep=%.3f cf_ident=%.3f pe_pose_at=%.3f\n",
                sec_avg[static_cast<int>(SplitWallSec::CapSort)],
                sec_avg[static_cast<int>(SplitWallSec::ParentEvents)],
                sec_avg[static_cast<int>(SplitWallSec::ChildFill)],
                sec_avg[static_cast<int>(SplitWallSec::ChildCap)],
                sec_avg[static_cast<int>(SplitWallSec::SubemitSetup)],
                sec_avg[static_cast<int>(SplitWallSec::PersistClaim)],
                sec_avg[static_cast<int>(SplitWallSec::RealizeMatch)],
                sec_avg[static_cast<int>(SplitWallSec::PlanGate)],
                sec_avg[static_cast<int>(SplitWallSec::VecGrow)],
                sec_avg[static_cast<int>(SplitWallSec::CfHash)],
                sec_avg[static_cast<int>(SplitWallSec::CfKeep)],
                sec_avg[static_cast<int>(SplitWallSec::CfIdent)],
                sec_avg[static_cast<int>(SplitWallSec::PePoseAt)]);
  out += buf;
  std::snprintf(buf, sizeof(buf),
                "  serial extra capselect=%.3f assemble=%.3f lockwait=%.3f pool_wake=%.3f "
                "pool_join=%.3f task_setup=%.3f\n",
                sec_avg[static_cast<int>(SplitWallSec::CapSelect)],
                sec_avg[static_cast<int>(SplitWallSec::Assemble)],
                sec_avg[static_cast<int>(SplitWallSec::LockWait)],
                sec_avg[static_cast<int>(SplitWallSec::PoolWake)],
                sec_avg[static_cast<int>(SplitWallSec::PoolJoin)],
                sec_avg[static_cast<int>(SplitWallSec::TaskSetup)]);
  out += buf;
  std::snprintf(buf, sizeof(buf), "  noise thread-sum=%.3f\n",
                sec_avg[static_cast<int>(SplitWallSec::Noise)]);
  out += buf;
  static const bool dump_frames = [] {
    const char* raw = std::getenv("WDS_SPLIT_WALL_FRAMES");
    return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
  }();
  if (dump_frames) {
    const int noise_i = static_cast<int>(SplitWallSec::Noise);
    const int frame_i = static_cast<int>(SplitWallSec::Frame);
    for (int i = 0; i < n; ++i) {
      const double fms = static_cast<double>(g_window[static_cast<size_t>(i)].sum[frame_i]) / 1e6;
      const double nms = static_cast<double>(g_window[static_cast<size_t>(i)].sum[noise_i]) / 1e6;
      std::snprintf(buf, sizeof(buf), "  wall_frame i=%d frame_ms=%.3f noise_ms=%.3f\n", i, fms,
                    nms);
      out += buf;
    }
  }
  if (wmax > 0 && b_for_util > 1e-9) {
    out += "  workers (busy/idle ms, util):";
    for (int w = 0; w < wmax; ++w) {
      const double busy = worker_busy_avg[w] / static_cast<double>(n) / 1e6;
      const double idle = std::max(0.0, b_for_util - busy);
      const double wu = busy / b_for_util;
      std::snprintf(buf, sizeof(buf), "  w%d=%.2f/%.2f (%.0f%%)", w, busy, idle, wu * 100.0);
      out += buf;
    }
    out += "\n";
  }
  g_window.clear();
  return out;
}

}  // namespace wds::chart_render

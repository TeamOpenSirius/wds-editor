#include "split_collect_pool.hpp"
#include "split_wall_profile.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <wds/chart_render/split_effect_particles.hpp>

#include <wds/common/crash_handler.hpp>
#include <wds/common/perf_stages.hpp>

#ifndef _WIN32
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <cstdio>
#include <cstring>
#include <dirent.h>
#endif

namespace wds::chart_render {
namespace {

struct Topo {
  int p_phys = 0;
  int e_phys = 0;
  int logical = 0;
  int cap = 1;
  int burst_cap = 1;
  bool threads_overridden = false;
};

int clamp_i(int v, int lo, int hi) noexcept {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

int env_thread_override() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_THREADS");
  if (raw == nullptr || raw[0] == '\0') {
    return 0;
  }
  char* end = nullptr;
  const long v = std::strtol(raw, &end, 10);
  if (end == raw) {
    return 0;
  }
  if (v <= 1) {
    return 1;
  }
  return v > 64 ? 64 : static_cast<int>(v);
}

#ifdef __APPLE__
int sysctl_int(const char* name) noexcept {
  int v = 0;
  size_t sz = sizeof(v);
  if (sysctlbyname(name, &v, &sz, nullptr, 0) != 0) {
    return 0;
  }
  return v;
}
#endif

#ifdef _WIN32
void fill_topo_windows(Topo& t) noexcept {
  t.logical = static_cast<int>(std::thread::hardware_concurrency());
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len == 0) {
    return;
  }
  std::vector<unsigned char> buf(len);
  auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) {
    return;
  }
    int class0 = 0;
    int class_hi = 0;
    unsigned char* cur = buf.data();
    unsigned char* end = buf.data() + len;
    while (cur < end) {
      auto* rec = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(cur);
      if (rec->Relationship == RelationProcessorCore) {
        // MSDN: 0 = most efficient (E) when a classification exists;
        // 9th-gen / Zen3 have no classification and stay 0.
        // PROCESSOR_RELATIONSHIP is { BYTE Flags; BYTE EfficiencyClass; BYTE Reserved[20]; ... } in the
        // Windows SDK, but older MinGW-w64 headers declare Flags + Reserved[21] and have no
        // EfficiencyClass member, so read that byte by offset instead of by name.
        static_assert(offsetof(PROCESSOR_RELATIONSHIP, Flags) == 0, "Flags is the first byte");
        const unsigned efficiency_class =
            reinterpret_cast<const unsigned char*>(&rec->Processor)[1];
        if (efficiency_class == 0) {
          ++class0;
        } else {
          ++class_hi;
        }
      }
      cur += rec->Size;
    }
    if (class_hi > 0) {
      t.p_phys = class_hi;
      t.e_phys = class0;
    } else {
      t.p_phys = class0;
      t.e_phys = 0;
    }
}
#endif

#ifdef __linux__
int read_sysfs_int(const char* path, int missing) noexcept {
  FILE* f = std::fopen(path, "r");
  if (f == nullptr) {
    return missing;
  }
  int v = missing;
  if (std::fscanf(f, "%d", &v) != 1) {
    v = missing;
  }
  std::fclose(f);
  return v;
}

int sibling_min_cpu(const char* path, int self) noexcept {
  FILE* f = std::fopen(path, "r");
  if (f == nullptr) {
    return self;
  }
  char buf[256];
  if (std::fgets(buf, sizeof(buf), f) == nullptr) {
    std::fclose(f);
    return self;
  }
  std::fclose(f);
  int min_id = self;
  const char* p = buf;
  while (*p != '\0') {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') {
      ++p;
    }
    if (*p < '0' || *p > '9') {
      break;
    }
    int a = 0;
    while (*p >= '0' && *p <= '9') {
      a = a * 10 + (*p++ - '0');
    }
    int b = a;
    if (*p == '-') {
      ++p;
      b = 0;
      while (*p >= '0' && *p <= '9') {
        b = b * 10 + (*p++ - '0');
      }
    }
    if (a < min_id) {
      min_id = a;
    }
    if (b < min_id) {
      min_id = b;
    }
  }
  return min_id;
}

void fill_topo_linux(Topo& t) noexcept {
  t.logical = static_cast<int>(std::thread::hardware_concurrency());
  DIR* dir = opendir("/sys/devices/system/cpu");
  if (dir == nullptr) {
    return;
  }
  SplitCollectLinuxCpu rows[256];
  int n = 0;
  while (dirent* de = readdir(dir)) {
    if (std::strncmp(de->d_name, "cpu", 3) != 0) {
      continue;
    }
    const char* num = de->d_name + 3;
    if (num[0] < '0' || num[0] > '9') {
      continue;
    }
    if (n >= 256) {
      break;
    }
    const int cpu = std::atoi(num);
    char path[320];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/%s/topology/core_id", de->d_name);
    const int core = read_sysfs_int(path, -1);
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/%s/topology/physical_package_id",
                  de->d_name);
    const int pkg = read_sysfs_int(path, 0);
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/%s/cpu_capacity", de->d_name);
    const int cap = read_sysfs_int(path, -1);
    SplitCollectLinuxCpu row;
    row.package_id = pkg;
    if (core >= 0) {
      row.core_id = core;
    } else {
      std::snprintf(path, sizeof(path),
                    "/sys/devices/system/cpu/%s/topology/thread_siblings_list", de->d_name);
      row.core_id = sibling_min_cpu(path, cpu);
    }
    row.capacity = cap;
    rows[n++] = row;
  }
  closedir(dir);
  int p = 0;
  int e = 0;
  split_collect_count_phys_linux(rows, static_cast<size_t>(n), &p, &e);
  t.p_phys = p > 0 ? p : (t.logical > 0 ? t.logical : 4);
  t.e_phys = e;
}
#endif

Topo detect_topo() noexcept {
  Topo t;
  t.logical = static_cast<int>(std::thread::hardware_concurrency());
#ifdef __APPLE__
  t.p_phys = sysctl_int("hw.perflevel0.physicalcpu");
  t.e_phys = sysctl_int("hw.perflevel1.physicalcpu");
  if (t.p_phys <= 0) {
    t.p_phys = sysctl_int("hw.physicalcpu");
  }
  if (t.logical <= 0) {
    t.logical = sysctl_int("hw.logicalcpu");
  }
#elif defined(_WIN32)
  fill_topo_windows(t);
#elif defined(__linux__)
  fill_topo_linux(t);
#endif
  if (t.p_phys <= 0) {
    t.p_phys = t.logical > 0 ? t.logical : 4;
  }
  if (t.logical <= 0) {
    t.logical = t.p_phys;
  }
  const int ov = env_thread_override();
  if (ov > 0) {
    t.threads_overridden = true;
    t.cap = ov;
    t.burst_cap = ov;
    return t;
  }
  const SplitCollectCaps caps = split_collect_caps_from_phys(t.p_phys, t.e_phys);
  t.cap = caps.cap;
  t.burst_cap = caps.burst_cap;
  return t;
}

const Topo& topo() noexcept {
  static const Topo t = detect_topo();
  return t;
}

#ifdef _WIN32
void win_set_heavy(bool heavy) noexcept {
  HANDLE th = GetCurrentThread();
  SetThreadPriority(th, heavy ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL);
  // THREAD_POWER_THROTTLING_STATE / ThreadPowerThrottling are missing from older MinGW-w64 headers, and
  // SetThreadInformation itself only exists from Windows 8. Mirror the SDK layout and resolve the call at
  // runtime; where it is unavailable the priority change above is all that is applied.
  struct ThreadPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
  };
  constexpr ULONG kCurrentVersion = 1;        // THREAD_POWER_THROTTLING_CURRENT_VERSION
  constexpr ULONG kExecutionSpeed = 0x1;      // THREAD_POWER_THROTTLING_EXECUTION_SPEED
  constexpr int kThreadPowerThrottling = 3;   // THREAD_INFORMATION_CLASS::ThreadPowerThrottling
  using SetThreadInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
  static const SetThreadInformationFn set_thread_information = []() -> SetThreadInformationFn {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<SetThreadInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadInformation")));
  }();
  if (set_thread_information == nullptr) {
    return;
  }
  ThreadPowerThrottlingState s{};
  s.Version = kCurrentVersion;
  s.ControlMask = kExecutionSpeed;
  s.StateMask = heavy ? 0 : kExecutionSpeed;
  set_thread_information(th, kThreadPowerThrottling, &s, static_cast<DWORD>(sizeof(s)));
}
#endif

void set_worker_qos(bool heavy) noexcept {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(heavy ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
  win_set_heavy(heavy);
#else
  (void)heavy;
#endif
}

thread_local int tls_worker_id = 0;
thread_local int tls_claim_depth = 0;
thread_local int tls_qos_state = -1;

void apply_worker_qos(bool heavy) noexcept {
  const int want = heavy ? 1 : 0;
  if (tls_qos_state == want) {
    return;
  }
  tls_qos_state = want;
  set_worker_qos(heavy);
}

std::atomic<int> g_last_participants{0};
std::atomic<int> g_frame_max_participants{0};
std::atomic<uint64_t> g_frame_wakeups{0};
std::atomic<uint64_t> g_work_hint{0};
std::atomic<int> g_work_tasks{0};
std::atomic<int> g_allow_burst{0};
std::atomic<int64_t> g_spin_until_ns{0};

// Diagnostics only (WDS_FRAME_DIAG). All updates sit behind perf::enabled();
// with diagnostics off these are never touched, and the frame reset only
// clears the per-call worst columns, so the cumulative columns stay valid
// across frames. Not part of scheduling: no decision reads these.
std::atomic<uint64_t> g_diag_tasks{0};        // pool tasks created this frame
std::atomic<uint64_t> g_diag_wakeups{0};      // notify_one issued this frame
std::atomic<uint64_t> g_diag_wait_us{0};      // caller blocked in cv_done_
std::atomic<uint64_t> g_diag_spin_us{0};      // caller claimed its own work
std::atomic<uint64_t> g_diag_inline{0};       // hint below threshold
std::atomic<uint64_t> g_diag_serial{0};       // cap<=1 or a single item: no pool
std::atomic<uint64_t> g_diag_run_inline{0};   // kMaxJobs exhausted: ran inline
std::atomic<uint64_t> g_diag_par_tasks{0};    // calls that published a Job
std::atomic<uint64_t> g_diag_sum_item_us{0};  // Σ top-level claim µs this frame
std::atomic<uint64_t> g_diag_wall_us{0};      // Σ wall µs of those pool waves
// Claim attribution: a task executed on the caller vs on a pool worker. This
// is the "is the pool helping or is the GUI thread doing everything" split.
std::atomic<uint64_t> g_diag_claims_caller{0};
std::atomic<uint64_t> g_diag_claims_worker{0};
std::atomic<uint64_t> g_diag_items_caller{0};
std::atomic<uint64_t> g_diag_items_worker{0};
// Time the caller blocked in cv_done_ (a subset of wall_us).
std::atomic<uint64_t> g_diag_join_us{0};
// Cumulative POOLSTAT columns (reset only by split_collect_diag_reset).
std::atomic<uint64_t> g_poolstat_tasks{0};
std::atomic<uint64_t> g_poolstat_participants_max{0};
std::atomic<uint64_t> g_poolstat_wait_us{0};
std::atomic<uint64_t> g_poolstat_spin_us{0};
std::atomic<uint64_t> g_poolstat_inline{0};
std::atomic<uint64_t> g_poolstat_serial{0};

inline uint64_t diag_load(const std::atomic<uint64_t>& slot) noexcept {
  return slot.load(std::memory_order_relaxed);
}

inline uint64_t diag_cum_max(std::atomic<uint64_t>& slot, uint64_t v) noexcept {
  uint64_t cur = slot.load(std::memory_order_relaxed);
  while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
  return v;
}

int64_t steady_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool heavy_hold_active() noexcept {
  return steady_now_ns() < g_spin_until_ns.load(std::memory_order_acquire);
}

bool burst_env_on() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_BURST");
  if (raw == nullptr || raw[0] == '\0') {
    // Measured cold A/B: waking E-cores did not help (10380/11711). Keep
    // the burst path behind WDS_SPLIT_BURST=1.
    return false;
  }
  return raw[0] == '1' && raw[1] == '\0';
}

int choose_participants(size_t nitems, uint64_t hint, [[maybe_unused]] bool record_diag = true) noexcept {
  const int cap = (g_allow_burst.load(std::memory_order_relaxed) != 0 && burst_env_on())
                      ? topo().burst_cap
                      : topo().cap;
  if (cap <= 1 || nitems <= 1) {
#if WDS_DIAG_ENABLED
    // Diagnostics: no worker can help (cap 1) or there is nothing to share.
    if (record_diag && wds::common::perf::enabled()) {
      g_diag_serial.fetch_add(1, std::memory_order_relaxed);
      g_poolstat_serial.fetch_add(1, std::memory_order_relaxed);
    }
#endif  // WDS_DIAG_ENABLED
    return 1;
  }
  // 512 / 8000 are microseconds of last_cost_us (wall time on this core).
  // After the first realize of a task the hint is already speed-correct, so a
  // startup FMADD/integrate calibration that scaled the thresholds would
  // double-count slower cores. The njob×steps formula is first-touch only
  // and was tuned as work-units on M4; it yields to last_cost_us immediately.
  int parts = 1;
  const bool hint_inline = hint < 512ull;
  if (hint_inline) {
    parts = 1;
  } else if (hint < 8000ull) {
    parts = clamp_i(2, 1, cap);
  } else {
    parts = cap;
  }
  if (static_cast<size_t>(parts) > nitems) {
    parts = static_cast<int>(nitems);
  }
  parts = parts < 1 ? 1 : parts;
#if WDS_DIAG_ENABLED
  // Diagnostics only: "the hint was below the threshold so work was inlined"
  // is exactly the hint<512 decision. parts<=1 for another reason (nitems
  // clamp) is a serial call, not a hint inline.
  if (record_diag && wds::common::perf::enabled() && hint_inline) {
    g_diag_inline.fetch_add(1, std::memory_order_relaxed);
    g_poolstat_inline.fetch_add(1, std::memory_order_relaxed);
  }
#endif  // WDS_DIAG_ENABLED
  return parts;
}

void record_call_participants(int parts) noexcept {
  g_last_participants.store(parts, std::memory_order_relaxed);
  int cur = g_frame_max_participants.load(std::memory_order_relaxed);
  while (parts > cur && !g_frame_max_participants.compare_exchange_weak(
                            cur, parts, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
#if WDS_DIAG_ENABLED
  if (parts > 0 && wds::common::perf::enabled()) {
    diag_cum_max(g_poolstat_participants_max, static_cast<uint64_t>(parts));
  }
#endif  // WDS_DIAG_ENABLED
}

class SplitCollectPool {
 public:
  explicit SplitCollectPool(int extra_workers) {
    extra_workers = extra_workers < 0 ? 0 : extra_workers;
    extra_cap_ = extra_workers;
    for (int i = 0; i < kMaxJobs; ++i) {
      free_[i] = &storage_[i];
    }
    free_count_ = kMaxJobs;
    workers_.reserve(static_cast<size_t>(extra_workers));
  }

  SplitCollectPool(const SplitCollectPool&) = delete;
  SplitCollectPool& operator=(const SplitCollectPool&) = delete;

  ~SplitCollectPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_wake_.notify_all();
    for (std::thread& t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  void wake_for_heavy() {
    ensure_workers(std::max(0, topo().cap - 1));
    {
      // Taking mu_ orders this notify after any worker's predicate check, so a worker that
      // just saw "no heavy hold" is already blocked in wait() and receives the wake-up.
      std::lock_guard<std::mutex> lock(mu_);
    }
    cv_wake_.notify_all();
  }

  void ensure_workers(int extra_needed) {
    if (extra_needed <= 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(spawn_mu_);
    while (static_cast<int>(workers_.size()) < extra_needed &&
           static_cast<int>(workers_.size()) < extra_cap_) {
      const int wid = static_cast<int>(workers_.size()) + 1;
      workers_.emplace_back([this, wid] {
        // Alternate signal stack so a stack overflow on this thread can still be reported.
        wds::common::install_thread_crash_stack();
        tls_worker_id = wid;
        worker_main();
      });
    }
  }

  void parallel_for(size_t begin, size_t end, size_t grain, int participants, void (*fn)(size_t, void*),
                    void* ctx) {
    if (begin >= end || fn == nullptr) {
      return;
    }
    if (grain < 1) {
      grain = 1;
    }
    if (participants < 1) {
      participants = 1;
    }
    // Diagnostics only: one clock read for the whole section, taken after the
    // early-outs so an empty call costs nothing. Never allocates.
#if WDS_DIAG_ENABLED
    const bool diag = wds::common::perf::enabled();
    const auto diag_t0 = diag ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
#endif  // WDS_DIAG_ENABLED
    Job* job = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (free_count_ > 0) {
        job = free_[--free_count_];
        job->next.store(begin, std::memory_order_relaxed);
        job->finished.store(0, std::memory_order_relaxed);
        job->begin = begin;
        job->end = end;
        job->grain = grain;
        job->fn = fn;
        job->ctx = ctx;
        job->error = nullptr;
        job->id = ++next_id_;
        job->max_workers = participants;
        active_[job_count_++] = job;
      }
    }
    if (job == nullptr) {
      // All kMaxJobs slots busy (nested publish overflow): run inline.
#if WDS_DIAG_ENABLED
      if (diag) {
        g_diag_run_inline.fetch_add(1, std::memory_order_relaxed);
      }
#endif  // WDS_DIAG_ENABLED
      for (size_t i = begin; i < end; ++i) {
        fn(i, ctx);
      }
      return;
    }
#if WDS_DIAG_ENABLED
    if (diag) {
      g_diag_tasks.fetch_add(1, std::memory_order_relaxed);
      g_diag_par_tasks.fetch_add(1, std::memory_order_relaxed);
      g_poolstat_tasks.fetch_add(1, std::memory_order_relaxed);
    }
#endif  // WDS_DIAG_ENABLED
    if (participants > 1) {
      ensure_workers(participants - 1);
      // Wake only the workers we want. Parked extras stay blocked.
      {
        SplitWallScope wall_wake(SplitWallSec::PoolWake);
        for (int i = 0; i < participants - 1; ++i) {
          cv_wake_.notify_one();
          g_frame_wakeups.fetch_add(1, std::memory_order_relaxed);
#if WDS_DIAG_ENABLED
          if (diag) {
            g_diag_wakeups.fetch_add(1, std::memory_order_relaxed);
          }
#endif  // WDS_DIAG_ENABLED
        }
      }
    }

    const size_t nitems = end - begin;
    const uint64_t id = job->id;
#if WDS_DIAG_ENABLED
    uint64_t diag_wait_ns = 0;
#endif  // WDS_DIAG_ENABLED
    while (job->finished.load(std::memory_order_acquire) < nitems) {
      if (!run_one_claim()) {
        SplitWallScope wall_join(SplitWallSec::PoolJoin);
        std::unique_lock<std::mutex> lock(mu_);
        // Measured separately from the claim time so wait vs spin stays
        // separable. 0 cost when diagnostics are off.
#if WDS_DIAG_ENABLED
        const auto w0 = diag ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
#endif  // WDS_DIAG_ENABLED
        cv_done_.wait(lock, [&] {
          return job->finished.load(std::memory_order_acquire) >= nitems || has_claimable();
        });
#if WDS_DIAG_ENABLED
        if (diag) {
          diag_wait_ns += static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - w0)
                  .count());
        }
#endif  // WDS_DIAG_ENABLED
      }
    }
#if WDS_DIAG_ENABLED
    if (diag) {
      const int64_t wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - diag_t0)
                                  .count();
      const uint64_t wall_us =
          wall_ns > 0 ? static_cast<uint64_t>(wall_ns / 1000) : 0ull;
      const uint64_t wait_us = diag_wait_ns / 1000;
      // The caller is one of the participants, so its own claim time is part
      // of sum_item but not part of the "wait"; spin is the remainder.
      const uint64_t spin_us = wall_us > wait_us ? wall_us - wait_us : 0ull;
      g_diag_wall_us.fetch_add(wall_us, std::memory_order_relaxed);
      g_diag_wait_us.fetch_add(wait_us, std::memory_order_relaxed);
      g_diag_join_us.fetch_add(wait_us, std::memory_order_relaxed);
      g_diag_spin_us.fetch_add(spin_us, std::memory_order_relaxed);
      g_poolstat_wait_us.fetch_add(wait_us, std::memory_order_relaxed);
      g_poolstat_spin_us.fetch_add(spin_us, std::memory_order_relaxed);
    }
#endif  // WDS_DIAG_ENABLED

    std::exception_ptr error;
    {
      std::lock_guard<std::mutex> lock(mu_);
      error = std::move(job->error);
      job->error = nullptr;
      pop_job_locked(id);
    }
    if (error) {
      std::rethrow_exception(error);
    }
  }

 private:
  static constexpr int kMaxJobs = 32;

  struct Job {
    std::atomic<size_t> next{0};
    std::atomic<size_t> finished{0};
    size_t begin = 0;
    size_t end = 0;
    size_t grain = 1;
    void (*fn)(size_t, void*) = nullptr;
    void* ctx = nullptr;
    uint64_t id = 0;
    int max_workers = 1;
    std::exception_ptr error;  // first exception thrown by fn; guarded by mu_
  };

  // Restores tls_claim_depth even when fn throws.
  struct ClaimDepthGuard {
    ClaimDepthGuard() { ++tls_claim_depth; }
    ~ClaimDepthGuard() { --tls_claim_depth; }
    ClaimDepthGuard(const ClaimDepthGuard&) = delete;
    ClaimDepthGuard& operator=(const ClaimDepthGuard&) = delete;
  };

  struct Claim {
    Job* job = nullptr;
    size_t i0 = 0;
    size_t i1 = 0;
    void (*fn)(size_t, void*) = nullptr;
    void* ctx = nullptr;
    int max_workers = 1;
  };

  bool has_claimable() const {
    for (int i = 0; i < job_count_; ++i) {
      const Job* j = active_[i];
      if (j != nullptr && j->fn != nullptr && j->next.load(std::memory_order_relaxed) < j->end) {
        return true;
      }
    }
    return false;
  }

  bool claim(Claim& c) {
    // Take the range under mu_ so a worker cannot fetch_add / read fn after
    // parallel_for recycles the Job into a later publish (TSAN: write
    // begin/fn vs stale claim).
    std::lock_guard<std::mutex> lock(mu_);
    for (int ji = job_count_ - 1; ji >= 0; --ji) {
      Job* cand = active_[ji];
      if (cand == nullptr || cand->fn == nullptr) {
        continue;
      }
      const size_t grain = cand->grain < 1 ? 1 : cand->grain;
      const size_t i0 = cand->next.fetch_add(grain, std::memory_order_relaxed);
      if (i0 >= cand->end) {
        continue;
      }
      size_t i1 = i0 + grain;
      if (i1 > cand->end) {
        i1 = cand->end;
      }
      c.job = cand;
      c.i0 = i0;
      c.i1 = i1;
      c.fn = cand->fn;
      c.ctx = cand->ctx;
      c.max_workers = cand->max_workers;
      return true;
    }
    return false;
  }

  bool run_one_claim() {
    Claim c;
    if (!claim(c) || c.fn == nullptr || c.job == nullptr) {
      return false;
    }
    const bool heavy = c.max_workers >= 3;
    if (tls_worker_id != 0) {
      // Workers past the steady P-cap stay UTILITY so the kernel can park
      // them on E-cores. Steady play never wakes them. P-cap workers keep
      // USER_INTERACTIVE across back-to-back claims in the same drain
      // (nested parent_ev grain is thousands of claims; dropping QoS after
      // each one is a syscall + E-core demotion on every collect).
      if (tls_worker_id >= topo().cap) {
        apply_worker_qos(false);
      } else {
        apply_worker_qos(heavy);
      }
    }
#if WDS_DIAG_ENABLED
    const bool time_busy = split_wall_enabled() && tls_claim_depth == 0;
    // Diagnostics: claim time is the per-item work unit for parallel
    // efficiency. Only timed when perf is on, and only for top-level claims so
    // a nested publish does not count the same work twice.
    const bool time_diag = wds::common::perf::enabled() && tls_claim_depth == 0;
#endif  // WDS_DIAG_ENABLED
    std::exception_ptr err;
    {
      ClaimDepthGuard depth_guard;
#if WDS_DIAG_ENABLED
      const auto t0 = (time_busy || time_diag) ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point();
#endif  // WDS_DIAG_ENABLED
      try {
        for (size_t i = c.i0; i < c.i1; ++i) {
          c.fn(i, c.ctx);
        }
      } catch (...) {
        // Never unwind past the claim: the items must still be counted as finished so the
        // job completes, and the caller thread rethrows once the job is popped.
        err = std::current_exception();
      }
#if WDS_DIAG_ENABLED
      if (time_busy || time_diag) {
        const auto t1 = std::chrono::steady_clock::now();
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (time_busy) {
          split_wall_note_worker_busy(tls_worker_id, ns);
        }
        if (time_diag && ns > 0) {
          g_diag_sum_item_us.fetch_add(static_cast<uint64_t>(ns / 1000),
                                       std::memory_order_relaxed);
        }
      }
#endif  // WDS_DIAG_ENABLED
    }
#if WDS_DIAG_ENABLED
    if (wds::common::perf::enabled()) {
      const uint64_t got_items = static_cast<uint64_t>(c.i1 - c.i0);
      if (tls_worker_id != 0) {
        g_diag_claims_worker.fetch_add(1, std::memory_order_relaxed);
        g_diag_items_worker.fetch_add(got_items, std::memory_order_relaxed);
      } else {
        g_diag_claims_caller.fetch_add(1, std::memory_order_relaxed);
        g_diag_items_caller.fetch_add(got_items, std::memory_order_relaxed);
      }
    }
#endif  // WDS_DIAG_ENABLED
    if (err) {
      std::lock_guard<std::mutex> lock(mu_);
      if (!c.job->error) {
        c.job->error = err;
      }
    }
    const size_t got = c.i1 - c.i0;
    const size_t nitems = c.job->end - c.job->begin;
    const size_t prev = c.job->finished.fetch_add(got, std::memory_order_acq_rel);
    if (prev + got >= nitems) {
      std::lock_guard<std::mutex> lock(mu_);
      cv_done_.notify_all();
    }
    return true;
  }

  void pop_job_locked(uint64_t id) {
    for (int i = 0; i < job_count_; ++i) {
      Job* j = active_[i];
      if (j == nullptr || j->id != id) {
        continue;
      }
      j->fn = nullptr;
      j->ctx = nullptr;
      active_[i] = active_[job_count_ - 1];
      active_[job_count_ - 1] = nullptr;
      --job_count_;
      free_[free_count_++] = j;
      // No notify: removing a job never makes a worker's wait predicate (stop_ / claimable work /
      // heavy hold) true, so waking parked workers here would only make them re-sleep.
      return;
    }
  }

  void worker_main() {
    apply_worker_qos(false);
    for (;;) {
      if (heavy_hold_active()) {
        if (tls_worker_id < topo().cap) {
          apply_worker_qos(true);
        }
        while (run_one_claim()) {
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (stop_) {
            return;
          }
        }
        std::this_thread::yield();
        continue;
      }
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_wake_.wait(lock, [&] {
          return stop_ || has_claimable() || heavy_hold_active();
        });
        if (stop_) {
          return;
        }
      }
      while (run_one_claim()) {
      }
      if (!heavy_hold_active()) {
        apply_worker_qos(false);
      }
    }
  }

  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::mutex spawn_mu_;
  std::condition_variable cv_wake_;
  std::condition_variable cv_done_;
  Job storage_[kMaxJobs]{};
  Job* active_[kMaxJobs]{};
  Job* free_[kMaxJobs]{};
  int job_count_ = 0;
  int free_count_ = 0;
  int extra_cap_ = 0;
  uint64_t next_id_ = 0;
  bool stop_ = false;
};

// Not std::call_once: after_fork_child must be able to reset the pool so the child re-creates it.
std::atomic<SplitCollectPool*> g_pool{nullptr};
std::mutex g_pool_mu;  // serialises creation and the atfork registration flag

#ifndef _WIN32
// Hold g_pool_mu across fork() so the child never inherits it locked by a vanished thread.
void before_fork() {
  g_pool_mu.lock();
}

void after_fork_parent() {
  g_pool_mu.unlock();
}

void after_fork_child() {
  g_pool.store(nullptr, std::memory_order_release);
  g_pool_mu.unlock();
}

// Caller holds g_pool_mu.
void ensure_atfork_locked() {
  static bool registered = false;
  if (registered) {
    return;
  }
  pthread_atfork(before_fork, after_fork_parent, after_fork_child);
  registered = true;
}
#endif

SplitCollectPool& ensure_pool(int threads) {
  SplitCollectPool* p = g_pool.load(std::memory_order_acquire);
  if (p != nullptr) {
    return *p;
  }
  std::lock_guard<std::mutex> lock(g_pool_mu);
  p = g_pool.load(std::memory_order_relaxed);
  if (p == nullptr) {
#ifndef _WIN32
    ensure_atfork_locked();
#endif
    const int extra = threads > 1 ? threads - 1 : 0;
    p = new SplitCollectPool(extra);
    g_pool.store(p, std::memory_order_release);
  }
  return *p;
}

size_t adaptive_grain(size_t n, int threads) noexcept {
  if (n <= 1 || threads <= 1) {
    return 1;
  }
  // More, smaller tasks so long-lived systems drain together.
  const size_t chunks = static_cast<size_t>(threads) * 8u;
  const size_t g = n / chunks;
  return g < 1 ? 1 : g;
}

}  // namespace

SplitCollectCaps split_collect_caps_from_phys(int p_phys, int e_phys) noexcept {
  if (p_phys < 0) {
    p_phys = 0;
  }
  if (e_phys < 0) {
    e_phys = 0;
  }
  // All-P and P<=6 (Zen3 5600X / 9th-gen 6c): leave ≥2 physical cores for
  // UI/audio. 4P hybrid M-series keeps 4 — UI/audio run on E-cores.
  // Hybrid P>4 leaves one P (UI can also use E). Clamp 8 / burst 16.
  int cap = p_phys;
  if (e_phys <= 0 && p_phys <= 6) {
    cap = p_phys - 2;
  } else if (p_phys > 4) {
    cap = p_phys - 1;
  }
  if (cap < 1) {
    cap = 1;
  }
  if (cap > 8) {
    cap = 8;
  }
  int burst = cap;
  if (e_phys > 0) {
    int all = p_phys + e_phys;
    if (all > 2) {
      all -= 2;
    }
    if (all > burst) {
      burst = all;
    }
  }
  if (burst < 1) {
    burst = 1;
  }
  if (burst > 16) {
    burst = 16;
  }
  SplitCollectCaps out;
  out.cap = cap;
  out.burst_cap = burst;
  return out;
}

void split_collect_count_phys_linux(const SplitCollectLinuxCpu* rows, size_t n, int* p_phys,
                                    int* e_phys) noexcept {
  if (p_phys != nullptr) {
    *p_phys = 0;
  }
  if (e_phys != nullptr) {
    *e_phys = 0;
  }
  if (rows == nullptr || n == 0) {
    return;
  }
  struct Acc {
    int pkg = 0;
    int core = 0;
    int cap = -1;
  };
  Acc acc[256];
  int nacc = 0;
  int max_cap = 0;
  bool any_cap = false;
  for (size_t i = 0; i < n; ++i) {
    const SplitCollectLinuxCpu& r = rows[i];
    int found = -1;
    for (int j = 0; j < nacc; ++j) {
      if (acc[j].pkg == r.package_id && acc[j].core == r.core_id) {
        found = j;
        break;
      }
    }
    if (found < 0) {
      if (nacc >= 256) {
        continue;
      }
      found = nacc++;
      acc[found].pkg = r.package_id;
      acc[found].core = r.core_id;
      acc[found].cap = r.capacity;
    } else if (r.capacity > acc[found].cap) {
      acc[found].cap = r.capacity;
    }
    if (r.capacity >= 0) {
      any_cap = true;
      if (r.capacity > max_cap) {
        max_cap = r.capacity;
      }
    }
  }
  int p = 0;
  int e = 0;
  for (int j = 0; j < nacc; ++j) {
    if (any_cap && max_cap > 0 && acc[j].cap >= 0 && acc[j].cap * 2 < max_cap) {
      ++e;
    } else {
      ++p;
    }
  }
  if (p_phys != nullptr) {
    *p_phys = p;
  }
  if (e_phys != nullptr) {
    *e_phys = e;
  }
}

int split_collect_thread_count() noexcept {
  return topo().cap;
}

int split_collect_pool_cap() noexcept {
  return topo().cap;
}

int split_collect_p_cores() noexcept {
  return topo().p_phys;
}

int split_collect_e_cores() noexcept {
  return topo().e_phys;
}

int split_collect_burst_cap() noexcept {
  return topo().burst_cap;
}

void split_collect_set_burst(bool on) noexcept {
  g_allow_burst.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool split_collect_burst_allowed() noexcept {
  return g_allow_burst.load(std::memory_order_relaxed) != 0 && burst_env_on();
}

int split_collect_last_participants() noexcept {
  return g_last_participants.load(std::memory_order_relaxed);
}

int split_collect_last_call_participants() noexcept {
  return split_collect_last_participants();
}

int split_collect_frame_max_participants() noexcept {
  return g_frame_max_participants.load(std::memory_order_relaxed);
}

uint64_t split_collect_frame_wakeups() noexcept {
  return g_frame_wakeups.load(std::memory_order_relaxed);
}

void split_collect_begin_frame() noexcept {
  g_work_hint.store(0, std::memory_order_relaxed);
  g_work_tasks.store(0, std::memory_order_relaxed);
  g_last_participants.store(0, std::memory_order_relaxed);
  g_frame_max_participants.store(0, std::memory_order_relaxed);
  g_frame_wakeups.store(0, std::memory_order_relaxed);
  g_allow_burst.store(0, std::memory_order_relaxed);
#if WDS_DIAG_ENABLED
  if (!wds::common::perf::enabled()) {
    return;
  }
  // Per-frame diagnostics columns. Cumulative POOLSTAT columns deliberately
  // survive; they are only cleared by split_collect_diag_reset().
  g_diag_tasks.store(0, std::memory_order_relaxed);
  g_diag_wakeups.store(0, std::memory_order_relaxed);
  g_diag_wait_us.store(0, std::memory_order_relaxed);
  g_diag_spin_us.store(0, std::memory_order_relaxed);
  g_diag_inline.store(0, std::memory_order_relaxed);
  g_diag_serial.store(0, std::memory_order_relaxed);
  g_diag_run_inline.store(0, std::memory_order_relaxed);
  g_diag_par_tasks.store(0, std::memory_order_relaxed);
  g_diag_sum_item_us.store(0, std::memory_order_relaxed);
  g_diag_wall_us.store(0, std::memory_order_relaxed);
  g_diag_claims_caller.store(0, std::memory_order_relaxed);
  g_diag_claims_worker.store(0, std::memory_order_relaxed);
  g_diag_items_caller.store(0, std::memory_order_relaxed);
  g_diag_items_worker.store(0, std::memory_order_relaxed);
  g_diag_join_us.store(0, std::memory_order_relaxed);
#endif  // WDS_DIAG_ENABLED
}

int split_collect_choose_participants(size_t nitems, uint64_t work_hint) noexcept {
  return choose_participants(nitems, work_hint, false);
}

void split_collect_prepare_heavy(int hold_ms) noexcept {
  if (hold_ms < 1) {
    hold_ms = 1;
  }
  if (hold_ms > 200) {
    hold_ms = 200;
  }
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms);
  g_spin_until_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch())
                            .count(),
                        std::memory_order_release);
  if (topo().cap <= 1) {
    return;
  }
  ensure_pool(topo().burst_cap).wake_for_heavy();
}

void split_collect_note_work(size_t ntasks, uint64_t step_hint) noexcept {
  g_work_tasks.store(static_cast<int>(ntasks > 0x7fffffffull ? 0x7fffffff : ntasks),
                     std::memory_order_relaxed);
  g_work_hint.store(step_hint, std::memory_order_relaxed);
}

int split_collect_claim_depth() noexcept {
  return tls_claim_depth;
}

#if WDS_DIAG_ENABLED
SplitCollectPoolDiag split_collect_pool_diag_frame() noexcept {
  SplitCollectPoolDiag d;
  d.tasks = diag_load(g_diag_tasks);
  d.wakeups = diag_load(g_diag_wakeups);
  d.wait_us = diag_load(g_diag_wait_us);
  d.spin_us = diag_load(g_diag_spin_us);
  d.inline_calls = diag_load(g_diag_inline);
  d.serial_calls = diag_load(g_diag_serial);
  d.run_inline_calls = diag_load(g_diag_run_inline);
  d.par_calls = diag_load(g_diag_par_tasks);
  d.sum_item_us = diag_load(g_diag_sum_item_us);
  d.wall_us = diag_load(g_diag_wall_us);
  d.claims_caller = diag_load(g_diag_claims_caller);
  d.claims_worker = diag_load(g_diag_claims_worker);
  d.items_caller = diag_load(g_diag_items_caller);
  d.items_worker = diag_load(g_diag_items_worker);
  d.join_us = diag_load(g_diag_join_us);
  return d;
}
#endif  // WDS_DIAG_ENABLED

SplitCollectPoolStat split_collect_pool_stat() noexcept {
  SplitCollectPoolStat s;
  s.tasks = diag_load(g_poolstat_tasks);
  s.participants_max = diag_load(g_poolstat_participants_max);
  s.wait_us_sum = diag_load(g_poolstat_wait_us);
  s.spin_us_sum = diag_load(g_poolstat_spin_us);
  s.inline_calls = diag_load(g_poolstat_inline);
  s.serial_calls = diag_load(g_poolstat_serial);
  return s;
}

void split_collect_diag_reset() noexcept {
  std::atomic<uint64_t>* slots[] = {&g_poolstat_tasks,     &g_poolstat_participants_max,
                                    &g_poolstat_wait_us,   &g_poolstat_spin_us,
                                    &g_poolstat_inline,    &g_poolstat_serial};
  for (std::atomic<uint64_t>* p : slots) {
    p->store(0, std::memory_order_relaxed);
  }
}

void split_collect_parallel_for(size_t begin, size_t end, size_t grain, void (*fn)(size_t, void*),
                                void* ctx, uint64_t work_hint) {
  if (begin >= end || fn == nullptr) {
    record_call_participants(0);
    return;
  }
  const size_t n = end - begin;
  const int cap = split_collect_thread_count();
  const int parts = choose_participants(n, work_hint);
  record_call_participants(parts);
  if (cap <= 1 || parts <= 1 || n == 1) {
    // Fallback: fn runs on the calling thread, no Job, no wake-up.
    // choose_participants already counted the cap<=1 / single-item cases.
    for (size_t i = begin; i < end; ++i) {
      fn(i, ctx);
    }
    return;
  }
  if (grain == 0) {
    grain = adaptive_grain(n, parts);
  }
  ensure_pool(topo().burst_cap).parallel_for(begin, end, grain, parts, fn, ctx);
}

void split_collect_parallel_for(size_t begin, size_t end, size_t grain, void (*fn)(size_t, void*),
                                void* ctx) {
  const uint64_t hint = end > begin ? static_cast<uint64_t>(end - begin) : 0ull;
  split_collect_parallel_for(begin, end, grain, fn, ctx, hint);
}

void split_collect_parallel_for(size_t n, void (*fn)(size_t, void*), void* ctx) {
  split_collect_parallel_for(0, n, 0, fn, ctx);
}

}  // namespace wds::chart_render

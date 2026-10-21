#include <wds/common/perf_stages.hpp>

#if WDS_DIAG_ENABLED



#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace wds::common::perf {
namespace {

constexpr int kMaxStages = 256;
constexpr int kMaxWindowFrames = 4096;
// Threads that ran split-collect work. Bounded: the pool caps at ~8 workers,
// plus a few nested/eval one-offs. Never reallocates.
constexpr int kMaxPoolTraceThreads = 32;

// OS thread id, used only to attribute CPU per thread class in the diag log.
// macOS/Win32 match the ids the log's per-thread CPU sampler reports; other
// platforms fall back to a process-unique id (the log does not sample threads
// there, so the value is only used for identity comparisons).
uint64_t current_thread_id() noexcept {
#if defined(_WIN32)
  return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
  uint64_t tid = 0;
  if (pthread_threadid_np(nullptr, &tid) == 0) {
    return tid;
  }
  return 0;
#else
  static std::atomic<uint64_t> next{1};
  thread_local const uint64_t self = next.fetch_add(1, std::memory_order_relaxed);
  return self;
#endif
}

bool frame_diag_env_enabled(const char* value) noexcept {
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool frame_diag_perf_env_disabled(const char* value) noexcept {
  return value != nullptr && value[0] == '0' && value[1] == '\0';
}

std::atomic<bool> g_enabled{frame_diag_env_enabled(std::getenv("WDS_FRAME_DIAG")) &&
                            !frame_diag_perf_env_disabled(std::getenv("WDS_FRAME_DIAG_PERF"))};

struct ThreadSlot {
  std::atomic<int64_t> ns[kMaxStages]{};
  std::atomic<int64_t> counter[kMaxStages]{};
  std::atomic<int64_t> scope_calls[kMaxStages]{};
};

thread_local std::shared_ptr<ThreadSlot> tls_slot;
// Cached OS thread id of this thread (0 = not resolved yet). Only touched on
// the first split.collect.* scope of the thread, then one branch per call.
thread_local uint64_t tls_pool_tid = 0;

std::mutex g_mu;
std::vector<std::shared_ptr<ThreadSlot>> g_threads;
std::vector<std::string> g_stage_names;
std::atomic<uint8_t> g_stage_has_counter[kMaxStages]{};
std::atomic<uint8_t> g_stage_has_time[kMaxStages]{};
// Stage ids whose literal starts with "split.collect." (chart-render pool work).
std::atomic<uint8_t> g_stage_is_collect[kMaxStages]{};
// Threads seen running split-collect work (see pool_thread_ids()).
std::atomic<uint64_t> g_pool_tids[kMaxPoolTraceThreads]{};
std::atomic<int> g_pool_tid_n{0};

struct FrameTotals {
  std::vector<int64_t> ns;
  std::vector<int64_t> counter;
  std::vector<int64_t> calls;
};

// Fixed-size ring of per-frame totals. Slots are reused, so after the first
// kMaxWindowFrames frames end_frame() performs no allocation at all (the old
// std::vector push_back/erase churned three allocations per frame and shifted
// the whole window once it filled). g_mu is still held for the duration, but
// only the GUI thread calls end_frame(); workers never take it except in
// ensure_tls_registered() / stage_id(), both one-shot per thread.
std::vector<FrameTotals> g_ring;
size_t g_ring_head = 0;
size_t g_ring_size = 0;

FrameTotals& ring_slot_for_next_frame() noexcept {
  if (g_ring.empty()) {
    // One allocation for the whole process lifetime; every slot is reused.
    g_ring.resize(static_cast<size_t>(kMaxWindowFrames));
  }
  const size_t cap = g_ring.size();
  if (g_ring_size < cap) {
    FrameTotals& slot = g_ring[(g_ring_head + g_ring_size) % cap];
    ++g_ring_size;
    return slot;
  }
  FrameTotals& oldest = g_ring[g_ring_head];
  g_ring_head = (g_ring_head + 1) % cap;
  return oldest;
}

void ring_clear() noexcept {
  g_ring_head = 0;
  g_ring_size = 0;
}

void ensure_tls_registered() noexcept {
  if (tls_slot) {
    return;
  }
  auto slot = std::make_shared<ThreadSlot>();
  tls_slot = slot;
  std::lock_guard lock(g_mu);
  g_threads.push_back(slot);
}

// Records "this thread ran split-collect work" once per thread for the whole
// process; afterwards it is one compare of a thread_local.
void note_collect_thread() noexcept {
  if (tls_pool_tid != 0) {
    return;
  }
  const uint64_t tid = current_thread_id();
  tls_pool_tid = tid == 0 ? 1 : tid;  // 1 = resolved but unknown; never re-resolve
  if (tid == 0) {
    return;
  }
  // Reserve a slot atomically: two pool workers can register at the same
  // instant, and a lost tid would silently under-report pool_ms.
  const int slot = g_pool_tid_n.fetch_add(1, std::memory_order_relaxed);
  if (slot >= 0 && slot < kMaxPoolTraceThreads) {
    g_pool_tids[slot].store(tid, std::memory_order_relaxed);
  }
}

void record_scope_call(int id) noexcept {
  if (id < 0 || id >= kMaxStages) {
    return;
  }
  ensure_tls_registered();
  tls_slot->scope_calls[id].fetch_add(1, std::memory_order_relaxed);
  // One relaxed load per scope call: the flag is set by stage_id() before any
  // Scope for that stage exists, so no synchronisation is needed beyond it.
  if (g_stage_is_collect[id].load(std::memory_order_relaxed) != 0) {
    note_collect_thread();
  }
}

}  // namespace

bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_enabled(bool on) noexcept { g_enabled.store(on, std::memory_order_relaxed); }

int pool_thread_ids(uint64_t* out, int cap) noexcept {
  if (!enabled() || out == nullptr || cap <= 0) {
    return 0;
  }
  const int have = std::min(cap, g_pool_tid_n.load(std::memory_order_relaxed));
  int n = 0;
  for (int i = 0; i < have; ++i) {
    const uint64_t tid = g_pool_tids[i].load(std::memory_order_relaxed);
    if (tid != 0) {
      out[n++] = tid;
    }
  }
  return n;
}

int stage_id(const char* name) {
  if (name == nullptr) {
    return -1;
  }
  std::lock_guard lock(g_mu);
  for (int i = 0; i < static_cast<int>(g_stage_names.size()); ++i) {
    if (g_stage_names[static_cast<size_t>(i)] == name) {
      return i;
    }
  }
  if (static_cast<int>(g_stage_names.size()) >= kMaxStages) {
    return -1;
  }
  const int id = static_cast<int>(g_stage_names.size());
  g_stage_names.emplace_back(name);
  if (std::strncmp(name, "split.collect.", 14) == 0) {
    g_stage_is_collect[id].store(1, std::memory_order_relaxed);
  }
  return id;
}

void add_ns(int id, int64_t ns) noexcept {
  if (!enabled() || id < 0 || id >= kMaxStages || ns <= 0) {
    return;
  }
  ensure_tls_registered();
  g_stage_has_time[id].store(1, std::memory_order_relaxed);
  tls_slot->ns[id].fetch_add(ns, std::memory_order_relaxed);
}

void add_count(int id, int64_t n) noexcept {
  if (!enabled() || id < 0 || id >= kMaxStages || n == 0) {
    return;
  }
  ensure_tls_registered();
  g_stage_has_counter[id].store(1, std::memory_order_relaxed);
  tls_slot->counter[id].fetch_add(n, std::memory_order_relaxed);
}

Scope::Scope(int id) noexcept : id_(id), active_(id >= 0 && id < kMaxStages) {
  if (!active_) {
    return;
  }
  start_ = std::chrono::steady_clock::now();
}

Scope::~Scope() {
  if (!active_) {
    return;
  }
  const auto end = std::chrono::steady_clock::now();
  const int64_t ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
  add_ns(id_, ns);
  record_scope_call(id_);
}

void end_frame() noexcept {
  if (!enabled()) {
    return;
  }
  std::lock_guard lock(g_mu);
  const int nstages = static_cast<int>(g_stage_names.size());
  FrameTotals& frame = ring_slot_for_next_frame();
  if (static_cast<int>(frame.ns.size()) < nstages) {
    // Slots are reused; this only runs the first time a new stage id appears,
    // so steady-state end_frame() never reallocates.
    frame.ns.resize(static_cast<size_t>(nstages));
    frame.counter.resize(static_cast<size_t>(nstages));
    frame.calls.resize(static_cast<size_t>(nstages));
  }
  for (int i = 0; i < nstages; ++i) {
    frame.ns[static_cast<size_t>(i)] = 0;
    frame.counter[static_cast<size_t>(i)] = 0;
    frame.calls[static_cast<size_t>(i)] = 0;
  }
  for (const std::shared_ptr<ThreadSlot>& slot : g_threads) {
    for (int i = 0; i < nstages; ++i) {
      frame.ns[static_cast<size_t>(i)] += slot->ns[i].exchange(0, std::memory_order_relaxed);
      frame.counter[static_cast<size_t>(i)] +=
          slot->counter[i].exchange(0, std::memory_order_relaxed);
      frame.calls[static_cast<size_t>(i)] +=
          slot->scope_calls[i].exchange(0, std::memory_order_relaxed);
    }
  }
}

int window_frames() noexcept {
  std::lock_guard lock(g_mu);
  return static_cast<int>(g_ring_size);
}

// i-th frame in the window, oldest first. Only valid under g_mu.
const FrameTotals& ring_at(size_t i) noexcept {
  return g_ring[(g_ring_head + i) % g_ring.size()];
}

std::string window_report() {
  std::lock_guard lock(g_mu);
  const int nstages = static_cast<int>(g_stage_names.size());
  const int nframes = static_cast<int>(g_ring_size);
  if (nstages == 0 || nframes == 0 || g_ring.empty()) {
    ring_clear();
    return {};
  }

  struct Row {
    int id = 0;
    // Ordering key. avg_ms alone hid stages that are ~always zero but spike
    // on a few frames (post-seek cold collect): avg_ms + p95_ms ranks them by
    // "cost when it happens" while still putting a steadily expensive stage
    // first. Both numbers stay on the line, so the log is still parseable.
    double sort_key = 0.0;
    std::string line;
  };
  std::vector<Row> rows;
  rows.reserve(static_cast<size_t>(nstages));

  auto frame_ns = [](const FrameTotals& fr, int sid) -> int64_t {
    if (sid < 0 || static_cast<size_t>(sid) >= fr.ns.size()) {
      return 0;
    }
    return fr.ns[static_cast<size_t>(sid)];
  };
  auto frame_calls = [](const FrameTotals& fr, int sid) -> int64_t {
    if (sid < 0 || static_cast<size_t>(sid) >= fr.calls.size()) {
      return 0;
    }
    return fr.calls[static_cast<size_t>(sid)];
  };
  auto frame_counter = [](const FrameTotals& fr, int sid) -> int64_t {
    if (sid < 0 || static_cast<size_t>(sid) >= fr.counter.size()) {
      return 0;
    }
    return fr.counter[static_cast<size_t>(sid)];
  };

  auto emit_time_row = [&](int sid) {
    const char* name = g_stage_names[static_cast<size_t>(sid)].c_str();
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(nframes));
    for (int fi = 0; fi < nframes; ++fi) {
      const int64_t v = frame_ns(ring_at(static_cast<size_t>(fi)), sid);
      if (v > 0) {
        samples.push_back(static_cast<double>(v) / 1e6);
      }
    }
    if (samples.empty()) {
      return;
    }
    double sum_ms = 0.0;
    int64_t sum_calls = 0;
    for (int fi = 0; fi < nframes; ++fi) {
      const FrameTotals& fr = ring_at(static_cast<size_t>(fi));
      sum_ms += static_cast<double>(frame_ns(fr, sid)) / 1e6;
      sum_calls += frame_calls(fr, sid);
    }
    const double avg_ms = sum_ms / static_cast<double>(nframes);
    std::sort(samples.begin(), samples.end());
    const size_t p95_i =
        samples.empty() ? 0 : std::min(samples.size() - 1, (samples.size() * 95) / 100);
    const double p95_ms = samples[p95_i];
    const double max_ms = samples.back();
    const double calls_per_frame =
        static_cast<double>(sum_calls) / static_cast<double>(nframes);
    Row row;
    row.id = sid;
    row.sort_key = avg_ms + p95_ms;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s avg_ms=%.3f p95_ms=%.3f max_ms=%.3f calls/frame=%.2f", name,
                  avg_ms, p95_ms, max_ms, calls_per_frame);
    row.line = buf;
    rows.push_back(std::move(row));
  };

  auto emit_counter_row = [&](int sid) {
    const char* name = g_stage_names[static_cast<size_t>(sid)].c_str();
    int64_t sum = 0;
    int64_t max_val = 0;
    for (int fi = 0; fi < nframes; ++fi) {
      const int64_t v = frame_counter(ring_at(static_cast<size_t>(fi)), sid);
      sum += v;
      max_val = std::max(max_val, v);
    }
    if (sum == 0 && max_val == 0) {
      return;
    }
    const double avg = static_cast<double>(sum) / static_cast<double>(nframes);
    Row row;
    row.id = sid;
    row.sort_key = avg;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s avg=%.3g max=%lld", name, avg,
                  static_cast<long long>(max_val));
    row.line = buf;
    rows.push_back(std::move(row));
  };

  for (int sid = 0; sid < nstages; ++sid) {
    if (g_stage_has_time[sid].load(std::memory_order_relaxed) != 0) {
      emit_time_row(sid);
    }
    if (g_stage_has_counter[sid].load(std::memory_order_relaxed) != 0) {
      emit_counter_row(sid);
    }
  }

  // Highest "cost when it happens" first. Tie-break on the raw id so equal
  // keys keep a stable order between reports.
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.sort_key != b.sort_key) {
      return a.sort_key > b.sort_key;
    }
    return a.id < b.id;
  });

  std::string out;
  for (const Row& r : rows) {
    out += r.line;
    out += '\n';
  }
  ring_clear();
  return out;
}

}  // namespace wds::common::perf

#endif  // WDS_DIAG_ENABLED

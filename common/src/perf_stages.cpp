#include <wds/common/perf_stages.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wds::common::perf {
namespace {

constexpr int kMaxStages = 256;
constexpr int kMaxWindowFrames = 4096;

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

std::mutex g_mu;
std::vector<std::shared_ptr<ThreadSlot>> g_threads;
std::vector<std::string> g_stage_names;
std::atomic<uint8_t> g_stage_has_counter[kMaxStages]{};
std::atomic<uint8_t> g_stage_has_time[kMaxStages]{};

struct FrameTotals {
  std::vector<int64_t> ns;
  std::vector<int64_t> counter;
  std::vector<int64_t> calls;
};

std::vector<FrameTotals> g_window;

void ensure_tls_registered() noexcept {
  if (tls_slot) {
    return;
  }
  auto slot = std::make_shared<ThreadSlot>();
  tls_slot = slot;
  std::lock_guard lock(g_mu);
  g_threads.push_back(slot);
}

void record_scope_call(int id) noexcept {
  if (id < 0 || id >= kMaxStages) {
    return;
  }
  ensure_tls_registered();
  tls_slot->scope_calls[id].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_enabled(bool on) noexcept { g_enabled.store(on, std::memory_order_relaxed); }

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
  FrameTotals frame;
  frame.ns.assign(static_cast<size_t>(nstages), 0);
  frame.counter.assign(static_cast<size_t>(nstages), 0);
  frame.calls.assign(static_cast<size_t>(nstages), 0);
  for (const std::shared_ptr<ThreadSlot>& slot : g_threads) {
    for (int i = 0; i < nstages; ++i) {
      frame.ns[static_cast<size_t>(i)] += slot->ns[i].exchange(0, std::memory_order_relaxed);
      frame.counter[static_cast<size_t>(i)] +=
          slot->counter[i].exchange(0, std::memory_order_relaxed);
      frame.calls[static_cast<size_t>(i)] +=
          slot->scope_calls[i].exchange(0, std::memory_order_relaxed);
    }
  }
  g_window.push_back(std::move(frame));
  if (static_cast<int>(g_window.size()) > kMaxWindowFrames) {
    g_window.erase(g_window.begin(),
                   g_window.begin() + (g_window.size() - static_cast<size_t>(kMaxWindowFrames)));
  }
}

int window_frames() noexcept {
  std::lock_guard lock(g_mu);
  return static_cast<int>(g_window.size());
}

std::string window_report() {
  std::lock_guard lock(g_mu);
  const int nstages = static_cast<int>(g_stage_names.size());
  const int nframes = static_cast<int>(g_window.size());
  if (nstages == 0 || nframes == 0) {
    g_window.clear();
    return {};
  }

  struct Row {
    int id = 0;
    double avg_sort = 0.0;
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
    for (const FrameTotals& fr : g_window) {
      const int64_t v = frame_ns(fr, sid);
      if (v > 0) {
        samples.push_back(static_cast<double>(v) / 1e6);
      }
    }
    if (samples.empty()) {
      return;
    }
    double sum_ms = 0.0;
    int64_t sum_calls = 0;
    for (const FrameTotals& fr : g_window) {
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
    row.avg_sort = avg_ms;
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
    for (const FrameTotals& fr : g_window) {
      const int64_t v = frame_counter(fr, sid);
      sum += v;
      max_val = std::max(max_val, v);
    }
    if (sum == 0 && max_val == 0) {
      return;
    }
    const double avg = static_cast<double>(sum) / static_cast<double>(nframes);
    Row row;
    row.id = sid;
    row.avg_sort = avg;
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

  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.avg_sort > b.avg_sort; });

  std::string out;
  for (const Row& r : rows) {
    out += r.line;
    out += '\n';
  }
  g_window.clear();
  return out;
}

}  // namespace wds::common::perf

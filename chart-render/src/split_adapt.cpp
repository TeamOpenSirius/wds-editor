#include "split_adapt.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace wds::chart_render {
namespace {

constexpr int kMinFillFallback = 256;
constexpr int kMinFillLo = 32;
constexpr int kMinFillHi = 4096;
// 256/170 from the Wave 1.5 10660 comment: break-even plus margin for EMA
// lag, clock noise, and packing waste when K≥2. 3/2 is a ratio, not a
// millisecond measured on this machine.
constexpr double kMinFillR = 1.5;
constexpr double kEmaAlpha = 1.0 / 16.0;

std::atomic<int> g_min_fill{kMinFillFallback};
std::atomic<int> g_have_pp{0};
std::atomic<int> g_have_ov{0};
std::atomic<double> g_ema_ns_per_fill{0.0};
std::atomic<double> g_ema_slice_ov_ns{0.0};

void ema_update(std::atomic<double>& slot, std::atomic<int>& have, double x) noexcept {
  if (!(x > 0.0) || !std::isfinite(x)) {
    return;
  }
  const int n = have.load(std::memory_order_relaxed);
  if (n <= 0) {
    slot.store(x, std::memory_order_relaxed);
    have.store(1, std::memory_order_relaxed);
    return;
  }
  const double old = slot.load(std::memory_order_relaxed);
  slot.store((1.0 - kEmaAlpha) * old + kEmaAlpha * x, std::memory_order_relaxed);
  if (n < 1000000) {
    have.store(n + 1, std::memory_order_relaxed);
  }
}

int clamp_i(int v, int lo, int hi) noexcept {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

int ceil_pos(double v) noexcept {
  if (!(v > 0.0) || !std::isfinite(v)) {
    return 0;
  }
  const int i = static_cast<int>(v);
  return static_cast<double>(i) < v ? i + 1 : i;
}

void recompute_min_fill() noexcept {
  if (g_have_pp.load(std::memory_order_relaxed) <= 0 ||
      g_have_ov.load(std::memory_order_relaxed) <= 0) {
    g_min_fill.store(kMinFillFallback, std::memory_order_relaxed);
    return;
  }
  const double ns_pp = g_ema_ns_per_fill.load(std::memory_order_relaxed);
  const double ov = g_ema_slice_ov_ns.load(std::memory_order_relaxed);
  if (!(ns_pp > 1.0) || !(ov > 0.0)) {
    g_min_fill.store(kMinFillFallback, std::memory_order_relaxed);
    return;
  }
  const int m = clamp_i(ceil_pos(kMinFillR * ov / ns_pp), kMinFillLo, kMinFillHi);
  g_min_fill.store(m, std::memory_order_relaxed);
}

bool adapt_log_env() noexcept {
  static const int v = []() {
    const char* e = std::getenv("WDS_SPLIT_ADAPT_LOG");
    return (e != nullptr && e[0] == '1' && e[1] == '\0') ? 1 : 0;
  }();
  return v != 0;
}

void print_adapt(const char* why) noexcept {
  std::fprintf(stderr,
               "ADAPT %s min_fill=%d ns/p=%.3f ov_ns=%.1f R=1.5 pp_n=%d ov_n=%d frames=%d\n",
               why, g_min_fill.load(std::memory_order_relaxed),
               g_ema_ns_per_fill.load(std::memory_order_relaxed),
               g_ema_slice_ov_ns.load(std::memory_order_relaxed),
               g_have_pp.load(std::memory_order_relaxed),
               g_have_ov.load(std::memory_order_relaxed),
               static_cast<int>(g_split_adapt_frame.load(std::memory_order_relaxed)));
}

void register_atexit() noexcept {
  static std::once_flag once;
  std::call_once(once, []() {
    std::atexit([]() { print_adapt("exit"); });
  });
}

}  // namespace

int split_adapt_min_fill() noexcept {
  return g_min_fill.load(std::memory_order_relaxed);
}

void split_adapt_note_fill_cost(int fill, std::uint32_t us) noexcept {
  if (fill <= 0 || us <= 1u) {
    return;
  }
  const double ns_pp = (static_cast<double>(us) * 1000.0) / static_cast<double>(fill);
  ema_update(g_ema_ns_per_fill, g_have_pp, ns_pp);
  recompute_min_fill();
}

void split_adapt_note_slice_overhead_ns(std::int64_t ns) noexcept {
  if (ns <= 0) {
    return;
  }
  ema_update(g_ema_slice_ov_ns, g_have_ov, static_cast<double>(ns));
  recompute_min_fill();
}

void split_adapt_maybe_log() noexcept {
  if (!adapt_log_env()) {
#if WDS_DIAG_ENABLED
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 4) {
      return;
    }
    print_adapt("diag");
#endif
    return;
  }
  register_atexit();
  const int f = static_cast<int>(g_split_adapt_frame.load(std::memory_order_relaxed));
  if (f <= 8 || (f & 63) == 0) {
    print_adapt("frame");
  }
}

}  // namespace wds::chart_render

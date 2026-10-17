#include "split_collect_pool.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using wds::chart_render::SplitCollectCaps;
using wds::chart_render::SplitCollectLinuxCpu;
using wds::chart_render::split_collect_caps_from_phys;
using wds::chart_render::split_collect_count_phys_linux;

int failures = 0;

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void expect_caps(int p, int e, int want_cap, int want_burst, const char* name) {
  const SplitCollectCaps c = split_collect_caps_from_phys(p, e);
  if (c.cap != want_cap || c.burst_cap != want_burst) {
    std::fprintf(stderr, "FAIL %s: cap=%d/%d burst=%d/%d (P=%d E=%d)\n", name, c.cap, want_cap,
                 c.burst_cap, want_burst, p, e);
    ++failures;
  }
}

void push_core(std::vector<SplitCollectLinuxCpu>& rows, int pkg, int core, int cap, int smt = 1) {
  for (int t = 0; t < smt; ++t) {
    SplitCollectLinuxCpu r;
    r.package_id = pkg;
    r.core_id = core;
    r.capacity = cap;
    rows.push_back(r);
  }
}

void test_linux_smt_not_cores() {
  std::vector<SplitCollectLinuxCpu> rows;
  // Intel 9th / Zen3 6c/12t: no cpu_capacity, 2 threads per core.
  for (int c = 0; c < 6; ++c) {
    push_core(rows, 0, c, -1, 2);
  }
  int p = 0;
  int e = 0;
  split_collect_count_phys_linux(rows.data(), rows.size(), &p, &e);
  CHECK(p == 6);
  CHECK(e == 0);
}

void test_linux_hybrid_12th() {
  std::vector<SplitCollectLinuxCpu> rows;
  for (int c = 0; c < 8; ++c) {
    push_core(rows, 0, c, 1024, 2);
  }
  for (int c = 0; c < 8; ++c) {
    push_core(rows, 0, 100 + c, 357, 1);
  }
  int p = 0;
  int e = 0;
  split_collect_count_phys_linux(rows.data(), rows.size(), &p, &e);
  CHECK(p == 8);
  CHECK(e == 8);
}

void test_named_topologies() {
  // Intel 9th 6c/12t and Zen3 5600X 6c/12t: all-P, leave ≥2.
  expect_caps(6, 0, 4, 4, "intel9-6c / zen3-5600x");
  // Intel 12th 8P+8E
  expect_caps(8, 8, 7, 14, "intel12-8p8e");
  // Zen3 16c/32t all-P
  expect_caps(16, 0, 8, 8, "zen3-16c");
  // M1 4P+4E: keep 4 (UI on E), burst 8-2=6
  expect_caps(4, 4, 4, 6, "m1-4p4e");
  // M3 Max 12P+4E
  expect_caps(12, 4, 8, 14, "m3max-12p4e");
  // M4 4P+6E: keep 4, burst 10-2=8
  expect_caps(4, 6, 4, 8, "m4-4p6e");
}

constexpr size_t kPoolItems = 256;
constexpr uint64_t kHeavyHint = 100000;  // forces the pool path when cap > 1

struct CountCtx {
  std::atomic<int> hits[kPoolItems];
  size_t throw_at = kPoolItems;
  CountCtx() {
    for (auto& h : hits) {
      h.store(0, std::memory_order_relaxed);
    }
  }
};

void count_fn(size_t i, void* p) {
  auto* ctx = static_cast<CountCtx*>(p);
  ctx->hits[i].fetch_add(1, std::memory_order_relaxed);
  if (i == ctx->throw_at || i == ctx->throw_at + 100) {
    throw std::runtime_error("split_collect test throw");
  }
}

void sum_fn(size_t i, void* p) {
  static_cast<std::atomic<long>*>(p)->fetch_add(static_cast<long>(i), std::memory_order_relaxed);
}

void test_exception_rethrown_and_pool_reusable() {
  for (int round = 0; round < 3; ++round) {
    CountCtx ctx;
    ctx.throw_at = 37;
    bool caught = false;
    try {
      wds::chart_render::split_collect_parallel_for(0, kPoolItems, 1, count_fn, &ctx, kHeavyHint);
    } catch (const std::runtime_error&) {
      caught = true;
    }
    CHECK(caught);
    bool at_most_once = true;
    for (const auto& h : ctx.hits) {
      if (h.load(std::memory_order_relaxed) > 1) {
        at_most_once = false;
      }
    }
    CHECK(at_most_once);
    CHECK(wds::chart_render::split_collect_claim_depth() == 0);

    std::atomic<long> sum{0};
    wds::chart_render::split_collect_parallel_for(0, kPoolItems, 1, sum_fn, &sum, kHeavyHint);
    CHECK(sum.load() == static_cast<long>(kPoolItems * (kPoolItems - 1) / 2));
  }
}

void test_concurrent_first_use() {
  constexpr int kThreads = 4;
  std::atomic<long> sums[kThreads];
  for (auto& s : sums) {
    s.store(0, std::memory_order_relaxed);
  }
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, &sums, &go] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      wds::chart_render::split_collect_parallel_for(0, 1000, 1, sum_fn, &sums[t], kHeavyHint);
    });
  }
  go.store(true, std::memory_order_release);
  for (std::thread& th : threads) {
    th.join();
  }
  for (const auto& s : sums) {
    CHECK(s.load() == 999L * 1000L / 2);
  }
}

}  // namespace

int main() {
  test_linux_smt_not_cores();
  test_linux_hybrid_12th();
  test_named_topologies();
  test_concurrent_first_use();
  test_exception_rethrown_and_pool_reusable();
  if (failures != 0) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("split_collect_topo_tests ok\n");
  return 0;
}

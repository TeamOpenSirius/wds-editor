#include "split_collect_pool.hpp"

#include <cstdio>
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

}  // namespace

int main() {
  test_linux_smt_not_cores();
  test_linux_hybrid_12th();
  test_named_topologies();
  if (failures != 0) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("split_collect_topo_tests ok\n");
  return 0;
}

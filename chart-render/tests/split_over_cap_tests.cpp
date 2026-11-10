#include "split_over_cap.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using wds::chart_render::OverCapCandidate;
using wds::chart_render::apply_over_cap_select;

int failures = 0;

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

OverCapCandidate make_c(int32_t line, uint16_t layer, uint32_t job, int32_t birth, uint8_t ribbon,
                        uint32_t sys = 0, uint32_t task = 0) {
  OverCapCandidate c;
  c.line_index = line;
  c.layer = layer;
  c.job_i = job;
  c.birth = birth;
  c.ribbon = ribbon;
  c.sys = sys;
  c.task_i = task;
  c.primary = 1;
  return c;
}

void test_stable_birth_order() {
  std::vector<OverCapCandidate> cs;
  for (int i = 0; i < 8; ++i) {
    cs.push_back(make_c(0, 0, static_cast<uint32_t>(7 - i), 7 - i, 0));
  }
  apply_over_cap_select(cs, 4, false);
  CHECK(cs.size() == 4);
  CHECK(cs[0].birth == 0 && cs[0].job_i == 0);
  CHECK(cs[1].birth == 1);
  CHECK(cs[2].birth == 2);
  CHECK(cs[3].birth == 3);
}

void test_ribbon_newest_contiguous() {
  std::vector<OverCapCandidate> cs;
  for (int i = 0; i < 10; ++i) {
    cs.push_back(make_c(0, 0, static_cast<uint32_t>(i), i, 1));
  }
  apply_over_cap_select(cs, 4, false);
  CHECK(cs.size() == 4);
  CHECK(cs[0].birth == 6);
  CHECK(cs[1].birth == 7);
  CHECK(cs[2].birth == 8);
  CHECK(cs[3].birth == 9);
  for (size_t i = 1; i < cs.size(); ++i) {
    CHECK(cs[i].birth == cs[i - 1].birth + 1);
  }
}

void test_ribbon_budget_moves_oldest_end() {
  std::vector<OverCapCandidate> a;
  std::vector<OverCapCandidate> b;
  for (int i = 0; i < 12; ++i) {
    a.push_back(make_c(0, 0, static_cast<uint32_t>(i), i, 1));
    b.push_back(make_c(0, 0, static_cast<uint32_t>(i), i, 1));
  }
  apply_over_cap_select(a, 5, false);
  apply_over_cap_select(b, 8, false);
  CHECK(a.size() == 5);
  CHECK(b.size() == 8);
  CHECK(a.back().birth == 11);
  CHECK(b.back().birth == 11);
  CHECK(a.front().birth == 7);
  CHECK(b.front().birth == 4);
}

void test_legacy_can_hole() {
  std::vector<OverCapCandidate> cs;
  for (int i = 0; i < 6; ++i) {
    cs.push_back(make_c(0, 0, static_cast<uint32_t>(i), i, 1));
    cs.push_back(make_c(0, 1, static_cast<uint32_t>(i), i, 1, 1));
  }
  apply_over_cap_select(cs, 4, true);
  CHECK(cs.size() == 4);
  bool hole = false;
  int last0 = -1;
  for (const auto& c : cs) {
    if (c.layer == 0) {
      if (last0 >= 0 && c.birth != last0 + 1) {
        hole = true;
      }
      last0 = c.birth;
    }
  }
  (void)hole;
}

void test_zero_cap() {
  std::vector<OverCapCandidate> cs;
  cs.push_back(make_c(0, 0, 0, 0, 1));
  apply_over_cap_select(cs, 0, false);
  CHECK(cs.empty());
}

void test_under_cap_sorts() {
  std::vector<OverCapCandidate> cs;
  cs.push_back(make_c(0, 0, 2, 2, 1));
  cs.push_back(make_c(0, 0, 0, 0, 1));
  cs.push_back(make_c(0, 0, 1, 1, 1));
  apply_over_cap_select(cs, 8, false);
  CHECK(cs.size() == 3);
  CHECK(cs[0].birth == 0 && cs[1].birth == 1 && cs[2].birth == 2);
}

bool same_all(const OverCapCandidate& a, const OverCapCandidate& b) {
  return wds::chart_render::over_cap_same_candidate(a, b);
}

void expect_identity_order(std::vector<OverCapCandidate> input, const char* tag) {
  std::vector<OverCapCandidate> sorted = input;
  std::sort(sorted.begin(), sorted.end(), wds::chart_render::over_cap_identity_lt);
  const int cap = static_cast<int>(input.size()) + 8;
  apply_over_cap_select(input, cap, false);
  if (input.size() != sorted.size()) {
    std::fprintf(stderr, "FAIL %s size %zu vs %zu\n", tag, input.size(), sorted.size());
    ++failures;
    return;
  }
  for (size_t i = 0; i < input.size(); ++i) {
    if (!same_all(input[i], sorted[i])) {
      std::fprintf(stderr, "FAIL %s i=%zu birth %d vs %d sys %u vs %u\n", tag, i, input[i].birth,
                   sorted[i].birth, input[i].sys, sorted[i].sys);
      ++failures;
      return;
    }
  }
}

void test_interleaved_subemit_under_cap() {
  std::vector<OverCapCandidate> cs;
  for (int e = 0; e < 40; ++e) {
    OverCapCandidate a = make_c(0, 7, static_cast<uint32_t>(2 * e), 2 * e, 0, 831, 4);
    a.priority = 3;
    a.primary = 0;
    a.ribbon = 1;
    cs.push_back(a);
    OverCapCandidate b = make_c(0, 7, static_cast<uint32_t>(2 * e + 1), 2 * e + 1, 0, 832, 4);
    b.priority = 3;
    b.primary = 0;
    b.ribbon = 0;
    cs.push_back(b);
  }
  for (int j = 0; j < 12; ++j) {
    OverCapCandidate p = make_c(0, 0, static_cast<uint32_t>(j), j, 0, 0, 1);
    p.priority = 1;
    p.primary = 1;
    cs.insert(cs.begin() + static_cast<size_t>(j), p);
  }
  expect_identity_order(cs, "interleaved");
}

void test_bucket_matches_identity_random() {
  std::mt19937 rng(20261110);
  const int layers[] = {0, 1, 3, 7, 12};
  const uint32_t sys_sets[][4] = {{0, 831, 832, 835}, {0, 1, 2, 3}, {0, 1867, 1868, 1870}};
  for (int trial = 0; trial < 250; ++trial) {
    std::vector<OverCapCandidate> cs;
    const int nlines = 1 + static_cast<int>(rng() % 4);
    const int nslots = 1 + static_cast<int>(rng() % 4);
    const int events = 2 + static_cast<int>(rng() % 8);
    const uint32_t* syss = sys_sets[rng() % 3];
    uint32_t task = 0;
    for (int line = 0; line < nlines; ++line) {
      const int npri = static_cast<int>(rng() % 5);
      for (int p = 0; p < npri; ++p) {
        const int jobs = 1 + static_cast<int>(rng() % 6);
        for (int j = 0; j < jobs; ++j) {
          OverCapCandidate c =
              make_c(line, static_cast<uint16_t>(layers[p % 5]), static_cast<uint32_t>(j), j,
                     static_cast<uint8_t>(rng() % 2), 0, task);
          c.instance_start_ms = 1000;
          c.priority = static_cast<int>(rng() % 5);
          c.primary = 1;
          cs.push_back(c);
        }
        ++task;
      }
      for (int e = 0; e < events; ++e) {
        for (int s = 0; s < nslots; ++s) {
          const int i = e * nslots + s;
          OverCapCandidate c =
              make_c(line, 7, static_cast<uint32_t>(i), i, static_cast<uint8_t>(s == 0),
                     syss[static_cast<size_t>(s % 4)], task);
          c.instance_start_ms = 1000;
          c.priority = 3;
          c.primary = 0;
          cs.push_back(c);
        }
      }
      ++task;
    }
    if ((trial % 7) == 0) {
      std::shuffle(cs.begin(), cs.end(), rng);
    }
    expect_identity_order(std::move(cs), "random");
  }
}

void test_bucket_shuffled_lines_fallback_or_match() {
  std::vector<OverCapCandidate> cs;
  for (int line = 3; line >= 0; --line) {
    for (int i = 0; i < 5; ++i) {
      OverCapCandidate c = make_c(line, 0, static_cast<uint32_t>(i), i, 0, 10u + static_cast<uint32_t>(i % 2),
                                  static_cast<uint32_t>(line));
      c.instance_start_ms = 50;
      c.priority = line;
      c.primary = 1;
      cs.push_back(c);
    }
  }
  expect_identity_order(cs, "rev-lines");
}

}  // namespace

int main() {
  test_stable_birth_order();
  test_ribbon_newest_contiguous();
  test_ribbon_budget_moves_oldest_end();
  test_legacy_can_hole();
  test_zero_cap();
  test_under_cap_sorts();
  test_interleaved_subemit_under_cap();
  test_bucket_matches_identity_random();
  test_bucket_shuffled_lines_fallback_or_match();
  if (failures != 0) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("split_over_cap_tests ok\n");
  return 0;
}

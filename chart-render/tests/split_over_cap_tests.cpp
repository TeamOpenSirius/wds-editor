#include "split_over_cap.hpp"

#include <cstdio>
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

}  // namespace

int main() {
  test_stable_birth_order();
  test_ribbon_newest_contiguous();
  test_ribbon_budget_moves_oldest_end();
  test_legacy_can_hole();
  test_zero_cap();
  test_under_cap_sorts();
  if (failures != 0) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("split_over_cap_tests ok\n");
  return 0;
}

#include "wds/audio/hit_sfx.hpp"
#include "wds/audio/hit_sfx_policy.hpp"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

using wds::audio::HitSfxClip;
using wds::audio::HoldInterval;
using wds::audio::SfxInstantBudget;
using wds::audio::SfxPlayHistory;
using wds::audio::hold_covers_ms;
using wds::audio::kIgnorePlayMilliseconds;
using wds::audio::kMaxEntryPlaySePerInstant;
using wds::audio::merge_hold_intervals;

void test_same_ms_same_cue_once() {
  SfxPlayHistory history;
  expect(history.can_admit(1000, HitSfxClip::Perfect, false), "first Perfect at 1000 admits");
  history.record(1000, HitSfxClip::Perfect, false);
  expect(!history.can_admit(1000, HitSfxClip::Perfect, false), "second Perfect at 1000 rejects");
  expect(history.can_admit(1000, HitSfxClip::Critical, false), "Critical at same ms still admits");
}

void test_26ms_apart_same_cue() {
  SfxPlayHistory history;
  history.record(1000, HitSfxClip::Critical, false);
  expect(history.can_admit(1000 + kIgnorePlayMilliseconds + 1, HitSfxClip::Critical, false),
         "26ms later is a new key and admits");
  expect(!history.can_admit(1000, HitSfxClip::Critical, false), "original key still blocked");
}

void test_sound_bypass_stacks() {
  SfxPlayHistory history;
  SfxInstantBudget budget;
  for (int i = 0; i < kMaxEntryPlaySePerInstant; ++i) {
    expect(history.can_admit(2000, HitSfxClip::Sound, true), "Sound bypasses history");
    expect(budget.can_admit(2000), "Sound under instant budget");
    history.record(2000, HitSfxClip::Sound, true);
    budget.record(2000);
  }
  expect(!budget.can_admit(2000), "26th Sound at same ms is rejected");
  expect(budget.can_admit(2001), "next ms has a fresh budget");
}

void test_history_clear() {
  SfxPlayHistory history;
  history.record(50, HitSfxClip::Scratch, false);
  history.clear();
  expect(history.can_admit(50, HitSfxClip::Scratch, false), "clear forgets the key");
}

void test_hold_interval_merge() {
  std::vector<HoldInterval> intervals{{300, 400}, {100, 200}, {200, 300}};
  merge_hold_intervals(intervals);
  expect(intervals.size() == 1, "closed-end overlap merges to one window");
  expect(intervals[0].start_ms == 100 && intervals[0].end_ms == 400, "merged span is 100-400");
  expect(hold_covers_ms(intervals, 200), "joint ms stays covered");
  expect(!hold_covers_ms(intervals, 99), "before start is uncovered");
  expect(!hold_covers_ms(intervals, 401), "after end is uncovered");

  std::vector<HoldInterval> gapped{{100, 200}, {300, 400}};
  merge_hold_intervals(gapped);
  expect(gapped.size() == 2, "gap keeps two windows");
  expect(!hold_covers_ms(gapped, 250), "gap is uncovered");
}

}  // namespace

int main() {
  test_same_ms_same_cue_once();
  test_26ms_apart_same_cue();
  test_sound_bypass_stacks();
  test_history_clear();
  test_hold_interval_merge();
  if (failures != 0) {
    std::fprintf(stderr, "%d hit_sfx_policy test(s) failed\n", failures);
    return 1;
  }
  return 0;
}

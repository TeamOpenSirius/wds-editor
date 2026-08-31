#include "test_support.hpp"

#include <wds/core/chart_validation.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/timing_map.hpp>
#include <wds/core/types.hpp>

#include <cstdint>
#include <vector>

namespace {

using namespace wds::chart_editor;

NotationNote make_note(int32_t id, NoteType type, int32_t tick, int32_t lane, int32_t width = 1,
                       int32_t end_tick = 0) {
  NotationNote note;
  note.id = id;
  note.note_type = type;
  note.start_tick = tick;
  note.end_tick = end_tick;
  note.lane = lane;
  note.width = width;
  return note;
}

bool pair_eq(const NoteOverlapPair& pair, int32_t tick, int32_t first, int32_t second) {
  return pair.tick == tick && pair.first_note_id == first && pair.second_note_id == second;
}

void test_empty_chart_has_no_overlaps() {
  const auto result = find_note_overlaps({});
  CHECK(result.pairs.empty());
  CHECK(result.error_ticks.empty());
}

void test_same_tick_intersecting_lanes_report_pair() {
  const auto result = find_note_overlaps({
      make_note(1, NoteType::Normal, 480, 2, 2),
      make_note(2, NoteType::Critical, 480, 3, 2),
  });
  CHECK_EQ(static_cast<int>(result.pairs.size()), 1);
  CHECK(pair_eq(result.pairs[0], 480, 1, 2));
  CHECK_EQ(static_cast<int>(result.error_ticks.size()), 1);
  CHECK_EQ(result.error_ticks[0], 480);
}

void test_same_tick_non_overlapping_adjacent_lanes_are_valid() {
  const auto result = find_note_overlaps({
      make_note(1, NoteType::Normal, 240, 0, 2),  // lanes 0-1
      make_note(2, NoteType::Flick, 240, 2, 2),   // lanes 2-3
  });
  CHECK(result.pairs.empty());
  CHECK(result.error_ticks.empty());
}

void test_inclusive_boundary_contact_is_overlap() {
  const auto result = find_note_overlaps({
      make_note(4, NoteType::Normal, 120, 1, 2),  // lanes 1-2
      make_note(7, NoteType::BlueTap, 120, 2, 2),  // lanes 2-3 share lane 2
  });
  CHECK_EQ(static_cast<int>(result.pairs.size()), 1);
  CHECK(pair_eq(result.pairs[0], 120, 4, 7));
}

void test_excludes_body_star_eighth_and_split() {
  NotationNote body = make_note(1, NoteType::Hold, 0, 2, 2, 480);
  NotationNote star = make_note(2, NoteType::Sound, 0, 2, 2);
  NotationNote scratch_star = make_note(3, NoteType::ScratchSound, 0, 3, 1);
  NotationNote eighth = make_note(4, NoteType::HoldEighth, 0, 2, 2);
  NotationNote split = make_note(5, NoteType::Normal, 0, 2, 2);
  split.gimmick_type = GimmickType::Split3;
  NotationNote tap = make_note(6, NoteType::Normal, 0, 2, 2);

  const auto result = find_note_overlaps({body, star, scratch_star, eighth, split, tap});
  CHECK(result.pairs.empty());
  CHECK(result.error_ticks.empty());
}

void test_hold_head_is_included() {
  const auto result = find_note_overlaps({
      make_note(1, NoteType::HoldStart, 0, 1, 2),
      make_note(2, NoteType::Normal, 0, 2, 1),
  });
  CHECK_EQ(static_cast<int>(result.pairs.size()), 1);
  CHECK(pair_eq(result.pairs[0], 0, 1, 2));
}

void test_normal_hold_tail_uses_encoded_body_span() {
  NotationNote hold = make_note(1, NoteType::Hold, 0, 3, 2, 480);
  NotationNote overlap = make_note(2, NoteType::Flick, 480, 4, 1);
  NotationNote adjacent = make_note(3, NoteType::Normal, 480, 5, 1);
  const auto hit = find_note_overlaps({hold, overlap});
  CHECK_EQ(static_cast<int>(hit.pairs.size()), 1);
  CHECK(pair_eq(hit.pairs[0], 480, 1, 2));
  const auto miss = find_note_overlaps({hold, adjacent});
  CHECK(miss.pairs.empty());
}

void test_scratch_hold_terminal_tail_uses_end_span() {
  NotationNote hold = make_note(1, NoteType::ScratchHold, 0, 2, 2, 480);
  hold.scratch_length = 4;  // [2, 5]
  NotationNote overlap = make_note(2, NoteType::Normal, 480, 5, 1);
  NotationNote miss = make_note(3, NoteType::Normal, 480, 1, 1);
  const auto hit = find_note_overlaps({hold, overlap});
  CHECK_EQ(static_cast<int>(hit.pairs.size()), 1);
  CHECK(pair_eq(hit.pairs[0], 480, 1, 2));
  const auto clear = find_note_overlaps({hold, miss});
  CHECK(clear.pairs.empty());
}

void test_legal_hold_head_body_pairing_is_not_reported() {
  NotationNote head = make_note(1, NoteType::HoldStart, 0, 2, 3);
  NotationNote body = make_note(2, NoteType::Hold, 0, 2, 3, 960);
  const auto result = find_note_overlaps({head, body});
  CHECK(result.pairs.empty());
  CHECK(result.error_ticks.empty());
}

void test_legal_regular_hold_chain_joint_is_not_reported() {
  NotationNote prev = make_note(1, NoteType::Hold, 0, 0, 5, 480);
  NotationNote next = make_note(2, NoteType::Hold, 480, 0, 6, 960);
  sync_scratch_chain_joint(prev, next);
  apply_hold_chain_gimmick(prev);
  NotationNote head = make_note(3, NoteType::HoldStart, 0, 0, 5);
  const auto result = find_note_overlaps({head, prev, next});
  CHECK(result.pairs.empty());
  CHECK(result.error_ticks.empty());
}

void test_legal_scratch_hold_chain_joint_is_not_reported() {
  NotationNote prev = make_note(1, NoteType::ScratchHold, 0, 2, 2, 480);
  NotationNote next = make_note(2, NoteType::ScratchHold, 480, 4, 2, 960);
  sync_scratch_chain_joint(prev, next);
  NotationNote head = make_note(3, NoteType::ScratchHoldStart, 0, 2, 2);
  const auto result = find_note_overlaps({head, prev, next});
  CHECK(result.pairs.empty());
  CHECK(result.error_ticks.empty());
}

void test_unrelated_tap_on_legal_scratch_joint_is_reported() {
  NotationNote prev = make_note(1, NoteType::ScratchHold, 0, 2, 2, 480);
  NotationNote next = make_note(2, NoteType::ScratchHold, 480, 4, 2, 960);
  sync_scratch_chain_joint(prev, next);
  NotationNote head = make_note(3, NoteType::ScratchHoldStart, 0, 2, 2);
  // Lane 2 sits on the joint cover / prev tail, but not on next's start span.
  NotationNote tap = make_note(4, NoteType::Normal, 480, 2, 1);
  const auto result = find_note_overlaps({head, prev, next, tap});
  CHECK_EQ(static_cast<int>(result.pairs.size()), 1);
  CHECK(pair_eq(result.pairs[0], 480, 1, 4));
  CHECK_EQ(static_cast<int>(result.error_ticks.size()), 1);
  CHECK_EQ(result.error_ticks[0], 480);
}

void test_three_note_collision_unique_sorted_pairs() {
  const auto result = find_note_overlaps({
      make_note(10, NoteType::Normal, 100, 2, 3),  // 2-4
      make_note(3, NoteType::Flick, 100, 1, 3),    // 1-3
      make_note(7, NoteType::Critical, 100, 3, 2),  // 3-4
  });
  CHECK_EQ(static_cast<int>(result.pairs.size()), 3);
  CHECK(pair_eq(result.pairs[0], 100, 3, 7));
  CHECK(pair_eq(result.pairs[1], 100, 3, 10));
  CHECK(pair_eq(result.pairs[2], 100, 7, 10));
  CHECK_EQ(static_cast<int>(result.error_ticks.size()), 1);
  CHECK_EQ(result.error_ticks[0], 100);
}

void test_error_ticks_sorted_and_deduplicated() {
  const auto result = find_note_overlaps({
      make_note(1, NoteType::Normal, 960, 0, 2),
      make_note(2, NoteType::Normal, 960, 1, 1),
      make_note(3, NoteType::Normal, 240, 4, 2),
      make_note(4, NoteType::Normal, 240, 5, 1),
      make_note(5, NoteType::Normal, 960, 0, 1),
  });
  CHECK_EQ(static_cast<int>(result.error_ticks.size()), 2);
  CHECK_EQ(result.error_ticks[0], 240);
  CHECK_EQ(result.error_ticks[1], 960);
  CHECK(result.pairs.size() >= 2);
  CHECK_EQ(result.pairs.front().tick, 240);
}

void test_events_stay_keyed_to_exact_ticks_across_bpm_changes() {
  NotationNote a = make_note(1, NoteType::Normal, 480, 0, 2);
  NotationNote b = make_note(2, NoteType::Normal, 480, 1, 1);
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {
      TimingPoint{0, 120.0, 4, 4, true, true},
      TimingPoint{240, 180.0, 4, 4, true, false},
  };
  normalize_timing_points(timing);
  (void)tick_to_milliseconds(480, timing);
  const auto result = find_note_overlaps({a, b});
  CHECK_EQ(static_cast<int>(result.pairs.size()), 1);
  CHECK_EQ(result.pairs[0].tick, 480);
  CHECK_EQ(result.error_ticks[0], 480);
}

void test_malformed_geometry_does_not_crash() {
  NotationNote zero_width = make_note(1, NoteType::Normal, 10, 3, 0);
  NotationNote negative_width = make_note(2, NoteType::Normal, 10, 3, -4);
  NotationNote inverted_hold = make_note(3, NoteType::Hold, 80, 1, 2, 20);
  NotationNote hispeed = make_note(4, NoteType::HiSpeed, 10, 3, 2);
  const auto result = find_note_overlaps({zero_width, negative_width, inverted_hold, hispeed});
  CHECK_EQ(static_cast<int>(result.pairs.size()), 1);
  CHECK_EQ(result.pairs[0].tick, 10);
}

void test_nontail_hold_does_not_emit_tail() {
  NotationNote body = make_note(1, NoteType::NontailHold, 0, 2, 2, 480);
  NotationNote tap = make_note(2, NoteType::Normal, 480, 2, 2);
  const auto result = find_note_overlaps({body, tap});
  CHECK(result.pairs.empty());
}

}  // namespace

int main() {
  test_empty_chart_has_no_overlaps();
  test_same_tick_intersecting_lanes_report_pair();
  test_same_tick_non_overlapping_adjacent_lanes_are_valid();
  test_inclusive_boundary_contact_is_overlap();
  test_excludes_body_star_eighth_and_split();
  test_hold_head_is_included();
  test_normal_hold_tail_uses_encoded_body_span();
  test_scratch_hold_terminal_tail_uses_end_span();
  test_legal_hold_head_body_pairing_is_not_reported();
  test_legal_regular_hold_chain_joint_is_not_reported();
  test_legal_scratch_hold_chain_joint_is_not_reported();
  test_unrelated_tap_on_legal_scratch_joint_is_reported();
  test_three_note_collision_unique_sorted_pairs();
  test_error_ticks_sorted_and_deduplicated();
  test_events_stay_keyed_to_exact_ticks_across_bpm_changes();
  test_malformed_geometry_does_not_crash();
  test_nontail_hold_does_not_emit_tail();

  const int failures = wds::chart_editor::test::failure_count();
  if (failures == 0) {
    std::printf("All chart_validation tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", failures);
  return 1;
}

#include "test_support.hpp"

#include <wds/core/chart_index.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>
#include <wds/core/preview_snapshot_builder.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/core/split_lane_simulator.hpp>
#include <wds/core/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace wds::chart_editor;

NotationNote make_tap(int32_t id, int32_t start_tick, int32_t lane) {
  NotationNote note;
  note.id = id;
  note.start_tick = start_tick;
  note.lane = lane;
  note.width = 1;
  note.note_type = NoteType::Normal;
  return note;
}

NotationNote make_split(int32_t id, int32_t start_tick, int32_t end_tick) {
  NotationNote note;
  note.id = id;
  note.start_tick = start_tick;
  note.end_tick = end_tick;
  note.lane = 0;
  note.width = 6;
  note.gimmick_type = GimmickType::Split3;
  return note;
}

int64_t span_ms(const NotationNote& note, const MusicTiming& timing) {
  return std::max<int64_t>(0, note.end_ms(timing) - note.start_ms(timing));
}

void sort_ids(std::vector<int32_t>& ids) { std::sort(ids.begin(), ids.end()); }

int64_t sat_add_nonneg(int64_t value, int64_t amount) {
  if (amount <= 0) {
    return value;
  }
  if (value > std::numeric_limits<int64_t>::max() - amount) {
    return std::numeric_limits<int64_t>::max();
  }
  return value + amount;
}

std::vector<int32_t> naive_active_split_ids(const std::vector<NotationNote>& notes,
                                            const MusicTiming& timing, int64_t preview_ms,
                                            const SplitLaneSimulator& simulator) {
  std::vector<int32_t> ids;
  for (const auto& note : notes) {
    if (simulator.is_split_active(note, timing, preview_ms)) {
      ids.push_back(note.id);
    }
  }
  sort_ids(ids);
  return ids;
}

std::vector<int32_t> snapshot_split_ids(const PreviewSnapshot& snapshot) {
  std::vector<int32_t> ids;
  ids.reserve(snapshot.split_lanes.size());
  for (const auto& lane : snapshot.split_lanes) {
    ids.push_back(lane.source_note_id);
  }
  sort_ids(ids);
  return ids;
}

bool split_lanes_equal(std::vector<PreviewSplitLaneInstance> a,
                       std::vector<PreviewSplitLaneInstance> b) {
  if (a.size() != b.size()) {
    return false;
  }
  auto by_id = [](const PreviewSplitLaneInstance& lhs, const PreviewSplitLaneInstance& rhs) {
    return lhs.source_note_id < rhs.source_note_id;
  };
  std::sort(a.begin(), a.end(), by_id);
  std::sort(b.begin(), b.end(), by_id);
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].source_note_id != b[i].source_note_id || a[i].start_ms != b[i].start_ms ||
        a[i].end_ms != b[i].end_ms || a[i].effective_lane_count != b[i].effective_lane_count ||
        a[i].is_continued != b[i].is_continued || a[i].split_anim_phase != b[i].split_anim_phase) {
      return false;
    }
    if (std::fabs(a[i].split_line_alpha - b[i].split_line_alpha) > 1e-4f ||
        std::fabs(a[i].split_percent_start - b[i].split_percent_start) > 1e-4f ||
        std::fabs(a[i].split_percent_end - b[i].split_percent_end) > 1e-4f ||
        std::fabs(a[i].stage_cover_alpha - b[i].stage_cover_alpha) > 1e-4f) {
      return false;
    }
  }
  return true;
}

bool snapshots_match(const PreviewSnapshot& a, const PreviewSnapshot& b) {
  if (a.timeline_ms != b.timeline_ms || a.active_lane_count != b.active_lane_count ||
      a.notes.size() != b.notes.size()) {
    return false;
  }
  if (!split_lanes_equal(a.split_lanes, b.split_lanes)) {
    return false;
  }
  auto notes = a.notes;
  auto other = b.notes;
  auto by_id = [](const PreviewNoteInstance& lhs, const PreviewNoteInstance& rhs) {
    return lhs.note_id < rhs.note_id;
  };
  std::sort(notes.begin(), notes.end(), by_id);
  std::sort(other.begin(), other.end(), by_id);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (notes[i].note_id != other[i].note_id) {
      return false;
    }
    if (std::fabs(notes[i].pos_y - other[i].pos_y) > 1e-4f) {
      return false;
    }
  }
  return true;
}

void test_query_split_lanes_range_boundaries() {
  MusicTiming timing;
  const NotationNote a = make_split(1, 0, 480);
  const NotationNote b = make_split(2, 480, 960);
  const NotationNote c = make_split(3, 960, 1440);
  const NotationNote d = make_split(4, 1440, 1920);
  const NotationNote tap = make_tap(10, 480, 2);

  ChartNoteIndex index;
  index.rebuild({a, b, c, d, tap}, timing);

  const int64_t ms_b = b.start_ms(timing);
  const int64_t ms_c = c.start_ms(timing);
  std::vector<int32_t> ids;

  index.query_split_lanes_in_range(ms_b, ms_c, ids);
  sort_ids(ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 2);
  CHECK_EQ(ids[0], 2);
  CHECK_EQ(ids[1], 3);

  index.query_split_lanes_in_range(ms_b, ms_b, ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 2);

  index.query_split_lanes_in_range(ms_b + 1, ms_c - 1, ids);
  CHECK(ids.empty());

  index.query_split_lanes_in_range(ms_c, ms_b, ids);
  CHECK(ids.empty());

  index.query_split_lanes_in_range(std::numeric_limits<int64_t>::max(),
                                   std::numeric_limits<int64_t>::min(), ids);
  CHECK(ids.empty());

  index.query_split_lanes_in_range(a.start_ms(timing), d.start_ms(timing), ids);
  sort_ids(ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 4);
  CHECK(std::find(ids.begin(), ids.end(), 10) == ids.end());
}

void test_max_split_span_add_update_remove_stale_rebuild() {
  MusicTiming timing;
  const NotationNote long_split = make_split(1, 0, 1920);
  const NotationNote short_split = make_split(2, 480, 960);
  const int64_t long_span = span_ms(long_split, timing);
  const int64_t short_span = span_ms(short_split, timing);

  ChartNoteIndex index;
  index.rebuild({long_split, short_split}, timing);
  CHECK_EQ(index.max_split_span_ms(), long_span);
  CHECK(!index.hold_span_stale());

  NotationNote longer = make_split(3, 960, 3840);
  const int64_t longer_span = span_ms(longer, timing);
  index.on_note_added(longer, timing);
  CHECK_EQ(index.max_split_span_ms(), longer_span);
  CHECK(!index.hold_span_stale());

  NotationNote tiny = make_split(4, 1440, 1680);
  index.on_note_added(tiny, timing);
  CHECK_EQ(index.max_split_span_ms(), longer_span);
  CHECK(!index.hold_span_stale());

  std::vector<int32_t> ids;
  index.query_split_lanes_in_range(tiny.start_ms(timing), tiny.start_ms(timing), ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 4);

  index.on_note_removed(tiny, timing);
  CHECK(!index.hold_span_stale());
  CHECK_EQ(index.max_split_span_ms(), longer_span);
  index.query_split_lanes_in_range(tiny.start_ms(timing), tiny.start_ms(timing), ids);
  CHECK(ids.empty());

  NotationNote short_moved = short_split;
  short_moved.start_tick = 1440;
  short_moved.end_tick = 1920;
  index.on_note_updated(short_split, short_moved, timing);
  CHECK(!index.hold_span_stale());
  CHECK_EQ(index.max_split_span_ms(), longer_span);
  index.query_split_lanes_in_range(short_split.start_ms(timing), short_split.start_ms(timing), ids);
  CHECK(ids.empty());
  index.query_split_lanes_in_range(short_moved.start_ms(timing), short_moved.start_ms(timing), ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 2);

  NotationNote longer_extended = longer;
  longer_extended.end_tick = 4800;
  const int64_t extended_span = span_ms(longer_extended, timing);
  index.on_note_updated(longer, longer_extended, timing);
  CHECK(!index.hold_span_stale());
  CHECK_EQ(index.max_split_span_ms(), extended_span);

  NotationNote longer_shortened = longer_extended;
  longer_shortened.end_tick = 1200;
  index.on_note_updated(longer_extended, longer_shortened, timing);
  CHECK(index.hold_span_stale());

  index.rebuild({long_split, short_moved, longer_shortened}, timing);
  CHECK(!index.hold_span_stale());
  CHECK_EQ(index.max_split_span_ms(), long_span);

  index.on_note_removed(long_split, timing);
  CHECK(index.hold_span_stale());

  index.rebuild({short_moved, longer_shortened}, timing);
  CHECK(!index.hold_span_stale());
  CHECK_EQ(index.max_split_span_ms(),
           std::max(span_ms(short_moved, timing), span_ms(longer_shortened, timing)));

  index.rebuild({long_split, short_split}, timing);
  NotationNote as_tap = long_split;
  as_tap.gimmick_type = GimmickType::None;
  index.on_note_updated(long_split, as_tap, timing);
  CHECK(index.hold_span_stale());

  index.rebuild({as_tap, short_split}, timing);
  CHECK(!index.hold_span_stale());
  CHECK_EQ(index.max_split_span_ms(), short_span);
  index.query_split_lanes_in_range(std::numeric_limits<int64_t>::min(),
                                   std::numeric_limits<int64_t>::max(), ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 2);
}

void test_tap_split_update_direct_index_without_document_rebuild() {
  MusicTiming timing;
  std::vector<int32_t> ids;

  ChartNoteIndex tap_to_split;
  const NotationNote tap = make_tap(1, 480, 0);
  tap_to_split.on_note_added(tap, timing);
  CHECK_EQ(tap_to_split.max_split_span_ms(), 0);
  CHECK(!tap_to_split.hold_span_stale());
  tap_to_split.query_split_lanes_in_range(tap.start_ms(timing), tap.start_ms(timing), ids);
  CHECK(ids.empty());

  NotationNote became_split = tap;
  became_split.gimmick_type = GimmickType::Split3;
  became_split.end_tick = 480 + 1920;
  tap_to_split.on_note_updated(tap, became_split, timing);
  CHECK(!tap_to_split.hold_span_stale());
  CHECK_EQ(tap_to_split.max_split_span_ms(), span_ms(became_split, timing));
  tap_to_split.query_split_lanes_in_range(became_split.start_ms(timing),
                                          became_split.start_ms(timing), ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 1);

  tap_to_split.on_note_updated(became_split, tap, timing);
  CHECK(tap_to_split.hold_span_stale());
  tap_to_split.query_split_lanes_in_range(tap.start_ms(timing), tap.start_ms(timing), ids);
  CHECK(ids.empty());

  const NotationNote max_split = make_split(10, 0, 1920);
  const NotationNote short_split = make_split(11, 480, 960);
  ChartNoteIndex keep_max;
  keep_max.rebuild({max_split, short_split}, timing);
  const int64_t max_span = span_ms(max_split, timing);
  CHECK_EQ(keep_max.max_split_span_ms(), max_span);
  CHECK(max_span > span_ms(short_split, timing));
  CHECK(!keep_max.hold_span_stale());

  NotationNote short_as_tap = short_split;
  short_as_tap.gimmick_type = GimmickType::None;
  short_as_tap.end_tick = 0;
  keep_max.on_note_updated(short_split, short_as_tap, timing);
  CHECK(!keep_max.hold_span_stale());
  CHECK_EQ(keep_max.max_split_span_ms(), max_span);
  keep_max.query_split_lanes_in_range(short_split.start_ms(timing), short_split.start_ms(timing),
                                      ids);
  CHECK(ids.empty());
  keep_max.query_split_lanes_in_range(max_split.start_ms(timing), max_split.start_ms(timing), ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 10);

  keep_max.on_note_updated(short_as_tap, short_split, timing);
  CHECK(!keep_max.hold_span_stale());
  CHECK_EQ(keep_max.max_split_span_ms(), max_span);
  keep_max.query_split_lanes_in_range(short_split.start_ms(timing), short_split.start_ms(timing),
                                      ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 11);
}

void test_long_chart_early_long_split_stays_active_and_candidates_shrink() {
  MusicTiming timing;
  PreviewConfig config;
  SplitLaneSimulator simulator(config);

  std::vector<NotationNote> notes;
  notes.reserve(140);
  for (int32_t i = 0; i < 100; ++i) {
    notes.push_back(make_split(i + 1, i * 480, i * 480 + 480));
  }
  const NotationNote long_split = make_split(1000, 70 * 480, 70 * 480 + 16 * 480);
  notes.push_back(long_split);
  for (int32_t i = 0; i < 20; ++i) {
    notes.push_back(make_tap(200 + i, i * 960, i % 6));
  }

  ChartNoteIndex index;
  index.rebuild(notes, timing);
  CHECK_EQ(index.max_split_span_ms(), span_ms(long_split, timing));
  CHECK(index.max_split_span_ms() > span_ms(notes[0], timing) * 8);

  const int64_t preview_ms = long_split.end_ms(timing) - 1000;
  CHECK(simulator.is_split_active(long_split, timing, preview_ms));
  const int64_t disappear_ms = split_fade_sec_to_ms(config.split_line_animation_end_sec);
  const int64_t appear_ms = split_fade_sec_to_ms(config.split_line_animation_start_sec);
  CHECK(long_split.start_ms(timing) < preview_ms - disappear_ms);

  const auto expected = naive_active_split_ids(notes, timing, preview_ms, simulator);
  CHECK(std::find(expected.begin(), expected.end(), 1000) != expected.end());
  CHECK(!expected.empty());

  PreviewSnapshotBuilder builder(config);
  PreviewSnapshot snapshot;
  builder.build_into(snapshot, notes, timing, {}, index, preview_ms, PreviewPlaybackState::Paused,
                     /*revision=*/1);
  CHECK(snapshot_split_ids(snapshot) == expected);

  std::vector<int32_t> up_to_ids;
  index.query_split_lanes_up_to(sat_add_nonneg(preview_ms, appear_ms), up_to_ids);
  CHECK(up_to_ids.size() >= 60);

  PreviewSnapshot empty;
  const auto estimate = builder.estimate_diff(empty, index, {}, preview_ms, /*revision=*/1);
  CHECK(estimate.split_candidate_count * 2 <= up_to_ids.size());
  CHECK(estimate.split_candidate_count >= expected.size());
}

void test_query_candidates_int64_extrema_and_negative_windows() {
  MusicTiming timing;
  const NotationNote tap0 = make_tap(1, 0, 0);
  const NotationNote tap1 = make_tap(2, 480, 1);
  const NotationNote tap2 = make_tap(3, 960, 2);
  ChartNoteIndex index;
  index.rebuild({tap0, tap1, tap2}, timing);

  const int64_t ms0 = tap0.start_ms(timing);
  const int64_t ms1 = tap1.start_ms(timing);
  const int64_t ms2 = tap2.start_ms(timing);
  std::vector<int32_t> ids;
  std::vector<int32_t> baseline;

  index.query_candidates(750, 300, 300, baseline);
  sort_ids(baseline);
  CHECK_EQ(static_cast<int32_t>(baseline.size()), 2);
  CHECK_EQ(baseline[0], 2);
  CHECK_EQ(baseline[1], 3);

  index.query_candidates(ms1, 0, 0, ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 2);

  index.query_candidates(ms1, -250, -100, ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 2);

  index.query_candidates(ms1, -1, 0, ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 2);

  index.query_candidates(ms1, 0, -1, ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 2);

  index.query_candidates(ms0, 0, 0, ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 1);

  index.query_candidates(ms2, 200, 200, ids);
  sort_ids(ids);
  CHECK_EQ(static_cast<int32_t>(ids.size()), 1);
  CHECK_EQ(ids[0], 3);

  index.query_candidates(std::numeric_limits<int64_t>::max(), 1000, 1000, ids);
  CHECK(ids.empty());

  index.query_candidates(std::numeric_limits<int64_t>::min(), 1000, 1000, ids);
  CHECK(ids.empty());

  index.query_candidates(std::numeric_limits<int64_t>::max(), -50, -25, ids);
  CHECK(ids.empty());

  index.query_candidates(std::numeric_limits<int64_t>::min(), -50, -25, ids);
  CHECK(ids.empty());

  index.query_candidates(750, 300, 300, ids);
  sort_ids(ids);
  CHECK(ids == baseline);
}

void test_split_span_stale_via_chart_document() {
  ChartDocument doc;
  NotationNote tap = make_tap(1, 0, 0);
  CHECK_EQ(doc.add_note(tap), 1);
  CHECK_EQ(doc.index().max_split_span_ms(), 0);
  CHECK(!doc.index().hold_span_stale());

  NotationNote as_split = tap;
  as_split.gimmick_type = GimmickType::Split3;
  as_split.end_tick = 1920;
  CHECK(doc.update_note(1, as_split));
  CHECK(!doc.index().hold_span_stale());
  CHECK_EQ(doc.index().max_split_span_ms(), span_ms(*doc.find_note(1), doc.timing()));

  NotationNote short_split = make_split(2, 480, 960);
  CHECK_EQ(doc.add_note(short_split), 2);
  const int64_t long_span = doc.index().max_split_span_ms();
  CHECK(long_span > span_ms(*doc.find_note(2), doc.timing()));
  CHECK(!doc.index().hold_span_stale());

  NotationNote short_as_tap = *doc.find_note(2);
  short_as_tap.gimmick_type = GimmickType::None;
  short_as_tap.end_tick = 0;
  CHECK(doc.update_note(2, short_as_tap));
  CHECK(!doc.index().hold_span_stale());
  CHECK_EQ(doc.index().max_split_span_ms(), long_span);

  ChartDocument twins;
  NotationNote a = make_split(10, 0, 1920);
  NotationNote b = make_split(11, 480, 480 + 1920);
  CHECK_EQ(twins.add_note(a), 10);
  CHECK_EQ(twins.add_note(b), 11);
  const int64_t twin_span = span_ms(*twins.find_note(10), twins.timing());
  CHECK_EQ(twins.index().max_split_span_ms(), twin_span);
  CHECK_EQ(span_ms(*twins.find_note(11), twins.timing()), twin_span);
  CHECK(!twins.index().hold_span_stale());

  CHECK(twins.remove_note(10));
  CHECK(!twins.index().hold_span_stale());
  CHECK_EQ(twins.index().max_split_span_ms(), span_ms(*twins.find_note(11), twins.timing()));

  ChartDocument twins_shorten;
  CHECK_EQ(twins_shorten.add_note(a), 10);
  CHECK_EQ(twins_shorten.add_note(b), 11);
  NotationNote shortened = *twins_shorten.find_note(11);
  shortened.end_tick = shortened.start_tick + 480;
  CHECK(twins_shorten.update_note(11, shortened));
  CHECK(!twins_shorten.index().hold_span_stale());
  CHECK_EQ(twins_shorten.index().max_split_span_ms(),
           span_ms(*twins_shorten.find_note(10), twins_shorten.timing()));

  ChartDocument zero_doc;
  NotationNote lasting = make_split(20, 0, 1920);
  NotationNote instant = make_split(21, 480, 0);
  CHECK_EQ(zero_doc.add_note(lasting), 20);
  CHECK_EQ(zero_doc.add_note(instant), 21);
  CHECK(zero_doc.index().max_split_span_ms() > 0);
  CHECK(zero_doc.remove_note(20));
  CHECK(!zero_doc.index().hold_span_stale());
  CHECK_EQ(zero_doc.index().max_split_span_ms(), 0);
}

void test_full_and_incremental_snapshot_splits_match() {
  MusicTiming timing;
  PreviewConfig config;
  std::vector<NotationNote> notes;
  notes.reserve(80);
  for (int32_t i = 0; i < 60; ++i) {
    notes.push_back(make_split(i + 1, i * 720, i * 720 + 960));
  }
  for (int32_t i = 0; i < 20; ++i) {
    notes.push_back(make_tap(100 + i, i * 480, i % 6));
  }

  ChartNoteIndex index;
  index.rebuild(notes, timing);

  const NotationNote fade_split = notes[8];
  const int64_t fade_in_ms =
      fade_split.start_ms(timing) - split_fade_sec_to_ms(config.split_line_animation_start_sec);
  const int64_t t0 = fade_in_ms - 48;
  const int64_t t1 = fade_in_ms + 48;
  PreviewSnapshotBuilder builder(config);

  PreviewSnapshot incremental;
  builder.build_into(incremental, notes, timing, {}, index, t0, PreviewPlaybackState::Paused,
                     /*revision=*/7);
  CHECK(incremental.find_split_lane(fade_split.id) == nullptr);

  for (int64_t t = t0; t <= t1; t += 16) {
    if (t != t0) {
      builder.update_incremental(incremental, notes, timing, {}, index, t,
                                 PreviewPlaybackState::Paused, /*revision=*/7);
    }
    PreviewSnapshot full;
    builder.build_into(full, notes, timing, {}, index, t, PreviewPlaybackState::Paused,
                       /*revision=*/7);
    CHECK(snapshots_match(incremental, full));
    if (t < fade_in_ms) {
      CHECK(full.find_split_lane(fade_split.id) == nullptr);
    } else {
      CHECK(full.find_split_lane(fade_split.id) != nullptr);
    }
  }

  PreviewSnapshot overflow_hi;
  builder.build_into(overflow_hi, notes, timing, {}, index, std::numeric_limits<int64_t>::max(),
                     PreviewPlaybackState::Paused, /*revision=*/8);
  PreviewSnapshot overflow_lo;
  builder.build_into(overflow_lo, notes, timing, {}, index, std::numeric_limits<int64_t>::min(),
                     PreviewPlaybackState::Paused, /*revision=*/9);
  CHECK_EQ(static_cast<int32_t>(overflow_hi.split_lanes.size()), 0);
  CHECK_EQ(static_cast<int32_t>(overflow_lo.split_lanes.size()), 0);
}

}  // namespace

int main() {
  test_query_split_lanes_range_boundaries();
  test_max_split_span_add_update_remove_stale_rebuild();
  test_tap_split_update_direct_index_without_document_rebuild();
  test_long_chart_early_long_split_stays_active_and_candidates_shrink();
  test_split_span_stale_via_chart_document();
  test_query_candidates_int64_extrema_and_negative_windows();
  test_full_and_incremental_snapshot_splits_match();

  const int failures = wds::chart_editor::test::failure_count();
  if (failures == 0) {
    std::printf("All split_index tests passed.\n");
    return 0;
  }
  std::printf("%d split_index test(s) failed.\n", failures);
  return 1;
}

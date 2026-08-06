#include "test_support.hpp"

#include <wds/core/chart_index.hpp>
#include <wds/core/chart_serializer.hpp>
#include <wds/core/chart_session.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/edit_history.hpp>
#include <wds/core/detail/start_ms_avl_index.hpp>
#include <wds/core/file_io.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/sus_chart.hpp>
#include <wds/core/timing_map.hpp>
#include <wds/core/core.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace wds::chart_editor;
namespace fs = std::filesystem;

NotationNote make_tap(int32_t start_tick, int32_t lane) {
  NotationNote note;
  note.start_tick = start_tick;
  note.lane = lane;
  note.note_type = NoteType::Normal;
  return note;
}

fs::path fixture_path(const char* name) {
  const fs::path candidates[] = {
      fs::path("tests/fixtures") / name,
      fs::path("core/tests/fixtures") / name,
      fs::path(__FILE__).parent_path() / "fixtures" / name,
  };

  for (const auto& path : candidates) {
    if (fs::exists(path)) {
      return path;
    }
  }
  return candidates[2];
}

fs::path temp_chart_path(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "wds_chart_editor_tests";
  fs::create_directories(dir);
  return dir / name;
}

void test_auto_note_id_starts_at_zero() {
  ChartDocument doc;
  const int32_t a = doc.add_note(make_tap(0, 0));
  const int32_t b = doc.add_note(make_tap(480, 1));
  const int32_t c = doc.add_note(make_tap(960, 2));

  CHECK_EQ(a, 0);
  CHECK_EQ(b, 1);
  CHECK_EQ(c, 2);
  CHECK_EQ(doc.next_note_id(), 3);
  CHECK_EQ(static_cast<int32_t>(doc.notes().size()), 3);
}

void test_concurrent_lines_multi_press_only() {
  ChartDocument doc;
  // Lone taps at different times → no sync line.
  doc.add_note(make_tap(0, 0));
  doc.add_note(make_tap(480, 1));
  CHECK_EQ(static_cast<int32_t>(doc.concurrent_lines().size()), 0);

  // Second note at the same tick → multi-press sync line.
  doc.add_note(make_tap(0, 4));
  CHECK_EQ(static_cast<int32_t>(doc.concurrent_lines().size()), 1);
  CHECK_EQ(doc.concurrent_lines()[0].start_lane, 0);
  CHECK_EQ(doc.concurrent_lines()[0].width, 5);  // lanes 0..4

  // Hold body / eighth / mid-star must not create or join sync lines by themselves.
  ChartDocument hold_doc;
  NotationNote body = make_tap(0, 2);
  body.note_type = NoteType::Hold;
  body.end_tick = 960;
  hold_doc.add_note(body);
  NotationNote eighth = make_tap(480, 2);
  eighth.note_type = NoteType::HoldEighth;
  hold_doc.add_note(eighth);
  NotationNote star = make_tap(240, 2);
  star.note_type = NoteType::Sound;
  hold_doc.add_note(star);
  CHECK_EQ(static_cast<int32_t>(hold_doc.concurrent_lines().size()), 0);

  // Hold tail + tap at the same end time → sync line (tail is a hit, not body/star).
  NotationNote tap_at_end = make_tap(960, 6);
  hold_doc.add_note(tap_at_end);
  CHECK_EQ(static_cast<int32_t>(hold_doc.concurrent_lines().size()), 1);
}

void test_explicit_note_id_zero() {
  ChartDocument doc;
  NotationNote note = make_tap(120, 3);
  note.id = 0;
  const int32_t id = doc.add_note(note);

  CHECK_EQ(id, 0);
  CHECK_EQ(doc.next_note_id(), 1);

  const auto found = doc.find_note(0);
  CHECK(found.has_value());
  CHECK_EQ(found->lane, 3);
}

void test_normalize_for_save_reassigns_zero_based() {
  ChartDocument doc;

  NotationNote late = make_tap(960, 2);
  late.id = 99;
  NotationNote early = make_tap(0, 0);
  early.id = 42;
  NotationNote mid = make_tap(480, 1);
  mid.end_tick = 960;
  mid.id = 7;

  doc.set_notes({late, early, mid});
  doc.normalize_for_save();

  const auto& notes = doc.notes();
  CHECK_EQ(static_cast<int32_t>(notes.size()), 3);
  CHECK_EQ(notes[0].start_tick, 0);
  CHECK_EQ(notes[1].start_tick, 480);
  CHECK_EQ(notes[2].start_tick, 960);
  CHECK_EQ(notes[0].id, 0);
  CHECK_EQ(notes[1].id, 1);
  CHECK_EQ(notes[2].id, 2);
  CHECK_EQ(doc.next_note_id(), 3);
}

void test_save_reload_normalizes_and_reloads() {
  ChartEditorEngine engine;

  NotationNote split = make_tap(0, 0);
  split.end_tick = 1920;
  split.width = 6;
  split.gimmick_type = GimmickType::Split3;
  engine.add_note(split);

  NotationNote tap = make_tap(480, 2);
  engine.add_note(tap);

  const fs::path path = temp_chart_path("save_reload.wdschart");
  const auto save = engine.save_to_file(path.string());
  CHECK_EQ(static_cast<int>(save.error), static_cast<int>(SerializeError::Ok));
  CHECK(!engine.is_dirty());

  // Scheme B: live document ids are unchanged after save.
  const auto& notes = engine.document().notes();
  CHECK_EQ(static_cast<int32_t>(notes.size()), 2);
  CHECK(engine.document().find_note(0).has_value());
  CHECK(engine.document().find_note(1).has_value());

  ChartEditorEngine reloaded;
  const auto load = reloaded.load_from_file(path.string());
  CHECK_EQ(static_cast<int>(load.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int32_t>(reloaded.document().notes().size()), 2);
  CHECK_EQ(reloaded.document().notes()[0].id, 0);
  CHECK_EQ(reloaded.document().notes()[1].id, 1);
  CHECK_EQ(reloaded.document().notes()[0].start_tick, 0);
  CHECK_EQ(reloaded.document().notes()[1].start_tick, 480);
}

void test_save_success_preserves_history_and_ids() {
  ChartEditorEngine engine;
  NotationNote late = make_tap(960, 2);
  late.id = 99;
  NotationNote early = make_tap(0, 0);
  early.id = 42;
  CHECK(engine.document().set_notes({late, early}));
  CHECK(engine.execute_command(
      std::make_unique<AddNotesCommand>(std::vector<NotationNote>{make_tap(480, 1)})));
  CHECK(engine.history().can_undo());
  CHECK(engine.document().find_note(99).has_value());
  CHECK(engine.document().find_note(42).has_value());

  const fs::path path = temp_chart_path("save_keeps_history.wdschart");
  const auto save = engine.save_to_file(path.string());
  CHECK_EQ(static_cast<int>(save.error), static_cast<int>(SerializeError::Ok));
  CHECK(!engine.is_dirty());
  CHECK(engine.history().can_undo());
  CHECK(engine.document().find_note(99).has_value());
  CHECK(engine.document().find_note(42).has_value());
  CHECK(engine.history().undo(engine.document()));
  // Undo removes the added tap; sparse ids for the original pair remain.
  CHECK_EQ(static_cast<int32_t>(engine.document().notes().size()), 2);
  CHECK(engine.document().find_note(99).has_value());
  CHECK(engine.document().find_note(42).has_value());

  ChartEditorEngine reloaded;
  CHECK_EQ(static_cast<int>(reloaded.load_from_file(path.string()).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int32_t>(reloaded.document().notes().size()), 3);
  CHECK_EQ(reloaded.document().notes()[0].id, 0);
  CHECK_EQ(reloaded.document().notes()[1].id, 1);
  CHECK_EQ(reloaded.document().notes()[2].id, 2);
}

void test_replace_file_atomic_preserves_target_on_failure() {
  const fs::path target = temp_chart_path("atomic_keep.txt");
  {
    std::ofstream out(target);
    out << "ORIGINAL";
  }
  const fs::path missing_temp = temp_chart_path("atomic_missing.wds-tmp");
  std::error_code ec;
  fs::remove(missing_temp, ec);
  const auto result = replace_file_atomic(target.string(), missing_temp.string());
  CHECK_NE(static_cast<int>(result.error), static_cast<int>(SerializeError::Ok));
  SerializeResult read_status;
  const std::string body = read_text_file(target.string(), read_status);
  CHECK_EQ(static_cast<int>(read_status.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(body, std::string("ORIGINAL"));
}

void test_load_rejects_huge_notes_count() {
  const fs::path path = temp_chart_path("huge_notes_count.wdschart");
  {
    std::ofstream out(path);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 0\nNOTES 1000001\nCONCURRENT 0\nEND\n";
  }
  NotationChart chart;
  const auto result = ChartSerializer::load_from_file(path.string(), chart);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(SerializeError::ParseError));
}

void test_load_rejects_lane_width_overflow() {
  const fs::path path = temp_chart_path("lane_width_overflow.wdschart");
  {
    std::ofstream out(path);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 0\nNOTES 1\n"
           "N 0 0 0 10 2000000000 2000000000 0 0\nCONCURRENT 0\nEND\n";
  }
  NotationChart chart;
  const auto result = ChartSerializer::load_from_file(path.string(), chart);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(SerializeError::ParseError));
}

void test_load_rejects_nonfinite_ticks() {
  const fs::path path = temp_chart_path("nonfinite_ticks.wdschart");
  {
    std::ofstream out(path);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 0\nNOTES 1\n"
           "N 0 nan inf 10 0 1 0 0\nCONCURRENT 0\nEND\n";
  }
  NotationChart chart;
  const auto result = ChartSerializer::load_from_file(path.string(), chart);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(SerializeError::ParseError));

  const fs::path path2 = temp_chart_path("huge_ticks.wdschart");
  {
    std::ofstream out(path2);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 0\nNOTES 1\n"
           "N 0 1e300 1e300 10 0 1 0 0\nCONCURRENT 0\nEND\n";
  }
  const auto result2 = ChartSerializer::load_from_file(path2.string(), chart);
  CHECK_EQ(static_cast<int>(result2.error), static_cast<int>(SerializeError::ParseError));
}

void test_measure_ticks_no_hang_near_int_max() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  // int32 t+=step near INT32_MAX would wrap negative and hang; int64 must finish.
  const auto ticks =
      measure_ticks_in_range(INT32_MAX - 5000, INT32_MAX - 10, timing);
  CHECK(ticks.size() < 100u);
}

void test_sus_rejects_huge_measurebs() {
  const std::string text =
      "#MEASUREBS 2000000000\n"
      "#00002: 1010\n";
  SusChartLoadResult loaded;
  const auto result = SusChartFormat::parse(text, loaded);
  // Invalid MEASUREBS is ignored or rejected — must not hang.
  CHECK(result.error == SerializeError::Ok || result.error == SerializeError::ParseError);
}

void test_export_sus_after_edit_preserves_engine_history() {
  ChartEditorEngine engine;
  CHECK(engine.execute_command(
      std::make_unique<AddNotesCommand>(std::vector<NotationNote>{make_tap(0, 0)})));
  CHECK(engine.history().can_undo());
  const fs::path path = temp_chart_path("export_keeps_history.sus");
  const auto exported = engine.export_sus_to_file(path.string(), SusChartSaveOptions{});
  CHECK_EQ(static_cast<int>(exported.error), static_cast<int>(SerializeError::Ok));
  CHECK(engine.history().can_undo());
}

void test_load_fixture_normalized_chart() {
  const fs::path path = fixture_path("normalized_chart.wdschart");
  CHECK(fs::exists(path));

  ChartEditorEngine engine;
  const auto load = engine.load_from_file(path.string());
  CHECK_EQ(static_cast<int>(load.error), static_cast<int>(SerializeError::Ok));

  const auto& notes = engine.document().notes();
  CHECK_EQ(static_cast<int32_t>(notes.size()), 3);

  const auto n0 = engine.document().find_note(0);
  const auto n1 = engine.document().find_note(1);
  const auto n2 = engine.document().find_note(2);
  CHECK(n0.has_value());
  CHECK(n1.has_value());
  CHECK(n2.has_value());
  CHECK_EQ(n0->start_tick, 0);
  CHECK_EQ(n1->start_tick, 480);
  CHECK_EQ(n2->gimmick_type, GimmickType::Split3);
  // load_from_chart rebuilds concurrent lines from notes (file CONCURRENT may be stale).
  // Fixture has one Normal + Split at tick 0 → no multi-press concurrent line.
  CHECK_EQ(static_cast<int32_t>(engine.document().concurrent_lines().size()), 0);
}

void test_start_ms_avl_index_range_query() {
  detail::StartMsAvlIndex index;
  index.insert(100, 10);
  index.insert(100, 11);
  index.insert(250, 20);
  index.insert(400, 30);

  std::vector<int32_t> ids;
  index.for_each_in_range(90, 260, [&](int64_t key, int32_t note_id) {
    ids.push_back(static_cast<int32_t>(key));
    ids.push_back(note_id);
  });

  CHECK_EQ(static_cast<int32_t>(ids.size()), 6);
  CHECK_EQ(ids[0], 100);
  CHECK_EQ(ids[1], 10);
  CHECK_EQ(ids[2], 100);
  CHECK_EQ(ids[3], 11);
  CHECK_EQ(ids[4], 250);
  CHECK_EQ(ids[5], 20);

  ids.clear();
  index.for_each_up_to(150, [&](int64_t /*key*/, int32_t note_id) { ids.push_back(note_id); });
  CHECK_EQ(static_cast<int32_t>(ids.size()), 2);
  CHECK_EQ(ids[0], 10);
  CHECK_EQ(ids[1], 11);

  index.erase(100, 10);
  ids.clear();
  index.for_each_in_range(0, 500, [&](int64_t /*key*/, int32_t note_id) { ids.push_back(note_id); });
  CHECK_EQ(static_cast<int32_t>(ids.size()), 3);
  CHECK(std::find(ids.begin(), ids.end(), 10) == ids.end());
}

void test_chart_note_index_candidates() {
  ChartDocument doc;
  doc.add_note(make_tap(0, 0));
  doc.add_note(make_tap(480, 1));
  doc.add_note(make_tap(960, 2));

  // start_ms ≈ 0, 500, 1000 at default 120 BPM / 480 TPQ
  std::vector<int32_t> candidates;
  doc.index().query_candidates(750, 300, 300, candidates);
  CHECK_EQ(static_cast<int32_t>(candidates.size()), 2);

  std::sort(candidates.begin(), candidates.end());
  CHECK_EQ(candidates[0], 1);
  CHECK_EQ(candidates[1], 2);
}

void test_remove_note_keeps_zero_based_ids() {
  ChartDocument doc;
  doc.add_note(make_tap(0, 0));
  doc.add_note(make_tap(480, 1));
  doc.add_note(make_tap(960, 2));

  CHECK(doc.remove_note(1));
  CHECK(!doc.find_note(1).has_value());
  CHECK(doc.find_note(0).has_value());
  CHECK(doc.find_note(2).has_value());
  CHECK_EQ(doc.next_note_id(), 3);
}

bool snapshot_contents_equal(PreviewSnapshot a, PreviewSnapshot b) {
  if (a.notes.size() != b.notes.size() || a.split_lanes.size() != b.split_lanes.size() ||
      a.concurrent_lines.size() != b.concurrent_lines.size() ||
      a.active_lane_count != b.active_lane_count) {
    return false;
  }

  auto by_note_id = [](const PreviewNoteInstance& lhs, const PreviewNoteInstance& rhs) {
    return lhs.note_id < rhs.note_id;
  };
  std::sort(a.notes.begin(), a.notes.end(), by_note_id);
  std::sort(b.notes.begin(), b.notes.end(), by_note_id);
  for (size_t i = 0; i < a.notes.size(); ++i) {
    if (a.notes[i].note_id != b.notes[i].note_id) {
      return false;
    }
    if (a.notes[i].visual_state != b.notes[i].visual_state) {
      return false;
    }
    if (std::abs(a.notes[i].pos_y - b.notes[i].pos_y) > 1e-4f) {
      return false;
    }
  }

  auto by_split_id = [](const PreviewSplitLaneInstance& lhs,
                        const PreviewSplitLaneInstance& rhs) {
    return lhs.source_note_id < rhs.source_note_id;
  };
  std::sort(a.split_lanes.begin(), a.split_lanes.end(), by_split_id);
  std::sort(b.split_lanes.begin(), b.split_lanes.end(), by_split_id);
  for (size_t i = 0; i < a.split_lanes.size(); ++i) {
    if (a.split_lanes[i].source_note_id != b.split_lanes[i].source_note_id) {
      return false;
    }
    if (a.split_lanes[i].effective_lane_count != b.split_lanes[i].effective_lane_count) {
      return false;
    }
    if (a.split_lanes[i].is_continued != b.split_lanes[i].is_continued) {
      return false;
    }
  }

  auto by_line = [](const PreviewConcurrentLineInstance& lhs,
                    const PreviewConcurrentLineInstance& rhs) {
    if (lhs.milliseconds != rhs.milliseconds) {
      return lhs.milliseconds < rhs.milliseconds;
    }
    return lhs.start_lane < rhs.start_lane;
  };
  std::sort(a.concurrent_lines.begin(), a.concurrent_lines.end(), by_line);
  std::sort(b.concurrent_lines.begin(), b.concurrent_lines.end(), by_line);
  for (size_t i = 0; i < a.concurrent_lines.size(); ++i) {
    if (a.concurrent_lines[i].milliseconds != b.concurrent_lines[i].milliseconds ||
        a.concurrent_lines[i].start_lane != b.concurrent_lines[i].start_lane) {
      return false;
    }
    if (std::abs(a.concurrent_lines[i].pos_y - b.concurrent_lines[i].pos_y) > 1e-4f) {
      return false;
    }
  }
  return true;
}

void test_snapshot_incremental_tick() {
  ChartEditorEngine engine;
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  engine.document().set_timing(timing);

  for (int i = 0; i < 8; ++i) {
    engine.add_note(make_tap(i * 480, i % 6));
  }

  // Large scrub forces FullRebuild; small tick should then IncrementalPatch.
  engine.seek(20000);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::FullRebuild));
  engine.seek(0);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::FullRebuild));

  engine.play();
  engine.tick(16);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::IncrementalPatch));

  PreviewSnapshotBuilder builder(engine.preview_config());
  const PreviewSnapshot full =
      builder.build(engine.document().notes(), engine.document().timing(),
                    engine.document().concurrent_lines(), engine.document().index(),
                    engine.timeline_ms(), engine.playback_state(),
                    engine.document().content_generation());
  CHECK(snapshot_contents_equal(engine.snapshot(), full));
  CHECK(engine.snapshot().find_note(0) != nullptr);
}

void test_snapshot_aux_objects_incremental() {
  ChartEditorEngine engine;
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  engine.document().set_timing(timing);

  NotationNote split = make_tap(0, 0);
  split.end_tick = 1920;
  split.width = 6;
  split.gimmick_type = GimmickType::Split3;
  engine.add_note(split);
  engine.add_note(make_tap(480, 2));

  ConcurrentLineNote line;
  line.milliseconds = 500;
  line.start_lane = 1;
  line.width = 2;
  engine.document().set_concurrent_lines({line});
  engine.rebuild_snapshot();

  engine.seek(20000);
  engine.seek(250);
  CHECK(engine.snapshot().find_split_lane(0) != nullptr);
  CHECK_EQ(engine.snapshot().active_lane_count, 9);

  engine.play();
  engine.tick(16);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::IncrementalPatch));
  CHECK(engine.snapshot().find_split_lane(0) != nullptr);
  CHECK(engine.snapshot().find_concurrent_line(500, 1) != nullptr);

  PreviewSnapshotBuilder builder(engine.preview_config());
  const PreviewSnapshot full =
      builder.build(engine.document().notes(), engine.document().timing(),
                    engine.document().concurrent_lines(), engine.document().index(),
                    engine.timeline_ms(), engine.playback_state(),
                    engine.document().content_generation());
  CHECK(snapshot_contents_equal(engine.snapshot(), full));

  PreviewSnapshot snap;
  PreviewSplitLaneInstance split_inst;
  split_inst.apply_identity(7, 3, SplitLaneType::BothEnds, 0, 0, 1000);
  split_inst.apply_state(true, true, 9);
  snap.upsert_split_lane(split_inst);
  CHECK(snap.find_split_lane(7) != nullptr);
  CHECK(snap.remove_split_lane(7));

  PreviewConcurrentLineInstance line_inst;
  line_inst.apply_identity(100, 2, 1);
  line_inst.apply_layout(3.5f);
  snap.upsert_concurrent_line(line_inst);
  CHECK(snap.find_concurrent_line(100, 2) != nullptr);
  CHECK(snap.remove_concurrent_line(100, 2));
}

void test_snapshot_large_seek_full_rebuild() {
  ChartEditorEngine engine;
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  engine.document().set_timing(timing);
  engine.add_note(make_tap(0, 0));
  engine.add_note(make_tap(480, 1));
  engine.add_note(make_tap(960, 2));

  engine.seek(0);
  engine.tick(16);
  engine.seek(5000);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::FullRebuild));
}

void test_preview_note_and_snapshot_incremental_api() {
  PreviewSnapshot snapshot;
  PreviewNoteInstance a;
  a.note_id = 1;
  a.apply_layout(1.0f, 2.0f, 3.0f, 4.0f, 0.0f);
  a.apply_visual(PreviewNoteVisualState::Approaching, false);
  snapshot.upsert_note(a);

  PreviewNoteInstance* found = snapshot.find_note(1);
  CHECK(found != nullptr);
  found->apply_layout(10.0f, 20.0f, 3.0f, 4.0f, 0.0f);
  CHECK_EQ(snapshot.notes.size(), static_cast<size_t>(1));
  CHECK_EQ(snapshot.notes[0].pos_x, 10.0f);

  PreviewNoteInstance b = a;
  b.note_id = 2;
  snapshot.upsert_note(b);
  CHECK_EQ(snapshot.notes.size(), static_cast<size_t>(2));
  CHECK(snapshot.remove_note(1));
  CHECK(snapshot.find_note(1) == nullptr);
  CHECK(snapshot.find_note(2) != nullptr);

  const size_t cap = snapshot.notes.capacity();
  snapshot.clear_keep_capacity();
  CHECK_EQ(snapshot.notes.size(), static_cast<size_t>(0));
  CHECK(snapshot.notes.capacity() >= cap);
}

void test_split_gimmick_range_and_combo_includes_heads() {
  CHECK(!is_split_lane_gimmick(GimmickType::None));
  CHECK(!is_split_lane_gimmick(GimmickType::JumpScratch));
  CHECK(is_split_lane_gimmick(GimmickType::Split3));
  CHECK(is_split_lane_gimmick(GimmickType::IgnoreSplit6));
  CHECK(!is_split_lane_gimmick(static_cast<GimmickType>(100)));

  MusicTiming timing;
  timing.bpm = 160.0;
  timing.ticks_per_quarter = 480;

  std::vector<NotationNote> notes;
  auto push = [&](float tick, NoteType type, int32_t lane = 0) {
    NotationNote n;
    n.id = static_cast<int32_t>(notes.size());
    n.start_tick = tick;
    n.end_tick = tick;
    n.lane = lane;
    n.width = 2;
    n.note_type = type;
    notes.push_back(n);
  };
  push(480, NoteType::Normal, 2);
  push(480, NoteType::Critical, 8);
  {
    NotationNote hs;
    hs.id = 2;
    hs.start_tick = 960;
    hs.end_tick = 1440;
    hs.lane = 1;
    hs.width = 3;
    hs.note_type = NoteType::HoldStart;
    notes.push_back(hs);
    NotationNote body = hs;
    body.id = 3;
    body.note_type = NoteType::Hold;
    notes.push_back(body);
    // Soft body judge must be authored as HoldEighth (not synthesized).
    NotationNote eighth = hs;
    eighth.id = 4;
    eighth.start_tick = 1200;
    eighth.end_tick = 1200;
    eighth.note_type = NoteType::HoldEighth;
    notes.push_back(eighth);
  }

  const int64_t at = tick_to_milliseconds(1500, timing);
  const PreviewComboState combo = compute_preview_combo(notes, timing, at);
  // 2 taps + HoldStart + HoldEighth + Hold tail
  CHECK_EQ(combo.combo, 5);
}

void test_split_appear_phase_before_start_ms() {
  ChartEditorEngine engine;
  PreviewConfig cfg;
  cfg.lane_count = 12;
  cfg.split_line_animation_start_sec = 0.75f;
  cfg.split_line_animation_end_sec = 0.20f;
  engine.set_preview_config(cfg);

  MusicTiming timing;
  timing.bpm = 160.0;
  timing.ticks_per_quarter = 480;
  engine.document().set_timing(timing);

  NotationNote split;
  split.start_tick = 4800;
  split.end_tick = 5760;
  split.lane = 0;
  split.width = 12;
  split.note_type = NoteType::Normal;
  split.gimmick_type = GimmickType::Split3;
  engine.add_note(split);

  const int64_t start_ms = tick_to_milliseconds(4800, timing);
  // Mid-appear: before beat, after beat-appear
  engine.seek(start_ms - 375);
  engine.rebuild_snapshot();
  CHECK_EQ(static_cast<int32_t>(engine.snapshot().split_lanes.size()), 1);
  CHECK_EQ(engine.snapshot().split_lanes[0].split_anim_phase, 0);
  CHECK(engine.snapshot().split_lanes[0].stage_cover_alpha > 0.1f);
  CHECK(engine.snapshot().split_lanes[0].stage_cover_alpha < 0.9f);
  CHECK(engine.snapshot().split_lanes[0].split_percent_start > 0.1f);

  engine.seek(start_ms);
  engine.rebuild_snapshot();
  CHECK_EQ(engine.snapshot().split_lanes[0].split_anim_phase, 1);
  CHECK_EQ(engine.snapshot().split_lanes[0].stage_cover_alpha, 0.0f);
}

void test_hold_start_visible_with_zero_end_tick() {
  // Official HoldStart rows historically imported as end_tick=0 while start_tick>0.
  // Heads must still approach / AutoHit and remain in the snapshot for SFX.
  ChartEditorEngine engine;
  MusicTiming timing;
  timing.bpm = 60.0;
  timing.ticks_per_quarter = 480;
  timing.offset_ms = 3019;
  engine.document().set_timing(timing);

  wds::chart_editor::PreviewConfig cfg;
  cfg.lane_count = 12;
  cfg.note_approach_seconds = 1.8f;
  cfg.auto_hit_feedback_ms = 500;
  engine.set_preview_config(cfg);

  NotationNote hs;
  hs.start_tick = 480;  // 1s @ BPM 60
  hs.end_tick = 0;      // buggy legacy / pre-fix import shape
  hs.lane = 0;
  hs.width = 3;
  hs.note_type = NoteType::HoldStart;
  engine.add_note(hs);

  NotationNote body;
  body.start_tick = 480;
  body.end_tick = 960;
  body.lane = 0;
  body.width = 3;
  body.note_type = NoteType::Hold;
  engine.add_note(body);

  const int64_t start_ms = tick_to_milliseconds(480, timing);
  CHECK_EQ(start_ms, 3019 + 1000);

  // Mid-approach: head must be visible (Approaching), not expired against end_ms=offset.
  engine.seek(start_ms - 500);
  engine.rebuild_snapshot();
  const PreviewNoteInstance* head = nullptr;
  for (const auto& n : engine.snapshot().notes) {
    if (n.note_type == NoteType::HoldStart) {
      head = &n;
    }
  }
  CHECK(head != nullptr);
  CHECK_EQ(static_cast<int>(head->visual_state),
           static_cast<int>(PreviewNoteVisualState::Approaching));

  // At judgment: AutoHit so SFX collector can see the head.
  engine.seek(start_ms);
  engine.rebuild_snapshot();
  head = nullptr;
  for (const auto& n : engine.snapshot().notes) {
    if (n.note_type == NoteType::HoldStart) {
      head = &n;
    }
  }
  CHECK(head != nullptr);
  CHECK_EQ(static_cast<int>(head->visual_state),
           static_cast<int>(PreviewNoteVisualState::AutoHit));
  CHECK_EQ(head->start_ms, start_ms);
  CHECK_EQ(head->end_ms, start_ms);  // end_ms() clamps instantaneous notes
}

void test_load_legacy_v1_wdschart() {
  const fs::path path = fixture_path("legacy_v1.wdschart");
  CHECK(fs::exists(path));

  ChartEditorEngine engine;
  const auto load = engine.load_from_file(path.string());
  CHECK_EQ(static_cast<int>(load.error), static_cast<int>(SerializeError::Ok));
  CHECK(engine.is_editable());
  CHECK_EQ(static_cast<int32_t>(engine.document().notes().size()), 2);
  CHECK_EQ(static_cast<int>(engine.document().notes()[1].note_type),
           static_cast<int>(NoteType::Critical));
  CHECK_EQ(engine.document().notes()[1].width, 2);
  CHECK_EQ(engine.document().notes()[1].scratch_length, 0);
}

void test_official_chart_import_and_roundtrip() {
  const fs::path chart_path = fixture_path("official_sample.csv");
  const fs::path music_path = fixture_path("official_music_config.csv");
  CHECK(fs::exists(chart_path));
  CHECK(fs::exists(music_path));

  OfficialMusicConfig music;
  const auto music_load = OfficialChartFormat::load_music_config_file(music_path.string(), music);
  CHECK_EQ(static_cast<int>(music_load.error), static_cast<int>(SerializeError::Ok));
  CHECK(std::abs(music.delay_seconds - 3.019) < 1e-6);

  ChartEditorEngine engine;
  const auto load = engine.load_official_from_file(chart_path.string(), music_path.string());
  CHECK_EQ(static_cast<int>(load.error), static_cast<int>(SerializeError::Ok));

  const auto& timing = engine.document().timing();
  CHECK_EQ(static_cast<int>(std::lround(timing.bpm)), 60);
  CHECK_EQ(timing.ticks_per_quarter, 480);
  CHECK_EQ(timing.offset_ms, 3019);

  const auto& notes = engine.document().notes();
  CHECK_EQ(static_cast<int32_t>(notes.size()), 13);

  // Document sorts by start time — first note is the split at t=0, not CSV row 0.
  const auto& first = notes[0];
  CHECK_EQ(static_cast<int>(first.note_type), static_cast<int>(NoteType::None));
  CHECK_EQ(static_cast<int>(first.gimmick_type), static_cast<int>(GimmickType::Split3));
  CHECK_EQ(first.scratch_length, 10170);

  bool found_critical = false;
  bool found_split = false;
  bool found_jump = false;
  bool found_flick = false;
  bool found_orphan_as_flick = false;
  for (const auto& n : notes) {
    if (n.note_type == NoteType::Critical && n.width == 4 && n.lane == 0) {
      found_critical = true;
      // seconds → ms (llround) → tick (llround); not plain seconds*tpq.
      MusicTiming local = timing;
      local.offset_ms = 0;
      CHECK_EQ(n.start_tick,
               milliseconds_to_tick(static_cast<int64_t>(std::llround(1.0169 * 1000.0)), local));
      // Instantaneous official rows: end_tick == start_tick (not 0).
      CHECK_EQ(n.end_tick, n.start_tick);
    }
    if (n.note_type == NoteType::None && n.gimmick_type == GimmickType::Split3) {
      found_split = true;
      CHECK_EQ(n.scratch_length, 10170);
      CHECK_EQ(n.lane, 0);
      CHECK_EQ(n.width, 0);
    }
    if (n.gimmick_type == GimmickType::JumpScratch) {
      found_jump = true;
      CHECK_EQ(n.scratch_length, 6);
      CHECK_EQ(static_cast<int>(n.note_type), static_cast<int>(NoteType::ScratchHold));
      CHECK_EQ(n.lane, 0);  // official lane 1
    }
    if (n.note_type == NoteType::Flick && n.width == 6) {
      found_flick = true;
      CHECK_EQ(n.lane, 3);  // official 4
    }
    // Official sample orphan type=40 (Sirius SoundPurple) → Flick.
    if (n.note_type == NoteType::Flick && n.width == 3 && n.lane == 3) {
      found_orphan_as_flick = true;
      MusicTiming local = timing;
      local.offset_ms = 0;
      CHECK_EQ(n.start_tick,
               milliseconds_to_tick(static_cast<int64_t>(std::llround(15.4237 * 1000.0)), local));
    }
  }
  CHECK(found_critical);
  CHECK(found_split);
  CHECK(found_jump);
  CHECK(found_flick);
  CHECK(found_orphan_as_flick);

  const fs::path out_path = temp_chart_path("official_roundtrip.csv");
  const auto save = engine.export_official_to_file(out_path.string());
  CHECK_EQ(static_cast<int>(save.error), static_cast<int>(SerializeError::ReadOnly));
  CHECK(engine.is_read_only());

  // Official import is preview-only: mutations and .wdschart save are rejected.
  CHECK_EQ(engine.add_note(make_tap(100, 1)), -1);
  CHECK(!engine.remove_note(0));
  CHECK_EQ(static_cast<int>(engine.save_to_file(temp_chart_path("should_fail.wdschart").string()).error),
           static_cast<int>(SerializeError::ReadOnly));

}

void test_official_chart_load_auto() {
  const fs::path chart_path = fixture_path("official_sample.csv");
  NotationChart chart;
  const auto result = ChartSerializer::load_auto(chart_path.string(), chart);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int32_t>(chart.notes.size()), 13);
}

void test_load_repo_test_official_charts() {
  const fs::path candidates[] = {
      fs::path("test/1.csv"),
      fs::path("../test/1.csv"),
      fs::path(__FILE__).parent_path().parent_path().parent_path() / "test" / "1.csv",
  };
  fs::path chart;
  for (const auto& p : candidates) {
    if (fs::exists(p)) {
      chart = p;
      break;
    }
  }
  if (chart.empty()) {
    return;
  }

  const fs::path music = chart.parent_path() / "music_config.csv";
  ChartEditorEngine engine;
  const auto load = engine.load_official_from_file(chart.string(), music.string());
  CHECK_EQ(static_cast<int>(load.error), static_cast<int>(SerializeError::Ok));
  CHECK(engine.document().notes().size() > 100);
  CHECK_EQ(engine.document().timing().offset_ms, 3019);

  for (int i = 2; i <= 5; ++i) {
    const fs::path p = chart.parent_path() / (std::to_string(i) + ".csv");
    CHECK(fs::exists(p));
    ChartEditorEngine e;
    const auto r = e.load_official_from_file(p.string(), music.string());
    CHECK_EQ(static_cast<int>(r.error), static_cast<int>(SerializeError::Ok));
    CHECK(!e.document().notes().empty());
  }
}

void test_wdsproject_format_roundtrip_and_relative_paths() {
  const fs::path project_path = fixture_path("sample.wdsproject");
  CHECK(fs::exists(project_path));

  WdsProject project;
  const auto load = ProjectSerializer::load_from_file(project_path.string(), project);
  CHECK_EQ(static_cast<int>(load.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(project.music_path, std::string("music.ogg"));
  CHECK_EQ(project.offset_ms, 3019);
  CHECK_EQ(project.chart_path(), std::string("chart.wdschart"));

  const std::string resolved_chart =
      ProjectSerializer::resolve_path(project_path.string(), project.chart_path());
  CHECK(fs::exists(resolved_chart));
  CHECK_EQ(ProjectSerializer::resolve_path(project_path.string(), "music.ogg"),
           (project_path.parent_path() / "music.ogg").generic_string());

  const fs::path temp_dir = temp_chart_path("project_roundtrip_dir");
  fs::create_directories(temp_dir);
  const fs::path temp_project = temp_dir / "song.wdsproject";
  const fs::path temp_chart = temp_dir / "notes.wdschart";
  const fs::path temp_music = temp_dir / "bgm" / "track.ogg";
  fs::create_directories(temp_music.parent_path());
  {
    std::ofstream touch(temp_music.string(), std::ios::binary | std::ios::trunc);
    touch << "placeholder";
  }

  ChartEditorEngine engine;
  NotationNote tap = make_tap(0, 1);
  engine.add_note(tap);
  MusicTiming timing = engine.document().timing();
  timing.offset_ms = 1500;
  engine.document().set_timing(timing);

  WdsProject to_save;
  to_save.music_path = temp_music.string();  // absolute → should relativize
  to_save.chart_paths = {"notes.wdschart"};
  to_save.offset_ms = 0;  // overwritten from document on save

  const auto save = engine.save_project_to_file(temp_project.string(), to_save);
  CHECK_EQ(static_cast<int>(save.error), static_cast<int>(SerializeError::Ok));
  CHECK(fs::exists(temp_chart));
  CHECK(fs::exists(temp_project));

  WdsProject reloaded_meta;
  const auto meta_load = ProjectSerializer::load_from_file(temp_project.string(), reloaded_meta);
  CHECK_EQ(static_cast<int>(meta_load.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(reloaded_meta.offset_ms, 1500);
  CHECK_EQ(reloaded_meta.chart_path(), std::string("notes.wdschart"));
  CHECK_EQ(reloaded_meta.music_path, std::string("bgm/track.ogg"));

  ChartEditorEngine opened;
  WdsProject opened_meta;
  const auto open = opened.load_project_from_file(temp_project.string(), &opened_meta);
  CHECK_EQ(static_cast<int>(open.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(opened.document().timing().offset_ms, 1500);
  CHECK_EQ(static_cast<int32_t>(opened.document().notes().size()), 1);
  CHECK_EQ(opened_meta.music_path, std::string("bgm/track.ogg"));

  // Project CHART_DELAY_MS is the song chart delay applied after loading the chart.
  ChartEditorEngine from_fixture;
  const auto fixture_open = from_fixture.load_project_from_file(project_path.string());
  CHECK_EQ(static_cast<int>(fixture_open.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(from_fixture.document().timing().offset_ms, 3019);
  CHECK_EQ(static_cast<int32_t>(from_fixture.document().notes().size()), 2);

  // Saved .wdschart must not contain chart-delay keys.
  {
    std::ifstream chart_file(temp_chart.string(), std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(chart_file)),
                         std::istreambuf_iterator<char>());
    CHECK(contents.find("OFFSET_MS") == std::string::npos);
    CHECK(contents.find("CHART_DELAY_MS") == std::string::npos);
  }

  // Saved .wdsproject uses CHART_DELAY_MS (legacy OFFSET_MS still loads).
  {
    std::ifstream project_file(temp_project.string(), std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(project_file)),
                         std::istreambuf_iterator<char>());
    CHECK(contents.find("CHART_DELAY_MS") != std::string::npos);
  }

  // Standalone chart load leaves offset at 0; legacy OFFSET_MS in file is ignored.
  ChartEditorEngine legacy;
  const auto legacy_load = legacy.load_from_file(fixture_path("legacy_v1.wdschart").string());
  CHECK_EQ(static_cast<int>(legacy_load.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(legacy.document().timing().offset_ms, 0);
}

void test_edit_grid_and_note_operations() {
  EditGridConfig grid;
  CHECK_EQ(round_to_int_tick(2.5f), 3);
  CHECK_EQ(subdivision_tick_step(grid), 120);
  CHECK_EQ(snap_tick(181.0f, grid), 240);
  CHECK_EQ(snap_lane(11.8f, grid), 11);
  CHECK_EQ(static_cast<int32_t>(subdivision_ticks_in_range(0, 480, grid).size()), 3);

  // Off-grid origin + grid delta must land on a division line (drag-move snap).
  CHECK_EQ(snap_tick(100.0f, grid) + 120, 240);

  // Odd subdivs: fixed-step accumulation left a gap before the next beat; exact
  // (i*beat)/subdivs must land flush on beat edges and still evenly space interiors.
  {
    EditGridConfig odd = grid;
    odd.subdivisions_per_beat = 7;
    const auto ticks = subdivision_ticks_in_range(0, 480, odd);
    CHECK_EQ(static_cast<int32_t>(ticks.size()), 6);
    CHECK_EQ(ticks.front(), static_cast<int32_t>((1 * 480) / 7));
    CHECK_EQ(ticks.back(), static_cast<int32_t>((6 * 480) / 7));
    CHECK(std::find(ticks.begin(), ticks.end(), 476) == ticks.end());
    CHECK_EQ(snap_tick(0.0f, odd), 0);
    CHECK_EQ(snap_tick(480.0f, odd), 480);
    CHECK_EQ(snap_tick(470.0f, odd), 480);
    CHECK_EQ(snap_tick(static_cast<float>((6 * 480) / 7), odd), (6 * 480) / 7);
  }

  NotationNote tap = make_tap(480, 3);
  tap.width = 2;
  const NotationNote hold = convert_note_type(tap, NoteType::Hold, 480);
  CHECK_EQ(static_cast<int>(hold.note_type), static_cast<int>(NoteType::Hold));
  CHECK_EQ(hold.end_tick, 960);
  const NotationNote restored = convert_note_type(hold, NoteType::Flick, 480);
  CHECK_EQ(static_cast<int>(restored.note_type), static_cast<int>(NoteType::Flick));
  CHECK_EQ(restored.end_tick, restored.start_tick);

  // ScratchHold → Flick must leave the hold family and clear ScratchHold-only fields.
  NotationNote scratch_body = make_tap(480, 3);
  scratch_body.width = 2;
  scratch_body.end_tick = 960;
  scratch_body.note_type = NoteType::ScratchHold;
  scratch_body.scratch_length = 4;
  scratch_body.gimmick_type = GimmickType::JumpScratch;
  const NotationNote from_scratch = convert_note_type(scratch_body, NoteType::Flick, 480);
  CHECK_EQ(static_cast<int>(from_scratch.note_type), static_cast<int>(NoteType::Flick));
  CHECK_EQ(from_scratch.end_tick, from_scratch.start_tick);
  CHECK_EQ(from_scratch.scratch_length, 1);
  CHECK_EQ(static_cast<int>(from_scratch.gimmick_type), static_cast<int>(GimmickType::None));
  const NotationNote back_to_tap = convert_note_type(from_scratch, NoteType::Normal, 480);
  CHECK_EQ(static_cast<int>(back_to_tap.note_type), static_cast<int>(NoteType::Normal));
  CHECK_EQ(back_to_tap.scratch_length, 0);

  // Lone tap must resolve ConvertHold → Hold (not HoldStart via legacy-head false positive).
  {
    ChartDocument resolve_doc;
    NotationNote lone = make_tap(0, 1);
    lone.id = 1;
    lone.width = 2;
    CHECK(resolve_doc.add_note(lone) == 1);
    CHECK(resolve_convert_target(resolve_doc, *resolve_doc.find_note(1), NoteType::Hold) ==
          NoteType::Hold);
    CHECK(resolve_convert_target(resolve_doc, *resolve_doc.find_note(1), NoteType::ScratchHold) ==
          NoteType::ScratchHold);
  }

  std::vector<NotationNote> notes = {make_tap(0, 0), make_tap(480, 10)};
  notes[1].width = 2;
  mirror_notes(notes, 12);
  CHECK_EQ(notes[0].lane, 11);
  CHECK_EQ(notes[1].lane, 0);
  CHECK(!nudge_notes_lane(notes, 1, 12));
  CHECK(!nudge_notes_time(notes, -1));

  // Flick / ScratchHold scratch_length is a signed direction/span — must flip on mirror
  // so right-extended tails near the left edge do not spill past lane_count after flip.
  NotationNote flick = make_tap(0, 0);
  flick.note_type = NoteType::Flick;
  flick.scratch_length = 1;
  NotationNote scratch_hold = make_tap(0, 0);
  scratch_hold.width = 2;
  scratch_hold.end_tick = 480;
  scratch_hold.note_type = NoteType::ScratchHold;
  scratch_hold.scratch_length = 4;  // end covers [0, 3]
  std::vector<NotationNote> dir_notes = {flick, scratch_hold};
  mirror_notes(dir_notes, 12);
  CHECK_EQ(dir_notes[0].lane, 11);
  CHECK_EQ(dir_notes[0].scratch_length, -1);
  CHECK_EQ(dir_notes[1].lane, 10);
  CHECK_EQ(dir_notes[1].scratch_length, -4);
  {
    const auto range = get_scratch_end_lane_range(dir_notes[1]);
    CHECK_EQ(range.first, 8);
    CHECK_EQ(range.second, 11);
  }

  NotationNote left_flick = make_tap(0, 5);
  left_flick.note_type = NoteType::Flick;
  left_flick.scratch_length = -1;
  NotationNote right_flick = make_tap(0, 7);
  right_flick.note_type = NoteType::Flick;
  right_flick.scratch_length = 1;
  std::vector<NotationNote> center_notes = {left_flick, right_flick};
  mirror_notes_about_center(center_notes);
  CHECK_EQ(center_notes[0].lane, 7);
  CHECK_EQ(center_notes[0].scratch_length, 1);
  CHECK_EQ(center_notes[1].lane, 5);
  CHECK_EQ(center_notes[1].scratch_length, -1);

  // Split-lane color id in scratch_length must not be negated.
  NotationNote split = make_tap(0, 2);
  split.gimmick_type = GimmickType::Split2;
  split.scratch_length = 3;
  std::vector<NotationNote> split_notes = {split};
  mirror_notes(split_notes, 12);
  CHECK_EQ(split_notes[0].lane, 9);
  CHECK_EQ(split_notes[0].scratch_length, 3);
}

void test_timing_bpm_meter_split_and_prune() {
  MusicTiming timing;
  timing.ticks_per_quarter = 480;
  timing.bpm = 120.0;
  timing.points = {
      TimingPoint{0, 120.0, 4, 4, true, true},
      TimingPoint{480, 180.0, 4, 4, true, false},   // BPM-only mid-bar
      TimingPoint{1920, 180.0, 3, 4, false, true},  // 3/4 at bar 2
      TimingPoint{2400, 180.0, 5, 4, false, true},  // orphan after 3/4 edit below
      TimingPoint{3360, 180.0, 4, 4, false, true},  // still a measure under 3/4
  };
  normalize_timing_points(timing);

  // BPM-only mid point must not move measure lines (still every 1920).
  const auto measures = measure_ticks_in_range(0, 4000, timing);
  CHECK(!measures.empty());
  CHECK_EQ(measures[0], 0);
  CHECK_EQ(measures[1], 1920);

  // Beats still step by quarter from meter anchors (BPM does not change tick step).
  const auto beats = beat_ticks_in_range(0, 1000, timing);
  CHECK(std::find(beats.begin(), beats.end(), 0) != beats.end());
  CHECK(std::find(beats.begin(), beats.end(), 480) != beats.end());
  CHECK(std::find(beats.begin(), beats.end(), 960) != beats.end());

  // Meter-aware odd subdivs: last interior tick + next beat must meet (no remainder gap).
  {
    const auto subdivs = subdivision_ticks_in_range(0, 480, timing, 7);
    CHECK(std::find(subdivs.begin(), subdivs.end(), (6 * 480) / 7) != subdivs.end());
    CHECK(std::find(subdivs.begin(), subdivs.end(), 480) != subdivs.end());
    CHECK(std::find(subdivs.begin(), subdivs.end(), 476) == subdivs.end());
    CHECK_EQ(snap_to_subdivision(476, timing, 7), 480);
    CHECK_EQ(snap_to_subdivision((6 * 480) / 7, timing, 7), (6 * 480) / 7);
    CHECK_EQ(subdivision_offset_ticks(480, 7, 7), 480);
  }

  // Edit 3/4 at 1920 → 2400 is no longer a measure; prune until 3360 which still is.
  for (auto& p : timing.points) {
    if (p.tick == 1920) {
      p.numerator = 3;
      p.denominator = 4;
      p.has_meter = true;
    }
  }
  prune_orphaned_meter_changes(timing, 1920);
  bool has_2400_meter = false;
  bool has_3360_meter = false;
  for (const auto& p : timing.points) {
    if (p.tick == 2400 && p.has_meter) has_2400_meter = true;
    if (p.tick == 3360 && p.has_meter) has_3360_meter = true;
  }
  CHECK(!has_2400_meter);
  CHECK(has_3360_meter);
  CHECK(is_measure_tick(3360, timing));

  // Serializer round-trip keeps flags.
  NotationChart chart;
  chart.timing = timing;
  const fs::path path = temp_chart_path("timing_flags.wdschart");
  CHECK_EQ(static_cast<int>(ChartSerializer::save_to_file(chart, path.string()).error),
           static_cast<int>(SerializeError::Ok));
  NotationChart loaded;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(path.string(), loaded).error),
           static_cast<int>(SerializeError::Ok));
  bool found_bpm_only = false;
  for (const auto& p : loaded.timing.points) {
    if (p.tick == 480) {
      CHECK(p.has_bpm);
      CHECK(!p.has_meter);
      found_bpm_only = true;
    }
  }
  CHECK(found_bpm_only);
}

void test_truncated_wdschart_rejected() {
  const fs::path path = temp_chart_path("truncated_missing_end.wdschart");
  {
    std::ofstream out(path);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 120 4 4 3\nNOTES 2\n"
           "N 0 0 0 10 0 1 0 0\n";
  }
  NotationChart chart;
  const auto result = ChartSerializer::load_from_file(path.string(), chart);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(SerializeError::ParseError));

  const fs::path path2 = temp_chart_path("truncated_count_mismatch.wdschart");
  {
    std::ofstream out(path2);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 120 4 4 3\nNOTES 2\n"
           "N 0 0 0 10 0 1 0 0\nCONCURRENT 0\nEND\n";
  }
  const auto result2 = ChartSerializer::load_from_file(path2.string(), chart);
  CHECK_EQ(static_cast<int>(result2.error), static_cast<int>(SerializeError::ParseError));
}

void test_official_csv_tempo_map_export() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.offset_ms = 3000;  // must not affect CSV seconds
  chart.timing.points = {
      TimingPoint{0, 120.0, 4, 4, true, true},
      TimingPoint{480, 240.0, 4, 4, true, false},
  };
  NotationNote note = make_tap(960, 0);
  note.id = 0;
  chart.notes.push_back(note);

  std::string text;
  OfficialChartSaveOptions options;
  options.convert_lane_to_one_based = false;
  const auto result = OfficialChartFormat::serialize_chart(chart, text, options);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(SerializeError::Ok));

  // 480 ticks @120 BPM = 0.5s, then 480 ticks @240 BPM = 0.25s → 0.75s.
  const double start_sec = std::stod(text.substr(0, text.find(',')));
  CHECK(std::abs(start_sec - 0.75) < 1e-3);
}

// Sirius type 40 mid-scratch splits purple holds; HoldEighth/Split export match sus2txt.
void test_official_sound_purple_split_and_row_semantics() {
  // ScratchHold 1..3s lanes 1-3 width 3; mid SoundPurple at 2s with scratchLength=3.
  const std::string csv =
      "1.0,3.0,110,1,3,0,0\n"
      "2.0,-1.0,40,1,3,0,3\n"
      "1.5,-1.0,31,1,3,0,0\n"
      "1.25,-1.0,900,1,3,0,0\n"
      "0.0,4.0,0,-1,0,12,42\n";

  NotationChart chart;
  OfficialChartLoadOptions load_opt;
  load_opt.convert_lane_to_zero_based = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::parse_chart(csv, chart, load_opt).error),
           static_cast<int>(SerializeError::Ok));

  int scratch_holds = 0;
  int jump = 0;
  int stars = 0;
  int eighths = 0;
  int type40 = 0;
  for (const auto& n : chart.notes) {
    if (static_cast<int32_t>(n.note_type) == 40) ++type40;
    if (n.note_type == NoteType::ScratchHold) {
      ++scratch_holds;
      if (n.gimmick_type == GimmickType::JumpScratch) {
        ++jump;
        CHECK_EQ(n.scratch_length, 3);
        CHECK_EQ(n.end_tick, 960);
      }
    }
    if (n.note_type == NoteType::ScratchSound) ++stars;
    if (n.note_type == NoteType::HoldEighth) ++eighths;
  }
  CHECK_EQ(type40, 0);
  CHECK_EQ(scratch_holds, 2);
  CHECK_EQ(jump, 1);
  CHECK_EQ(stars, 1);
  CHECK_EQ(eighths, 1);

  // Export: HoldEighth endTime=-1; Split leftLane=-1; JumpScratch name when scratch≠0.
  NotationChart export_chart;
  export_chart.timing.bpm = 60.0;
  export_chart.timing.ticks_per_quarter = 480;
  {
    NotationNote eighth;
    eighth.id = 0;
    eighth.start_tick = 480;
    eighth.end_tick = 480;
    eighth.note_type = NoteType::HoldEighth;
    eighth.lane = 0;
    eighth.width = 1;
    export_chart.notes.push_back(eighth);

    NotationNote split;
    split.id = 1;
    split.start_tick = 0;
    split.end_tick = 960;
    split.note_type = NoteType::None;
    split.lane = 0;
    split.width = 0;
    split.gimmick_type = GimmickType::Split2;
    split.scratch_length = 7;
    export_chart.notes.push_back(split);

    NotationNote body;
    body.id = 2;
    body.start_tick = 0;
    body.end_tick = 480;
    body.note_type = NoteType::ScratchHold;
    body.lane = 1;
    body.width = 2;
    body.gimmick_type = GimmickType::None;
    body.scratch_length = 2;  // same-track ±width → still JumpScratch on export
    export_chart.notes.push_back(body);
  }

  std::string out;
  OfficialChartSaveOptions save_opt;
  save_opt.use_gimmick_names = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::serialize_chart(export_chart, out, save_opt).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(out.find(",900,") != std::string::npos);
  CHECK(out.find("-1.0,900,") != std::string::npos || out.find("-1,900,") != std::string::npos);
  // HoldEighth line should use -1.0 endTime (writes_end false).
  {
    bool eighth_ok = false;
    for (const auto& line : [&] {
      std::vector<std::string> lines;
      std::string cur;
      for (char c : out) {
        if (c == '\n') {
          lines.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(c);
        }
      }
      if (!cur.empty()) lines.push_back(cur);
      return lines;
    }()) {
      if (line.find(",900,") == std::string::npos) continue;
      // start,end,type,...
      const auto c1 = line.find(',');
      const auto c2 = line.find(',', c1 + 1);
      CHECK(c1 != std::string::npos && c2 != std::string::npos);
      const std::string end_field = line.substr(c1 + 1, c2 - c1 - 1);
      CHECK(end_field.rfind("-1", 0) == 0);
      eighth_ok = true;
    }
    CHECK(eighth_ok);
  }
  CHECK(out.find("-1,0,12,") != std::string::npos || out.find("-1,0,Split2,") != std::string::npos);
  CHECK(out.find("JumpScratch,2") != std::string::npos);
}

void test_migrate_wdschart_scratch_to_flick_script() {
  const fs::path script = fs::path(__FILE__).parent_path().parent_path().parent_path() /
                          "tools" / "migrate_wdschart_scratch_to_flick.py";
  std::error_code ec;
  if (!fs::is_regular_file(script, ec) || ec) {
    return;  // optional if tree layout differs
  }

  const fs::path src = temp_chart_path("legacy_scratch.wdschart");
  const fs::path dst = temp_chart_path("legacy_scratch.migrated.wdschart");
  {
    std::ofstream out(src);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 120 4 4 3\nNOTES 2\n"
           "N 0 0 0 10 0 1 0 0\n"
           "N 1 480 480 40 2 1 0 0\n"
           "CONCURRENT 0\nEND\n";
  }

  const std::string cmd = "python3 \"" + script.string() + "\" \"" + src.string() + "\" -o \"" +
                          dst.string() + "\"";
  CHECK_EQ(std::system(cmd.c_str()), 0);

  NotationChart chart;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(dst.string(), chart).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(chart.notes.size()), 2);
  CHECK(chart.notes[1].note_type == NoteType::Flick);

  NotationChart rejected;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(src.string(), rejected).error),
           static_cast<int>(SerializeError::ParseError));
}

void test_save_failure_preserves_note_ids() {
  ChartEditorEngine engine;
  NotationNote late = make_tap(960, 2);
  late.id = 99;
  NotationNote early = make_tap(0, 0);
  early.id = 42;
  CHECK(engine.document().set_notes({late, early}));
  CHECK(engine.execute_command(
      std::make_unique<AddNotesCommand>(std::vector<NotationNote>{make_tap(480, 1)})));
  CHECK(engine.history().can_undo());
  CHECK(engine.document().find_note(99).has_value());
  CHECK(engine.document().find_note(42).has_value());

  // Force I/O failure: parent path is a regular file, so create_directories cannot succeed
  // (write_text_atomic creates missing dirs on both Win and Linux).
  const fs::path blocker = fs::temp_directory_path() / "wds_save_fail_blocker_file";
  {
    std::error_code ec;
    fs::remove_all(blocker, ec);
    std::ofstream out(blocker);
    out << "not-a-directory";
  }
  const auto result = engine.save_to_file((blocker / "fail.wdschart").string());
  CHECK_NE(static_cast<int>(result.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int32_t>(engine.document().notes().size()), 3);
  CHECK(engine.document().find_note(99).has_value());
  CHECK(engine.document().find_note(42).has_value());
  CHECK(engine.history().can_undo());
}

void test_sus_meter_and_mid_measure_bpm_roundtrip() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  // 3/4 measure (1440 ticks); BPM change halfway through first bar.
  chart.timing.points = {
      TimingPoint{0, 120.0, 3, 4, true, true},
      TimingPoint{720, 180.0, 3, 4, true, false},
  };
  NotationNote tap = make_tap(0, 1);
  tap.id = 0;
  chart.notes.push_back(tap);

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(text.find("#00002:") != std::string::npos);
  CHECK(text.find("#00008:") != std::string::npos);
  // Mid-bar BPM must not collapse to a single measure-head token pair.
  const auto pos08 = text.find("#00008:");
  CHECK(pos08 != std::string::npos);
  const auto line_end = text.find('\n', pos08);
  const std::string line08 = text.substr(pos08, line_end - pos08);
  CHECK(line08.size() > std::string("#00008: 01").size());

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  bool found_meter = false;
  bool found_mid_bpm = false;
  for (const auto& p : loaded.chart.timing.points) {
    if (p.tick == 0 && p.has_meter) {
      CHECK_EQ(p.numerator, 3);
      CHECK_EQ(p.denominator, 4);
      found_meter = true;
    }
    if (p.has_bpm && std::abs(p.bpm - 180.0) < 1e-6) {
      CHECK_EQ(p.tick, 720);
      found_mid_bpm = true;
    }
  }
  CHECK(found_meter);
  CHECK(found_mid_bpm);
}

void test_chart_session_preserves_per_chart_history() {
  ChartSession session;  // starts with one empty chart
  ChartDocument* doc_a = session.document();
  CHECK(doc_a != nullptr);
  NotationNote tap = make_tap(0, 0);
  tap.id = 7;
  CHECK(session.history()->execute(std::make_unique<AddNotesCommand>(std::vector<NotationNote>{tap}),
                                   *doc_a));
  CHECK(session.history()->can_undo());

  session.add_chart(ChartDocument{});
  CHECK(session.switch_chart(1));
  CHECK(!session.history()->can_undo());

  CHECK(session.switch_chart(0));
  CHECK(session.history()->can_undo());
  CHECK(session.document()->find_note(7).has_value());
  CHECK(session.history()->undo(*session.document()));
  CHECK(!session.document()->find_note(7).has_value());
}

void test_history_hold_eighths_and_project_v2() {
  ChartDocument doc;
  NotationNote note = make_tap(0, 0);
  note.id = 9;
  EditHistory history;
  CHECK(history.execute(std::make_unique<AddNotesCommand>(std::vector<NotationNote>{note}), doc));
  CHECK(doc.find_note(9).has_value());
  CHECK(history.undo(doc));
  CHECK(!doc.find_note(9).has_value());
  CHECK(history.redo(doc));
  CHECK(doc.find_note(9).has_value());

  {
    const MusicTiming before = doc.timing();
    MusicTiming after = before;
    after.points.push_back(TimingPoint{480, 180.0, 3, 4});
    CHECK(history.execute(std::make_unique<SetTimingCommand>(before, after, "Add timing"), doc));
    CHECK_EQ(static_cast<int>(doc.timing().points.size()), 2);
    CHECK(std::fabs(doc.timing().points[1].bpm - 180.0) < 0.01);
    CHECK_EQ(doc.timing().points[1].numerator, 3);
    CHECK(history.undo(doc));
    CHECK_EQ(static_cast<int>(doc.timing().points.size()), 1);
    CHECK(history.redo(doc));
    CHECK_EQ(static_cast<int>(doc.timing().points.size()), 2);
    CHECK(std::fabs(doc.timing().points[1].bpm - 180.0) < 0.01);
  }

  NotationNote hold = make_tap(0, 2);
  hold.id = 10;
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  CHECK(doc.add_note(hold) == 10);
  CHECK(recompute_hold_eighths(doc, hold));
  int eighths = 0;
  for (const auto& n : doc.notes()) if (n.note_type == NoteType::HoldEighth) ++eighths;
  CHECK_EQ(eighths, 3);

  const fs::path path = temp_chart_path("v2.wdsproject");
  WdsProject project;
  project.music_path = "music.ogg";
  project.chart_paths = {"a.wdschart", "b.wdschart"};
  project.active_chart_index = 1;
  CHECK_EQ(static_cast<int>(ProjectSerializer::save_to_file(project, path.string()).error),
           static_cast<int>(SerializeError::Ok));
  WdsProject loaded;
  CHECK_EQ(static_cast<int>(ProjectSerializer::load_from_file(path.string(), loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int32_t>(loaded.chart_paths.size()), 2);
  CHECK_EQ(loaded.chart_path(), std::string("b.wdschart"));
}

}  // namespace

void test_hold_head_pairs_but_attached_excludes_head() {
  ChartDocument doc;
  NotationNote hold = make_tap(0, 2);
  hold.id = 1;
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  CHECK(doc.add_note(hold) == 1);
  CHECK(ensure_hold_head_if_needed(doc, *doc.find_note(1)));
  auto head = paired_hold_head_for(doc, *doc.find_note(1));
  CHECK(head.has_value());
  CHECK(is_hold_head_note(*head));
  auto body = paired_hold_body_for(doc, *head);
  CHECK(body.has_value());
  CHECK_EQ(body->id, 1);
  // Delete dependents must not include the paired head.
  for (const auto& dep : hold_attached_notes_for(doc, *doc.find_note(1))) {
    CHECK(dep.id != head->id);
    CHECK(!is_hold_head_note(dep));
  }
}

void test_scratch_hold_end_lane_encoding() {
  NotationNote note = make_tap(0, 3);
  note.width = 2;  // body lanes 3-4
  note.end_tick = 960;
  note.note_type = NoteType::ScratchHold;
  note.scratch_length = 0;
  auto equal = get_scratch_end_lane_range(note);
  CHECK_EQ(equal.first, 3);
  CHECK_EQ(equal.second, 4);

  set_scratch_hold_end_lanes(note, 3, 7);  // extend right
  CHECK_EQ(note.scratch_length, 5);
  auto right = get_scratch_end_lane_range(note);
  CHECK_EQ(right.first, 3);
  CHECK_EQ(right.second, 7);

  set_scratch_hold_end_lanes(note, 1, 4);  // extend left
  CHECK(note.scratch_length < 0);
  auto left = get_scratch_end_lane_range(note);
  CHECK_EQ(left.first, 1);
  CHECK_EQ(left.second, 4);

  set_scratch_hold_end_lanes(note, 3, 4);  // back to equal → bidirectional
  CHECK_EQ(note.scratch_length, 0);

  CHECK(scratch_hold_end_cover_representable(note, 3, 4));
  CHECK(scratch_hold_end_cover_representable(note, 3, 7));
  CHECK(scratch_hold_end_cover_representable(note, 1, 4));
  CHECK(!scratch_hold_end_cover_representable(note, 1, 7));  // both sides
}

void test_scratch_chain_joint_direction() {
  NotationNote prev = make_tap(0, 3);
  prev.width = 2;  // lanes 3-4
  prev.end_tick = 480;
  prev.note_type = NoteType::ScratchHold;
  prev.scratch_length = 0;

  NotationNote next = make_tap(480, 3);
  next.width = 2;  // same lanes → bidirectional
  next.end_tick = 960;
  next.note_type = NoteType::ScratchHold;
  CHECK_EQ(scratch_chain_joint_direction_score(prev, next), 0);
  set_scratch_hold_end_lanes(prev, 3, 4);
  apply_scratch_chain_joint_direction(prev, next);
  CHECK_EQ(prev.scratch_length, 0);

  next.lane = 1;  // left shift both edges → left
  next.width = 2;  // lanes 1-2
  CHECK_EQ(scratch_chain_joint_direction_score(prev, next), -2);
  set_scratch_hold_end_lanes(prev, 1, 4);
  apply_scratch_chain_joint_direction(prev, next);
  CHECK(prev.scratch_length < 0);

  next.lane = 5;  // right shift
  next.width = 2;  // lanes 5-6
  CHECK_EQ(scratch_chain_joint_direction_score(prev, next), 2);
  set_scratch_hold_end_lanes(prev, 3, 6);
  apply_scratch_chain_joint_direction(prev, next);
  CHECK(prev.scratch_length > 0);

  // Next inset on the right only, cover equals prev body → score -1 → left via -width.
  next.lane = 3;
  next.width = 1;  // lanes 3-3
  CHECK_EQ(scratch_chain_joint_direction_score(prev, next), -1);
  set_scratch_hold_end_lanes(prev, 3, 4);
  apply_scratch_chain_joint_direction(prev, next);
  CHECK_EQ(prev.scratch_length, -prev.width);
}

void test_hold_head_suppressed_by_non_body_overlap_not_by_hold_body() {
  ChartDocument doc;
  NotationNote prior_body = make_tap(0, 2);
  prior_body.id = 1;
  prior_body.width = 2;
  prior_body.end_tick = 480;
  prior_body.note_type = NoteType::Hold;
  CHECK(doc.add_note(prior_body) == 1);

  NotationNote next = make_tap(480, 4);
  next.id = 2;
  next.width = 2;
  next.end_tick = 960;
  next.note_type = NoteType::Hold;
  CHECK(doc.add_note(next) == 2);
  // Prior hold ends at next.start but lanes do not overlap → head covers full body.
  CHECK(ensure_hold_head_if_needed(doc, *doc.find_note(2)));

  ChartDocument doc2;
  NotationNote tap = make_tap(0, 2);
  tap.id = 1;
  tap.width = 2;
  CHECK(doc2.add_note(tap) == 1);
  NotationNote hold = make_tap(0, 2);
  hold.id = 2;
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  CHECK(doc2.add_note(hold) == 2);
  // Fully covered by tap → no free lane → no head.
  CHECK(!ensure_hold_head_if_needed(doc2, *doc2.find_note(2)));
}

void test_hold_head_partial_overlap_single_free_run() {
  ChartDocument doc;
  NotationNote tap = make_tap(0, 4);
  tap.id = 1;
  tap.width = 2;  // lanes 4-5
  CHECK(doc.add_note(tap) == 1);

  NotationNote hold = make_tap(0, 2);
  hold.id = 2;
  hold.width = 4;  // lanes 2-5; free 2-3
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  CHECK(doc.add_note(hold) == 2);

  auto head = make_auto_hold_head(doc, *doc.find_note(2));
  CHECK(head.has_value());
  CHECK_EQ(head->lane, 2);
  CHECK_EQ(head->width, 2);
  CHECK(head->note_type == NoteType::HoldStart);
  CHECK(ensure_hold_head_if_needed(doc, *doc.find_note(2)));
  const NotationNote* added = nullptr;
  for (const auto& n : doc.notes()) {
    if (n.note_type == NoteType::HoldStart && n.start_tick == 0) {
      added = &n;
      break;
    }
  }
  CHECK(added != nullptr);
  CHECK_EQ(added->lane, 2);
  CHECK_EQ(added->width, 2);
}

void test_hold_head_partial_overlap_multiple_free_runs_skipped() {
  ChartDocument doc;
  NotationNote tap = make_tap(0, 3);
  tap.id = 1;
  tap.width = 1;  // lane 3 splits hold 2-5 into free 2 and 4-5
  CHECK(doc.add_note(tap) == 1);

  NotationNote hold = make_tap(0, 2);
  hold.id = 2;
  hold.width = 4;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  CHECK(doc.add_note(hold) == 2);

  CHECK(!make_auto_hold_head(doc, *doc.find_note(2)).has_value());
  CHECK(!ensure_hold_head_if_needed(doc, *doc.find_note(2)));
}

void test_hold_head_partial_overlap_with_prior_hold_tail() {
  ChartDocument doc;
  NotationNote prior = make_tap(0, 2);
  prior.id = 1;
  prior.width = 4;  // lanes 2-5; tail at 480
  prior.end_tick = 480;
  prior.note_type = NoteType::Hold;
  CHECK(doc.add_note(prior) == 1);

  NotationNote next = make_tap(480, 4);
  next.id = 2;
  next.width = 4;  // lanes 4-7; overlap 4-5 with prior tail → free 6-7
  next.end_tick = 960;
  next.note_type = NoteType::Hold;
  CHECK(doc.add_note(next) == 2);

  auto head = make_auto_hold_head(doc, *doc.find_note(2));
  CHECK(head.has_value());
  CHECK_EQ(head->lane, 6);
  CHECK_EQ(head->width, 2);

  // Fully covered by prior tail → no head.
  ChartDocument doc2;
  NotationNote prior2 = make_tap(0, 2);
  prior2.id = 1;
  prior2.width = 4;
  prior2.end_tick = 480;
  prior2.note_type = NoteType::Hold;
  CHECK(doc2.add_note(prior2) == 1);
  NotationNote next2 = make_tap(480, 2);
  next2.id = 2;
  next2.width = 4;
  next2.end_tick = 960;
  next2.note_type = NoteType::Hold;
  CHECK(doc2.add_note(next2) == 2);
  CHECK(!make_auto_hold_head(doc2, *doc2.find_note(2)).has_value());
}

void test_scratch_hold_auto_head_is_scratch_hold_start() {
  ChartDocument doc;
  NotationNote hold = make_tap(0, 2);
  hold.id = 1;
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::ScratchHold;
  CHECK(doc.add_note(hold) == 1);

  auto head = make_auto_hold_head(doc, *doc.find_note(1));
  CHECK(head.has_value());
  CHECK(head->note_type == NoteType::ScratchHoldStart);
  CHECK(is_hold_head_note(*head));
  CHECK(ensure_hold_head_if_needed(doc, *doc.find_note(1)));
  auto paired = paired_hold_head_for(doc, *doc.find_note(1));
  CHECK(paired.has_value());
  CHECK(paired->note_type == NoteType::ScratchHoldStart);
}

void test_repair_legacy_scratch_hold_heads() {
  ChartDocument doc;
  NotationNote body = make_tap(0, 3);
  body.id = 1;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::ScratchHold;
  CHECK(doc.add_note(body) == 1);

  NotationNote legacy = make_tap(0, 3);
  legacy.id = 2;
  legacy.width = 2;
  legacy.note_type = NoteType::Normal;  // pre-alignment auto-head
  CHECK(doc.add_note(legacy) == 2);

  CHECK(paired_hold_head_for(doc, *doc.find_note(1)).has_value());
  CHECK_EQ(repair_legacy_hold_heads(doc), 1);
  auto head = paired_hold_head_for(doc, *doc.find_note(1));
  CHECK(head.has_value());
  CHECK(head->note_type == NoteType::ScratchHoldStart);
  CHECK(is_hold_head_note(*head));
  CHECK_EQ(repair_legacy_hold_heads(doc), 0);
}

int count_note_type(const ChartDocument& doc, NoteType type) {
  int n = 0;
  for (const auto& note : doc.notes()) {
    if (note.note_type == type) ++n;
  }
  return n;
}

// Plain SUS hold (no covering tap) auto-generates a HoldStart via make_auto_hold_head.
void test_sus_hold_auto_generates_head_when_uncovered() {
  const char* sus =
      "This file was generated by WDS Editor\n"
      "#TITLE \"hold\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00020a: 1121\n";  // lane0 ch-a: start@0 width1, end@half-bar width1

  SusChartLoadResult loaded;
  const auto result = SusChartFormat::parse(sus, loaded);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(SerializeError::Ok));

  const NotationNote* head = nullptr;
  const NotationNote* body = nullptr;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::HoldStart || n.note_type == NoteType::ScratchHoldStart) {
      head = &n;
    }
    if (n.note_type == NoteType::Hold || n.note_type == NoteType::ScratchHold) {
      body = &n;
    }
  }
  CHECK(head != nullptr);
  CHECK(body != nullptr);
  CHECK(head->end_tick == head->start_tick);
  CHECK(is_hold_head_note(*head));
  if (head != nullptr && body != nullptr) {
    ChartDocument doc;
    doc.set_notes(loaded.chart.notes);
    doc.set_timing(loaded.chart.timing);
    CHECK(paired_hold_head_for(doc, *body).has_value());
  }
}

// recompute_hold_eighths outside EditHistory leaves orphans on undo.
// Correct pattern: fold eighths into SetNotesCommand before/after.
void test_hold_eighths_outside_history_undo_redo() {
  ChartDocument doc;
  EditHistory history;
  NotationNote hold = make_tap(0, 2);
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;

  // Anti-pattern: AddNotes then recompute outside history leaves orphans.
  {
    ChartDocument orphan_doc;
    EditHistory orphan_history;
    CHECK(orphan_history.execute(
        std::make_unique<AddNotesCommand>(std::vector<NotationNote>{hold}), orphan_doc));
    CHECK(recompute_hold_eighths(orphan_doc, *orphan_doc.find_note(orphan_doc.notes()[0].id)));
    CHECK(orphan_history.undo(orphan_doc));
    const int eighths_after_undo = count_note_type(orphan_doc, NoteType::HoldEighth);
    CHECK_EQ(eighths_after_undo, 3);  // documents the orphan bug of the anti-pattern
  }

  // Correct transactional place: eighths live inside the same SetNotesCommand.
  auto before = doc.notes();
  auto after = before;
  hold.id = kAutoNoteId;
  after.push_back(hold);
  const NotationNote hold_for_eighths = after.back();
  after = with_recomputed_hold_eighths(std::move(after), hold_for_eighths,
                                       doc.timing().ticks_per_quarter);
  CHECK(history.execute(std::make_unique<SetNotesCommand>(before, after, "Place hold"), doc));
  CHECK_EQ(count_note_type(doc, NoteType::HoldEighth), 3);
  CHECK(history.undo(doc));
  CHECK_EQ(count_note_type(doc, NoteType::Hold), 0);
  CHECK_EQ(count_note_type(doc, NoteType::HoldEighth), 0);
  CHECK(history.redo(doc));
  CHECK_EQ(count_note_type(doc, NoteType::Hold), 1);
  CHECK_EQ(count_note_type(doc, NoteType::HoldEighth), 3);
}

// CompositeCommand::undo must roll back already-undone children on partial failure.
class CountingCommand final : public IEditCommand {
 public:
  CountingCommand(int* counter, bool fail_undo, std::string label)
      : counter_(counter), fail_undo_(fail_undo), label_(std::move(label)) {}
  bool execute(ChartDocument&) override {
    ++(*counter_);
    return true;
  }
  bool undo(ChartDocument&) override {
    if (fail_undo_) return false;
    --(*counter_);
    return true;
  }
  std::string label() const override { return label_; }

 private:
  int* counter_;
  bool fail_undo_;
  std::string label_;
};

void test_composite_undo_rolls_back_on_partial_failure() {
  ChartDocument doc;
  int counter = 0;
  auto composite = std::make_unique<CompositeCommand>("partial undo");
  // Undo order is reverse: c, b, then a. Make `a` fail so b+c are already undone.
  composite->add(std::make_unique<CountingCommand>(&counter, true, "a-fail"));
  composite->add(std::make_unique<CountingCommand>(&counter, false, "b"));
  composite->add(std::make_unique<CountingCommand>(&counter, false, "c"));

  CHECK(composite->execute(doc));
  CHECK_EQ(counter, 3);
  const bool undone = composite->undo(doc);

  CHECK(!undone);
  // Already-undone children are re-executed → counter restored to 3.
  CHECK_EQ(counter, 3);
}

// paired_hold_head_for matches exact integer ticks (notes are grid-snapped).
void test_paired_hold_head_tolerates_subtick_drift() {
  ChartDocument doc;
  NotationNote body = make_tap(480, 2);
  body.id = 1;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::Hold;
  CHECK(doc.add_note(body) == 1);

  NotationNote head = make_tap(480, 2);
  head.id = 2;
  head.width = 2;
  head.end_tick = 480;
  head.note_type = NoteType::HoldStart;
  CHECK(doc.add_note(head) == 2);

  auto paired = paired_hold_head_for(doc, *doc.find_note(1));
  CHECK(paired.has_value());
  CHECK_EQ(paired->id, 2);
}

// Mid-star on an exact subdivision tick suppresses the eighth at that tick.
void test_recompute_hold_eighths_respects_fractional_star() {
  ChartDocument doc;
  NotationNote hold = make_tap(0, 2);
  hold.id = 1;
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  CHECK(doc.add_note(hold) == 1);

  NotationNote star = make_tap(240, 2);
  star.id = 2;
  star.width = 2;
  star.end_tick = star.start_tick;
  star.note_type = NoteType::Sound;
  CHECK(doc.add_note(star) == 2);

  CHECK(recompute_hold_eighths(doc, *doc.find_note(1)));
  int eighth_on_star = 0;
  int sound_count = 0;
  for (const auto& n : doc.notes()) {
    if (n.note_type == NoteType::Sound) ++sound_count;
    if (n.note_type == NoteType::HoldEighth && n.start_tick == star.start_tick) {
      ++eighth_on_star;
    }
  }

  CHECK_EQ(sound_count, 1);
  CHECK_EQ(eighth_on_star, 0);
}

int count_holds_with_tail(const NotationChart& chart) {
  int n = 0;
  for (const auto& note : chart.notes) {
    if (is_hold_with_tail(note.note_type)) ++n;
  }
  return n;
}

int count_hold_heads(const NotationChart& chart) {
  int n = 0;
  for (const auto& note : chart.notes) {
    if (is_hold_head_note(note)) ++n;
  }
  return n;
}

// Same channel reused after end must yield two holds (SUS 2.7 channel linking).
void test_sus_channel_reuse_two_holds() {
  // 8 slots/bar @ 1920 ticks → slot step 240. start@0, end@960, start@1200, end@1680.
  const char* sus =
      "#TITLE \"reuse\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00020a: 110000002100001100000021\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_holds_with_tail(loaded.chart), 2);
  CHECK_EQ(count_hold_heads(loaded.chart), 2);
}

// Hold start at bar head (tick 0) and end in next measure.
void test_sus_cross_measure_hold_at_bar_head() {
  const char* sus =
      "#TITLE \"cross\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00020a: 11\n"
      "#00120a: 21\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  const NotationNote* body = nullptr;
  int hold_starts = 0;
  for (const auto& n : loaded.chart.notes) {
    if (is_hold_head_note(n)) ++hold_starts;
    if (is_hold_with_tail(n.note_type)) body = &n;
  }
  CHECK(body != nullptr);
  CHECK_EQ(hold_starts, 1);
  CHECK_EQ(body->start_tick, 0);
  CHECK_EQ(body->end_tick, 1920);
}

// Spec example hold `#00020a: 14002400`.
void test_sus_spec_example_hold_14002400() {
  const char* sus =
      "#TITLE \"spec\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00020a: 14002400\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  const NotationNote* body = nullptr;
  for (const auto& n : loaded.chart.notes) {
    if (is_hold_with_tail(n.note_type)) body = &n;
  }
  CHECK(body != nullptr);
  CHECK_EQ(body->width, 4);
  CHECK_EQ(body->start_tick, 0);
  CHECK_EQ(body->end_tick, 960);
}

// Export must emit headless hold bodies with a Damage marker (no authored HoldStart).
void test_sus_export_headless_hold_body() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote body = make_tap(0, 2);
  body.id = 0;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::Hold;
  chart.notes.push_back(body);

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  // Truly headless → Damage tap (#1 type 4, width 2) at lane 2.
  CHECK(text.find("#00012:") != std::string::npos);
  {
    const auto pos = text.find("#00012:");
    CHECK(pos != std::string::npos);
    const auto colon = text.find(':', pos);
    const auto end = text.find('\n', pos);
    CHECK(colon != std::string::npos);
    const std::string data =
        text.substr(colon + 1, end == std::string::npos ? std::string::npos : end - (colon + 1));
    CHECK(data.find("42") != std::string::npos);
  }

  SusChartLoadResult roundtrip;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, roundtrip).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_holds_with_tail(roundtrip.chart), 1);
  CHECK_EQ(count_hold_heads(roundtrip.chart), 0);
  // Damage marker must not become a Normal tap.
  int taps = 0;
  bool found = false;
  for (const auto& n : roundtrip.chart.notes) {
    if (is_hold_with_tail(n.note_type) && n.end_tick == 960) found = true;
    if (n.note_type == NoteType::Normal || n.note_type == NoteType::Critical) ++taps;
  }
  CHECK(found);
  CHECK_EQ(taps, 0);
}

// Hold start already fully covered by a tap → no Damage marker on export.
void test_sus_export_covered_hold_skips_damage() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote tap = make_tap(0, 2);
  tap.id = 0;
  tap.width = 2;
  tap.note_type = NoteType::Normal;
  NotationNote body = make_tap(0, 2);
  body.id = 1;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::Hold;
  chart.notes = {tap, body};

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  const auto pos = text.find("#00012:");
  CHECK(pos != std::string::npos);
  const auto colon = text.find(':', pos);
  const auto end = text.find('\n', pos);
  CHECK(colon != std::string::npos);
  const std::string data =
      text.substr(colon + 1, end == std::string::npos ? std::string::npos : end - (colon + 1));
  // Normal tap type 1, not Damage type 4.
  CHECK(data.find("12") != std::string::npos);
  CHECK(data.find("42") == std::string::npos);

  SusChartLoadResult roundtrip;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, roundtrip).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_holds_with_tail(roundtrip.chart), 1);
  CHECK_EQ(count_hold_heads(roundtrip.chart), 0);
  int normals = 0;
  for (const auto& n : roundtrip.chart.notes) {
    if (n.note_type == NoteType::Normal) ++normals;
  }
  CHECK_EQ(normals, 1);
}

// Partial-width hold head must still pair to body on export.
void test_sus_export_partial_hold_head_pairs_body() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};

  NotationNote body = make_tap(0, 2);
  body.id = 0;
  body.width = 4;
  body.end_tick = 960;
  body.note_type = NoteType::Hold;
  NotationNote head = make_tap(0, 4);
  head.id = 1;
  head.width = 2;
  head.end_tick = 0;
  head.note_type = NoteType::HoldStart;
  chart.notes = {body, head};

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));

  SusChartLoadResult roundtrip;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, roundtrip).error),
           static_cast<int>(SerializeError::Ok));
  int32_t body_end = -1;
  for (const auto& n : roundtrip.chart.notes) {
    if (is_hold_with_tail(n.note_type)) body_end = n.end_tick;
  }
  CHECK_EQ(body_end, 960);
}

// Mid-star (type 3) inside hold.
void test_sus_hold_mid_star_roundtrip() {
  // 8 slots × 240 ticks: start@0, mid@960 (slot4), end@1680 (slot7)
  const char* sus =
      "#TITLE \"mid\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00020a: 1100000031000021\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int sounds = 0;
  int32_t star_tick = -1;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Sound) {
      ++sounds;
      star_tick = n.start_tick;
    }
  }
  CHECK_EQ(sounds, 1);
  CHECK_EQ(star_tick, 960);
}

// Slide with different end lane → ScratchHold; no extra plain Hold.
void test_sus_slide_export_not_orphan_hold_start() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote body = make_tap(0, 2);
  body.id = 0;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::ScratchHold;
  set_scratch_hold_end_lanes(body, 4, 5);
  NotationNote head = make_tap(0, 2);
  head.id = 1;
  head.width = 2;
  head.end_tick = 0;
  head.note_type = NoteType::ScratchHoldStart;
  chart.notes = {body, head};

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));

  SusChartLoadResult roundtrip;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, roundtrip).error),
           static_cast<int>(SerializeError::Ok));
  int scratch = 0;
  int plain_hold = 0;
  for (const auto& n : roundtrip.chart.notes) {
    if (n.note_type == NoteType::ScratchHold) ++scratch;
    if (n.note_type == NoteType::Hold) ++plain_hold;
  }
  CHECK_EQ(scratch, 1);
  CHECK_EQ(plain_hold, 0);
}

// Standalone Flick must be paired Flick (#1 type3) + Air (#5); orphan Air is ignored.
void test_sus_standalone_directional_keeps_width() {
  const char* sus =
      "#TITLE \"dir\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00012: 34\n"   // Flick type 3, width 4 at lane 2
      "#00052: 14\n";  // Air up, width 4

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  const NotationNote* flick = nullptr;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Flick) flick = &n;
  }
  CHECK(flick != nullptr);
  if (flick == nullptr) return;
  CHECK_EQ(flick->width, 4);
  CHECK_EQ(flick->scratch_length, 0);  // up
}

// Orphan Air alone must not become a Flick.
void test_sus_orphan_air_ignored() {
  const char* sus =
      "#TITLE \"orphan\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00052: 14\n";
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(loaded.chart.notes.size()), 0);
  CHECK(!loaded.warnings.empty());
}

// Flick (#1 type3) + Air at same tick/lane → flick with Air direction.
void test_sus_tap_plus_directional_becomes_flick() {
  const char* sus =
      "#TITLE \"tapdir\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00012: 33\n"   // Flick type 3, width 3
      "#00052: 43\n";  // right-up Air width 3

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(loaded.chart.notes.size()), 1);
  CHECK_EQ(static_cast<int>(loaded.chart.notes[0].note_type), static_cast<int>(NoteType::Flick));
  CHECK_EQ(loaded.chart.notes[0].width, 3);
  CHECK_EQ(loaded.chart.notes[0].scratch_length, 1);
}

// Ched: Slide #3 without end Flick+Air → blue Hold; with pair → ScratchHold.
void test_sus_ched_slide_air_distinguishes_hold_family() {
  const char* blue =
      "#TITLE \"blue\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00032a: 1222\n";  // Slide start@0 end@half
  SusChartLoadResult blue_loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(blue, blue_loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_holds_with_tail(blue_loaded.chart), 1);
  CHECK_EQ(static_cast<int>(blue_loaded.chart.notes[0].note_type),
           static_cast<int>(NoteType::Hold));

  const char* purple2 =
      "#TITLE \"purple\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00032a: 12002200\n"
      "#00012: 00003200\n"
      "#00052: 00001200\n";
  SusChartLoadResult purple_loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(purple2, purple_loaded).error),
           static_cast<int>(SerializeError::Ok));
  int scratch = 0;
  for (const auto& n : purple_loaded.chart.notes) {
    if (n.note_type == NoteType::ScratchHold) ++scratch;
  }
  CHECK_EQ(scratch, 1);
}

// Export: blue Hold is #3 without end Flick/Air; purple writes paired Flick+Air.
void test_sus_ched_export_slide_and_end_pair() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote blue = make_tap(0, 0);
  blue.id = 0;
  blue.width = 1;
  blue.end_tick = 960;
  blue.note_type = NoteType::Hold;
  NotationNote head = make_tap(0, 0);
  head.id = 1;
  head.width = 1;
  head.end_tick = 0;
  head.note_type = NoteType::HoldStart;
  NotationNote purple = make_tap(0, 2);
  purple.id = 2;
  purple.width = 1;
  purple.end_tick = 960;
  purple.note_type = NoteType::ScratchHold;
  purple.scratch_length = 0;
  NotationNote phead = make_tap(0, 2);
  phead.id = 3;
  phead.width = 1;
  phead.end_tick = 0;
  phead.note_type = NoteType::ScratchHoldStart;
  chart.notes = {blue, head, purple, phead};

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(text.find("#00020") == std::string::npos);  // no Hold #2 channel
  CHECK(text.find("#00030") != std::string::npos);  // Slide #3
  // Purple end at tick 960 → measure 0 half → Flick+Air present
  CHECK(text.find("#00012:") != std::string::npos);
  CHECK(text.find("#00052:") != std::string::npos);

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int holds = 0, scratches = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Hold) ++holds;
    if (n.note_type == NoteType::ScratchHold) ++scratches;
  }
  CHECK_EQ(holds, 1);
  CHECK_EQ(scratches, 1);
}

// Mid Flick+Air on a purple slide splits into JumpScratch ScratchHold segments.
void test_sus_ched_mid_flick_splits_jump_scratch() {
  const char* sus =
      "#TITLE \"jump\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00032a: 120032002200\n"  // start@0, mid@2, end@4 of 6 slots
      "#00012: 000032000032\n"  // Flick at mid and end
      "#00052: 000012000012\n"; // Air up at mid and end
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int scratch_bodies = 0;
  int jump = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::ScratchHold) {
      ++scratch_bodies;
      if (n.gimmick_type == GimmickType::JumpScratch) ++jump;
    }
  }
  CHECK_EQ(scratch_bodies, 2);
  CHECK(jump >= 1);
}

// Split gimmick ↔ #TIL01.
void test_sus_til01_split_roundtrip() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote split;
  split.id = 0;
  split.start_tick = 0;
  split.end_tick = 1920;
  split.lane = 0;
  split.width = 12;
  split.note_type = NoteType::None;
  split.gimmick_type = GimmickType::Split3;
  split.scratch_length = 2;
  chart.notes.push_back(split);

  SusChartSaveOptions options;
  options.ched_lane_padding = true;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(text.find("#TIL01") != std::string::npos);

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int splits = 0;
  for (const auto& n : loaded.chart.notes) {
    if (is_split_lane_gimmick(n.gimmick_type)) {
      ++splits;
      CHECK_EQ(get_split_count(n.gimmick_type), 3);
      CHECK_EQ(n.scratch_length, 2);
    }
  }
  CHECK_EQ(splits, 1);
}

// .wdschart export → import must preserve note/timing semantics.
void test_wdschart_export_import_preserves_chart() {
  NotationChart chart;
  chart.timing.bpm = 150.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.offset_ms = 0;
  chart.timing.points = {TimingPoint{0, 150.0, 4, 4, true, true},
                         TimingPoint{1920, 180.0, 3, 4, true, true}};

  auto add = [&](NoteType type, int32_t start, int32_t end, int32_t lane, int32_t width,
                 GimmickType g = GimmickType::None, int32_t scratch = 0) {
    NotationNote n;
    n.id = static_cast<int32_t>(chart.notes.size());
    n.start_tick = start;
    n.end_tick = end;
    n.lane = lane;
    n.width = width;
    n.note_type = type;
    n.gimmick_type = g;
    n.scratch_length = scratch;
    chart.notes.push_back(n);
  };
  add(NoteType::Normal, 0, 0, 0, 1);
  add(NoteType::Critical, 240, 240, 1, 2);
  add(NoteType::Flick, 480, 480, 3, 1, GimmickType::None, -1);
  add(NoteType::HoldStart, 960, 960, 2, 2);
  add(NoteType::Hold, 960, 1920, 2, 2);
  add(NoteType::Sound, 1440, 1440, 2, 1);
  add(NoteType::ScratchHoldStart, 1920, 1920, 5, 1);
  add(NoteType::ScratchHold, 1920, 2880, 5, 1, GimmickType::JumpScratch, 3);
  add(NoteType::ScratchSound, 2400, 2400, 5, 1);
  add(NoteType::None, 0, 3840, 0, 12, GimmickType::Split2, 1);

  chart.concurrent_lines = build_concurrent_lines(chart.notes, chart.timing);

  const fs::path path = temp_chart_path("preserve_roundtrip.wdschart");
  CHECK_EQ(static_cast<int>(ChartSerializer::save_to_file(chart, path.string()).error),
           static_cast<int>(SerializeError::Ok));
  NotationChart loaded;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(path.string(), loaded).error),
           static_cast<int>(SerializeError::Ok));

  CHECK_EQ(static_cast<int>(loaded.notes.size()), static_cast<int>(chart.notes.size()));
  CHECK(std::abs(loaded.timing.bpm - chart.timing.bpm) < 1e-6);
  CHECK_EQ(loaded.timing.ticks_per_quarter, chart.timing.ticks_per_quarter);

  auto key = [](const NotationNote& n) {
    return std::tuple{n.start_tick, n.end_tick, n.lane, n.width,
                      static_cast<int32_t>(n.note_type), static_cast<int32_t>(n.gimmick_type),
                      n.scratch_length};
  };
  std::vector<NotationNote> a = chart.notes;
  std::vector<NotationNote> b = loaded.notes;
  std::sort(a.begin(), a.end(), [&](const NotationNote& x, const NotationNote& y) {
    return key(x) < key(y);
  });
  std::sort(b.begin(), b.end(), [&](const NotationNote& x, const NotationNote& y) {
    return key(x) < key(y);
  });
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK_EQ(a[i].start_tick, b[i].start_tick);
    CHECK_EQ(a[i].end_tick, b[i].end_tick);
    CHECK_EQ(a[i].lane, b[i].lane);
    CHECK_EQ(a[i].width, b[i].width);
    CHECK_EQ(static_cast<int>(a[i].note_type), static_cast<int>(b[i].note_type));
    CHECK_EQ(static_cast<int>(a[i].gimmick_type), static_cast<int>(b[i].gimmick_type));
    CHECK_EQ(a[i].scratch_length, b[i].scratch_length);
  }
}

// Slide invisible mid (type 5) must not become a visible Sound star.
void test_sus_slide_invisible_mid_not_sound() {
  // category 3 slide: start, invisible mid (5), end — 4 slots
  const char* sus =
      "#TITLE \"inv\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00030a: 14005424\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int sounds = 0;
  int bodies = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Sound) ++sounds;
    if (is_hold_with_tail(n.note_type)) ++bodies;
  }
  CHECK_EQ(bodies, 1);
  CHECK_EQ(sounds, 0);
}

// Fractional #mmm02 measure length must preserve tick span (e.g. 3.5 beats).
void test_sus_fractional_measure_length() {
  const char* sus =
      "#TITLE \"frac\"\n"
      "#BPM01: 120.0\n"
      "#00002: 3.5\n"
      "#00008: 01\n"
      "#00010: 11\n"
      "#00110: 11\n";  // second bar starts after 3.5 beats

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int32_t second = -1;
  for (const auto& n : loaded.chart.notes) {
    if (n.start_tick > 0) second = n.start_tick;
  }
  // 3.5 beats * 480 tpq = 1680 ticks
  CHECK_EQ(second, 1680);
  // Timing meter should preserve the half-beat (7/8), not round to 4/4.
  CHECK_EQ(loaded.chart.timing.points[0].numerator, 7);
  CHECK_EQ(loaded.chart.timing.points[0].denominator, 8);
}

// #MEASUREBS shifts subsequent measure numbers.
void test_sus_measurebs_offset() {
  const char* sus =
      "#TITLE \"bs\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#MEASUREBS 1000\n"
      "#00010: 11\n";  // absolute measure 1000

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(loaded.chart.notes.size()), 1);
  // 1000 bars of 4 beats * 480 = 1_920_000
  CHECK(loaded.chart.notes[0].start_tick == 1920000);
}

// WAVEOFFSET seconds → chart offset_ms; export restores seconds.
void test_sus_waveoffset_roundtrip() {
  const char* sus =
      "#TITLE \"off\"\n"
      "#WAVEOFFSET -0.5\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00010: 11\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(loaded.meta.wave_offset_sec, -0.5);
  CHECK_EQ(loaded.chart.timing.offset_ms, -500);

  SusChartSaveOptions options;
  options.meta = loaded.meta;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(loaded.chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(text.find("#WAVEOFFSET -0.500") != std::string::npos ||
        text.find("#WAVEOFFSET -0.5") != std::string::npos);
}

// m7: dense same-lane taps must survive remapping without silent overwrite drops.
void test_sus_export_slot_remap_preserves_note_count() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  // Offsets that force coarse→fine remaps within one measure (lane 0 taps).
  const int32_t ticks[] = {0, 160, 240, 320, 480, 640, 720, 800, 960, 1120, 1200, 1440};
  for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i) {
    NotationNote tap = make_tap(ticks[i], 0);
    tap.id = static_cast<int32_t>(i);
    tap.width = 1;
    chart.notes.push_back(tap);
  }
  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(loaded.chart.notes.size()), static_cast<int>(chart.notes.size()));
}

// Critical tap roundtrip (SUS type 2).
void test_sus_critical_tap_roundtrip() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote tap = make_tap(0, 1);
  tap.id = 0;
  tap.width = 2;
  tap.note_type = NoteType::Critical;
  chart.notes.push_back(tap);

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(loaded.chart.notes.size()), 1);
  CHECK_EQ(static_cast<int>(loaded.chart.notes[0].note_type), static_cast<int>(NoteType::Critical));
  CHECK_EQ(loaded.chart.notes[0].width, 2);
}

// Defined #BPMzz without #mmm08 should pick the lowest id (not unordered begin()).
void test_sus_bpm_defs_without_change_pick_lowest_id() {
  const char* sus =
      "#TITLE \"bpm\"\n"
      "#BPM02: 180.0\n"
      "#BPM01: 100.0\n"
      "#00010: 11\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(std::abs(loaded.chart.timing.bpm - 100.0) < 1e-6);
}

// Export of notes past measure 1000 must emit #MEASUREBS (not collide at %1000).
void test_sus_measurebs_export_roundtrip() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote tap = make_tap(1920000, 0);  // measure 1000 @ 4/4
  tap.id = 0;
  tap.width = 1;
  chart.notes.push_back(tap);

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(text.find("#MEASUREBS 1000") != std::string::npos);

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(loaded.chart.notes.size()), 1);
  CHECK(loaded.chart.notes[0].start_tick == 1920000);
}


// Roundtrip: ScratchHold stays purple family; CriticalHold stays gold family;
// headless stays headless; HoldEighth not imported as Sound; ched pad lanes stable.
void test_sus_roundtrip_hold_families_and_lanes() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};

  // Purple ScratchHold lanes 0-1 (same end span — must still roundtrip as scratch).
  NotationNote scratch_head = make_tap(0, 0);
  scratch_head.id = 0;
  scratch_head.width = 2;
  scratch_head.end_tick = 0;
  scratch_head.note_type = NoteType::ScratchHoldStart;
  NotationNote scratch_body = make_tap(0, 0);
  scratch_body.id = 1;
  scratch_body.width = 2;
  scratch_body.end_tick = 960;
  scratch_body.note_type = NoteType::ScratchHold;
  scratch_body.scratch_length = 0;

  // Headless Hold lanes 3-4.
  NotationNote headless = make_tap(0, 3);
  headless.id = 2;
  headless.width = 2;
  headless.end_tick = 960;
  headless.note_type = NoteType::Hold;

  // CriticalHold (gold) lanes 7-8 — reproduces [7,9)-style ched pad shift.
  NotationNote gold_head = make_tap(0, 7);
  gold_head.id = 3;
  gold_head.width = 2;
  gold_head.end_tick = 0;
  gold_head.note_type = NoteType::CriticalHoldStart;
  NotationNote gold_body = make_tap(0, 7);
  gold_body.id = 4;
  gold_body.width = 2;
  gold_body.end_tick = 960;
  gold_body.note_type = NoteType::CriticalHold;

  // HoldEighth inside gold body — must not become Sound after roundtrip.
  NotationNote eighth = make_tap(480, 7);
  eighth.id = 5;
  eighth.width = 2;
  eighth.end_tick = 480;
  eighth.note_type = NoteType::HoldEighth;

  chart.notes = {scratch_head, scratch_body, headless, gold_head, gold_body, eighth};

  SusChartSaveOptions options;
  options.ched_lane_padding = true;  // UI default
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));

  int scratch_bodies = 0, hold_bodies = 0, crit_bodies = 0;
  int heads_at_960 = 0, sounds = 0, headless_holds = 0;
  int gold_lane = -1;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::ScratchHold || n.note_type == NoteType::ScratchCriticalHold)
      ++scratch_bodies;
    if (n.note_type == NoteType::Hold) ++hold_bodies;
    if (n.note_type == NoteType::CriticalHold || n.note_type == NoteType::ScratchCriticalHold)
      ++crit_bodies;
    if (is_hold_head_note(n) && n.start_tick == 960) ++heads_at_960;
    if (n.note_type == NoteType::Sound || n.note_type == NoteType::ScratchSound) ++sounds;
    if (is_hold_with_tail(n.note_type) && n.lane == 7) gold_lane = n.lane;
  }
  // Headless: Hold body at lane 3 with no paired head.
  {
    ChartDocument doc;
    doc.set_notes(loaded.chart.notes);
    doc.set_timing(loaded.chart.timing);
    for (const auto& n : loaded.chart.notes) {
      if (n.note_type == NoteType::Hold && n.lane == 3) {
        if (!paired_hold_head_for(doc, n).has_value()) ++headless_holds;
      }
    }
  }

  CHECK_EQ(scratch_bodies, 1);
  CHECK_EQ(hold_bodies, 1);          // the headless plain hold
  CHECK_EQ(crit_bodies, 1);          // gold family preserved
  CHECK_EQ(heads_at_960, 0);         // no extra head at tail
  CHECK_EQ(sounds, 0);               // eighth must not become Sound
  CHECK_EQ(headless_holds, 1);
  CHECK_EQ(gold_lane, 7);            // ched +2 roundtrip
}


// Critical Hold exports as Critical + hold channel; import stays headless (no HoldStart).
void test_sus_critical_hold_exports_as_critical_plus_hold() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};

  NotationNote head = make_tap(0, 2);
  head.id = 0;
  head.width = 2;
  head.end_tick = 0;
  head.note_type = NoteType::CriticalHoldStart;
  NotationNote body = make_tap(0, 2);
  body.id = 1;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::CriticalHold;
  chart.notes = {head, body};

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  // Critical tap (#mmm1x type 2) must appear alongside the hold channel.
  CHECK(text.find("#00012:") != std::string::npos);

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int criticals = 0;
  int hold_starts = 0;
  int hold_bodies = 0;
  int critical_hold_family = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Critical) ++criticals;
    if (is_hold_head_note(n)) ++hold_starts;
    if (n.note_type == NoteType::Hold) ++hold_bodies;
    if (n.note_type == NoteType::CriticalHoldStart || n.note_type == NoteType::CriticalHold ||
        n.note_type == NoteType::ScratchCriticalHoldStart ||
        n.note_type == NoteType::ScratchCriticalHold) {
      ++critical_hold_family;
    }
  }
  CHECK_EQ(criticals, 1);
  CHECK_EQ(hold_starts, 0);  // headless: Critical covers the start judgment
  CHECK_EQ(hold_bodies, 0);
  CHECK_EQ(critical_hold_family, 1);  // body restored as CriticalHold
}

// Critical tap overlapping SUS hold start → CriticalHold body, no HoldStart 二判.
void test_sus_critical_plus_hold_imports_headless() {
  const char* sus =
      "#TITLE \"crithold\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00012: 22\n"      // Critical width 2 at lane 2
      "#00022a: 1222\n";  // hold start@0 end@half, same lane/width

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int criticals = 0;
  int hold_starts = 0;
  int crit_hold_bodies = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Critical) ++criticals;
    if (is_hold_head_note(n)) ++hold_starts;
    if (n.note_type == NoteType::CriticalHold) ++crit_hold_bodies;
  }
  CHECK_EQ(criticals, 1);
  CHECK_EQ(hold_starts, 0);
  CHECK_EQ(crit_hold_bodies, 1);
}

// Helpers for comprehensive SUS roundtrip coverage.
namespace {
NotationNote make_body(NoteType type, int32_t id, int32_t start, int32_t end, int32_t lane,
                       int32_t width) {
  NotationNote n = make_tap(start, lane);
  n.id = id;
  n.width = width;
  n.end_tick = end;
  n.note_type = type;
  return n;
}

NotationNote make_head(NoteType type, int32_t id, int32_t tick, int32_t lane, int32_t width) {
  NotationNote n = make_tap(tick, lane);
  n.id = id;
  n.width = width;
  n.end_tick = 0;
  n.note_type = type;
  return n;
}
}  // namespace

// Comprehensive SUS export→import covering known edge cases and SUS limitations
// (legacy Scratch no longer exists; Nontail→tailed Hold, partial head→full auto head).
void test_sus_comprehensive_roundtrip_all_cases() {
  NotationChart chart;
  chart.timing.bpm = 185.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {
      TimingPoint{0, 185.0, 4, 4, true, true},
      TimingPoint{1920, 200.0, 4, 4, true, false},
      TimingPoint{2880, 200.0, 3, 4, false, true},
      TimingPoint{3840, 160.0, 4, 4, true, true},
  };

  int32_t next_id = 0;
  auto nid = [&]() { return next_id++; };

  // --- taps ---
  chart.notes.push_back(make_body(NoteType::Normal, nid(), 0, 0, 0, 1));
  chart.notes.push_back(make_body(NoteType::Critical, nid(), 240, 0, 2, 2));
  {
    NotationNote f = make_body(NoteType::Flick, nid(), 480, 0, 4, 1);
    f.scratch_length = 0;  // up
    chart.notes.push_back(f);
  }
  {
    NotationNote f = make_body(NoteType::Flick, nid(), 720, 0, 5, 2);
    f.scratch_length = -1;  // left
    chart.notes.push_back(f);
  }
  {
    NotationNote f = make_body(NoteType::Flick, nid(), 960, 0, 7, 1);
    f.scratch_length = 1;  // right
    chart.notes.push_back(f);
  }
  // Extra Normal where legacy Scratch(40) used to sit (lane 9 @ 1200).
  chart.notes.push_back(make_body(NoteType::Normal, nid(), 1200, 0, 9, 1));

  // Hold + authored head
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 1440, 0, 2));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 1440, 2400, 0, 2));

  // Headless Hold (no cover → Damage)
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 1440, 2400, 3, 2));

  // Covered Hold by Normal
  chart.notes.push_back(make_body(NoteType::Normal, nid(), 1440, 0, 6, 2));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 1440, 2400, 6, 2));

  // Covered Hold by Flick
  {
    NotationNote f = make_body(NoteType::Flick, nid(), 1680, 0, 8, 1);
    f.scratch_length = 0;
    chart.notes.push_back(f);
  }
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 1680, 2640, 8, 1));

  // CriticalHold authored (exports Critical + hold; import CriticalHold headless)
  chart.notes.push_back(make_head(NoteType::CriticalHoldStart, nid(), 2880, 0, 2));
  chart.notes.push_back(make_body(NoteType::CriticalHold, nid(), 2880, 3840, 0, 2));

  // Critical fully covering CriticalHold → headless CriticalHold; Critical tap kept
  chart.notes.push_back(make_body(NoteType::Critical, nid(), 2880, 0, 3, 2));
  chart.notes.push_back(make_body(NoteType::CriticalHold, nid(), 2880, 3840, 3, 2));

  // ScratchHold stationary + authored head
  chart.notes.push_back(make_head(NoteType::ScratchHoldStart, nid(), 2880, 5, 1));
  {
    NotationNote body = make_body(NoteType::ScratchHold, nid(), 2880, 3840, 5, 1);
    body.scratch_length = 0;
    chart.notes.push_back(body);
  }

  // ScratchHold moving end lane
  {
    NotationNote body = make_body(NoteType::ScratchHold, nid(), 2880, 3840, 7, 1);
    set_scratch_hold_end_lanes(body, 9, 9);
    chart.notes.push_back(body);
  }

  // ScratchCriticalHold
  chart.notes.push_back(make_head(NoteType::ScratchCriticalHoldStart, nid(), 2880, 10, 1));
  {
    NotationNote body =
        make_body(NoteType::ScratchCriticalHold, nid(), 2880, 3840, 10, 1);
    body.scratch_length = 0;
    chart.notes.push_back(body);
  }

  // Sound mid on Hold
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 4320, 0, 1));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 4320, 5280, 0, 1));
  chart.notes.push_back(make_body(NoteType::Sound, nid(), 4800, 0, 0, 1));

  // HoldEighth must not become Sound
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 4320, 2, 1));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 4320, 5280, 2, 1));
  chart.notes.push_back(make_body(NoteType::HoldEighth, nid(), 4800, 4800, 2, 1));

  // High lanes for ched padding
  chart.notes.push_back(make_body(NoteType::Normal, nid(), 5760, 0, 7, 2));
  chart.notes.push_back(make_body(NoteType::Critical, nid(), 6000, 0, 9, 1));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 5760, 6720, 8, 2));

  // Cross-measure long hold
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 0, 3840, 4, 1));

  // Partial-width authored head
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 6720, 5, 2));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 6720, 7680, 5, 3));

  // NontailHold (SUS has no nontail bit → imports as tailed Hold)
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 7680, 0, 1));
  chart.notes.push_back(make_body(NoteType::NontailHold, nid(), 7680, 8640, 0, 1));

  // ScratchSound mid (imports as Sound)
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 7680, 2, 1));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 7680, 8640, 2, 1));
  chart.notes.push_back(make_body(NoteType::ScratchSound, nid(), 8160, 0, 2, 1));

  // Edge lanes + wide tap under ched padding
  chart.notes.push_back(make_body(NoteType::Normal, nid(), 8640, 0, 0, 4));
  chart.notes.push_back(make_body(NoteType::Critical, nid(), 8640, 0, 11, 1));

  // Two simultaneous Hold bodies (different lanes / channels)
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 9600, 1, 1));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 9600, 10560, 1, 1));
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 9600, 3, 1));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 9600, 10560, 3, 1));

  // Hold ending exactly on measure boundary
  chart.notes.push_back(make_head(NoteType::HoldStart, nid(), 1920, 11, 1));
  chart.notes.push_back(make_body(NoteType::Hold, nid(), 1920, 3840, 11, 1));

  SusChartSaveOptions options;
  options.ched_lane_padding = true;
  options.meta.title = "comprehensive-roundtrip";
  options.meta.artist = "debug";
  options.meta.designer = "wds-editor";
  options.meta.difficulty = "expert";
  options.meta.play_level = "14";
  options.meta.song_id = "999";
  options.meta.wave_offset_sec = -0.125;
  options.meta.ticks_per_beat = 480;

  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));

  auto count_type = [](const NotationChart& c, NoteType t) {
    return static_cast<int>(std::count_if(
        c.notes.begin(), c.notes.end(), [t](const NotationNote& n) { return n.note_type == t; }));
  };

  CHECK(count_type(loaded.chart, NoteType::Hold) >= 1);
  CHECK(count_type(loaded.chart, NoteType::ScratchHold) >= 1);
  CHECK(count_type(loaded.chart, NoteType::ScratchCriticalHold) >= 1);
  CHECK(count_type(loaded.chart, NoteType::CriticalHold) >= 1);
  CHECK(count_type(loaded.chart, NoteType::Flick) >= 1);
  {
    bool lane9_normal = false;
    for (const auto& n : loaded.chart.notes) {
      if (n.note_type == NoteType::Normal && n.start_tick == 1200 &&
          n.lane == 9) {
        lane9_normal = true;
      }
    }
    CHECK(lane9_normal);
  }
  CHECK(count_type(loaded.chart, NoteType::Sound) >= 1);
  CHECK_EQ(count_type(loaded.chart, NoteType::HoldEighth), 0);
  // ScratchSound on a blue Hold has no Ched encoding → imports as Sound; Nontail → tailed.
  CHECK_EQ(count_type(loaded.chart, NoteType::ScratchSound), 0);
  CHECK_EQ(count_type(loaded.chart, NoteType::NontailHold), 0);

  // High lanes stay in WDS space under ched padding
  {
    bool found_lane7 = false;
    bool found_lane9 = false;
    for (const auto& n : loaded.chart.notes) {
      if (n.note_type == NoteType::Normal && n.start_tick == 5760 &&
          n.lane == 7 && n.width == 2) {
        found_lane7 = true;
      }
      if (n.note_type == NoteType::Critical && n.start_tick == 6000 &&
          n.lane == 9) {
        found_lane9 = true;
      }
    }
    CHECK(found_lane7);
    CHECK(found_lane9);
  }

  // Stationary ScratchHold remains ScratchHold (not blue Hold)
  {
    const NotationNote* sh = nullptr;
    for (const auto& n : loaded.chart.notes) {
      if (n.note_type == NoteType::ScratchHold && n.start_tick == 2880 &&
          n.lane == 5) {
        sh = &n;
        break;
      }
    }
    CHECK(sh != nullptr);
  }

  // Headless hold at lane 3 tick 1440
  {
    ChartDocument doc;
    doc.set_notes(loaded.chart.notes);
    doc.set_timing(loaded.chart.timing);
    bool has_body = false;
    bool has_head = false;
    for (const auto& n : loaded.chart.notes) {
      if (n.start_tick != 1440 || n.lane != 3) continue;
      if (n.note_type == NoteType::Hold) {
        has_body = true;
        has_head = paired_hold_head_for(doc, n).has_value();
      }
    }
    CHECK(has_body);
    CHECK(!has_head);
  }

  // Critical covering CriticalHold: body headless; Critical tap kept for judgment.
  {
    bool has_crit_hold = false;
    bool has_crit_head = false;
    bool has_crit_tap = false;
    for (const auto& n : loaded.chart.notes) {
      if (n.start_tick != 2880 || n.lane != 3) continue;
      if (n.note_type == NoteType::CriticalHold) has_crit_hold = true;
      if (n.note_type == NoteType::CriticalHoldStart) has_crit_head = true;
      if (n.note_type == NoteType::Critical) has_crit_tap = true;
    }
    CHECK(has_crit_hold);
    CHECK(!has_crit_head);
    CHECK(has_crit_tap);
  }

  // Partial-width head must not shrink body width
  {
    int body_w = -1;
    for (const auto& n : loaded.chart.notes) {
      if (n.note_type == NoteType::Hold && n.start_tick == 6720 &&
          n.lane == 5) {
        body_w = n.width;
      }
    }
    CHECK_EQ(body_w, 3);
  }

  // Moving ScratchHold keeps end span (Sirius clamps end to cover body → [7,9])
  {
    const NotationNote* moving = nullptr;
    for (const auto& n : loaded.chart.notes) {
      if (n.note_type == NoteType::ScratchHold && n.start_tick == 2880 &&
          n.lane == 7) {
        moving = &n;
        break;
      }
    }
    CHECK(moving != nullptr);
    if (moving) {
      const auto range = get_scratch_end_lane_range(*moving);
      CHECK_EQ(range.first, 7);
      CHECK_EQ(range.second, 9);
      CHECK(moving->scratch_length > 0);
    }
  }

  // Edge lane 11 Critical preserved under ched padding
  {
    bool found = false;
    for (const auto& n : loaded.chart.notes) {
      if (n.note_type == NoteType::Critical && n.start_tick == 8640 &&
          n.lane == 11) {
        found = true;
      }
    }
    CHECK(found);
  }

  CHECK(std::fabs(loaded.chart.timing.bpm - 185.0) < 0.01);
  CHECK(std::fabs(loaded.meta.wave_offset_sec - (-0.125)) < 0.0001);
  CHECK(loaded.chart.timing.points.size() >= 2);
  CHECK_EQ(loaded.chart.timing.offset_ms, -125);
}

void test_timing_tick_ms_roundtrip_multi_bpm() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.offset_ms = 1000;
  timing.points = {
      TimingPoint{0, 120.0, 4, 4, true, true},
      TimingPoint{480, 240.0, 4, 4, true, false},
      TimingPoint{960, 60.0, 4, 4, true, false},
  };
  normalize_timing_points(timing);

  NotationNote a = make_tap(0, 0);
  NotationNote b = make_tap(480, 0);
  NotationNote c = make_tap(960, 0);
  NotationNote d = make_tap(1440, 0);

  // offset 1000: tick0→1000ms; +480@120BPM→500ms → 1500; +480@240→250ms → 1750;
  // +480@60→1000ms → 2750.
  CHECK_EQ(a.start_ms(timing), 1000);
  CHECK_EQ(b.start_ms(timing), 1500);
  CHECK_EQ(c.start_ms(timing), 1750);
  CHECK_EQ(d.start_ms(timing), 2750);
}

void test_resolve_convert_scratch_head_stays_official() {
  ChartDocument doc;
  NotationNote body = make_tap(0, 2);
  body.id = 1;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::ScratchHold;
  CHECK(doc.add_note(body) == 1);
  NotationNote head = make_tap(0, 2);
  head.id = 2;
  head.width = 2;
  head.note_type = NoteType::ScratchHoldStart;
  CHECK(doc.add_note(head) == 2);

  // Head-only: legal retints stay in the ScratchHold head family.
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Critical) ==
        NoteType::ScratchCriticalHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Normal) ==
        NoteType::ScratchHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::HoldStart) ==
        NoteType::ScratchHoldStart);
  // Scratch head cannot become HoldStart while body stays ScratchHold.
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Hold) ==
        NoteType::ScratchHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::ScratchHold) ==
        NoteType::ScratchHoldStart);
  // Flick is not a legal head conversion → keep current type.
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Flick) ==
        NoteType::ScratchHoldStart);
  // Bodies still collapse to the instantaneous / forced body type.
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Hold) == NoteType::Hold);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::ScratchHold) ==
        NoteType::ScratchHold);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Normal) == NoteType::Normal);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Critical) == NoteType::Critical);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::HoldStart) ==
        NoteType::ScratchHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Flick) == NoteType::Flick);
}

int main() {
  test_auto_note_id_starts_at_zero();
  test_concurrent_lines_multi_press_only();
  test_hold_head_pairs_but_attached_excludes_head();
  test_scratch_hold_end_lane_encoding();
  test_scratch_chain_joint_direction();
  test_hold_head_suppressed_by_non_body_overlap_not_by_hold_body();
  test_hold_head_partial_overlap_single_free_run();
  test_hold_head_partial_overlap_multiple_free_runs_skipped();
  test_hold_head_partial_overlap_with_prior_hold_tail();
  test_scratch_hold_auto_head_is_scratch_hold_start();
  test_repair_legacy_scratch_hold_heads();
  test_resolve_convert_scratch_head_stays_official();
  test_explicit_note_id_zero();
  test_normalize_for_save_reassigns_zero_based();
  test_save_reload_normalizes_and_reloads();
  test_save_success_preserves_history_and_ids();
  test_replace_file_atomic_preserves_target_on_failure();
  test_load_rejects_huge_notes_count();
  test_load_rejects_lane_width_overflow();
  test_load_rejects_nonfinite_ticks();
  test_measure_ticks_no_hang_near_int_max();
  test_sus_rejects_huge_measurebs();
  test_export_sus_after_edit_preserves_engine_history();
  test_load_fixture_normalized_chart();
  test_start_ms_avl_index_range_query();
  test_chart_note_index_candidates();
  test_remove_note_keeps_zero_based_ids();
  test_preview_note_and_snapshot_incremental_api();
  test_snapshot_incremental_tick();
  test_snapshot_aux_objects_incremental();
  test_snapshot_large_seek_full_rebuild();
  test_split_gimmick_range_and_combo_includes_heads();
  test_split_appear_phase_before_start_ms();
  test_hold_start_visible_with_zero_end_tick();
  test_load_legacy_v1_wdschart();
  test_official_chart_import_and_roundtrip();
  test_official_chart_load_auto();
  test_load_repo_test_official_charts();
  test_wdsproject_format_roundtrip_and_relative_paths();
  test_edit_grid_and_note_operations();
  test_timing_bpm_meter_split_and_prune();
  test_truncated_wdschart_rejected();
  test_official_csv_tempo_map_export();
  test_official_sound_purple_split_and_row_semantics();
  test_migrate_wdschart_scratch_to_flick_script();
  test_save_failure_preserves_note_ids();
  test_sus_meter_and_mid_measure_bpm_roundtrip();
  test_chart_session_preserves_per_chart_history();
  test_history_hold_eighths_and_project_v2();
  test_sus_hold_auto_generates_head_when_uncovered();
  test_hold_eighths_outside_history_undo_redo();
  test_composite_undo_rolls_back_on_partial_failure();
  test_paired_hold_head_tolerates_subtick_drift();
  test_recompute_hold_eighths_respects_fractional_star();
  test_timing_tick_ms_roundtrip_multi_bpm();
  test_sus_channel_reuse_two_holds();
  test_sus_cross_measure_hold_at_bar_head();
  test_sus_spec_example_hold_14002400();
  test_sus_export_headless_hold_body();
  test_sus_export_covered_hold_skips_damage();
  test_sus_export_partial_hold_head_pairs_body();
  test_sus_hold_mid_star_roundtrip();
  test_sus_slide_export_not_orphan_hold_start();
  test_sus_standalone_directional_keeps_width();
  test_sus_orphan_air_ignored();
  test_sus_tap_plus_directional_becomes_flick();
  test_sus_ched_slide_air_distinguishes_hold_family();
  test_sus_ched_export_slide_and_end_pair();
  test_sus_ched_mid_flick_splits_jump_scratch();
  test_sus_til01_split_roundtrip();
  test_wdschart_export_import_preserves_chart();
  test_sus_slide_invisible_mid_not_sound();
  test_sus_fractional_measure_length();
  test_sus_measurebs_offset();
  test_sus_waveoffset_roundtrip();
  test_sus_export_slot_remap_preserves_note_count();
  test_sus_critical_tap_roundtrip();
  test_sus_bpm_defs_without_change_pick_lowest_id();
  test_sus_measurebs_export_roundtrip();
  test_sus_roundtrip_hold_families_and_lanes();
  test_sus_critical_hold_exports_as_critical_plus_hold();
  test_sus_critical_plus_hold_imports_headless();
  test_sus_comprehensive_roundtrip_all_cases();

  // SUS sample (optional — skip if missing).
  {
    const fs::path sus = fixture_path("../test/chart.sus");
    const fs::path sus2 = fixture_path("../../test/chart.sus");
    const fs::path sus3 = fs::path(__FILE__).parent_path().parent_path().parent_path() / "test" /
                          "chart.sus";
    fs::path path;
    for (const auto& p : {sus, sus2, sus3, fs::path("test/chart.sus")}) {
      std::error_code ec;
      if (fs::is_regular_file(p, ec) && !ec) {
        path = p;
        break;
      }
    }
    if (!path.empty()) {
      SusChartLoadResult loaded;
      const auto r = SusChartFormat::load_file(path.string(), loaded);
      CHECK(r.error == SerializeError::Ok);
      CHECK(!loaded.chart.notes.empty());
      CHECK(loaded.meta.ticks_per_beat == 480);
      CHECK(std::fabs(loaded.chart.timing.bpm - 260.0) < 0.01);
      NotationChart via_auto;
      ChartEditMode mode = ChartEditMode::Editable;
      const auto auto_r = ChartSerializer::load_auto(path.string(), via_auto, {}, &mode);
      CHECK(auto_r.error == SerializeError::Ok);
      CHECK(mode == ChartEditMode::OfficialPreviewOnly);
      CHECK(via_auto.notes.size() == loaded.chart.notes.size());
    }
  }

  const int failures = wds::chart_editor::test::failure_count();
  if (failures == 0) {
    std::printf("All chart_editor tests passed.\n");
    return 0;
  }

  std::printf("%d test(s) failed.\n", failures);
  return 1;
}

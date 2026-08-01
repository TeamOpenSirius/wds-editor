#include "test_support.hpp"

#include <wds/core/chart_index.hpp>
#include <wds/core/chart_serializer.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/edit_history.hpp>
#include <wds/core/detail/start_ms_avl_index.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/sus_chart.hpp>
#include <wds/core/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace wds::chart_editor;
namespace fs = std::filesystem;

NotationNote make_tap(float start_tick, int32_t lane) {
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
  const int32_t a = doc.add_note(make_tap(0.0f, 0));
  const int32_t b = doc.add_note(make_tap(480.0f, 1));
  const int32_t c = doc.add_note(make_tap(960.0f, 2));

  CHECK_EQ(a, 0);
  CHECK_EQ(b, 1);
  CHECK_EQ(c, 2);
  CHECK_EQ(doc.next_note_id(), 3);
  CHECK_EQ(static_cast<int32_t>(doc.notes().size()), 3);
}

void test_concurrent_lines_multi_press_only() {
  ChartDocument doc;
  // Lone taps at different times → no sync line.
  doc.add_note(make_tap(0.0f, 0));
  doc.add_note(make_tap(480.0f, 1));
  CHECK_EQ(static_cast<int32_t>(doc.concurrent_lines().size()), 0);

  // Second note at the same tick → multi-press sync line.
  doc.add_note(make_tap(0.0f, 4));
  CHECK_EQ(static_cast<int32_t>(doc.concurrent_lines().size()), 1);
  CHECK_EQ(doc.concurrent_lines()[0].start_lane, 0);
  CHECK_EQ(doc.concurrent_lines()[0].width, 5);  // lanes 0..4

  // Hold body / eighth / mid-star must not create or join sync lines by themselves.
  ChartDocument hold_doc;
  NotationNote body = make_tap(0.0f, 2);
  body.note_type = NoteType::Hold;
  body.end_tick = 960.0f;
  hold_doc.add_note(body);
  NotationNote eighth = make_tap(480.0f, 2);
  eighth.note_type = NoteType::HoldEighth;
  hold_doc.add_note(eighth);
  NotationNote star = make_tap(240.0f, 2);
  star.note_type = NoteType::Sound;
  hold_doc.add_note(star);
  CHECK_EQ(static_cast<int32_t>(hold_doc.concurrent_lines().size()), 0);

  // Hold tail + tap at the same end time → sync line (tail is a hit, not body/star).
  NotationNote tap_at_end = make_tap(960.0f, 6);
  hold_doc.add_note(tap_at_end);
  CHECK_EQ(static_cast<int32_t>(hold_doc.concurrent_lines().size()), 1);
}

void test_explicit_note_id_zero() {
  ChartDocument doc;
  NotationNote note = make_tap(120.0f, 3);
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

  NotationNote late = make_tap(960.0f, 2);
  late.id = 99;
  NotationNote early = make_tap(0.0f, 0);
  early.id = 42;
  NotationNote mid = make_tap(480.0f, 1);
  mid.end_tick = 960.0f;
  mid.id = 7;

  doc.set_notes({late, early, mid});
  doc.normalize_for_save();

  const auto& notes = doc.notes();
  CHECK_EQ(static_cast<int32_t>(notes.size()), 3);
  CHECK_EQ(notes[0].start_tick, 0.0f);
  CHECK_EQ(notes[1].start_tick, 480.0f);
  CHECK_EQ(notes[2].start_tick, 960.0f);
  CHECK_EQ(notes[0].id, 0);
  CHECK_EQ(notes[1].id, 1);
  CHECK_EQ(notes[2].id, 2);
  CHECK_EQ(doc.next_note_id(), 3);
}

void test_save_reload_normalizes_and_reloads() {
  ChartEditorEngine engine;

  NotationNote split = make_tap(0.0f, 0);
  split.end_tick = 1920.0f;
  split.width = 6;
  split.gimmick_type = GimmickType::Split3;
  engine.add_note(split);

  NotationNote tap = make_tap(480.0f, 2);
  engine.add_note(tap);

  const fs::path path = temp_chart_path("save_reload.wdschart");
  const auto save = engine.save_to_file(path.string());
  CHECK_EQ(static_cast<int>(save.error), static_cast<int>(SerializeError::Ok));
  CHECK(!engine.is_dirty());

  const auto& notes = engine.document().notes();
  CHECK_EQ(static_cast<int32_t>(notes.size()), 2);
  CHECK_EQ(notes[0].id, 0);
  CHECK_EQ(notes[1].id, 1);
  CHECK_EQ(notes[0].start_tick, 0.0f);
  CHECK_EQ(notes[1].start_tick, 480.0f);

  ChartEditorEngine reloaded;
  const auto load = reloaded.load_from_file(path.string());
  CHECK_EQ(static_cast<int>(load.error), static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int32_t>(reloaded.document().notes().size()), 2);
  CHECK_EQ(reloaded.document().notes()[0].id, 0);
  CHECK_EQ(reloaded.document().notes()[1].id, 1);
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
  CHECK_EQ(n0->start_tick, 0.0f);
  CHECK_EQ(n1->start_tick, 480.0f);
  CHECK_EQ(n2->gimmick_type, GimmickType::Split3);
  CHECK_EQ(static_cast<int32_t>(engine.document().concurrent_lines().size()), 1);
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
  doc.add_note(make_tap(0.0f, 0));
  doc.add_note(make_tap(480.0f, 1));
  doc.add_note(make_tap(960.0f, 2));

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
  doc.add_note(make_tap(0.0f, 0));
  doc.add_note(make_tap(480.0f, 1));
  doc.add_note(make_tap(960.0f, 2));

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
    engine.add_note(make_tap(static_cast<float>(i * 480), i % 6));
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
                    engine.timeline_ms(), engine.playback_state(), engine.revision());
  CHECK(snapshot_contents_equal(engine.snapshot(), full));
  CHECK(engine.snapshot().find_note(0) != nullptr);
}

void test_snapshot_aux_objects_incremental() {
  ChartEditorEngine engine;
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  engine.document().set_timing(timing);

  NotationNote split = make_tap(0.0f, 0);
  split.end_tick = 1920.0f;
  split.width = 6;
  split.gimmick_type = GimmickType::Split3;
  engine.add_note(split);
  engine.add_note(make_tap(480.0f, 2));

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
                    engine.timeline_ms(), engine.playback_state(), engine.revision());
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
  engine.add_note(make_tap(0.0f, 0));
  engine.add_note(make_tap(480.0f, 1));
  engine.add_note(make_tap(960.0f, 2));

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
  hs.start_tick = 480.0f;  // 1s @ BPM 60
  hs.end_tick = 0.0f;      // buggy legacy / pre-fix import shape
  hs.lane = 0;
  hs.width = 3;
  hs.note_type = NoteType::HoldStart;
  engine.add_note(hs);

  NotationNote body;
  body.start_tick = 480.0f;
  body.end_tick = 960.0f;
  body.lane = 0;
  body.width = 3;
  body.note_type = NoteType::Hold;
  engine.add_note(body);

  const int64_t start_ms = tick_to_milliseconds(480.0f, timing);
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
  bool found_scratch = false;
  for (const auto& n : notes) {
    if (n.note_type == NoteType::Critical && n.width == 4 && n.lane == 0) {
      found_critical = true;
      CHECK(std::abs(n.start_tick - 1.0169f * 480.0f) < 0.5f);
      // Instantaneous official rows: end_tick == start_tick (not 0).
      CHECK(std::abs(n.end_tick - n.start_tick) < 0.5f);
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
    if (n.note_type == NoteType::Flick) {
      found_flick = true;
      CHECK_EQ(n.lane, 3);  // official 4
      CHECK_EQ(n.width, 6);
    }
    if (n.note_type == NoteType::Scratch) {
      found_scratch = true;
      CHECK_EQ(n.lane, 3);  // official 4
    }
  }
  CHECK(found_critical);
  CHECK(found_split);
  CHECK(found_jump);
  CHECK(found_flick);
  CHECK(found_scratch);

  const fs::path out_path = temp_chart_path("official_roundtrip.csv");
  const auto save = engine.export_official_to_file(out_path.string());
  CHECK_EQ(static_cast<int>(save.error), static_cast<int>(SerializeError::ReadOnly));
  CHECK(engine.is_read_only());

  // Official import is preview-only: mutations and .wdschart save are rejected.
  CHECK_EQ(engine.add_note(make_tap(100.0f, 1)), -1);
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
  NotationNote tap = make_tap(0.0f, 1);
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

  NotationNote tap = make_tap(480.0f, 3);
  tap.width = 2;
  const NotationNote hold = convert_note_type(tap, NoteType::Hold, 480);
  CHECK_EQ(static_cast<int>(hold.note_type), static_cast<int>(NoteType::Hold));
  CHECK_EQ(hold.end_tick, 960.0f);
  const NotationNote restored = convert_note_type(hold, NoteType::Flick, 480);
  CHECK_EQ(static_cast<int>(restored.note_type), static_cast<int>(NoteType::Flick));

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

  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Critical) ==
        NoteType::ScratchCriticalHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Normal) ==
        NoteType::ScratchHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::HoldStart) ==
        NoteType::ScratchHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Hold) ==
        NoteType::ScratchHoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::HoldStart) ==
        NoteType::ScratchHold);
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
  test_history_hold_eighths_and_project_v2();

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

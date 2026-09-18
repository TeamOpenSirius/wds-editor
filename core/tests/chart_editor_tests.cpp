#include "test_support.hpp"

#include <wds/core/chart_index.hpp>
#include <wds/core/chart_serializer.hpp>
#include <wds/core/chart_session.hpp>
#include <wds/core/easing.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/edit_history.hpp>
#include <wds/core/detail/start_ms_avl_index.hpp>
#include <wds/core/file_io.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/note_position_calculator.hpp>
#include <wds/core/scratch_hold_curve.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/core/split_lane_simulator.hpp>
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
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

void test_normalize_remaps_and_rebinds_star_parents() {
  ChartDocument doc;
  NotationNote hold = make_tap(0, 6);
  hold.id = 1147;
  hold.width = 6;
  hold.end_tick = 1920;
  hold.note_type = NoteType::Hold;
  NotationNote bound = make_tap(480, 6);
  bound.id = 2000;
  bound.width = 6;
  bound.end_tick = 480;
  bound.note_type = NoteType::Sound;
  bound.parent_hold_id = 1147;
  NotationNote dangling = make_tap(960, 6);
  dangling.id = 2001;
  dangling.width = 6;
  dangling.end_tick = 960;
  dangling.note_type = NoteType::Sound;
  dangling.parent_hold_id = 99999;
  NotationNote unbound = make_tap(1440, 6);
  unbound.id = 2002;
  unbound.width = 6;
  unbound.end_tick = 1440;
  unbound.note_type = NoteType::Sound;
  unbound.parent_hold_id = kNoBoundHoldId;
  CHECK(doc.set_notes({hold, bound, dangling, unbound}));

  const auto normalized = doc.normalized_chart();
  const NotationNote* n_hold = nullptr;
  int rebound = 0;
  for (const auto& n : normalized.notes) {
    if (n.note_type == NoteType::Hold) n_hold = &n;
    if (n.note_type == NoteType::Sound) {
      CHECK_EQ(n.parent_hold_id, 0);
      ++rebound;
    }
  }
  CHECK(n_hold != nullptr);
  if (n_hold != nullptr) CHECK_EQ(n_hold->id, 0);
  CHECK_EQ(rebound, 3);

  CHECK(doc.normalize_for_save());
  CHECK_EQ(doc.find_note(0)->note_type, NoteType::Hold);
  for (const auto& n : doc.notes()) {
    if (n.note_type == NoteType::Sound) CHECK_EQ(n.parent_hold_id, 0);
  }
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

SerializeError load_chart_error(const char* name, const std::string& body) {
  const fs::path path = temp_chart_path(name);
  {
    std::ofstream out(path);
    out << body;
  }
  NotationChart chart;
  return ChartSerializer::load_from_file(path.string(), chart).error;
}

void test_load_rejects_invalid_tpq_and_nonfinite_bpm() {
  CHECK_EQ(kMaxTicksPerQuarter, INT32_MAX / 128);
  const int32_t max_tpq = kMaxTicksPerQuarter;
  CHECK_EQ(static_cast<int>(load_chart_error(
               "tpq_zero.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ 0\nTIMING 0\nNOTES 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "tpq_negative.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ -1\nTIMING 0\nNOTES 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "tpq_too_large.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ " + std::to_string(max_tpq + 1) +
                   "\nTIMING 0\nNOTES 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "bpm_zero.wdschart",
               "WDSCHART 4\nBPM 0\nTPQ 480\nTIMING 0\nNOTES 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "bpm_nan.wdschart",
               "WDSCHART 4\nBPM nan\nTPQ 480\nTIMING 0\nNOTES 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "bpm_inf.wdschart",
               "WDSCHART 4\nBPM inf\nTPQ 480\nTIMING 0\nNOTES 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "timing_bpm_nan.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 nan 4 4 3\nNOTES 0\n"
               "CONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "timing_bpm_inf.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 inf 4 4 3\nNOTES 0\n"
               "CONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
  CHECK_EQ(static_cast<int>(load_chart_error(
               "timing_bpm_zero.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 0 4 4 3\nNOTES 0\n"
               "CONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));

  NotationChart accepted;
  const fs::path ok = temp_chart_path("tpq_max_ok.wdschart");
  {
    std::ofstream out(ok);
    out << "WDSCHART 4\nBPM 120\nTPQ " << max_tpq
        << "\nTIMING 0\nNOTES 0\nCONCURRENT 0\nEND\n";
  }
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(ok.string(), accepted).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(accepted.timing.ticks_per_quarter, max_tpq);
}

void test_set_timing_rejects_illegal_tpq_atomically() {
  ChartDocument doc;
  CHECK_EQ(doc.add_note(make_tap(0, 0)), 0);
  doc.mark_saved();
  const uint64_t gen = doc.content_generation();
  const int32_t tpq0 = doc.timing().ticks_per_quarter;
  CHECK_EQ(tpq0, 480);

  MusicTiming zero = doc.timing();
  zero.ticks_per_quarter = 0;
  CHECK(!doc.set_timing(zero));
  CHECK_EQ(doc.timing().ticks_per_quarter, tpq0);
  CHECK_EQ(doc.content_generation(), gen);
  CHECK(!doc.is_dirty());
  CHECK_EQ(static_cast<int32_t>(doc.notes().size()), 1);

  MusicTiming huge = doc.timing();
  huge.ticks_per_quarter = INT32_MAX / 128 + 1;
  CHECK(!doc.set_timing(huge));
  CHECK_EQ(doc.timing().ticks_per_quarter, tpq0);
  CHECK_EQ(doc.content_generation(), gen);
  CHECK(!doc.is_dirty());

  MusicTiming ok = doc.timing();
  ok.bpm = 140.0;
  ok.points = {TimingPoint{0, 140.0, 4, 4, true, true}};
  ok.ticks_per_quarter = 480;
  CHECK(doc.set_timing(ok));
  CHECK_EQ(doc.timing().ticks_per_quarter, 480);
  CHECK(std::fabs(doc.timing().bpm - 140.0) < 1e-9);
}

void test_construct_normalize_fallback_illegal_tpq() {
  MusicTiming zero;
  zero.ticks_per_quarter = 0;
  ChartDocument from_zero(zero);
  CHECK_EQ(from_zero.timing().ticks_per_quarter, 480);

  MusicTiming huge;
  huge.ticks_per_quarter = INT32_MAX / 128 + 1;
  normalize_timing_points(huge);
  CHECK_EQ(huge.ticks_per_quarter, 480);

  MusicTiming negative;
  negative.ticks_per_quarter = -12;
  ChartDocument from_neg(negative);
  CHECK_EQ(from_neg.timing().ticks_per_quarter, 480);

  MusicTiming valid;
  valid.ticks_per_quarter = 240;
  normalize_timing_points(valid);
  CHECK_EQ(valid.ticks_per_quarter, 240);
}

void test_tick_ms_saturates_nan_inf_and_extremes() {
  MusicTiming normal;
  normal.bpm = 120.0;
  normal.ticks_per_quarter = 480;
  normal.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  normalize_timing_points(normal);
  CHECK_EQ(tick_to_milliseconds(480, normal), 500);
  CHECK_EQ(milliseconds_to_tick(500, normal), 480);

  MusicTiming tiny;
  tiny.bpm = 1e-300;
  tiny.ticks_per_quarter = 480;
  tiny.points = {TimingPoint{0, 1e-300, 4, 4, true, true}};
  normalize_timing_points(tiny);
  CHECK_EQ(tick_to_milliseconds(1, tiny), std::numeric_limits<int64_t>::max());

  MusicTiming nan_bpm;
  nan_bpm.bpm = 120.0;
  nan_bpm.ticks_per_quarter = 480;
  nan_bpm.points = {TimingPoint{0, std::numeric_limits<double>::quiet_NaN(), 4, 4, true, true}};
  nan_bpm.prefix_ms = {0.0};
  const int64_t nan_ms = tick_to_milliseconds(480, nan_bpm);
  CHECK(nan_ms == 500 || nan_ms == 0 || nan_ms == std::numeric_limits<int64_t>::max() ||
        nan_ms == std::numeric_limits<int64_t>::min());

  normal.offset_ms = 1000;
  CHECK_EQ(milliseconds_to_tick(std::numeric_limits<int64_t>::min(), normal), 0);
  CHECK_EQ(milliseconds_to_tick(std::numeric_limits<int64_t>::max(), normal), INT32_MAX);
  CHECK_EQ(milliseconds_to_tick(1500, normal), 480);
}

void test_negative_offset_legal_tick_and_violation_query() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  normalize_timing_points(timing);
  CHECK_EQ(first_legal_note_tick(timing), 0);

  ChartDocument doc(timing);
  CHECK(doc.set_offset_ms(-2000));
  CHECK_EQ(doc.timing().offset_ms, -2000);
  CHECK_EQ(first_legal_note_tick(doc.timing()), 1920);

  NotationNote tap_early = make_tap(0, 0);
  tap_early.id = 1;
  NotationNote tap_ok = make_tap(1920, 1);
  tap_ok.id = 2;
  NotationNote hold;
  hold.id = 3;
  hold.note_type = NoteType::Hold;
  hold.start_tick = 1800;
  hold.end_tick = 2400;
  hold.lane = 2;
  hold.width = 1;
  NotationNote split;
  split.id = 4;
  split.note_type = NoteType::None;
  split.gimmick_type = GimmickType::Split3;
  split.start_tick = 1920;
  split.end_tick = 2880;
  NotationNote split_early;
  split_early.id = 5;
  split_early.note_type = NoteType::None;
  split_early.gimmick_type = GimmickType::Split3;
  split_early.start_tick = 0;
  split_early.end_tick = 480;
  NotationNote hispeed;
  hispeed.id = 6;
  hispeed.note_type = NoteType::HiSpeed;
  hispeed.start_tick = 0;

  CHECK(note_intersects_negative_music_time(tap_early, doc.timing()));
  CHECK(!note_intersects_negative_music_time(tap_ok, doc.timing()));
  CHECK(note_intersects_negative_music_time(hold, doc.timing()));
  CHECK(!note_intersects_negative_music_time(split, doc.timing()));
  CHECK(note_intersects_negative_music_time(split_early, doc.timing()));
  CHECK(!note_intersects_negative_music_time(hispeed, doc.timing()));

  const auto ids = notes_in_negative_music_time(
      {tap_early, tap_ok, hold, split, split_early, hispeed}, doc.timing());
  CHECK_EQ(static_cast<int>(ids.size()), 3);
  CHECK_EQ(ids[0], 1);
  CHECK_EQ(ids[1], 3);
  CHECK_EQ(ids[2], 5);
}

void test_int32_tick_range_measure_and_snap_no_hang() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  normalize_timing_points(timing);

  const auto full = measure_ticks_in_range(0, INT32_MAX, timing);
  CHECK(!full.empty());
  CHECK_EQ(full.front(), 0);
  CHECK(full.size() < 2000000u);

  const int32_t last_bar = (INT32_MAX / 1920) * 1920;
  const auto near_end = measure_ticks_in_range(last_bar - 10, INT32_MAX, timing);
  CHECK(!near_end.empty());
  CHECK_EQ(near_end.front(), last_bar);

  CHECK_EQ(snap_to_measure(INT32_MAX, timing), last_bar);
  CHECK(snap_to_measure(INT32_MAX, timing) >= 0);

  TimingPoint den1{0, 120.0, 32, 1, true, true};
  CHECK_EQ(beat_length_ticks(den1, 536870912), INT32_MAX);
  CHECK_EQ(measure_length_ticks(den1, 100000000), INT32_MAX);
  CHECK_EQ(beat_length_ticks(TimingPoint{0, 120.0, 4, 4, true, true}, 480), 480);

  const int32_t fade = seconds_to_ticks_at(1.0f, timing, 0);
  CHECK_EQ(fade, 960);
  CHECK_EQ(seconds_to_ticks_at(std::numeric_limits<float>::infinity(), timing, 0), INT32_MAX);
  CHECK_EQ(seconds_to_ticks_at(std::numeric_limits<float>::quiet_NaN(), timing, 0), 1);
  CHECK_EQ(seconds_to_ticks_at(1e30f, timing, 0), INT32_MAX);
}

void test_note_id_max_and_auto_exhaust_are_atomic() {
  ChartDocument doc;
  NotationNote at_max = make_tap(0, 0);
  at_max.id = INT32_MAX;
  CHECK_EQ(doc.add_note(at_max), -1);
  CHECK(doc.notes().empty());
  CHECK_EQ(doc.next_note_id(), 0);

  NotationNote almost = make_tap(0, 0);
  almost.id = INT32_MAX - 1;
  CHECK_EQ(doc.add_note(almost), INT32_MAX - 1);
  CHECK_EQ(doc.next_note_id(), INT32_MAX);
  const uint64_t gen = doc.content_generation();

  CHECK_EQ(doc.add_note(make_tap(480, 1)), -1);
  CHECK_EQ(static_cast<int32_t>(doc.notes().size()), 1);
  CHECK_EQ(doc.next_note_id(), INT32_MAX);
  CHECK_EQ(doc.content_generation(), gen);

  NotationNote auto_note = make_tap(480, 1);
  CHECK(!doc.set_notes({auto_note}));
  CHECK_EQ(static_cast<int32_t>(doc.notes().size()), 1);
  CHECK_EQ(doc.notes()[0].id, INT32_MAX - 1);
  CHECK_EQ(doc.content_generation(), gen);
  CHECK_EQ(doc.next_note_id(), INT32_MAX);

  NotationNote explicit_max = make_tap(960, 2);
  explicit_max.id = INT32_MAX;
  CHECK(!doc.set_notes({explicit_max}));
  CHECK_EQ(doc.notes()[0].id, INT32_MAX - 1);
  CHECK_EQ(doc.content_generation(), gen);

  ChartDocument fresh;
  NotationNote a = make_tap(0, 0);
  NotationNote b = make_tap(480, 1);
  b.id = -5;
  CHECK(fresh.set_notes({a, b}));
  CHECK_EQ(static_cast<int32_t>(fresh.notes().size()), 2);
  CHECK_EQ(fresh.notes()[0].id, 0);
  CHECK_EQ(fresh.notes()[1].id, 1);
  CHECK_EQ(fresh.next_note_id(), 2);

  NotationNote high = make_tap(960, 2);
  high.id = INT32_MAX - 2;
  CHECK(fresh.set_notes({high}));
  CHECK_EQ(fresh.next_note_id(), INT32_MAX - 1);
  CHECK_EQ(fresh.notes()[0].id, INT32_MAX - 2);
}

void test_set_notes_auto_explicit_collision_is_atomic() {
  ChartDocument empty;
  empty.mark_saved();
  const uint64_t gen0 = empty.content_generation();
  CHECK(!empty.is_dirty());
  CHECK_EQ(empty.next_note_id(), 0);
  CHECK(empty.notes().empty());
  CHECK(!empty.find_note(0).has_value());

  NotationNote auto_first = make_tap(0, 0);
  NotationNote explicit_zero = make_tap(480, 1);
  explicit_zero.id = 0;
  CHECK(!empty.set_notes({auto_first, explicit_zero}));
  CHECK(empty.notes().empty());
  CHECK(!empty.find_note(0).has_value());
  CHECK_EQ(empty.next_note_id(), 0);
  CHECK_EQ(empty.content_generation(), gen0);
  CHECK(!empty.is_dirty());

  ChartDocument seeded;
  CHECK_EQ(seeded.add_note(make_tap(0, 3)), 0);
  seeded.mark_saved();
  const uint64_t gen1 = seeded.content_generation();
  const int32_t next1 = seeded.next_note_id();
  CHECK_EQ(next1, 1);
  NotationNote collide_one = make_tap(480, 1);
  collide_one.id = 1;
  CHECK(!seeded.set_notes({auto_first, collide_one}));
  CHECK_EQ(static_cast<int32_t>(seeded.notes().size()), 1);
  CHECK_EQ(seeded.notes()[0].id, 0);
  CHECK_EQ(seeded.notes()[0].lane, 3);
  CHECK(seeded.find_note(0).has_value());
  CHECK_EQ(seeded.find_note(0)->lane, 3);
  CHECK_EQ(seeded.next_note_id(), next1);
  CHECK_EQ(seeded.content_generation(), gen1);
  CHECK(!seeded.is_dirty());

  ChartDocument after_one;
  NotationNote explicit_one = make_tap(0, 0);
  explicit_one.id = 1;
  NotationNote auto_after = make_tap(480, 2);
  CHECK(after_one.set_notes({explicit_one, auto_after}));
  CHECK_EQ(static_cast<int32_t>(after_one.notes().size()), 2);
  CHECK(after_one.find_note(1).has_value());
  CHECK(after_one.find_note(2).has_value());
  CHECK_EQ(after_one.find_note(1)->start_tick, 0);
  CHECK_EQ(after_one.find_note(2)->start_tick, 480);
  CHECK_NE(after_one.find_note(1)->id, after_one.find_note(2)->id);
  CHECK(!after_one.find_note(0).has_value());
  CHECK_EQ(after_one.next_note_id(), 3);

  ChartDocument two_auto;
  NotationNote neg_a = make_tap(0, 0);
  neg_a.id = -1;
  NotationNote neg_b = make_tap(960, 4);
  neg_b.id = -9;
  CHECK(two_auto.set_notes({neg_a, neg_b}));
  CHECK_EQ(static_cast<int32_t>(two_auto.notes().size()), 2);
  CHECK_EQ(two_auto.notes()[0].id, 0);
  CHECK_EQ(two_auto.notes()[1].id, 1);
  CHECK_NE(two_auto.notes()[0].id, two_auto.notes()[1].id);
  CHECK_EQ(two_auto.find_note(0)->start_tick, 0);
  CHECK_EQ(two_auto.find_note(1)->start_tick, 960);
  CHECK_EQ(two_auto.next_note_id(), 2);
}

void expect_unique_safe_ids(const ChartDocument& doc) {
  std::unordered_set<int32_t> ids;
  ids.reserve(doc.notes().size());
  for (const auto& note : doc.notes()) {
    CHECK(note.id >= 0);
    CHECK(note.id < INT32_MAX);
    CHECK(ids.insert(note.id).second);
  }
  CHECK(doc.next_note_id() >= 0);
  CHECK(doc.next_note_id() < INT32_MAX || doc.notes().empty());
  if (!doc.notes().empty()) {
    CHECK(doc.next_note_id() == static_cast<int32_t>(doc.notes().size()) ||
          doc.next_note_id() > doc.notes().back().id ||
          doc.find_note(doc.next_note_id() - 1).has_value());
  }
}

void test_load_rejects_explicit_int32_max_note_id() {
  CHECK_EQ(static_cast<int>(load_chart_error(
               "note_id_max.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 0\nNOTES 1\nN " +
                   std::to_string(INT32_MAX) + " 0 0 10 0 1 0 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
}

void test_load_from_chart_renormalizes_unsafe_ids_keeps_sparse() {
  NotationChart sparse;
  NotationNote keep_a = make_tap(0, 0);
  keep_a.id = 10;
  NotationNote keep_b = make_tap(480, 2);
  keep_b.id = 42;
  sparse.notes = {keep_a, keep_b};
  ChartDocument sparse_doc;
  sparse_doc.load_from_chart(sparse);
  CHECK_EQ(static_cast<int32_t>(sparse_doc.notes().size()), 2);
  CHECK(sparse_doc.find_note(10).has_value());
  CHECK(sparse_doc.find_note(42).has_value());
  CHECK_EQ(sparse_doc.find_note(10)->lane, 0);
  CHECK_EQ(sparse_doc.find_note(42)->lane, 2);
  CHECK_EQ(sparse_doc.next_note_id(), 43);

  NotationChart max_auto;
  NotationNote at_max = make_tap(0, 0);
  at_max.id = INT32_MAX;
  NotationNote auto_note = make_tap(480, 1);
  auto_note.id = -1;
  max_auto.notes = {at_max, auto_note};
  ChartDocument max_doc;
  max_doc.load_from_chart(max_auto);
  CHECK_EQ(static_cast<int32_t>(max_doc.notes().size()), 2);
  expect_unique_safe_ids(max_doc);
  CHECK(!max_doc.find_note(INT32_MAX).has_value());
  CHECK_EQ(max_doc.notes()[0].id, 0);
  CHECK_EQ(max_doc.notes()[1].id, 1);
  CHECK_EQ(max_doc.next_note_id(), 2);

  NotationChart dup;
  NotationNote d0 = make_tap(0, 0);
  d0.id = 7;
  NotationNote d1 = make_tap(960, 3);
  d1.id = 7;
  dup.notes = {d0, d1};
  ChartDocument dup_doc;
  dup_doc.load_from_chart(dup);
  CHECK_EQ(static_cast<int32_t>(dup_doc.notes().size()), 2);
  expect_unique_safe_ids(dup_doc);
  CHECK_EQ(dup_doc.notes()[0].id, 0);
  CHECK_EQ(dup_doc.notes()[1].id, 1);
  CHECK_EQ(dup_doc.next_note_id(), 2);
  CHECK_EQ(dup_doc.find_note(0)->start_tick, 0);
  CHECK_EQ(dup_doc.find_note(1)->start_tick, 960);

  NotationChart collide;
  NotationNote auto_first = make_tap(0, 0);
  auto_first.id = -1;
  NotationNote explicit_zero = make_tap(240, 1);
  explicit_zero.id = 0;
  collide.notes = {auto_first, explicit_zero};
  ChartDocument collide_doc;
  collide_doc.load_from_chart(collide);
  expect_unique_safe_ids(collide_doc);
  CHECK_EQ(static_cast<int32_t>(collide_doc.notes().size()), 2);
  CHECK_EQ(collide_doc.next_note_id(), 2);
}

void test_seconds_to_ticks_large_negative_finite() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  normalize_timing_points(timing);
  CHECK_EQ(seconds_to_ticks_at(-1e20f, timing, 0), 1);
  CHECK_EQ(seconds_to_ticks_at(-0.5f, timing, 0), 1);
  CHECK_EQ(seconds_to_ticks_at(0.0f, timing, 0), 1);
}

void test_set_timing_rejects_negative_tick_atomically() {
  ChartDocument doc;
  CHECK_EQ(doc.add_note(make_tap(0, 0)), 0);
  doc.mark_saved();
  const uint64_t gen = doc.content_generation();
  MusicTiming bad = doc.timing();
  bad.points.push_back(TimingPoint{-120, 140.0, 4, 4, true, false});
  CHECK(!doc.set_timing(bad));
  CHECK_EQ(doc.content_generation(), gen);
  CHECK(!doc.is_dirty());
  CHECK_EQ(doc.timing().points.size(), 1u);
  CHECK_EQ(doc.timing().points.front().tick, 0);
}

void test_normalize_clamps_negative_ticks_before_merge() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{-240, 180.0, 3, 4, true, true},
                   TimingPoint{480, 90.0, 4, 4, true, false}};
  normalize_timing_points(timing);
  CHECK(!timing.points.empty());
  CHECK_EQ(timing.points.front().tick, 0);
  for (const auto& p : timing.points) {
    CHECK(p.tick >= 0);
  }
  CHECK(std::fabs(timing.points.front().bpm - 180.0) < 1e-9);
  CHECK_EQ(timing.ticks_per_quarter, 480);
}

void test_load_rejects_negative_timing_tick() {
  CHECK_EQ(static_cast<int>(load_chart_error(
               "neg_timing_tick.wdschart",
               "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT -1 120 4 4 3\nNOTES 0\n"
               "CONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::ParseError));
}

void test_legacy_and_missing_tpq_remain_compatible() {
  NotationChart missing;
  CHECK_EQ(static_cast<int>(load_chart_error(
               "missing_tpq_v2.wdschart",
               "WDSCHART 2\nBPM 120\nNOTES 0\nCONCURRENT 0\nEND\n")),
           static_cast<int>(SerializeError::Ok));
  const fs::path path = temp_chart_path("missing_tpq_v2_load.wdschart");
  {
    std::ofstream out(path);
    out << "WDSCHART 2\nBPM 120\nNOTES 0\nCONCURRENT 0\nEND\n";
  }
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(path.string(), missing).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(missing.timing.ticks_per_quarter, 480);

  const fs::path v1 = fixture_path("legacy_v1.wdschart");
  CHECK(fs::exists(v1));
  NotationChart v1_chart;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(v1.string(), v1_chart).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(v1_chart.timing.ticks_per_quarter, 480);
}

void test_tick_ms_hits_saturation_gates() {
  MusicTiming tiny;
  tiny.bpm = 1e-300;
  tiny.ticks_per_quarter = 480;
  tiny.offset_ms = 0;
  tiny.points = {TimingPoint{0, 1e-300, 4, 4, true, true}};
  normalize_timing_points(tiny);
  CHECK_EQ(tick_to_milliseconds(1, tiny), std::numeric_limits<int64_t>::max());

  MusicTiming pref;
  pref.bpm = 120.0;
  pref.ticks_per_quarter = 480;
  pref.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  pref.prefix_ms = {std::numeric_limits<double>::infinity()};
  CHECK_EQ(tick_to_milliseconds(0, pref), std::numeric_limits<int64_t>::max());
  pref.prefix_ms = {-std::numeric_limits<double>::infinity()};
  CHECK_EQ(tick_to_milliseconds(0, pref), std::numeric_limits<int64_t>::min());
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

void test_snapshot_sub_ms_timeline_us_survives_rebuild_and_patch() {
  ChartEditorEngine engine;
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  engine.document().set_timing(timing);
  engine.set_preview_lead_in_visible_ms(2000);

  for (int i = 0; i < 8; ++i) {
    engine.add_note(make_tap(i * 480, i % 6));
  }

  const int64_t lead_ms = engine.preview_lead_in_visible_ms();
  const int64_t positions_us[] = {300, 600, 9900};

  auto apply_us = [&](int64_t us) {
    wds::common::TimelineSnapshot snap;
    snap.position = wds::common::Microseconds{us};
    snap.state = wds::common::PlaybackState::Paused;
    engine.apply_timeline(snap);
  };

  auto expect_mapped_clock = [&](int64_t input_us) {
    const int64_t visual_us = EditLeadIn::preview_chart_us(input_us, lead_ms);
    CHECK_EQ(engine.timeline_us(), input_us);
    CHECK_EQ(engine.snapshot().timeline_us, visual_us);
    CHECK_EQ(engine.timeline_ms(), input_us / 1000);
    CHECK_EQ(engine.snapshot().timeline_ms, visual_us / 1000);
    CHECK_NE(engine.snapshot().timeline_us, engine.snapshot().timeline_ms * 1000);
  };

  // Force FullRebuild at each high-refresh / sub-ms transport position.
  for (int64_t us : positions_us) {
    engine.seek(20000);
    apply_us(us);
    CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
             static_cast<int>(SnapshotUpdateStrategy::FullRebuild));
    expect_mapped_clock(us);
  }

  // Consecutive applies: first rebuild, then IncrementalPatch must keep µs.
  engine.seek(20000);
  apply_us(300);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::FullRebuild));
  expect_mapped_clock(300);

  apply_us(600);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::IncrementalPatch));
  expect_mapped_clock(600);

  apply_us(9900);
  CHECK_EQ(static_cast<int>(engine.snapshot().last_update_strategy),
           static_cast<int>(SnapshotUpdateStrategy::IncrementalPatch));
  expect_mapped_clock(9900);
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
  split.scratch_length = 10392;  // z=180: tip grows from far
  engine.add_note(split);

  const int64_t start_ms = tick_to_milliseconds(4800, timing);
  // Mid-appear: t=0.5 of the 0.75s window (start−375ms). Screen coverage is
  // the cubic local-Y tip after LaneGroup tilt + FOV 50 — not 69.12/8.
  engine.seek(start_ms - 375);
  engine.rebuild_snapshot();
  CHECK_EQ(static_cast<int32_t>(engine.snapshot().split_lanes.size()), 1);
  CHECK_EQ(engine.snapshot().split_lanes[0].split_anim_phase, 0);
  CHECK(engine.snapshot().split_lanes[0].stage_cover_alpha > 0.1f);
  CHECK(engine.snapshot().split_lanes[0].stage_cover_alpha < 0.9f);
  CHECK(engine.snapshot().split_lanes[0].split_percent_end > 0.2f);
  CHECK(engine.snapshot().split_lanes[0].split_percent_end < 0.6f);

  engine.seek(start_ms);
  engine.rebuild_snapshot();
  CHECK_EQ(engine.snapshot().split_lanes[0].split_anim_phase, 1);
  CHECK_EQ(engine.snapshot().split_lanes[0].stage_cover_alpha, 0.0f);
}

void test_split_fadein_direction_from_linehight_z180() {
  // Official SplitEffects/{id} LineHight.localRotation (client 1.96.0):
  // identity → grow from judge (percent [1-scale, 1]);
  // z=180    → grow from tip   (percent [0, scale]).
  // Direction is the prefab, not gimmickType and not scratch_length%2.
  CHECK(!split_fade_grows_from_tip(0));
  CHECK(!split_fade_grows_from_tip(10390));  // even + identity: falsifies id%2
  CHECK(!split_fade_grows_from_tip(10391));
  CHECK(!split_fade_grows_from_tip(11611));
  CHECK(!split_fade_grows_from_tip(11613));
  CHECK(!split_fade_grows_from_tip(11615));
  CHECK(!split_fade_grows_from_tip(11617));
  for (int32_t id : {10392, 10393, 10518, 10631, 11331, 11511, 11591, 11612, 11614, 11616,
                     11700, 11792, 11805}) {
    CHECK(split_fade_grows_from_tip(id));
  }

  PreviewConfig cfg;
  cfg.split_line_animation_start_sec = 1.0f;
  SplitLaneSimulator sim(cfg);

  MusicTiming timing;
  timing.bpm = 60.0;
  timing.ticks_per_quarter = 480;

  auto make_split = [](int32_t color, GimmickType gimmick) {
    NotationNote note;
    note.start_tick = 4800;
    note.end_tick = 9600;
    note.gimmick_type = gimmick;
    note.scratch_length = color;
    return note;
  };

  const int64_t start_ms = tick_to_milliseconds(4800, timing);
  const int64_t mid = start_ms - 500;  // t=0.5 → tilt+FOV projected tips
  const float kTip = official_split_fade_in_from_tip_end(0.5f);
  const float kId = official_split_fade_in_from_judge_start(0.5f);

  PreviewSplitLaneInstance bottom;
  sim.fill_instance(bottom, make_split(10390, GimmickType::Split4), timing, mid);
  CHECK(std::fabs(bottom.split_percent_start - kId) < 0.02f);
  CHECK(std::fabs(bottom.split_percent_end - 1.0f) < 0.02f);
  CHECK(bottom.split_percent_start < 0.10f);

  PreviewSplitLaneInstance tip;
  sim.fill_instance(tip, make_split(10392, GimmickType::Split4), timing, mid);
  CHECK(std::fabs(tip.split_percent_start - 0.0f) < 0.02f);
  CHECK(std::fabs(tip.split_percent_end - kTip) < 0.02f);

  // 4.txt: 11611/13/15/17 grow from judge; 11612/14/16 grow from tip.
  PreviewSplitLaneInstance id_11611;
  sim.fill_instance(id_11611, make_split(11611, GimmickType::Split6), timing, mid);
  CHECK(id_11611.split_percent_start < 0.10f);
  CHECK(std::fabs(id_11611.split_percent_end - 1.0f) < 0.02f);

  PreviewSplitLaneInstance id_11612;
  sim.fill_instance(id_11612, make_split(11612, GimmickType::Split6), timing, mid);
  CHECK(id_11612.split_percent_start < 0.02f);
  CHECK(id_11612.split_percent_end < 0.85f);

  // Same prefab ID: 16 vs 36 only changes particles, not fade clip.
  PreviewSplitLaneInstance both_ends;
  PreviewSplitLaneInstance full;
  sim.fill_instance(both_ends, make_split(11613, GimmickType::Split6), timing, mid);
  sim.fill_instance(full, make_split(11613, GimmickType::FullSplit6), timing, mid);
  CHECK(std::fabs(both_ends.split_percent_start - full.split_percent_start) < 0.001f);
  CHECK(std::fabs(both_ends.split_percent_end - full.split_percent_end) < 0.001f);
  CHECK(both_ends.split_percent_start < 0.10f);

  PreviewSplitLaneInstance tip_both;
  PreviewSplitLaneInstance tip_full;
  sim.fill_instance(tip_both, make_split(11614, GimmickType::Split6), timing, mid);
  sim.fill_instance(tip_full, make_split(11614, GimmickType::FullSplit6), timing, mid);
  CHECK(std::fabs(tip_both.split_percent_start - tip_full.split_percent_start) < 0.001f);
  CHECK(tip_both.split_percent_start < 0.02f);
}

void test_official_split_fade_in_cubic_and_zero_length() {
  // Official SplitEffect_fadeIn_anim scale.y cubic (t=0 key):
  // 0.30408168 t^3 − 1.60816336 t^2 + 2.30408168 t. Duration 1.0s, a stays 1.
  // Animator state speed=1, fadeIn→fadeOut TransitionDuration=0.
  CHECK(std::fabs(PreviewConfig{}.split_line_animation_start_sec - 1.0f) < 1e-5f);
  CHECK(std::fabs(official_split_fade_in_scale(0.0f) - 0.0f) < 1e-5f);
  CHECK(std::fabs(official_split_fade_in_scale(0.25f) - 0.480263f) < 0.002f);
  CHECK(std::fabs(official_split_fade_in_scale(0.5f) - 0.788010f) < 0.002f);
  CHECK(std::fabs(official_split_fade_in_scale(1.0f) - 1.0f) < 1e-5f);
  // Not the old ease-out quad (0.75 at t=0.5).
  CHECK(std::fabs(official_split_fade_in_scale(0.5f) - 0.75f) > 0.02f);

  // Official scheduler: fadeIn at start−1s; fadeOut at EndTickCount.
  // MinimumShowingMilliseconds is unused. Zero-length still gets the 1s show
  // window, then hide starts immediately at start==end (no start+1500 pad).
  PreviewConfig cfg;
  SplitLaneSimulator sim(cfg);
  MusicTiming timing;
  timing.bpm = 60.0;
  timing.ticks_per_quarter = 480;

  NotationNote note;
  note.start_tick = 4800;
  note.end_tick = 4800;
  note.gimmick_type = GimmickType::Split3;
  note.scratch_length = 10390;

  const int64_t start_ms = tick_to_milliseconds(4800, timing);
  CHECK(sim.is_split_active(note, timing, start_ms - 1000));
  CHECK(!sim.is_split_active(note, timing, start_ms - 1001));

  PreviewSplitLaneInstance mid_in;
  sim.fill_instance(mid_in, note, timing, start_ms - 500);
  CHECK_EQ(mid_in.split_anim_phase, 0);
  CHECK(std::fabs(mid_in.split_line_alpha - 1.0f) < 0.02f);
  CHECK(std::fabs(mid_in.split_percent_start - official_split_fade_in_from_judge_start(0.5f)) <
        0.02f);

  PreviewSplitLaneInstance at_end;
  sim.fill_instance(at_end, note, timing, start_ms);
  CHECK_EQ(at_end.split_anim_phase, 1);

  PreviewSplitLaneInstance fade_out;
  sim.fill_instance(fade_out, note, timing, start_ms + 75);
  CHECK_EQ(fade_out.split_anim_phase, 2);
  CHECK(std::fabs(fade_out.split_line_alpha - 0.84375f) < 0.02f);
  CHECK(!sim.is_split_active(note, timing, start_ms + 300));
  CHECK(!sim.is_split_active(note, timing, start_ms + 1500));
}

void test_official_playfield_visual_lanes_are_six() {
  // SplitEffect max splitCount=6; default note width = 12/6 = 2.
  // Persistent borders sit on the 7 visual-track edges, not 13 column edges.
  CHECK_EQ(kOfficialVisualLaneCount, 6);
  CHECK_EQ(kOfficialLogicalLanesPerVisual, 2);
  CHECK_EQ(kOfficialVisualBorderEdgeCount, 7);
  CHECK_EQ(kOfficialLaneBorderSpriteWidth, 1115);
  CHECK_EQ(kOfficialLaneBorderSpriteHeight, 640);
  CHECK_EQ(kOfficialJudgeSpritePixelWidth, 1119);
  CHECK_EQ(kOfficialJudgeSpritePixelHeight, 72);
  CHECK_EQ(split_default_note_width(6, 12), 2);
  CHECK_EQ(official_visual_border_edge_index(0), 0);
  CHECK_EQ(official_visual_border_edge_index(1), 2);
  CHECK_EQ(official_visual_border_edge_index(3), 6);
  CHECK_EQ(official_visual_border_edge_index(6), 12);
  CHECK(std::fabs(official_lane_border_alpha(100) - 51.0f / 255.0f) < 1e-5f);
}

void test_official_judge_sprite_pink_peaks_sit_on_track_edges() {
  // img_ingame_judgment_area3 is Simple-draw at native px/PPU. The outer pink
  // stroke peaks (columns 5 and 1113) must land on the 12-lane outer edges.
  // Squashing the quad to BG_Lane 11.11 pulls those peaks inward.
  const float native_w = static_cast<float>(kOfficialJudgeSpritePixelWidth) / kOfficialNoteSpritePpu;
  CHECK(std::fabs(kOfficialJudgeSpriteWidth - native_w) < 1e-5f);

  auto peak_world_x = [](float peak_px) {
    const float w = static_cast<float>(kOfficialJudgeSpritePixelWidth);
    return (peak_px + 0.5f) / w * kOfficialJudgeSpriteWidth - 0.5f * kOfficialJudgeSpriteWidth;
  };
  CHECK(std::fabs(peak_world_x(5.0f) - official_lane_edge_x(0)) < 0.01f);
  CHECK(std::fabs(peak_world_x(1113.0f) - official_lane_edge_x(12)) < 0.01f);
}

void test_official_note_visual_width_subtracts_margin() {
  // TapNoteEntity.SetActive: size.x = notationWidth - NoteMarginWidth (0.15).
  // HoldNoteObject.Set: size.x = notationWidth - 0.15 + HoldNoteLineAdditionalWidth (0.10).
  CHECK(std::fabs(kOfficialNoteMarginWidth - 0.15f) < 1e-6f);
  CHECK(std::fabs(kOfficialHoldNoteLineAdditionalWidth - 0.10f) < 1e-6f);
  CHECK(std::fabs(kOfficialNoteSpritePpu - 100.0f) < 1e-6f);

  const float one = official_note_width(1);
  CHECK(std::fabs(one - 0.915f) < 1e-6f);
  CHECK(std::fabs(official_tap_visual_width(one) - 0.765f) < 1e-6f);
  CHECK(std::fabs(official_hold_line_visual_width(one) - 0.865f) < 1e-6f);

  const float four = official_note_width(4);
  CHECK(std::fabs(official_tap_visual_width(four) - (four - 0.15f)) < 1e-6f);

  const float gap = (kOfficialNoteWidthPerLane + kOfficialLaneBorderWidth) -
                    official_tap_visual_width(one);
  CHECK(std::fabs(gap - 0.16f) < 1e-6f);

  // Unity Sliced corners: border_px / PPU, not dest_h / tex_h.
  CHECK(std::fabs(official_sliced_cap_world(65.0f) - 0.65f) < 1e-6f);
  CHECK(std::fabs(official_sliced_cap_fraction(65.0f, 0.765f) - (0.65f / 0.765f)) < 1e-6f);
  CHECK(official_sliced_cap_fraction(65.0f, 0.765f) > 0.5f);
}

void test_official_concurrent_line_is_full_notation_sliced() {
  // ConcurrentLineNote.prefab + NoteConcurrentLine.asset:
  // Sliced 12×8 @ 100 ppu, m_Border L/R=4 T/B=3, m_Size.y=0.1, local Rx=90°.
  // Spawn sets size.x = GetNoteWidth (no NoteMarginWidth), so the bar is
  // notation-wide and peeks past tap sides (tap = notation − 0.15).
  CHECK_EQ(kOfficialConcurrentLineSpriteWidthPx, 12);
  CHECK_EQ(kOfficialConcurrentLineSpriteHeightPx, 8);
  CHECK(std::fabs(kOfficialConcurrentLineBorderL - 4.0f) < 1e-6f);
  CHECK(std::fabs(kOfficialConcurrentLineBorderR - 4.0f) < 1e-6f);
  CHECK(std::fabs(kOfficialConcurrentLineSpriteHeight - 0.1f) < 1e-6f);
  CHECK(std::fabs(kOfficialConcurrentLineLocalRotationX - 90.0f) < 1e-6f);

  const float one = official_note_width(1);
  const float four = official_note_width(4);
  CHECK(std::fabs(official_concurrent_line_visual_width(one) - one) < 1e-6f);
  CHECK(std::fabs(official_concurrent_line_visual_width(four) - four) < 1e-6f);
  CHECK(official_concurrent_line_visual_width(one) - official_tap_visual_width(one) > 0.14f);

  // End-cap world size stays 4/100; stretching a long line must not elongate the fade.
  CHECK(std::fabs(official_sliced_cap_world(kOfficialConcurrentLineBorderL) - 0.04f) < 1e-6f);
  const float cap = official_sliced_cap_fraction(kOfficialConcurrentLineBorderL, four);
  CHECK(cap < 0.03f);
  CHECK(cap * 2.0f + 0.5f < 1.0f);
}

void test_official_playfield_judge_ndc_and_perspective() {
  CHECK(std::fabs(kOfficialPreviewAspect - 16.0f / 9.0f) < 1e-6f);
  CHECK(kOfficialDefaultScreenWidth == 1280);
  CHECK(kOfficialDefaultScreenHeight == 720);

  const auto ndc = project_judge_xyz(0.0f, 0.0f, 0.0f);
  CHECK(std::fabs(ndc.y + 0.485f) < 0.02f);
  CHECK(official_judge_y_to_percent(0.0f) > 0.70f);
  CHECK(official_judge_y_to_percent(0.0f) < 0.80f);

  // Unity vertical FOV 50: the same 11.11 world plate fits 16:9 and overflows 4:3.
  const float half = 0.5f * kOfficialBgLaneWidth;
  const auto wide = project_judge_xyz(half, 0.0f, 0.0f, kOfficialPreviewAspect);
  const auto four_three = project_judge_xyz(half, 0.0f, 0.0f, 4.0f / 3.0f);
  CHECK(std::fabs(wide.x) < 1.0f);
  CHECK(std::fabs(four_three.x) > 1.0f);
  CHECK(std::fabs(wide.x) < std::fabs(four_three.x));

  const float p0 = official_judge_y_to_percent(0.0f);
  const float p1 = official_judge_y_to_percent(1.0f);
  const float p2 = official_judge_y_to_percent(2.0f);
  const float p10 = official_judge_y_to_percent(10.0f);
  CHECK(p10 < p2);
  CHECK(p2 < p1);
  CHECK(p1 < p0);
  CHECK(std::fabs(p0 - p1) > std::fabs(p1 - p2));
}

void test_official_calculate_position_y_matches_il2cpp() {
  const float y = official_note_local_y(500, 0, 5.0);
  const float t = official_speed_rate(5.0) * 0.5f;
  const float expected = static_cast<float>(0.2 * t * t * t + 10.0 * t);
  CHECK(std::fabs(y - expected) < 1e-4f);
  CHECK(std::fabs(y - 0.5f * 8.0f) > 0.5f);

  const float y5 = official_note_local_y(500, 0, 5.0);
  const float y10 = official_note_local_y(500, 0, 10.0);
  CHECK(y10 > y5);

  NotePositionCalculator calc;
  CHECK(std::fabs(calc.calculate_position_y(500, 0) - y5) < 1e-4f);
  CHECK(std::fabs(calc.move_seconds() - official_move_seconds(5.0)) < 1e-4f);
  CHECK(std::fabs(calc.speed_rate() - 3.0f) < 1e-4f);
  CHECK(std::fabs(official_move_seconds(5.0) - (7.4166667f / 5.0f)) < 1e-4f);
}

void test_official_setting_value_ranges() {
  CHECK(official_note_speed_valid(1.0));
  CHECK(official_note_speed_valid(5.0));
  CHECK(official_note_speed_valid(5.1));
  CHECK(official_note_speed_valid(25.0));
  CHECK(!official_note_speed_valid(0.9));
  CHECK(!official_note_speed_valid(25.1));
  CHECK(!official_note_speed_valid(5.05));
  CHECK(std::fabs(official_clamp_note_speed(5.14) - 5.1) < 1e-9);
  CHECK(std::fabs(official_clamp_note_speed(5.16) - 5.2) < 1e-9);
  CHECK(std::fabs(official_clamp_note_speed(0.5) - 1.0) < 1e-9);
  CHECK(std::fabs(official_clamp_note_speed(30.0) - 25.0) < 1e-9);

  CHECK(official_note_height_level_valid(1));
  CHECK(official_note_height_level_valid(10));
  CHECK(!official_note_height_level_valid(0));
  CHECK(!official_note_height_level_valid(11));
  CHECK_EQ(official_clamp_note_height_level(0), 1);
  CHECK_EQ(official_clamp_note_height_level(11), 10);

  CHECK(official_note_start_offset_valid(0));
  CHECK(official_note_start_offset_valid(35));
  CHECK(official_note_start_offset_valid(100));
  CHECK(!official_note_start_offset_valid(3));
  CHECK(!official_note_start_offset_valid(105));
  CHECK_EQ(official_clamp_note_start_offset(3), 5);
  CHECK_EQ(official_clamp_note_start_offset(2), 0);

  CHECK(official_split_effect_line_opacity_valid(10));
  CHECK(official_split_effect_line_opacity_valid(100));
  CHECK(!official_split_effect_line_opacity_valid(0));
  CHECK(!official_split_effect_line_opacity_valid(15));
  CHECK_EQ(official_clamp_split_effect_line_opacity(0), 10);
  CHECK_EQ(official_clamp_split_effect_line_opacity(14), 10);
  CHECK_EQ(official_clamp_split_effect_line_opacity(16), 20);
}

void test_official_hidden_line_and_note_height_defaults() {
  CHECK(std::fabs(official_note_visible_position_y(0) - 58.0f) < 1e-5f);
  CHECK(std::fabs(official_lane_mask_scale_y(0) - 12.5f) < 1e-4f);
  CHECK(std::fabs(official_note_height_rotation_x(8) + 15.0f) < 1e-5f);
  CHECK(std::fabs(official_note_height_rotation_x(1) - 6.0f) < 1e-5f);
  CHECK(official_preview_note_ndc_height(8) > official_preview_note_ndc_height(1));
  CHECK(official_preview_sound_note_ndc_height() > official_preview_note_ndc_height(8));
  CHECK(std::fabs(official_preview_note_height_px(8, 328.9248f) - 22.7127f) < 0.05f);
  CHECK(std::fabs(official_preview_sound_note_height_px(328.9248f) - 31.8984f) < 0.05f);
  CHECK(std::fabs(official_hidden_line_center_y() -
                  (official_note_visible_position_y(0) + kOfficialStartLineSpriteLocalY)) < 1e-5f);
  CHECK(std::fabs(official_hidden_line_center_y(35) -
                  (official_note_visible_position_y(35) + kOfficialStartLineSpriteLocalY)) < 1e-5f);
  CHECK(std::fabs(official_hidden_line_center_y(0) - 58.4f) < 1e-5f);
  CHECK(std::fabs(official_hidden_line_center_y(35) - 30.3f) < 1e-5f);
  // Offset 0 sits at the far spawn (screen top). Raising NoteStartOffset
  // walks the plate toward the judgeline (larger percent).
  CHECK(official_hidden_line_center_percent(0) < 0.02f);
  CHECK(official_hidden_line_center_percent(35) > official_hidden_line_center_percent(0));
  CHECK(official_hidden_line_center_percent(100) > official_hidden_line_center_percent(35));
  CHECK(official_hidden_line_center_percent(35) > 0.06f);
  CHECK(official_hidden_line_center_percent(35) < 0.10f);
  // Mask bottom is visibleY, not the StartLine center (+0.4).
  CHECK(std::fabs(official_lane_mask_bottom_y(0) - 58.0f) < 1e-5f);
  CHECK(std::fabs(official_lane_mask_bottom_y(35) - official_note_visible_position_y(35)) <
        1e-5f);
  CHECK(official_lane_mask_bottom_percent(0) > official_hidden_line_center_percent(0));
  CHECK(official_lane_mask_bottom_percent(35) > official_hidden_line_center_percent(35));
}

void test_official_split_tip_span_matches_sprite_cap() {
  CHECK(std::fabs(kOfficialSplitLineSpriteTipFrac - (45.0f / 256.0f)) < 1e-6f);
  CHECK(std::fabs(official_split_line_tip_world() - 12.15f) < 1e-3f);
  CHECK(std::fabs(official_split_line_texture_tip_world() - 3.456f) < 1e-3f);
  CHECK(std::fabs(official_split_visible_tip_span(0.0f, 1.0f, false) - (45.0f / 256.0f)) < 1e-5f);
  const float z180 = official_split_visible_tip_span(0.0f, 1.0f, true);
  CHECK(z180 > 0.40f);
  CHECK(z180 < 0.70f);
  CHECK(std::fabs(official_start_line_sprite_height(0) - kOfficialStartLineSpriteHeight) < 1e-5f);
  CHECK(official_start_line_sprite_height(0) > 4.9f);
  CHECK(official_start_line_sprite_height(90) < 0.75f);
}

void test_official_split_fade_in_visible_hits_screen_before_clip_end() {
  // Official fadeIn is cubic localScale.y projected through LaneGroup Rx=60°
  // + perspective FOV 50. z=180 starts at camera top (percent 0) and
  // accelerates toward the bottom; identity starts below the lens and the
  // remaining far sliver creeps in. Not sirius_ease, not 69/8, not 2.5/2.56.
  CHECK(official_split_fade_in_from_tip_end(0.09f) < 0.05f);
  CHECK(official_split_fade_in_from_tip_end(0.50f) > 0.28f);
  CHECK(official_split_fade_in_from_tip_end(0.50f) < 0.45f);
  CHECK(official_split_fade_in_from_tip_end(0.83f) >= 0.99f);
  CHECK(std::fabs(official_split_fade_in_from_tip_end(1.0f) - 1.0f) < 1e-5f);
  const float first =
      official_split_fade_in_from_tip_end(0.2f) - official_split_fade_in_from_tip_end(0.0f);
  const float last =
      official_split_fade_in_from_tip_end(0.7f) - official_split_fade_in_from_tip_end(0.5f);
  CHECK(last > first);

  CHECK(official_split_fade_in_from_judge_start(0.09f) > 0.25f);
  CHECK(official_split_fade_in_from_judge_start(0.09f) < 0.50f);
  CHECK(official_split_fade_in_from_judge_start(0.50f) < 0.08f);
  CHECK(official_split_fade_in_from_judge_start(0.09f) < 0.70f);

  PreviewConfig cfg;
  SplitLaneSimulator sim(cfg);
  MusicTiming timing;
  timing.bpm = 60.0;
  timing.ticks_per_quarter = 480;

  auto make_split = [](int32_t color) {
    NotationNote note;
    note.start_tick = 4800;
    note.end_tick = 9600;
    note.gimmick_type = GimmickType::Split3;
    note.scratch_length = color;
    return note;
  };

  const int64_t start_ms = tick_to_milliseconds(4800, timing);
  const auto identity = make_split(10390);
  const auto tip = make_split(10518);

  PreviewSplitLaneInstance early_id;
  PreviewSplitLaneInstance early_tip;
  sim.fill_instance(early_id, identity, timing, start_ms - 910);  // t=0.09
  sim.fill_instance(early_tip, tip, timing, start_ms - 910);
  CHECK(early_id.split_percent_start > 0.25f);
  CHECK(early_id.split_percent_start < 0.50f);
  CHECK(early_tip.split_percent_end < 0.05f);
  CHECK(std::fabs(early_id.split_percent_start - (1.0f - early_tip.split_percent_end)) > 0.10f);

  PreviewSplitLaneInstance mid_id;
  PreviewSplitLaneInstance mid_tip;
  sim.fill_instance(mid_id, identity, timing, start_ms - 500);
  sim.fill_instance(mid_tip, tip, timing, start_ms - 500);
  CHECK(std::fabs(mid_id.split_percent_start - official_split_fade_in_from_judge_start(0.5f)) <
        0.01f);
  CHECK(std::fabs(mid_tip.split_percent_end - official_split_fade_in_from_tip_end(0.5f)) < 0.01f);
  CHECK(mid_tip.split_percent_end > mid_id.split_percent_start);

  PreviewSplitLaneInstance late_id;
  PreviewSplitLaneInstance late_tip;
  sim.fill_instance(late_id, identity, timing, start_ms - 170);  // t=0.83
  sim.fill_instance(late_tip, tip, timing, start_ms - 170);
  CHECK(late_id.split_percent_start < 0.02f);
  CHECK(late_tip.split_percent_end > 0.98f);
}

void test_official_split_fade_in_scale_is_playfield_coverage() {
  // Asset cubic is still the Unity localScale.y curve. Preview percent uses
  // official_split_fade_in_from_tip_end (tilt+FOV), not this cubic alone.
  CHECK(official_split_fade_in_scale(0.09f) < 0.25f);
  CHECK(std::fabs(official_split_fade_in_scale(0.5f) - 0.788010f) < 0.002f);

  PreviewConfig cfg;
  SplitLaneSimulator sim(cfg);
  MusicTiming timing;
  timing.bpm = 60.0;
  timing.ticks_per_quarter = 480;

  NotationNote tip;
  tip.start_tick = 4800;
  tip.end_tick = 9600;
  tip.gimmick_type = GimmickType::Split3;
  tip.scratch_length = 10518;

  const int64_t start_ms = tick_to_milliseconds(4800, timing);
  PreviewSplitLaneInstance tip_mid;
  sim.fill_instance(tip_mid, tip, timing, start_ms - 500);  // t=0.5
  CHECK_EQ(tip_mid.split_anim_phase, 0);
  CHECK(tip_mid.split_percent_start < 0.02f);
  CHECK(std::fabs(tip_mid.split_percent_end - official_split_fade_in_visible(0.5f)) < 0.02f);
}

void test_official_split_fade_out_is_300ms_smoothstep_from_end() {
  // Official SplitEffect_fadeOut_anim (1.96.0 animators.bundle): m_Color.a keys
  // at t=0 (1) and t=0.3 (0). Cubic is 1 - smoothstep(u), u=t/0.3.
  // Hide starts at EndMilliseconds — not start+1500.
  CHECK(std::fabs(PreviewConfig{}.split_line_animation_end_sec - 0.3f) < 1e-5f);
  CHECK(std::fabs(official_split_fade_out_alpha(0.0f) - 1.0f) < 1e-5f);
  CHECK(std::fabs(official_split_fade_out_alpha(0.25f) - 0.84375f) < 1e-5f);
  CHECK(std::fabs(official_split_fade_out_alpha(0.5f) - 0.5f) < 1e-5f);
  CHECK(std::fabs(official_split_fade_out_alpha(1.0f) - 0.0f) < 1e-5f);

  PreviewConfig cfg;
  SplitLaneSimulator sim(cfg);

  MusicTiming timing;
  timing.bpm = 60.0;
  timing.ticks_per_quarter = 480;

  NotationNote note;
  note.start_tick = 4800;  // 10s
  note.end_tick = 5280;    // 11s — 1000ms span, below the old 1500ms pad
  note.gimmick_type = GimmickType::Split3;
  note.scratch_length = 10390;

  const int64_t start_ms = tick_to_milliseconds(4800, timing);
  const int64_t end_ms = tick_to_milliseconds(5280, timing);
  CHECK_EQ(end_ms - start_ms, 1000);

  PreviewSplitLaneInstance at_end;
  sim.fill_instance(at_end, note, timing, end_ms);
  CHECK_EQ(at_end.split_anim_phase, 1);
  CHECK(std::fabs(at_end.split_line_alpha - 1.0f) < 0.02f);

  PreviewSplitLaneInstance quarter;
  sim.fill_instance(quarter, note, timing, end_ms + 75);
  CHECK_EQ(quarter.split_anim_phase, 2);
  CHECK(std::fabs(quarter.split_line_alpha - 0.84375f) < 0.02f);

  PreviewSplitLaneInstance mid;
  sim.fill_instance(mid, note, timing, end_ms + 150);
  CHECK_EQ(mid.split_anim_phase, 2);
  CHECK(std::fabs(mid.split_line_alpha - 0.5f) < 0.02f);

  CHECK(sim.is_split_active(note, timing, end_ms + 299));
  CHECK(!sim.is_split_active(note, timing, end_ms + 300));
  CHECK(!sim.is_split_active(note, timing, start_ms + 1500));
}

void test_split_color_slot_mirrors_linehight_z180() {
  // 4.txt uses split_count=6 (7 lines). Z=180 mirrors official Line index onto world X.
  CHECK_EQ(split_color_slot(11611, 6, 0), 0);
  CHECK_EQ(split_color_slot(11617, 6, 6), 6);
  CHECK_EQ(split_color_slot(11613, 6, 2), 2);

  CHECK_EQ(split_color_slot(11612, 6, 1), 5);  // world left-of-center ← official slot 5 (blue)
  CHECK_EQ(split_color_slot(11616, 6, 5), 1);  // world right-of-center ← official slot 1 (orange)
  CHECK_EQ(split_color_slot(11614, 6, 3), 3);  // center is fixed

  CHECK_EQ(split_color_slot(10390, 4, 0), 0);  // identity, even id
  CHECK_EQ(split_color_slot(10392, 4, 0), 4);
  CHECK_EQ(split_color_slot(10392, 4, 4), 0);

  // 10518 is tip-grow; official controller order is used, then mirrored.
  CHECK_EQ(split_color_slot(10518, 4, 0), 4);
  CHECK(split_fade_grows_from_tip(10518));
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
  int authored = 0;
  int eighths = 0;
  for (const auto& n : notes) {
    if (n.note_type == NoteType::HoldEighth) ++eighths;
    else ++authored;
  }
  CHECK_EQ(authored, 12);
  CHECK_EQ(eighths, 1);

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
  int eighths = 0;
  for (const auto& n : chart.notes) {
    if (n.note_type == NoteType::HoldEighth) ++eighths;
  }
  CHECK_EQ(eighths, 1);
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

void test_selection_drag_snaps_only_anchor_tick() {
  EditGridConfig grid;
  CHECK_EQ(subdivision_tick_step(grid), 120);
  const int32_t anchor = 100;
  const int32_t follower = 250;
  const int32_t pointer_delta = 120;
  const int32_t delta = selection_drag_tick_delta(anchor, pointer_delta, grid);
  const int32_t anchor_new = anchor + delta;
  const int32_t follower_new = follower + delta;
  CHECK_EQ(anchor_new, snap_tick(static_cast<float>(anchor), grid) + pointer_delta);
  CHECK_EQ(follower_new - anchor_new, follower - anchor);
  // Snapping every note independently would change the follower's offset.
  const int32_t snapped_both_offset =
      (snap_tick(static_cast<float>(follower), grid) + pointer_delta) -
      (snap_tick(static_cast<float>(anchor), grid) + pointer_delta);
  CHECK_NE(snapped_both_offset, follower - anchor);
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
  CHECK_EQ(from_scratch.scratch_length, 4);
  CHECK_EQ(static_cast<int>(from_scratch.gimmick_type), static_cast<int>(GimmickType::None));
  const NotationNote back_to_tap = convert_note_type(from_scratch, NoteType::Normal, 480);
  CHECK_EQ(static_cast<int>(back_to_tap.note_type), static_cast<int>(NoteType::Normal));
  CHECK_EQ(back_to_tap.scratch_length, 0);

  // Flick direction is not a hold end-span: Flick → Hold / ScratchHold is
  // equal-width bidirectional (scratch_length 0). Tail arrows are computed later.
  NotationNote flick_left = make_tap(240, 2);
  flick_left.note_type = NoteType::Flick;
  flick_left.scratch_length = -1;
  const NotationNote flick_to_hold = convert_note_type(flick_left, NoteType::Hold, 480);
  CHECK_EQ(static_cast<int>(flick_to_hold.note_type), static_cast<int>(NoteType::Hold));
  CHECK_EQ(flick_to_hold.end_tick, 720);
  CHECK_EQ(flick_to_hold.scratch_length, 0);
  NotationNote flick_right = flick_left;
  flick_right.scratch_length = 1;
  const NotationNote flick_to_scratch = convert_note_type(flick_right, NoteType::ScratchHold, 480);
  CHECK_EQ(static_cast<int>(flick_to_scratch.note_type), static_cast<int>(NoteType::ScratchHold));
  CHECK_EQ(flick_to_scratch.end_tick, 720);
  CHECK_EQ(flick_to_scratch.scratch_length, 0);
  CHECK_EQ(flick_to_scratch.width, flick_right.width);

  // Nontail bodies keep duration when staying a hold; collapse when leaving.
  NotationNote nontail = make_tap(0, 3);
  nontail.note_type = NoteType::NontailHold;
  nontail.end_tick = 1920;
  const NotationNote nontail_to_hold = convert_note_type(nontail, NoteType::Hold, 480);
  CHECK_EQ(static_cast<int>(nontail_to_hold.note_type), static_cast<int>(NoteType::Hold));
  CHECK_EQ(nontail_to_hold.end_tick, 1920);
  const NotationNote nontail_to_tap = convert_note_type(nontail, NoteType::Normal, 480);
  CHECK_EQ(nontail_to_tap.end_tick, nontail_to_tap.start_tick);
  const NotationNote tap_to_nontail = convert_note_type(make_tap(100, 1), NoteType::NontailHold, 480);
  CHECK_EQ(tap_to_nontail.end_tick, 580);

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

  // Center-mirror bounds include ScratchHold tail cover, not just the body.
  // Body [3,4] + sl=6 → occupied [3,8]; after flip, body [7,8] and tail [3,8].
  NotationNote wide_hold = make_tap(0, 3);
  wide_hold.width = 2;
  wide_hold.end_tick = 480;
  wide_hold.note_type = NoteType::ScratchHold;
  wide_hold.scratch_length = 6;
  std::vector<NotationNote> wide = {wide_hold};
  mirror_notes_about_center(wide);
  CHECK_EQ(wide[0].lane, 7);
  CHECK_EQ(wide[0].scratch_length, -6);
  {
    const auto range = get_scratch_end_lane_range(wide[0]);
    CHECK_EQ(range.first, 3);
    CHECK_EQ(range.second, 8);
  }

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

int count_chart_note_type(const NotationChart& chart, NoteType type) {
  int n = 0;
  for (const auto& note : chart.notes) {
    if (note.note_type == type) ++n;
  }
  return n;
}

int count_doc_note_type(const ChartDocument& doc, NoteType type) {
  int n = 0;
  for (const auto& note : doc.notes()) {
    if (note.note_type == type) ++n;
  }
  return n;
}

// HoldEighth is derived (tpq/2). wdschart never stores it; old files are ignored.
void test_wdschart_omits_and_ignores_eighths() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote hold = make_tap(0, 2);
  hold.id = 0;
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  NotationNote stale;
  stale.id = 1;
  stale.start_tick = 120;  // off the auto grid
  stale.end_tick = 120;
  stale.lane = 2;
  stale.width = 2;
  stale.note_type = NoteType::HoldEighth;
  chart.notes = {hold, stale};

  const fs::path path = temp_chart_path("no_eighth.wdschart");
  CHECK_EQ(static_cast<int>(ChartSerializer::save_to_file(chart, path.string()).error),
           static_cast<int>(SerializeError::Ok));
  {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find(" 900 ") == std::string::npos);
    CHECK(text.find("NOTES 1\n") != std::string::npos);
  }

  const fs::path legacy = temp_chart_path("legacy_eighth.wdschart");
  {
    std::ofstream out(legacy);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 120 4 4 3\nNOTES 3\n"
           "N 0 0 0 80 2 2 0 0\n"
           "N 1 0 960 100 2 2 0 0\n"
           "N 2 120 120 900 2 2 0 0\n"
           "CONCURRENT 0\nEND\n";
  }
  NotationChart loaded;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(legacy.string(), loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_chart_note_type(loaded, NoteType::HoldEighth), 0);
  CHECK_EQ(count_chart_note_type(loaded, NoteType::Hold), 1);
  CHECK_EQ(static_cast<int>(loaded.notes.size()), 2);

  ChartEditorEngine engine;
  CHECK_EQ(static_cast<int>(engine.load_from_file(legacy.string()).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_doc_note_type(engine.document(), NoteType::HoldEighth), 3);
  for (const auto& n : engine.document().notes()) {
    if (n.note_type == NoteType::HoldEighth) {
      CHECK(n.start_tick != 120);
    }
  }
}

void test_official_keeps_file_eighth_and_export_generates() {
  const std::string csv =
      "0.0,2.0,100,1,2,0,0\n"
      "0.25,-1.0,900,1,2,0,0\n";  // 0.25s is off the tpq/2 grid from 0
  NotationChart parsed;
  OfficialChartLoadOptions load_opt;
  load_opt.convert_lane_to_zero_based = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::parse_chart(csv, parsed, load_opt).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_chart_note_type(parsed, NoteType::HoldEighth), 1);
  CHECK_EQ(count_chart_note_type(parsed, NoteType::Hold), 1);

  ChartEditorEngine engine;
  engine.load_chart(parsed, ChartEditMode::OfficialPreviewOnly);
  CHECK_EQ(count_doc_note_type(engine.document(), NoteType::HoldEighth), 1);
  for (const auto& n : engine.document().notes()) {
    if (n.note_type == NoteType::HoldEighth) {
      CHECK_EQ(n.start_tick, 120);
    }
  }

  NotationChart hold_only;
  hold_only.timing.bpm = 60.0;
  hold_only.timing.ticks_per_quarter = 480;
  NotationNote hold = make_tap(0, 0);
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  hold_only.notes.push_back(hold);
  std::string out;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::serialize_chart(hold_only, out).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(out.find(",900,") != std::string::npos);
  CHECK(out.find("-1.0,900,") != std::string::npos || out.find("-1,900,") != std::string::npos);
  int generated = 0;
  {
    std::string line;
    std::istringstream ss(out);
    while (std::getline(ss, line)) {
      if (line.find(",900,") != std::string::npos) ++generated;
    }
  }
  CHECK_EQ(generated, 3);
}

void test_official_export_sanitizes_eighths_and_sorts_by_time() {
  NotationChart chart;
  chart.timing.bpm = 60.0;
  chart.timing.ticks_per_quarter = 480;

  NotationNote late = make_tap(960, 1);
  late.id = 0;
  late.note_type = NoteType::Normal;

  NotationNote hold = make_tap(0, 0);
  hold.id = 1;
  hold.width = 3;
  hold.end_tick = 480;
  hold.note_type = NoteType::ScratchHold;
  hold.gimmick_type = GimmickType::JumpScratch;
  hold.scratch_length = 0;
  chart.notes = {late, hold};

  std::string out;
  OfficialChartSaveOptions opt;
  opt.convert_lane_to_one_based = true;
  opt.use_gimmick_names = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::serialize_chart(chart, out, opt).error),
           static_cast<int>(SerializeError::Ok));

  CHECK(out.find("JumpScratch") == std::string::npos);
  CHECK(out.find(",110,1,3,0,0") != std::string::npos);
  CHECK(out.find(",900,") != std::string::npos);

  double prev = -1.0;
  bool saw_hold = false;
  bool saw_late = false;
  bool saw_eighth = false;
  std::string line;
  std::istringstream ss(out);
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    const double t = std::stod(line.substr(0, line.find(',')));
    CHECK(t + 1e-9 >= prev);
    prev = t;
    if (line.find(",110,") != std::string::npos) saw_hold = true;
    if (line.find(",10,") != std::string::npos) saw_late = true;
    if (line.find(",900,") != std::string::npos) {
      saw_eighth = true;
      CHECK(line.find("JumpScratch") == std::string::npos);
      CHECK(line.find(",0,0") != std::string::npos);
    }
  }
  CHECK(saw_hold);
  CHECK(saw_late);
  CHECK(saw_eighth);
  CHECK(saw_hold && out.find(",110,") < out.find(",10,"));
}

void test_official_export_jump_scratch_eighths_stay_plain() {
  NotationChart chart;
  chart.timing.bpm = 60.0;
  chart.timing.ticks_per_quarter = 480;
  NotationNote hold = make_tap(0, 0);
  hold.id = 0;
  hold.width = 3;
  hold.end_tick = 960;
  hold.note_type = NoteType::ScratchHold;
  hold.gimmick_type = GimmickType::JumpScratch;
  hold.scratch_length = 2;
  chart.notes.push_back(hold);

  std::string out;
  OfficialChartSaveOptions opt;
  opt.use_gimmick_names = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::serialize_chart(chart, out, opt).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(out.find("JumpScratch,2") != std::string::npos);
  std::string line;
  std::istringstream ss(out);
  while (std::getline(ss, line)) {
    if (line.find(",900,") == std::string::npos) continue;
    CHECK(line.find("JumpScratch") == std::string::npos);
    CHECK(line.find(",0,0") != std::string::npos);
  }
}

void test_sus_ignores_file_eighth_and_export_generates() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  NotationNote hold = make_tap(0, 2);
  hold.id = 0;
  hold.width = 2;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  chart.notes.push_back(hold);

  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(chart, options, text).error),
           static_cast<int>(SerializeError::Ok));
  bool wrote_invisible_eighth = false;
  {
    std::string line;
    std::istringstream ss(text);
    while (std::getline(ss, line)) {
      const auto colon = line.find(':');
      if (line.size() < 6 || line[0] != '#' || colon == std::string::npos) continue;
      const std::string header = line.substr(1, colon - 1);
      if (header.size() < 4 || header[3] != '3') continue;
      std::string data = line.substr(colon + 1);
      while (!data.empty() && (data.front() == ' ' || data.front() == '\t')) data.erase(data.begin());
      for (size_t i = 0; i + 1 < data.size(); i += 2) {
        if (data[i] == '5') wrote_invisible_eighth = true;
      }
    }
  }
  CHECK(wrote_invisible_eighth);

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(text, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_chart_note_type(loaded.chart, NoteType::HoldEighth), 0);
  CHECK_EQ(count_chart_note_type(loaded.chart, NoteType::Sound), 0);

  const fs::path path = temp_chart_path("eighth.sus");
  CHECK_EQ(static_cast<int>(SusChartFormat::save_file(chart, path.string(), options).error),
           static_cast<int>(SerializeError::Ok));
  ChartEditorEngine engine;
  CHECK_EQ(static_cast<int>(engine.load_sus_from_file(path.string()).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(count_doc_note_type(engine.document(), NoteType::HoldEighth), 3);
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

// Editor Flick stays 0/±width; official CSV writes OneDirection 0/1.
void test_official_csv_flick_scratch_length_encodes_width() {
  CHECK_EQ(encode_flick_scratch_length(-1, 4), -4);
  CHECK_EQ(encode_flick_scratch_length(1, 3), 3);
  CHECK_EQ(encode_flick_scratch_length(0, 6), 0);
  CHECK_EQ(official_flick_scratch_length(-1, 4), -4);
  CHECK_EQ(official_flick_scratch_length(1, 3), 3);
  CHECK_EQ(official_flick_scratch_length(0, 6), 0);
  CHECK_EQ(official_flick_scratch_length(-12, 12), -12);
  CHECK_EQ(official_flick_scratch_length(6, 5), 6);

  NotationChart chart;
  chart.timing.bpm = 60.0;
  chart.timing.ticks_per_quarter = 480;

  auto add_flick = [&](int32_t tick, int32_t lane, int32_t width, int32_t sl) {
    NotationNote n = make_tap(tick, lane);
    n.id = static_cast<int32_t>(chart.notes.size());
    n.note_type = NoteType::Flick;
    n.width = width;
    n.scratch_length = sl;
    chart.notes.push_back(n);
  };
  add_flick(0, 0, 4, -1);    // historic editor direction-only
  add_flick(480, 2, 3, 1);
  add_flick(960, 0, 6, 0);
  add_flick(1440, 1, 5, -5);  // already official
  {
    NotationNote hold = make_tap(1920, 0);
    hold.id = static_cast<int32_t>(chart.notes.size());
    hold.note_type = NoteType::ScratchHold;
    hold.width = 4;
    hold.end_tick = 2400;
    hold.scratch_length = -1;  // genuine 1-lane left cover, not direction-only
    chart.notes.push_back(hold);
  }

  std::string csv;
  OfficialChartSaveOptions save_opt;
  save_opt.convert_lane_to_one_based = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::serialize_chart(chart, csv, save_opt).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(csv.find("OneDirection,0") != std::string::npos);
  CHECK(csv.find("OneDirection,1") != std::string::npos);

  NotationChart parsed;
  OfficialChartLoadOptions load_opt;
  load_opt.convert_lane_to_zero_based = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::parse_chart(csv, parsed, load_opt).error),
           static_cast<int>(SerializeError::Ok));

  const NotationNote* left = nullptr;
  const NotationNote* right = nullptr;
  const NotationNote* both = nullptr;
  const NotationNote* authored = nullptr;
  const NotationNote* hold = nullptr;
  for (const auto& n : parsed.notes) {
    if (n.note_type == NoteType::Flick && n.start_tick == 0) left = &n;
    if (n.note_type == NoteType::Flick && n.start_tick == 480) right = &n;
    if (n.note_type == NoteType::Flick && n.start_tick == 960) both = &n;
    if (n.note_type == NoteType::Flick && n.start_tick == 1440) authored = &n;
    if (n.note_type == NoteType::ScratchHold) hold = &n;
  }
  CHECK(left && right && both && authored && hold);
  if (left) {
    CHECK_EQ(left->width, 4);
    CHECK_EQ(left->scratch_length, -4);
  }
  if (right) {
    CHECK_EQ(right->width, 3);
    CHECK_EQ(right->scratch_length, 3);
  }
  if (both) {
    CHECK_EQ(both->width, 6);
    CHECK_EQ(both->scratch_length, 0);
  }
  if (authored) {
    CHECK_EQ(authored->width, 5);
    CHECK_EQ(authored->scratch_length, -5);
  }
  if (hold) {
    CHECK_EQ(hold->width, 4);
    CHECK_EQ(hold->scratch_length, -1);
  }

  // Old editor None,±1/±width must survive import as left/right (not official both).
  {
    const std::string raw = "1.0,-1.0,50,1,4,0,-1\n";
    NotationChart raw_chart;
    CHECK_EQ(static_cast<int>(OfficialChartFormat::parse_chart(raw, raw_chart, load_opt).error),
             static_cast<int>(SerializeError::Ok));
    CHECK_EQ(static_cast<int>(raw_chart.notes.size()), 1);
    if (!raw_chart.notes.empty()) {
      CHECK_EQ(static_cast<int>(raw_chart.notes[0].note_type), static_cast<int>(NoteType::Flick));
      CHECK_EQ(raw_chart.notes[0].width, 4);
      CHECK_EQ(raw_chart.notes[0].scratch_length, -1);
      CHECK_EQ(static_cast<int>(raw_chart.notes[0].gimmick_type), static_cast<int>(GimmickType::None));
    }
  }

  // Official OneDirection 0/1 → editor-internal None + ±width. None,0 stays both.
  {
    const std::string official =
        "1.0,-1.0,50,2,3,OneDirection,0\n"
        "2.0,-1.0,50,2,3,OneDirection,1\n"
        "3.0,-1.0,50,2,3,0,0\n"
        "4.0,-1.0,50,2,3,JumpScratch,6\n"
        "5.0,-1.0,50,2,3,JumpScratch,-6\n";
    NotationChart official_chart;
    CHECK_EQ(static_cast<int>(OfficialChartFormat::parse_chart(official, official_chart, load_opt).error),
             static_cast<int>(SerializeError::Ok));
    CHECK_EQ(static_cast<int>(official_chart.notes.size()), 5);
    const NotationNote* left = nullptr;
    const NotationNote* right = nullptr;
    const NotationNote* both = nullptr;
    const NotationNote* jump_r = nullptr;
    const NotationNote* jump_l = nullptr;
    for (const auto& n : official_chart.notes) {
      if (n.start_tick == 480) left = &n;
      if (n.start_tick == 960) right = &n;
      if (n.start_tick == 1440) both = &n;
      if (n.start_tick == 1920) jump_r = &n;
      if (n.start_tick == 2400) jump_l = &n;
    }
    CHECK(left && right && both && jump_r && jump_l);
    if (left) {
      CHECK_EQ(static_cast<int>(left->gimmick_type), static_cast<int>(GimmickType::None));
      CHECK_EQ(left->scratch_length, -3);
      CHECK_EQ(left->width, 3);
    }
    if (right) {
      CHECK_EQ(static_cast<int>(right->gimmick_type), static_cast<int>(GimmickType::None));
      CHECK_EQ(right->scratch_length, 3);
    }
    if (both) {
      CHECK_EQ(static_cast<int>(both->gimmick_type), static_cast<int>(GimmickType::None));
      CHECK_EQ(both->scratch_length, 0);
    }
    if (jump_r) {
      CHECK_EQ(static_cast<int>(jump_r->gimmick_type), static_cast<int>(GimmickType::JumpScratch));
      CHECK_EQ(jump_r->scratch_length, 6);
      CHECK_EQ(jump_r->width, 3);
      const auto [lo, hi] = get_scratch_end_lane_range(*jump_r);
      CHECK_EQ(lo, jump_r->lane);
      CHECK_EQ(hi, jump_r->lane + 5);
      CHECK_EQ(official_scratch_arrow_lane_count(*jump_r), 6);
      CHECK_EQ(official_scratch_arrow_count(6, true), 18);
      CHECK(std::fabs(official_jump_scratch_end_offset_x(jump_r->lane, jump_r->width, 6) -
                      1.3875f) < 1e-4f);
    }
    if (jump_l) {
      CHECK_EQ(jump_l->scratch_length, -6);
      const auto [lo, hi] = get_scratch_end_lane_range(*jump_l);
      CHECK_EQ(hi, jump_l->end_lane());
      CHECK_EQ(lo, jump_l->end_lane() - 5);
      CHECK(std::fabs(official_jump_scratch_end_offset_x(jump_l->lane, jump_l->width, -6) +
                      1.3875f) < 1e-4f);
    }

    std::string exported;
    CHECK_EQ(static_cast<int>(
                 OfficialChartFormat::serialize_chart(official_chart, exported, save_opt).error),
             static_cast<int>(SerializeError::Ok));
    CHECK(exported.find("OneDirection,0") != std::string::npos);
    CHECK(exported.find("OneDirection,1") != std::string::npos);
    CHECK(exported.find("JumpScratch,6") != std::string::npos);
    CHECK(exported.find("JumpScratch,-6") != std::string::npos);
  }

  CHECK_EQ(official_scratch_arrow_side_sign(GimmickType::None, -4), 0);
  CHECK_EQ(official_scratch_arrow_side_sign(GimmickType::OneDirection, 0), -1);
  CHECK_EQ(official_scratch_arrow_side_sign(GimmickType::OneDirection, 1), 1);

  // Convert-bar direction intent expands to ±width.
  {
    ChartDocument doc;
    CHECK(doc.set_timing(chart.timing));
    NotationNote flick = make_tap(0, 1);
    flick.id = 1;
    flick.width = 3;
    flick.note_type = NoteType::Normal;
    CHECK(doc.add_note(flick) == 1);
    const auto result =
        convert_notes_in_selection(doc, std::unordered_set<int32_t>{1}, NoteType::Flick, 1);
    CHECK_EQ(result.updates.at(1).scratch_length, 3);
  }
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

void test_resolve_end_lane_span_matches_scratch_and_jump() {
  NotationNote scratch = make_tap(0, 3);
  scratch.width = 2;
  scratch.end_tick = 960;
  scratch.note_type = NoteType::ScratchHold;
  set_scratch_hold_end_lanes(scratch, 3, 7);
  auto span = resolve_end_lane_span(scratch);
  CHECK_EQ(span.first, 3);
  CHECK_EQ(span.second, 5);  // lanes 3..7

  NotationNote jump = make_tap(0, 2);
  jump.width = 2;  // body 2-3
  jump.end_tick = 480;
  jump.note_type = NoteType::Hold;
  jump.gimmick_type = GimmickType::JumpScratch;
  jump.scratch_length = 4;  // right span from lane
  span = resolve_end_lane_span(jump);
  const auto raw = get_jump_scratch_lane_range(jump);
  CHECK_EQ(span.first, std::min(raw.first, raw.second));
  CHECK_EQ(span.second, std::abs(raw.second - raw.first) + 1);

  NotationNote plain = make_tap(0, 5);
  plain.width = 3;
  plain.note_type = NoteType::Hold;
  span = resolve_end_lane_span(plain);
  CHECK_EQ(span.first, 5);
  CHECK_EQ(span.second, 3);

  // Terminal regular hold: OneDirection/None + encoded sl must still use the tail
  // span (last-segment resize does not always rewrite gimmick to JumpScratch).
  NotationNote terminal = make_tap(0, 2);
  terminal.width = 2;  // body 2-3
  terminal.end_tick = 480;
  terminal.note_type = NoteType::Hold;
  terminal.gimmick_type = GimmickType::OneDirection;
  set_scratch_hold_end_lanes(terminal, 2, 6);
  span = resolve_end_lane_span(terminal);
  CHECK_EQ(span.first, 2);
  CHECK_EQ(span.second, 5);  // lanes 2..6
  const auto occ = occupied_lane_span(terminal);
  CHECK_EQ(occ.first, 2);
  CHECK_EQ(occ.second, 5);
}

void test_snap_scratch_chain_next_lane_splits_illegal_zone() {
  NotationNote prev = make_tap(0, 3);
  prev.width = 2;  // lanes 3-4
  prev.end_tick = 480;
  prev.note_type = NoteType::ScratchHold;

  // Next width 4: illegal open interval for left edge is (1, 3); mid = 2.
  // Left of mid → lane 1; right of mid → lane 3. Lane 2 is both-sides.
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 4, 1.0f, 12), 1);
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 4, 1.9f, 12), 1);
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 4, 2.0f, 12), 3);
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 4, 2.5f, 12), 3);
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 4, 3.0f, 12), 3);
  // Already legal stays put.
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 4, 0.4f, 12), 0);
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 4, 4.2f, 12), 4);

  // Wider illegal band: prev 5-6, next width 6 → illegal (1, 5), mid = 3.
  NotationNote wide_prev = make_tap(0, 5);
  wide_prev.width = 2;
  wide_prev.end_tick = 480;
  wide_prev.note_type = NoteType::ScratchHold;
  CHECK_EQ(snap_scratch_chain_next_lane(wide_prev, 6, 2.9f, 12), 1);
  CHECK_EQ(snap_scratch_chain_next_lane(wide_prev, 6, 3.0f, 12), 5);
  CHECK_EQ(snap_scratch_chain_next_lane(wide_prev, 6, 4.0f, 12), 5);

  // Equal/narrower next: no both-side illegal zone.
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 2, 2.0f, 12), 2);
  CHECK_EQ(snap_scratch_chain_next_lane(prev, 2, 3.0f, 12), 3);
}

void test_snap_scratch_hold_segment_lane_avoids_both_side_cover() {
  auto make_body = [](int32_t lane, int32_t width) {
    NotationNote n = make_tap(0, lane);
    n.width = width;
    n.end_tick = 480;
    n.note_type = NoteType::ScratchHold;
    return n;
  };

  // Prev [3-4], body width 4: illegal open interval (1, 3) → lane 2 hangs both sides.
  NotationNote prev = make_body(3, 2);
  NotationNote body = make_body(0, 4);
  CHECK_EQ(snap_scratch_hold_segment_lane(&prev, body, nullptr, 2.0f, 12), 1);
  body.lane = 5;
  CHECK_EQ(snap_scratch_hold_segment_lane(&prev, body, nullptr, 2.0f, 12), 3);
  body.lane = 1;
  CHECK_EQ(snap_scratch_hold_segment_lane(&prev, body, nullptr, 1.0f, 12), 1);
  CHECK_EQ(snap_scratch_hold_segment_lane(&prev, body, nullptr, 3.0f, 12), 3);

  // Next [6-9] width 4, body width 2: nested at lane 7 (both-side hang on THIS).
  NotationNote next = make_body(6, 4);
  body = make_body(2, 2);
  CHECK_EQ(snap_scratch_hold_segment_lane(nullptr, body, &next, 7.0f, 12), 6);
  body.lane = 10;
  CHECK_EQ(snap_scratch_hold_segment_lane(nullptr, body, &next, 7.0f, 12), 8);
  body.lane = 6;
  CHECK_EQ(snap_scratch_hold_segment_lane(nullptr, body, &next, 6.0f, 12), 6);

  // Terminal JS overhang 8-11 on body 8-9: cannot shift right on a 12-lane field.
  NotationNote last = make_body(8, 2);
  set_scratch_hold_end_lanes(last, 8, 11);
  CHECK_EQ(snap_scratch_hold_segment_lane(nullptr, last, nullptr, 9.0f, 12), 8);
  CHECK_EQ(snap_scratch_hold_segment_lane(nullptr, last, nullptr, 7.0f, 12), 7);
}

void test_sync_scratch_chain_joint_exact_union() {
  NotationNote prev = make_tap(0, 2);
  prev.width = 2;
  prev.end_tick = 480;
  prev.note_type = NoteType::ScratchHold;
  prev.scratch_length = 0;

  NotationNote next = make_tap(480, 4);
  next.width = 2;
  next.end_tick = 960;
  next.note_type = NoteType::ScratchHold;

  sync_scratch_chain_joint(prev, next);
  const auto cover = get_scratch_end_lane_range(prev);
  CHECK_EQ(cover.first, 2);
  CHECK_EQ(cover.second, 5);
  CHECK(prev.scratch_length > 0);

  next.lane = 2;
  next.width = 2;
  sync_scratch_chain_joint(prev, next);
  const auto same = get_scratch_end_lane_range(prev);
  CHECK_EQ(same.first, 2);
  CHECK_EQ(same.second, 3);
}

void test_scratch_hold_segment_horizontal_move_keeps_chain() {
  ChartDocument doc;
  NotationNote first = make_tap(0, 2);
  first.id = 1;
  first.width = 2;
  first.end_tick = 480;
  first.note_type = NoteType::ScratchHold;
  NotationNote second = make_tap(480, 4);
  second.id = 2;
  second.width = 2;
  second.end_tick = 960;
  second.note_type = NoteType::ScratchHold;
  sync_scratch_chain_joint(first, second);
  CHECK_EQ(doc.add_note(first), 1);
  CHECK_EQ(doc.add_note(second), 2);
  CHECK(chained_next_scratch_hold(doc, *doc.find_note(1)).has_value());

  NotationNote moved = *doc.find_note(2);
  const auto prev = doc.find_note(1);
  CHECK(prev.has_value());
  const int32_t lane =
      snap_scratch_hold_segment_lane(prev ? &*prev : nullptr, moved, nullptr, 6.0f, 12);
  moved.lane = lane;
  NotationNote joint = *prev;
  sync_scratch_chain_joint(joint, moved);
  CHECK(doc.update_note(1, joint));
  CHECK(doc.update_note(2, moved));
  const auto next = chained_next_scratch_hold(doc, *doc.find_note(1));
  CHECK(next.has_value());
  if (next) CHECK_EQ(next->id, 2);
  const auto cover = get_scratch_end_lane_range(*doc.find_note(1));
  CHECK_EQ(cover.first, 2);
  CHECK_EQ(cover.second, moved.end_lane());
}

void test_scratch_hold_jump_scratch_stays_in_lane_bounds() {
  // Terminal JumpScratch can overhang the body. Lane-bounds checks must use
  // that cover — body-only [lane, width] lets the last cap leave [0, lane_count).

  NotationNote last = make_tap(0, 8);
  last.width = 2;  // body 8-9
  last.end_tick = 480;
  last.note_type = NoteType::ScratchHold;
  set_scratch_hold_end_lanes(last, 8, 11);  // last JumpScratch covers 8-11
  {
    const auto occ = occupied_lane_span(last);
    CHECK_EQ(occ.first, 8);
    CHECK_EQ(occ.second, 4);
  }

  std::vector<NotationNote> notes = {last};
  CHECK(!nudge_notes_lane(notes, 1, 12));  // JS would become 9-12
  CHECK_EQ(notes[0].lane, 8);

  NotationNote left = make_tap(0, 1);
  left.width = 2;  // body 1-2
  left.end_tick = 480;
  left.note_type = NoteType::ScratchHold;
  set_scratch_hold_end_lanes(left, 0, 2);
  notes = {left};
  CHECK(!nudge_notes_lane(notes, -1, 12));  // JS would become -1-1
  CHECK_EQ(notes[0].lane, 1);

  NotationNote mid = make_tap(0, 6);
  mid.width = 2;
  mid.end_tick = 480;
  mid.note_type = NoteType::ScratchHold;
  set_scratch_hold_end_lanes(mid, 6, 9);
  notes = {mid};
  CHECK(nudge_notes_lane(notes, 1, 12));
  CHECK_EQ(notes[0].lane, 7);
  const auto range = get_scratch_end_lane_range(notes[0]);
  CHECK_EQ(range.first, 7);
  CHECK_EQ(range.second, 10);

  NotationNote tap = make_tap(0, 10);
  tap.width = 2;
  notes = {tap};
  CHECK(!nudge_notes_lane(notes, 1, 12));
  CHECK(nudge_notes_lane(notes, 0, 12));
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

void test_scratch_hold_chain_requires_exact_jump_scratch_cover() {
  // Two headless ScratchHold bodies that abut in time are a chain only when
  // prev's JumpScratch span is exactly the union of both bodies — not merely
  // wide enough to contain them.

  auto add_body = [](ChartDocument& doc, int32_t id, int32_t start, int32_t end, int32_t lane,
                     int32_t width, int32_t cover_left, int32_t cover_right) {
    NotationNote body = make_tap(start, lane);
    body.id = id;
    body.width = width;
    body.end_tick = end;
    body.note_type = NoteType::ScratchHold;
    body.gimmick_type = GimmickType::JumpScratch;
    set_scratch_hold_end_lanes(body, cover_left, cover_right);
    CHECK_EQ(doc.add_note(body), id);
  };

  // Wider-than-union terminal flick: prev [2-3] JS [2-6], next same-lane [2-3].
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 480, 2, 2, 2, 6);
    add_body(doc, 2, 480, 960, 2, 2, 2, 3);
    CHECK(!chained_next_scratch_hold(doc, *doc.find_note(1)).has_value());
    CHECK(!chained_prev_scratch_hold(doc, *doc.find_note(2)).has_value());
  }

  // Exact union of a lane-shifted next: prev [2-3] + next [4-5] → JS [2-5].
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 480, 2, 2, 2, 5);
    add_body(doc, 2, 480, 960, 4, 2, 4, 5);
    const auto next = chained_next_scratch_hold(doc, *doc.find_note(1));
    CHECK(next.has_value());
    if (next) CHECK_EQ(next->id, 2);
    const auto prev = chained_prev_scratch_hold(doc, *doc.find_note(2));
    CHECK(prev.has_value());
    if (prev) CHECK_EQ(prev->id, 1);
  }

  // Same-lane continuation: JumpScratch equal to both bodies.
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 480, 2, 2, 2, 3);
    add_body(doc, 2, 480, 960, 2, 2, 2, 3);
    const auto next = chained_next_scratch_hold(doc, *doc.find_note(1));
    CHECK(next.has_value());
    if (next) CHECK_EQ(next->id, 2);
  }

  // JS covers next but not the full union (prev [0-2] JS [1-4], next [3-4]).
  // Encoding forces body cover, so use a JS that still misses the next body.
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 480, 0, 3, 0, 2);  // cover == prev body, misses next [4-5]
    add_body(doc, 2, 480, 960, 4, 2, 4, 5);
    CHECK(!chained_next_scratch_hold(doc, *doc.find_note(1)).has_value());
    CHECK(!chained_prev_scratch_hold(doc, *doc.find_note(2)).has_value());
  }
}

void test_hold_chain_family_predicates() {
  CHECK(is_hold_chain_body(NoteType::Hold));
  CHECK(is_hold_chain_body(NoteType::CriticalHold));
  CHECK(is_hold_chain_body(NoteType::ScratchHold));
  CHECK(is_hold_chain_body(NoteType::ScratchCriticalHold));
  CHECK(!is_hold_chain_body(NoteType::NontailHold));
  CHECK(!is_hold_chain_body(NoteType::NontailScratchHold));
  CHECK(!is_hold_chain_body(NoteType::HoldStart));
  CHECK(!is_hold_chain_body(NoteType::Normal));

  CHECK(same_hold_chain_family(NoteType::Hold, NoteType::CriticalHold));
  CHECK(same_hold_chain_family(NoteType::ScratchHold, NoteType::ScratchCriticalHold));
  CHECK(!same_hold_chain_family(NoteType::Hold, NoteType::ScratchHold));
  CHECK(!same_hold_chain_family(NoteType::CriticalHold, NoteType::ScratchCriticalHold));
  CHECK(!same_hold_chain_family(NoteType::Hold, NoteType::NontailHold));

  CHECK_EQ(static_cast<int>(drawn_hold_body_type(false, false)), static_cast<int>(NoteType::Hold));
  CHECK_EQ(static_cast<int>(drawn_hold_body_type(false, true)),
           static_cast<int>(NoteType::CriticalHold));
  CHECK_EQ(static_cast<int>(drawn_hold_body_type(true, false)),
           static_cast<int>(NoteType::ScratchHold));
  CHECK_EQ(static_cast<int>(drawn_hold_body_type(true, true)),
           static_cast<int>(NoteType::ScratchCriticalHold));
}

void test_occupied_lane_span_includes_hold_jump_scratch() {
  NotationNote hold = make_tap(0, 0);
  hold.width = 6;
  hold.end_tick = 480;
  hold.note_type = NoteType::Hold;
  hold.gimmick_type = GimmickType::JumpScratch;
  hold.scratch_length = 7;  // [0, 6]
  const auto occ = occupied_lane_span(hold);
  CHECK_EQ(occ.first, 0);
  CHECK_EQ(occ.second, 7);

  NotationNote isolated = hold;
  isolated.gimmick_type = GimmickType::None;
  isolated.scratch_length = 0;
  const auto body = occupied_lane_span(isolated);
  CHECK_EQ(body.first, 0);
  CHECK_EQ(body.second, 6);
}

void test_apply_hold_chain_gimmick_encodes_regular_joint() {
  NotationNote prev = make_tap(0, 0);
  prev.width = 5;
  prev.end_tick = 480;
  prev.note_type = NoteType::Hold;
  NotationNote next = make_tap(480, 0);
  next.width = 6;
  next.end_tick = 960;
  next.note_type = NoteType::Hold;
  sync_scratch_chain_joint(prev, next);
  CHECK_EQ(static_cast<int>(prev.gimmick_type), static_cast<int>(GimmickType::JumpScratch));
  CHECK_EQ(prev.scratch_length, 6);
  apply_hold_chain_gimmick(prev);
  CHECK_EQ(static_cast<int>(prev.gimmick_type), static_cast<int>(GimmickType::JumpScratch));
  CHECK_EQ(prev.scratch_length, 6);

  NotationNote same_prev = make_tap(0, 2);
  same_prev.width = 3;
  same_prev.end_tick = 480;
  same_prev.note_type = NoteType::Hold;
  NotationNote same_next = make_tap(480, 2);
  same_next.width = 3;
  same_next.end_tick = 960;
  same_next.note_type = NoteType::Hold;
  sync_scratch_chain_joint(same_prev, same_next);
  apply_hold_chain_gimmick(same_prev);
  CHECK_EQ(static_cast<int>(same_prev.gimmick_type), static_cast<int>(GimmickType::OneDirection));
  CHECK_EQ(same_prev.scratch_length, 0);

  NotationNote purple = make_tap(0, 2);
  purple.width = 2;
  purple.end_tick = 480;
  purple.note_type = NoteType::ScratchHold;
  NotationNote purple_next = make_tap(480, 4);
  purple_next.width = 2;
  purple_next.end_tick = 960;
  purple_next.note_type = NoteType::ScratchHold;
  sync_scratch_chain_joint(purple, purple_next);
  const int32_t sl = purple.scratch_length;
  const auto gimmick = purple.gimmick_type;
  apply_hold_chain_gimmick(purple);
  CHECK_EQ(purple.scratch_length, sl);
  CHECK_EQ(static_cast<int>(purple.gimmick_type), static_cast<int>(gimmick));
}

void test_regular_hold_chain_and_cross_family_negative() {
  auto add_hold = [](ChartDocument& doc, int32_t id, NoteType type, int32_t start, int32_t end,
                     int32_t lane, int32_t width, int32_t cover_left, int32_t cover_right,
                     GimmickType gimmick) {
    NotationNote body = make_tap(start, lane);
    body.id = id;
    body.width = width;
    body.end_tick = end;
    body.note_type = type;
    body.gimmick_type = gimmick;
    set_scratch_hold_end_lanes(body, cover_left, cover_right);
    CHECK_EQ(doc.add_note(body), id);
  };

  {
    ChartDocument doc;
    add_hold(doc, 1, NoteType::Hold, 0, 480, 0, 5, 0, 5, GimmickType::JumpScratch);
    add_hold(doc, 2, NoteType::Hold, 480, 960, 0, 6, 0, 6, GimmickType::JumpScratch);
    add_hold(doc, 3, NoteType::Hold, 960, 1440, 0, 7, 0, 7, GimmickType::JumpScratch);
    add_hold(doc, 4, NoteType::Hold, 1440, 1920, 0, 8, 0, 7, GimmickType::OneDirection);
    auto tail = doc.find_note(4);
    CHECK(tail.has_value());
    tail->scratch_length = 0;
    tail->gimmick_type = GimmickType::OneDirection;
    CHECK(doc.update_note(4, *tail));

    const auto n1 = chained_next_scratch_hold(doc, *doc.find_note(1));
    CHECK(n1.has_value());
    if (n1) CHECK_EQ(n1->id, 2);
    const auto n2 = chained_next_scratch_hold(doc, *doc.find_note(2));
    CHECK(n2.has_value());
    if (n2) CHECK_EQ(n2->id, 3);
    const auto n3 = chained_next_scratch_hold(doc, *doc.find_note(3));
    CHECK(n3.has_value());
    if (n3) CHECK_EQ(n3->id, 4);
    CHECK(!chained_next_scratch_hold(doc, *doc.find_note(4)).has_value());
    CHECK(chained_prev_scratch_hold(doc, *doc.find_note(4)).has_value());
  }

  {
    ChartDocument doc;
    add_hold(doc, 1, NoteType::Hold, 0, 480, 0, 4, 0, 3, GimmickType::JumpScratch);
    add_hold(doc, 2, NoteType::ScratchHold, 480, 960, 0, 4, 0, 3, GimmickType::JumpScratch);
    CHECK(!chained_next_scratch_hold(doc, *doc.find_note(1)).has_value());
    CHECK(!chained_prev_scratch_hold(doc, *doc.find_note(2)).has_value());
  }

  {
    ChartDocument doc;
    add_hold(doc, 1, NoteType::Hold, 0, 480, 2, 2, 2, 3, GimmickType::None);
    add_hold(doc, 2, NoteType::CriticalHold, 480, 960, 2, 2, 2, 3, GimmickType::OneDirection);
    auto second = doc.find_note(2);
    second->scratch_length = 0;
    CHECK(doc.update_note(2, *second));
    auto first = doc.find_note(1);
    first->scratch_length = 0;
    first->gimmick_type = GimmickType::OneDirection;
    CHECK(doc.update_note(1, *first));
    const auto next = chained_next_scratch_hold(doc, *doc.find_note(1));
    CHECK(next.has_value());
    if (next) CHECK_EQ(next->id, 2);
  }
}

void test_convert_note_type_preserves_hold_chain_gimmick() {
  NotationNote hold = make_tap(480, 1);
  hold.width = 6;
  hold.end_tick = 960;
  hold.note_type = NoteType::Hold;
  hold.gimmick_type = GimmickType::JumpScratch;
  hold.scratch_length = 7;

  const NotationNote to_purple = convert_note_type(hold, NoteType::ScratchHold, 480);
  CHECK_EQ(static_cast<int>(to_purple.note_type), static_cast<int>(NoteType::ScratchHold));
  CHECK_EQ(static_cast<int>(to_purple.gimmick_type), static_cast<int>(GimmickType::JumpScratch));
  CHECK_EQ(to_purple.scratch_length, 7);

  const NotationNote to_crit = convert_note_type(hold, NoteType::CriticalHold, 480);
  CHECK_EQ(static_cast<int>(to_crit.gimmick_type), static_cast<int>(GimmickType::JumpScratch));
  CHECK_EQ(to_crit.scratch_length, 7);

  const NotationNote to_tap = convert_note_type(hold, NoteType::Normal, 480);
  CHECK_EQ(static_cast<int>(to_tap.gimmick_type), static_cast<int>(GimmickType::None));
  CHECK_EQ(to_tap.scratch_length, 0);

  NotationNote purple = hold;
  purple.note_type = NoteType::ScratchHold;
  const NotationNote to_hold = convert_note_type(purple, NoteType::Hold, 480);
  CHECK_EQ(static_cast<int>(to_hold.gimmick_type), static_cast<int>(GimmickType::JumpScratch));
  CHECK_EQ(to_hold.scratch_length, 7);

  NotationNote one_dir = hold;
  one_dir.gimmick_type = GimmickType::OneDirection;
  one_dir.scratch_length = 0;
  const NotationNote from_od = convert_note_type(one_dir, NoteType::Flick, 480);
  CHECK_EQ(static_cast<int>(from_od.gimmick_type), static_cast<int>(GimmickType::None));
}

void test_official_and_wdschart_roundtrip_hold_chain_fragment() {
  const std::string csv =
      "20.2105,-1.0,81,1,5,0,0\n"
      "20.2105,20.2500,101,1,5,JumpScratch,6\n"
      "20.2500,20.2895,100,1,6,JumpScratch,7\n"
      "20.2895,20.3289,100,1,7,JumpScratch,8\n"
      "20.3289,20.3684,100,1,8,OneDirection,0\n"
      "24.9474,-1.0,81,5,4,0,0\n"
      "24.9474,25.5000,101,5,4,0,0\n"
      "25.2632,-1.0,50,1,12,JumpScratch,-12\n"
      "25.2632,25.5000,110,1,3,0,0\n";

  NotationChart chart;
  OfficialChartLoadOptions load_opt;
  load_opt.convert_lane_to_zero_based = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::parse_chart(csv, chart, load_opt).error),
           static_cast<int>(SerializeError::Ok));

  int js_holds = 0;
  int one_dir = 0;
  int plain_crit = 0;
  int flick_js = 0;
  int scratch_holds = 0;
  for (const auto& n : chart.notes) {
    if (n.note_type == NoteType::Hold && n.gimmick_type == GimmickType::JumpScratch) ++js_holds;
    if (n.note_type == NoteType::Hold && n.gimmick_type == GimmickType::OneDirection) {
      ++one_dir;
      CHECK_EQ(n.scratch_length, 0);
      CHECK_EQ(n.lane, 0);
      CHECK_EQ(n.width, 8);
    }
    if (n.note_type == NoteType::CriticalHold && n.gimmick_type == GimmickType::JumpScratch) {
      CHECK_EQ(n.scratch_length, 6);
      CHECK_EQ(n.lane, 0);
      CHECK_EQ(n.width, 5);
    }
    if (n.note_type == NoteType::CriticalHold && n.gimmick_type == GimmickType::None) {
      ++plain_crit;
      CHECK_EQ(n.scratch_length, 0);
      CHECK_EQ(n.lane, 4);
      CHECK_EQ(n.width, 4);
    }
    if (n.note_type == NoteType::Flick && n.gimmick_type == GimmickType::JumpScratch) {
      ++flick_js;
      CHECK_EQ(n.scratch_length, -12);
      CHECK_EQ(n.lane, 0);
      CHECK_EQ(n.width, 12);
    }
    if (n.note_type == NoteType::ScratchHold) {
      ++scratch_holds;
      CHECK_EQ(static_cast<int>(n.gimmick_type), static_cast<int>(GimmickType::None));
      CHECK_EQ(n.scratch_length, 0);
    }
  }
  CHECK_EQ(js_holds, 2);
  CHECK_EQ(one_dir, 1);
  CHECK_EQ(plain_crit, 1);
  CHECK_EQ(flick_js, 1);
  CHECK_EQ(scratch_holds, 1);

  std::string exported;
  OfficialChartSaveOptions save_opt;
  save_opt.convert_lane_to_one_based = true;
  save_opt.use_gimmick_names = true;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::serialize_chart(chart, exported, save_opt).error),
           static_cast<int>(SerializeError::Ok));

  NotationChart reparsed;
  CHECK_EQ(static_cast<int>(OfficialChartFormat::parse_chart(exported, reparsed, load_opt).error),
           static_cast<int>(SerializeError::Ok));

  auto drop_eighths = [](std::vector<NotationNote> notes) {
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [](const NotationNote& n) {
                                 return n.note_type == NoteType::HoldEighth;
                               }),
                notes.end());
    return notes;
  };
  auto authored_chart = drop_eighths(chart.notes);
  auto authored_reparsed = drop_eighths(reparsed.notes);
  CHECK_EQ(static_cast<int>(authored_reparsed.size()), static_cast<int>(authored_chart.size()));

  auto key = [](const NotationNote& n) {
    return std::tuple{static_cast<int32_t>(n.note_type), n.lane, n.width,
                      static_cast<int32_t>(n.gimmick_type), n.scratch_length, n.start_tick};
  };
  auto a = authored_chart;
  auto b = authored_reparsed;
  std::sort(a.begin(), a.end(), [&](const auto& l, const auto& r) { return key(l) < key(r); });
  std::sort(b.begin(), b.end(), [&](const auto& l, const auto& r) { return key(l) < key(r); });
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK_EQ(static_cast<int>(a[i].note_type), static_cast<int>(b[i].note_type));
    CHECK_EQ(a[i].lane, b[i].lane);
    CHECK_EQ(a[i].width, b[i].width);
    CHECK_EQ(static_cast<int>(a[i].gimmick_type), static_cast<int>(b[i].gimmick_type));
    CHECK_EQ(a[i].scratch_length, b[i].scratch_length);
    CHECK(std::abs(a[i].start_tick - b[i].start_tick) <= 1);
    CHECK(std::abs(a[i].end_tick - b[i].end_tick) <= 1);
  }

  const fs::path path = temp_chart_path("hold_chain_fragment.wdschart");
  CHECK_EQ(static_cast<int>(ChartSerializer::save_to_file(chart, path.string()).error),
           static_cast<int>(SerializeError::Ok));
  NotationChart from_wds;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(path.string(), from_wds).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(from_wds.notes.size()), static_cast<int>(chart.notes.size()));
  auto c = from_wds.notes;
  std::sort(c.begin(), c.end(), [&](const auto& l, const auto& r) { return key(l) < key(r); });
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK_EQ(static_cast<int>(a[i].note_type), static_cast<int>(c[i].note_type));
    CHECK_EQ(a[i].lane, c[i].lane);
    CHECK_EQ(a[i].width, c[i].width);
    CHECK_EQ(static_cast<int>(a[i].gimmick_type), static_cast<int>(c[i].gimmick_type));
    CHECK_EQ(a[i].scratch_length, c[i].scratch_length);
  }

  // Load must not rewrite imported gimmicks when repairing heads / eighths.
  ChartDocument doc;
  CHECK(doc.set_timing(chart.timing));
  CHECK(doc.set_notes(chart.notes));
  repair_legacy_hold_heads(doc);
  recompute_hold_eighths(doc);
  int still_one_dir = 0;
  int still_plain_crit = 0;
  for (const auto& n : doc.notes()) {
    if (n.note_type == NoteType::Hold && n.gimmick_type == GimmickType::OneDirection) {
      ++still_one_dir;
    }
    if (n.note_type == NoteType::CriticalHold && n.gimmick_type == GimmickType::None &&
        n.width == 4) {
      ++still_plain_crit;
    }
  }
  CHECK_EQ(still_one_dir, 1);
  CHECK_EQ(still_plain_crit, 1);
}

void test_generate_scratch_hold_curve_regular_hold_gimmick() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  ScratchHoldCurveRequest req;
  req.start_tick = 0;
  req.end_tick = 480;
  req.start_center = 0.0;
  req.end_center = 4.0;
  req.width = 2;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;
  req.note_type = NoteType::Hold;
  const auto bodies = generate_scratch_hold_curve(req, timing);
  CHECK(bodies.size() >= 2);
  for (const auto& body : bodies) {
    CHECK_EQ(static_cast<int>(body.note_type), static_cast<int>(NoteType::Hold));
  }
  for (size_t i = 0; i + 1 < bodies.size(); ++i) {
    const auto [lo, hi] = get_scratch_end_lane_range(bodies[i]);
    const int32_t union_l = std::min(bodies[i].lane, bodies[i + 1].lane);
    const int32_t union_r = std::max(bodies[i].end_lane(), bodies[i + 1].end_lane());
    CHECK_EQ(lo, union_l);
    CHECK_EQ(hi, union_r);
    if (lo == bodies[i].lane && hi == bodies[i].end_lane()) {
      CHECK_EQ(static_cast<int>(bodies[i].gimmick_type), static_cast<int>(GimmickType::OneDirection));
      CHECK_EQ(bodies[i].scratch_length, 0);
    } else {
      CHECK_EQ(static_cast<int>(bodies[i].gimmick_type), static_cast<int>(GimmickType::JumpScratch));
    }
  }
}

void test_regular_hold_jump_scratch_tail_covers_next_head() {
  ChartDocument doc;
  NotationNote prev = make_tap(0, 0);
  prev.id = 1;
  prev.width = 5;
  prev.end_tick = 480;
  prev.note_type = NoteType::Hold;
  prev.gimmick_type = GimmickType::JumpScratch;
  prev.scratch_length = 6;
  CHECK_EQ(doc.add_note(prev), 1);

  NotationNote next = make_tap(480, 0);
  next.id = 2;
  next.width = 6;
  next.end_tick = 960;
  next.note_type = NoteType::Hold;
  CHECK_EQ(doc.add_note(next), 2);

  CHECK(!make_auto_hold_head(doc, *doc.find_note(2)).has_value());
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
  // Auto-generated partial heads are not recognized as the body's pair.
  CHECK(!paired_hold_head_for(doc, *doc.find_note(2)).has_value());
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
  CHECK(!hold_head_pairs_with_body(*head, *doc.find_note(2)));

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

void test_hold_head_ignores_body_eighth_and_star_overlap() {
  // Mid-span overlap with another hold body + its eighth must not shorten the head.
  ChartDocument doc;
  NotationNote prior = make_tap(0, 2);
  prior.id = 1;
  prior.width = 4;  // lanes 2-5
  prior.end_tick = 960;
  prior.note_type = NoteType::Hold;
  CHECK(doc.add_note(prior) == 1);

  NotationNote eighth = make_tap(480, 2);
  eighth.id = 2;
  eighth.width = 4;
  eighth.note_type = NoteType::HoldEighth;
  CHECK(doc.add_note(eighth) == 2);

  NotationNote star = make_tap(480, 3);
  star.id = 3;
  star.width = 1;
  star.note_type = NoteType::Sound;
  CHECK(doc.add_note(star) == 3);

  NotationNote next = make_tap(480, 4);
  next.id = 4;
  next.width = 4;  // lanes 4-7; overlaps prior body/eighth/star on 4-5
  next.end_tick = 1440;
  next.note_type = NoteType::Hold;
  CHECK(doc.add_note(next) == 4);

  auto head = make_auto_hold_head(doc, *doc.find_note(4));
  CHECK(head.has_value());
  CHECK_EQ(head->lane, 4);
  CHECK_EQ(head->width, 4);

  // Scratch mid-star likewise ignored.
  ChartDocument doc2;
  NotationNote scratch_star = make_tap(0, 2);
  scratch_star.id = 1;
  scratch_star.width = 2;
  scratch_star.note_type = NoteType::ScratchSound;
  CHECK(doc2.add_note(scratch_star) == 1);
  NotationNote hold = make_tap(0, 2);
  hold.id = 2;
  hold.width = 4;
  hold.end_tick = 960;
  hold.note_type = NoteType::ScratchHold;
  CHECK(doc2.add_note(hold) == 2);
  auto head2 = make_auto_hold_head(doc2, *doc2.find_note(2));
  CHECK(head2.has_value());
  CHECK_EQ(head2->lane, 2);
  CHECK_EQ(head2->width, 4);
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

  CHECK(!paired_hold_head_for(doc, *doc.find_note(1)).has_value());
  CHECK_EQ(repair_legacy_hold_heads(doc), 1);
  auto head = paired_hold_head_for(doc, *doc.find_note(1));
  CHECK(head.has_value());
  CHECK(head->note_type == NoteType::ScratchHoldStart);
  CHECK(is_hold_head_note(*head));
  CHECK_EQ(repair_legacy_hold_heads(doc), 0);
}

void test_hold_head_pairs_exact_span_and_family() {
  auto add_body = [](ChartDocument& doc, int32_t id, int32_t tick, int32_t lane, int32_t width,
                     NoteType type) {
    NotationNote body = make_tap(tick, lane);
    body.id = id;
    body.width = width;
    body.end_tick = tick + 480;
    body.note_type = type;
    CHECK(doc.add_note(body) == id);
  };
  auto add_head = [](ChartDocument& doc, int32_t id, int32_t tick, int32_t lane, int32_t width,
                     NoteType type) {
    NotationNote head = make_tap(tick, lane);
    head.id = id;
    head.width = width;
    head.end_tick = tick;
    head.note_type = type;
    CHECK(doc.add_note(head) == id);
  };

  // Overlapping but different width / lane is not a pair.
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 2, 4, NoteType::Hold);
    add_head(doc, 2, 0, 4, 2, NoteType::HoldStart);
    CHECK(!paired_hold_head_for(doc, *doc.find_note(1)).has_value());
    CHECK(!paired_hold_body_for(doc, *doc.find_note(2)).has_value());
    CHECK(!hold_head_pairs_with_body(*doc.find_note(2), *doc.find_note(1)));
  }

  // Same span, wrong family: blue body ignores pink/gold-scratch heads.
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 3, 2, NoteType::Hold);
    add_head(doc, 2, 0, 3, 2, NoteType::ScratchHoldStart);
    CHECK(!paired_hold_head_for(doc, *doc.find_note(1)).has_value());
    add_head(doc, 3, 0, 3, 2, NoteType::ScratchCriticalHoldStart);
    CHECK(!paired_hold_head_for(doc, *doc.find_note(1)).has_value());
  }

  // Same span, wrong family: purple body ignores blue/gold-blue heads.
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 3, 2, NoteType::ScratchHold);
    add_head(doc, 2, 0, 3, 2, NoteType::HoldStart);
    CHECK(!paired_hold_head_for(doc, *doc.find_note(1)).has_value());
    add_head(doc, 3, 0, 3, 2, NoteType::CriticalHoldStart);
    CHECK(!paired_hold_head_for(doc, *doc.find_note(1)).has_value());
  }

  // Blue body accepts blue / gold heads at the exact start span.
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 3, 2, NoteType::Hold);
    add_head(doc, 2, 0, 3, 2, NoteType::HoldStart);
    auto paired = paired_hold_head_for(doc, *doc.find_note(1));
    CHECK(paired.has_value());
    CHECK_EQ(paired->id, 2);
    CHECK_EQ(paired_hold_body_for(doc, *doc.find_note(2))->id, 1);
  }
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 3, 2, NoteType::CriticalHold);
    add_head(doc, 2, 0, 3, 2, NoteType::CriticalHoldStart);
    CHECK(paired_hold_head_for(doc, *doc.find_note(1)).has_value());
  }

  // First purple segment uses the same exact-span rule (not JumpScratch cover).
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 3, 2, NoteType::ScratchHold);
    add_head(doc, 2, 0, 3, 2, NoteType::ScratchHoldStart);
    CHECK(paired_hold_head_for(doc, *doc.find_note(1)).has_value());
    CHECK_EQ(paired_hold_head_for(doc, *doc.find_note(1))->id, 2);
  }
  {
    ChartDocument doc;
    add_body(doc, 1, 0, 3, 2, NoteType::ScratchCriticalHold);
    add_head(doc, 2, 0, 3, 2, NoteType::ScratchCriticalHoldStart);
    CHECK(paired_hold_head_for(doc, *doc.find_note(1)).has_value());
  }
  {
    ChartDocument doc;
    NotationNote body = make_tap(0, 3);
    body.id = 1;
    body.width = 2;
    body.end_tick = 480;
    body.note_type = NoteType::ScratchHold;
    set_scratch_hold_end_lanes(body, 3, 7);
    CHECK(doc.add_note(body) == 1);
    add_head(doc, 2, 0, 3, 5, NoteType::ScratchHoldStart);  // wider than start span
    CHECK(!paired_hold_head_for(doc, *doc.find_note(1)).has_value());
  }
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

void test_star_hold_bind_legacy_uniqueness_and_attachment() {
  NotationChart chart;
  chart.timing.bpm = 120.0;
  chart.timing.ticks_per_quarter = 480;
  chart.timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};

  NotationNote hold_a = make_tap(0, 2);
  hold_a.id = 0;
  hold_a.width = 2;
  hold_a.end_tick = 1920;
  hold_a.note_type = NoteType::Hold;
  NotationNote hold_b = make_tap(480, 2);
  hold_b.id = 1;
  hold_b.width = 2;
  hold_b.end_tick = 2400;
  hold_b.note_type = NoteType::Hold;
  NotationNote star = make_tap(960, 2);
  star.id = 2;
  star.width = 2;
  star.end_tick = 960;
  star.note_type = NoteType::Sound;
  NotationNote scratch_hold = make_tap(0, 5);
  scratch_hold.id = 3;
  scratch_hold.width = 1;
  scratch_hold.end_tick = 1920;
  scratch_hold.note_type = NoteType::ScratchHold;
  NotationNote blue_star_on_purple = make_tap(960, 5);
  blue_star_on_purple.id = 4;
  blue_star_on_purple.width = 1;
  blue_star_on_purple.end_tick = 960;
  blue_star_on_purple.note_type = NoteType::Sound;
  NotationNote mismatched_width = make_tap(1200, 2);
  mismatched_width.id = 5;
  mismatched_width.width = 1;
  mismatched_width.end_tick = 1200;
  mismatched_width.note_type = NoteType::Sound;
  chart.notes = {hold_a, hold_b, star, scratch_hold, blue_star_on_purple, mismatched_width};

  const fs::path path = temp_chart_path("legacy_star_bind.wdschart");
  {
    std::ofstream out(path);
    out << "WDSCHART 4\nBPM 120\nTPQ 480\nTIMING 1\nT 0 120 4 4 3\nNOTES 6\n";
    for (const auto& n : chart.notes) {
      out << "N " << n.id << ' ' << n.start_tick << ' ' << n.end_tick << ' '
          << static_cast<int32_t>(n.note_type) << ' ' << n.lane << ' ' << n.width << " 0 0\n";
    }
    out << "CONCURRENT 0\nEND\n";
  }
  NotationChart loaded;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(path.string(), loaded).error),
           static_cast<int>(SerializeError::Ok));
  const NotationNote* loaded_star = nullptr;
  const NotationNote* loaded_type_mismatch = nullptr;
  const NotationNote* loaded_width_mismatch = nullptr;
  for (const auto& n : loaded.notes) {
    if (n.id == 2) loaded_star = &n;
    if (n.id == 4) loaded_type_mismatch = &n;
    if (n.id == 5) loaded_width_mismatch = &n;
  }
  CHECK(loaded_star != nullptr);
  if (loaded_star != nullptr) CHECK_EQ(loaded_star->parent_hold_id, 0);
  CHECK(loaded_type_mismatch != nullptr);
  if (loaded_type_mismatch != nullptr) CHECK_EQ(loaded_type_mismatch->parent_hold_id, kNoBoundHoldId);
  CHECK(loaded_width_mismatch != nullptr);
  if (loaded_width_mismatch != nullptr) {
    CHECK_EQ(loaded_width_mismatch->parent_hold_id, kNoBoundHoldId);
  }

  ChartDocument doc;
  CHECK(doc.set_notes(loaded.notes));
  auto attached_a = hold_attached_notes_for(doc, *doc.find_note(0));
  int visible_on_a = 0;
  for (const auto& n : attached_a) {
    if (n.note_type == NoteType::Sound || n.note_type == NoteType::ScratchSound) ++visible_on_a;
  }
  CHECK_EQ(visible_on_a, 1);
  auto attached_b = hold_attached_notes_for(doc, *doc.find_note(1));
  int visible_on_b = 0;
  for (const auto& n : attached_b) {
    if (n.note_type == NoteType::Sound || n.note_type == NoteType::ScratchSound) ++visible_on_b;
  }
  CHECK_EQ(visible_on_b, 0);

  CHECK(hold_has_visible_star_at(doc, 0, 960, kNoBoundHoldId));
  CHECK(!hold_has_visible_star_at(doc, 1, 960, kNoBoundHoldId));

  NotationNote dup = *doc.find_note(2);
  dup.id = 9;
  auto notes = doc.notes();
  notes.push_back(dup);
  CHECK(visible_star_tick_conflicts(notes));
  CHECK(!visible_star_tick_conflicts(doc.notes()));

  // v5 roundtrip keeps the inferred bind.
  const fs::path v5_path = temp_chart_path("star_bind_v5.wdschart");
  CHECK_EQ(static_cast<int>(ChartSerializer::save_to_file(loaded, v5_path.string()).error),
           static_cast<int>(SerializeError::Ok));
  NotationChart v5;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(v5_path.string(), v5).error),
           static_cast<int>(SerializeError::Ok));
  bool found_bound = false;
  for (const auto& n : v5.notes) {
    if (n.note_type == NoteType::Sound && n.lane == 2 && n.width == 2) {
      CHECK_EQ(n.parent_hold_id, 0);
      found_bound = true;
    }
  }
  CHECK(found_bound);

  // SUS export hangs the bound star on hold A, not the overlapping hold B.
  SusChartSaveOptions options;
  options.ched_lane_padding = false;
  std::string sus_text;
  CHECK_EQ(static_cast<int>(SusChartFormat::serialize(loaded, options, sus_text).error),
           static_cast<int>(SerializeError::Ok));
  SusChartLoadResult sus_loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus_text, sus_loaded).error),
           static_cast<int>(SerializeError::Ok));
  int32_t hold_a_id = -1;
  int32_t hold_b_id = -1;
  int32_t star_parent = -2;
  for (const auto& n : sus_loaded.chart.notes) {
    if (n.note_type == NoteType::Hold && n.start_tick == 0) hold_a_id = n.id;
    if (n.note_type == NoteType::Hold && n.start_tick == 480) hold_b_id = n.id;
    if (n.note_type == NoteType::Sound) star_parent = n.parent_hold_id;
  }
  CHECK(hold_a_id >= 0);
  CHECK(hold_b_id >= 0);
  CHECK_EQ(star_parent, hold_a_id);
  CHECK(star_parent != hold_b_id);
}

void test_load_v5_rebinds_dangling_and_unbound_star_parents() {
  const fs::path path = temp_chart_path("v5_dangling_star_parent.wdschart");
  {
    std::ofstream out(path);
    out << "WDSCHART 5\nBPM 179\nTPQ 480\nTIMING 1\nT 0 179 4 4 3\nNOTES 4\n"
           "N 681 126240 130320 100 6 6 0 0 -1\n"
           "N 683 126720 126720 30 6 6 0 0 1147\n"
           "N 684 126960 126960 30 6 6 0 0 -1\n"
           "N 10 0 0 10 0 1 0 0 -1\n"
           "CONCURRENT 0\nEND\n";
  }
  NotationChart loaded;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(path.string(), loaded).error),
           static_cast<int>(SerializeError::Ok));
  int stars = 0;
  for (const auto& n : loaded.notes) {
    if (n.note_type != NoteType::Sound) continue;
    ++stars;
    CHECK_EQ(n.parent_hold_id, 681);
  }
  CHECK_EQ(stars, 2);

  ChartDocument doc;
  doc.load_from_chart(loaded);
  const auto saved = doc.normalized_chart();
  const NotationNote* hold = nullptr;
  for (const auto& n : saved.notes) {
    if (n.note_type == NoteType::Hold) hold = &n;
  }
  CHECK(hold != nullptr);
  int saved_stars = 0;
  for (const auto& n : saved.notes) {
    if (n.note_type != NoteType::Sound) continue;
    ++saved_stars;
    if (hold != nullptr) CHECK_EQ(n.parent_hold_id, hold->id);
  }
  CHECK_EQ(saved_stars, 2);

  const fs::path roundtrip = temp_chart_path("v5_rebound_roundtrip.wdschart");
  CHECK_EQ(static_cast<int>(ChartSerializer::save_to_file(saved, roundtrip.string()).error),
           static_cast<int>(SerializeError::Ok));
  NotationChart again;
  CHECK_EQ(static_cast<int>(ChartSerializer::load_from_file(roundtrip.string(), again).error),
           static_cast<int>(SerializeError::Ok));
  const NotationNote* again_hold = nullptr;
  for (const auto& n : again.notes) {
    if (n.note_type == NoteType::Hold) again_hold = &n;
  }
  CHECK(again_hold != nullptr);
  for (const auto& n : again.notes) {
    if (n.note_type == NoteType::Sound && again_hold != nullptr) {
      CHECK_EQ(n.parent_hold_id, again_hold->id);
    }
  }
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

// Partial-width head does not pair; the body still exports as a headless hold.
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
  const NotationNote* hold = nullptr;
  const NotationNote* star = nullptr;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Hold) hold = &n;
    if (n.note_type == NoteType::Sound) star = &n;
  }
  CHECK(hold != nullptr);
  CHECK(star != nullptr);
  if (hold != nullptr && star != nullptr) {
    CHECK_EQ(star->parent_hold_id, hold->id);
  }
}

// Overlapping full-width slides with mids on the same tick (2338_03-style).
// Dense late taps push pre-sort hold ids into the tap range after 0..n-1 rewrite;
// parent_hold_id must be remapped or infer_legacy_star_hold_binds stacks both
// stars on the first matching hold and every drag snaps back.
void test_sus_overlapping_full_width_mids_keep_distinct_parents() {
  const char* sus =
      "#TITLE \"overlap-mids\"\n"
      "#REQUEST \"ticks_per_beat 480\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#000320: 1c\n"
      "#000321: 1c\n"
      "#001320: 3c\n"
      "#001321: 3c\n"
      "#002320: 2c\n"
      "#002321: 2c\n"
      "#00312: 13131313131313131313131313131313\n"
      "#00315: 13131313131313131313131313131313\n";

  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK(!visible_star_tick_conflicts(loaded.chart.notes));

  std::vector<const NotationNote*> stars;
  std::unordered_map<int32_t, const NotationNote*> by_id;
  for (const auto& n : loaded.chart.notes) {
    by_id[n.id] = &n;
    if (n.note_type == NoteType::Sound) stars.push_back(&n);
  }
  CHECK_EQ(static_cast<int>(stars.size()), 2);
  if (stars.size() == 2) {
    CHECK_EQ(stars[0]->start_tick, stars[1]->start_tick);
    CHECK(stars[0]->parent_hold_id != stars[1]->parent_hold_id);
    for (const auto* star : stars) {
      const auto it = by_id.find(star->parent_hold_id);
      CHECK(it != by_id.end());
      if (it == by_id.end()) continue;
      CHECK(is_bindable_hold_body(it->second->note_type));
      CHECK_EQ(it->second->lane, star->lane);
      CHECK_EQ(it->second->width, star->width);
      CHECK(it->second->start_tick < star->start_tick);
      CHECK(star->start_tick < it->second->end_tick);
    }
  }

  ChartEditorEngine engine;
  engine.load_chart(loaded.chart, ChartEditMode::Editable);
  CHECK(!visible_star_tick_conflicts(engine.document().notes()));
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

// Standalone leftover #5 becomes Flick; lane/width come from Air (sus2txt).
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

// Leftover #5 (no slide) → standalone Flick; scratchLength = ±width for type 3/4.
void test_sus_leftover_air_becomes_flick() {
  const char* sus =
      "#TITLE \"orphan\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00050: 14\n";  // Air up, lane 0 width 4 — no #1 pair required
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  CHECK_EQ(static_cast<int>(loaded.chart.notes.size()), 1);
  if (loaded.chart.notes.size() != 1) return;
  CHECK_EQ(static_cast<int>(loaded.chart.notes[0].note_type), static_cast<int>(NoteType::Flick));
  CHECK_EQ(loaded.chart.notes[0].width, 4);
  CHECK_EQ(loaded.chart.notes[0].scratch_length, 0);
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
  if (loaded.chart.notes.size() != 1) return;
  CHECK_EQ(static_cast<int>(loaded.chart.notes[0].note_type), static_cast<int>(NoteType::Flick));
  CHECK_EQ(loaded.chart.notes[0].width, 3);
  CHECK_EQ(loaded.chart.notes[0].scratch_length, 3);  // sus2txt: type 4 → +width
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
  // Mid/end Air type 1 on the exact body span encodes sl=0, which is not JumpScratch.
  CHECK_EQ(jump, 0);
}

// Start #5 on a purple slide (end also has #5) suppresses ScratchHoldStart (addStart=false).
void test_sus_start_air_suppresses_pink_head() {
  const char* sus =
      "#TITLE \"pink\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00030a: 1222\n"
      "#00050: 1212\n";  // Air on body at start AND end; no #1 type3
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int scratch = 0;
  int pink = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::ScratchHold) ++scratch;
    if (n.note_type == NoteType::ScratchHoldStart) ++pink;
  }
  CHECK_EQ(scratch, 1);
  CHECK_EQ(pink, 0);
}

// Left-extended Air sharing the body right edge (different left lane, no #1) binds JumpScratch.
void test_sus_left_jump_air_binds_scratch_hold() {
  const char* sus =
      "#TITLE \"leftjump\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00032a: 1222\n"  // body [2,3] (lane 0 used by Air → no Ched offset)
      "#00050: 0014\n";  // Air [0,3] at end — i-r-1 = 0-3-1 = -4
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  const NotationNote* body = nullptr;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::ScratchHold) body = &n;
  }
  CHECK(body != nullptr);
  if (body == nullptr) return;
  CHECK_EQ(body->lane, 2);
  CHECK_EQ(body->width, 2);
  CHECK_EQ(body->scratch_length, -4);
  CHECK_EQ(static_cast<int>(body->gimmick_type), static_cast<int>(GimmickType::JumpScratch));
}

// Slide A ends and slide B starts at the same tick, sharing an adjacent #5.
void test_sus_jump_scratch_chain_joint_no_pink_head() {
  const char* sus =
      "#TITLE \"chain\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00030a: 1222\n"  // A [0,1] 0→960
      "#00032b: 0012\n"  // B start @960 [2,3]
      "#00132b: 22\n"    // B end @1920
      "#00050: 0014\n"   // Air [0,3] at joint (A's JumpScratch + B addStart=false)
      "#00152: 12\n";    // B end Air on [2,3] — keeps B purple (no shouldUnscratch)
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  const NotationNote* first = nullptr;
  const NotationNote* second = nullptr;
  int pink = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::ScratchHold) {
      if (n.start_tick == 0) first = &n;
      if (n.start_tick == 960) second = &n;
    }
    if (n.note_type == NoteType::ScratchHoldStart && n.start_tick == 960) ++pink;
  }
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  if (first == nullptr || second == nullptr) return;
  CHECK_EQ(first->scratch_length, 4);
  CHECK_EQ(static_cast<int>(first->gimmick_type), static_cast<int>(GimmickType::JumpScratch));
  CHECK_EQ(pink, 0);
  ChartDocument doc;
  doc.set_notes(loaded.chart.notes);
  doc.set_timing(loaded.chart.timing);
  const auto next = chained_next_scratch_hold(doc, *first);
  CHECK(next.has_value());
  if (next) {
    CHECK_EQ(next->id, second->id);
  }
}

// Mid #5 on the exact body span splits without a Slide type-3 diamond.
void test_sus_mid_air_only_splits_jump_scratch() {
  const char* sus =
      "#TITLE \"midair\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00030a: 12002200\n"  // start@0 end@2 of 4 slots
      "#00050: 00120012\n";  // Air at mid and end, exact body span
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
  // Type-1 Air on the exact span is a purple cut, not JumpScratch (sl=0).
  CHECK_EQ(jump, 0);
}

void test_sus_mid_air_right_span_is_jump_scratch() {
  const char* sus =
      "#TITLE \"midair-js\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00030a: 12002200\n"
      "#00050: 00420042\n";  // Air type 4 at mid and end → sl = +width
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int scratch_bodies = 0;
  int jump = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::ScratchHold) {
      ++scratch_bodies;
      if (n.gimmick_type == GimmickType::JumpScratch) {
        ++jump;
        CHECK(n.scratch_length != 0);
      }
    }
  }
  CHECK_EQ(scratch_bodies, 2);
  CHECK(jump >= 1);
}

// Start #5 only → shouldUnscratch back to blue Hold with a head.
void test_sus_start_air_only_unscratches_to_blue_hold() {
  const char* sus =
      "#TITLE \"unscratch\"\n"
      "#BPM01: 120.0\n"
      "#00008: 01\n"
      "#00030a: 1222\n"
      "#00050: 1200\n";  // Air only at start
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int holds = 0;
  int scratches = 0;
  int heads = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Hold) ++holds;
    if (n.note_type == NoteType::ScratchHold) ++scratches;
    if (is_hold_head_note(n)) ++heads;
  }
  CHECK_EQ(holds, 1);
  CHECK_EQ(scratches, 0);
  CHECK_EQ(heads, 1);
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
  // sus2txt splits header/body on ": " — a bare "#TIL01 \"" is eaten as garbage.
  CHECK(text.find("#TIL01: \"") != std::string::npos);
  const auto til_pos = text.find("#TIL01: \"");
  CHECK(til_pos != std::string::npos);
  if (til_pos != std::string::npos) {
    const auto q1 = text.find('"', til_pos);
    const auto q2 = text.find('"', q1 + 1);
    CHECK(q1 != std::string::npos);
    CHECK(q2 != std::string::npos);
    if (q1 != std::string::npos && q2 != std::string::npos) {
      const std::string body = text.substr(q1 + 1, q2 - q1 - 1);
      CHECK(body.find(":") != std::string::npos);
      CHECK(body.find(", ") != std::string::npos);
      CHECK(body.find("-") != std::string::npos);
    }
  }

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

void test_sus_type3_does_not_copy_to_sibling_hold() {
  const char* sus =
      "#TITLE \"sib\"\n"
      "#REQUEST \"ticks_per_beat 480\"\n"
      "#BPM01: 230.0\n"
      "#00008: 01\n"
      "#000320: 1200320022000000\n"
      "#0003c0: 0000320000000000\n";
  SusChartLoadResult loaded;
  CHECK_EQ(static_cast<int>(SusChartFormat::parse(sus, loaded).error),
           static_cast<int>(SerializeError::Ok));
  int sounds = 0;
  int sounds_on_right = 0;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type != NoteType::Sound) continue;
    ++sounds;
    if (n.lane >= 8) ++sounds_on_right;
  }
  CHECK_EQ(sounds, 1);
  CHECK_EQ(sounds_on_right, 0);
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
  {
    const int32_t hold_id = nid();
    chart.notes.push_back(make_body(NoteType::Hold, hold_id, 4320, 5280, 0, 1));
    NotationNote star = make_body(NoteType::Sound, nid(), 4800, 0, 0, 1);
    star.parent_hold_id = hold_id;
    chart.notes.push_back(star);
  }

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
  {
    const int32_t hold_id = nid();
    chart.notes.push_back(make_body(NoteType::Hold, hold_id, 7680, 8640, 2, 1));
    NotationNote star = make_body(NoteType::ScratchSound, nid(), 8160, 0, 2, 1);
    star.parent_hold_id = hold_id;
    chart.notes.push_back(star);
  }

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

void test_timing_first_bpm_is_song_start_without_tick0() {
  MusicTiming raw;
  raw.bpm = 999.0;  // header field must not win over the first authored BPM label
  raw.ticks_per_quarter = 480;
  raw.offset_ms = 0;
  raw.points = {TimingPoint{480, 60.0, 4, 4, true, true}};

  MusicTiming normalized = raw;
  normalize_timing_points(normalized);
  CHECK_EQ(normalized.points.front().tick, 0);
  CHECK(std::fabs(normalized.points.front().bpm - 60.0) < 1e-9);

  CHECK_EQ(tick_to_milliseconds(0, raw), tick_to_milliseconds(0, normalized));
  CHECK_EQ(tick_to_milliseconds(480, raw), tick_to_milliseconds(480, normalized));
  CHECK_EQ(tick_to_milliseconds(960, raw), tick_to_milliseconds(960, normalized));
  CHECK_EQ(milliseconds_to_tick(1000, raw), milliseconds_to_tick(1000, normalized));
}

void test_hold_span_stale_skips_rebuild_without_holds() {
  ChartDocument doc;
  NotationNote tap = make_tap(0, 0);
  tap.id = 1;
  CHECK_EQ(doc.add_note(tap), 1);
  CHECK(!doc.index().hold_span_stale());
  CHECK_EQ(doc.index().max_hold_span_ms(), 0);

  NotationNote tap2 = make_tap(480, 1);
  tap2.id = 2;
  CHECK_EQ(doc.add_note(tap2), 2);
  CHECK(doc.remove_note(2));
  CHECK(!doc.index().hold_span_stale());
  CHECK_EQ(doc.index().max_hold_span_ms(), 0);

  NotationNote body = make_tap(0, 2);
  body.id = 3;
  body.end_tick = 960;
  body.note_type = NoteType::Hold;
  CHECK_EQ(doc.add_note(body), 3);
  CHECK(doc.index().max_hold_span_ms() > 0);
  CHECK(doc.remove_note(3));
  CHECK(!doc.index().hold_span_stale());
  CHECK_EQ(doc.index().max_hold_span_ms(), 0);
}

void test_resolve_convert_target_is_identity() {
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

  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Critical) == NoteType::Critical);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Normal) == NoteType::Normal);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::HoldStart) == NoteType::HoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Hold) == NoteType::Hold);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::ScratchHold) ==
        NoteType::ScratchHold);
  CHECK(resolve_convert_target(doc, *doc.find_note(2), NoteType::Flick) == NoteType::Flick);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Hold) == NoteType::Hold);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::ScratchHold) ==
        NoteType::ScratchHold);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Normal) == NoteType::Normal);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Critical) == NoteType::Critical);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::HoldStart) == NoteType::HoldStart);
  CHECK(resolve_convert_target(doc, *doc.find_note(1), NoteType::Flick) == NoteType::Flick);
}

void test_convert_notes_in_selection_hold_stars() {
  ChartDocument doc;
  NotationNote body = make_tap(0, 2);
  body.id = 1;
  body.width = 2;
  body.end_tick = 960;
  body.note_type = NoteType::Hold;
  CHECK(doc.add_note(body) == 1);
  NotationNote head = make_tap(0, 2);
  head.id = 2;
  head.width = 2;
  head.note_type = NoteType::CriticalHoldStart;
  CHECK(doc.add_note(head) == 2);
  NotationNote star_a = make_tap(240, 2);
  star_a.id = 3;
  star_a.width = 2;
  star_a.note_type = NoteType::Sound;
  star_a.parent_hold_id = 1;
  CHECK(doc.add_note(star_a) == 3);
  NotationNote star_b = make_tap(480, 2);
  star_b.id = 4;
  star_b.width = 2;
  star_b.note_type = NoteType::Sound;
  star_b.parent_hold_id = 1;
  CHECK(doc.add_note(star_b) == 4);

  {
    const auto to_scratch =
        convert_notes_in_selection(doc, {1}, NoteType::ScratchHold);
    CHECK(to_scratch.removals.empty());
    CHECK(to_scratch.updates.at(1).note_type == NoteType::ScratchHold);
    CHECK_EQ(to_scratch.updates.at(1).end_tick, 960);
    CHECK(to_scratch.updates.at(2).note_type == NoteType::ScratchCriticalHoldStart);
    CHECK(to_scratch.updates.at(3).note_type == NoteType::ScratchSound);
    CHECK_EQ(to_scratch.updates.at(3).parent_hold_id, 1);
    CHECK(to_scratch.updates.at(4).note_type == NoteType::ScratchSound);
  }

  {
    const auto collapse = convert_notes_in_selection(doc, {1}, NoteType::Normal);
    CHECK(collapse.updates.at(1).note_type == NoteType::Normal);
    CHECK_EQ(collapse.updates.at(1).end_tick, 0);
    CHECK(collapse.updates.find(3) == collapse.updates.end());
    CHECK(collapse.updates.find(4) == collapse.updates.end());
    CHECK_EQ(static_cast<int>(collapse.removals.size()), 3);
    std::unordered_set<int32_t> removed;
    for (const auto& n : collapse.removals) removed.insert(n.id);
    CHECK(removed.count(2));
    CHECK(removed.count(3));
    CHECK(removed.count(4));
  }

  {
    const auto keep_star = convert_notes_in_selection(doc, {1, 3}, NoteType::Flick);
    CHECK(keep_star.updates.at(1).note_type == NoteType::Flick);
    CHECK(keep_star.updates.at(3).note_type == NoteType::Flick);
    CHECK_EQ(keep_star.updates.at(3).parent_hold_id, kNoBoundHoldId);
    std::unordered_set<int32_t> removed;
    for (const auto& n : keep_star.removals) removed.insert(n.id);
    CHECK(removed.count(2));
    CHECK(removed.count(4));
    CHECK(!removed.count(3));
  }

  {
    const auto star_only = convert_notes_in_selection(doc, {3}, NoteType::Normal);
    CHECK(star_only.removals.empty());
    CHECK(star_only.updates.at(3).note_type == NoteType::Normal);
    CHECK(star_only.updates.find(1) == star_only.updates.end());
    CHECK(doc.find_note(4)->note_type == NoteType::Sound);
  }

  {
    const auto head_to_flick = convert_notes_in_selection(doc, {2}, NoteType::Flick);
    CHECK(head_to_flick.removals.empty());
    CHECK(head_to_flick.updates.at(2).note_type == NoteType::Flick);
    CHECK(head_to_flick.updates.find(1) == head_to_flick.updates.end());
  }

  ChartDocument scratch_doc;
  NotationNote sbody = make_tap(0, 1);
  sbody.id = 10;
  sbody.width = 2;
  sbody.end_tick = 720;
  sbody.note_type = NoteType::ScratchHold;
  CHECK(scratch_doc.add_note(sbody) == 10);
  NotationNote shead = make_tap(0, 1);
  shead.id = 11;
  shead.width = 2;
  shead.note_type = NoteType::ScratchHoldStart;
  CHECK(scratch_doc.add_note(shead) == 11);
  NotationNote sstar = make_tap(360, 1);
  sstar.id = 12;
  sstar.width = 2;
  sstar.note_type = NoteType::ScratchSound;
  sstar.parent_hold_id = 10;
  CHECK(scratch_doc.add_note(sstar) == 12);
  const auto to_hold = convert_notes_in_selection(scratch_doc, {10}, NoteType::Hold);
  CHECK(to_hold.updates.at(10).note_type == NoteType::Hold);
  CHECK(to_hold.updates.at(11).note_type == NoteType::HoldStart);
  CHECK(to_hold.updates.at(12).note_type == NoteType::Sound);
  CHECK_EQ(to_hold.updates.at(12).parent_hold_id, 10);
}

NotationNote note_with_id(int32_t id, int32_t start_tick, int32_t lane) {
  NotationNote note = make_tap(start_tick, lane);
  note.id = id;
  return note;
}

bool notes_match_by_id(const ChartDocument& a, const ChartDocument& b) {
  if (a.notes().size() != b.notes().size()) {
    return false;
  }
  for (const auto& note : a.notes()) {
    const auto found = b.find_note(note.id);
    if (!found) {
      return false;
    }
    if (found->id != note.id || found->start_tick != note.start_tick ||
        found->end_tick != note.end_tick || found->lane != note.lane ||
        found->width != note.width || found->note_type != note.note_type ||
        found->gimmick_type != note.gimmick_type ||
        found->scratch_length != note.scratch_length) {
      return false;
    }
  }
  return true;
}

bool concurrent_lines_match(const ChartDocument& a, const ChartDocument& b) {
  const auto& la = a.concurrent_lines();
  const auto& lb = b.concurrent_lines();
  if (la.size() != lb.size()) {
    return false;
  }
  for (size_t i = 0; i < la.size(); ++i) {
    if (la[i].milliseconds != lb[i].milliseconds || la[i].start_lane != lb[i].start_lane ||
        la[i].width != lb[i].width) {
      return false;
    }
  }
  return true;
}

bool index_query_ids_match(const ChartDocument& a, const ChartDocument& b, int64_t time_ms,
                           int64_t lead_ms, int64_t tail_ms) {
  std::vector<int32_t> ca;
  std::vector<int32_t> cb;
  a.index().query_candidates(time_ms, lead_ms, tail_ms, ca);
  b.index().query_candidates(time_ms, lead_ms, tail_ms, cb);
  std::sort(ca.begin(), ca.end());
  std::sort(cb.begin(), cb.end());
  return ca == cb;
}

bool notation_note_fields_equal(const NotationNote& a, const NotationNote& b) {
  return a.id == b.id && a.start_tick == b.start_tick && a.end_tick == b.end_tick &&
         a.lane == b.lane && a.width == b.width && a.note_type == b.note_type &&
         a.gimmick_type == b.gimmick_type && a.scratch_length == b.scratch_length;
}

bool notes_vector_equal(const std::vector<NotationNote>& a, const std::vector<NotationNote>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (!notation_note_fields_equal(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

void sorted_query_candidates(const ChartDocument& doc, int64_t time_ms, int64_t lead_ms,
                             int64_t tail_ms, std::vector<int32_t>& out) {
  doc.index().query_candidates(time_ms, lead_ms, tail_ms, out);
  std::sort(out.begin(), out.end());
}

void sorted_split_query(const ChartDocument& doc, int64_t time_ms, std::vector<int32_t>& out) {
  doc.index().query_split_lanes_up_to(time_ms, out);
  std::sort(out.begin(), out.end());
}

bool split_query_ids_match(const ChartDocument& a, const ChartDocument& b, int64_t time_ms) {
  std::vector<int32_t> sa;
  std::vector<int32_t> sb;
  sorted_split_query(a, time_ms, sa);
  sorted_split_query(b, time_ms, sb);
  return sa == sb;
}

struct IndexProbe {
  std::vector<int32_t> candidates;
  std::vector<int32_t> splits;
};

IndexProbe make_index_probe(const ChartDocument& doc, int64_t time_ms, int64_t lead_ms,
                            int64_t tail_ms) {
  IndexProbe probe;
  sorted_query_candidates(doc, time_ms, lead_ms, tail_ms, probe.candidates);
  sorted_split_query(doc, time_ms, probe.splits);
  return probe;
}

bool index_probe_equal(const IndexProbe& a, const IndexProbe& b) {
  return a.candidates == b.candidates && a.splits == b.splits;
}

void add_split_to_fixture(ChartDocument& doc, int32_t id, int32_t start_tick) {
  NotationNote split = note_with_id(id, start_tick, 0);
  split.end_tick = start_tick + 1920;
  split.gimmick_type = GimmickType::Split3;
  CHECK_EQ(doc.add_note(split), id);
}

void seed_apply_note_updates_fixture(ChartDocument& doc) {
  NotationNote tap_a = note_with_id(1, 0, 0);
  NotationNote tap_b = note_with_id(2, 480, 2);
  NotationNote tap_c = note_with_id(3, 1440, 4);
  NotationNote hold = note_with_id(4, 0, 6);
  hold.end_tick = 1920;
  hold.note_type = NoteType::Hold;
  NotationNote hold_short = note_with_id(5, 480, 8);
  hold_short.end_tick = 960;
  hold_short.note_type = NoteType::Hold;
  CHECK(doc.set_notes({tap_a, tap_b, tap_c, hold, hold_short}));
}

std::vector<NoteUpdate> make_batch_move_and_shorten() {
  NotationNote tap_a = note_with_id(1, 0, 1);
  NotationNote tap_c = note_with_id(3, 0, 5);
  NotationNote hold = note_with_id(4, 0, 6);
  hold.end_tick = 480;
  hold.note_type = NoteType::Hold;
  return {{1, tap_a}, {3, tap_c}, {4, hold}};
}

void test_apply_note_updates_empty_does_not_change_generation() {
  ChartDocument doc;
  seed_apply_note_updates_fixture(doc);
  const uint64_t gen = doc.content_generation();
  const bool dirty = doc.is_dirty();
  CHECK(doc.apply_note_updates({}));
  CHECK_EQ(doc.content_generation(), gen);
  CHECK_EQ(doc.is_dirty(), dirty);
}

void test_apply_note_updates_batch_equivalent_and_generation_plus_one() {
  ChartDocument sequential;
  seed_apply_note_updates_fixture(sequential);
  ChartDocument batched;
  batched.load_from_chart(sequential.to_notation_chart(), sequential.edit_mode());

  const auto updates = make_batch_move_and_shorten();
  for (const auto& update : updates) {
    CHECK(sequential.update_note(update.id, update.note));
  }
  const uint64_t gen_before = batched.content_generation();
  CHECK(batched.apply_note_updates(updates));
  CHECK_EQ(batched.content_generation(), gen_before + 1);
  CHECK(batched.is_dirty());

  CHECK(notes_match_by_id(sequential, batched));
  CHECK(concurrent_lines_match(sequential, batched));
  CHECK_EQ(sequential.index().hold_span_stale(), batched.index().hold_span_stale());
  CHECK_EQ(sequential.index().max_hold_span_ms(), batched.index().max_hold_span_ms());
  CHECK(index_query_ids_match(sequential, batched, 0, 100, 100));
  CHECK(index_query_ids_match(sequential, batched, 500, 300, 300));

  // Single update_note must reuse the batch path (exactly +1, not per-rebuild +2).
  ChartDocument single;
  seed_apply_note_updates_fixture(single);
  const uint64_t single_gen = single.content_generation();
  NotationNote moved = *single.find_note(2);
  moved.lane = 3;
  CHECK(single.update_note(2, moved));
  CHECK_EQ(single.content_generation(), single_gen + 1);
}

void test_apply_note_updates_rejects_duplicate_and_unknown_atomically() {
  ChartDocument doc;
  seed_apply_note_updates_fixture(doc);
  add_split_to_fixture(doc, 6, 480);
  doc.mark_saved();
  const uint64_t gen = doc.content_generation();
  const bool dirty = doc.is_dirty();
  const auto notes_before = doc.notes();
  const auto lines_before = doc.concurrent_lines();
  const int64_t span = doc.index().max_hold_span_ms();
  const bool stale = doc.index().hold_span_stale();
  const int64_t probe_times[] = {0, 500, 1500, 2500};
  std::vector<IndexProbe> probes_before;
  probes_before.reserve(4);
  for (int64_t t : probe_times) {
    probes_before.push_back(make_index_probe(doc, t, 80, 80));
  }

  auto assert_unchanged = [&] {
    CHECK_EQ(doc.content_generation(), gen);
    CHECK_EQ(doc.is_dirty(), dirty);
    CHECK(notes_vector_equal(doc.notes(), notes_before));
    CHECK_EQ(doc.concurrent_lines().size(), lines_before.size());
    for (size_t i = 0; i < lines_before.size(); ++i) {
      CHECK_EQ(doc.concurrent_lines()[i].milliseconds, lines_before[i].milliseconds);
      CHECK_EQ(doc.concurrent_lines()[i].start_lane, lines_before[i].start_lane);
      CHECK_EQ(doc.concurrent_lines()[i].width, lines_before[i].width);
    }
    CHECK_EQ(doc.index().max_hold_span_ms(), span);
    CHECK_EQ(doc.index().hold_span_stale(), stale);
    for (size_t i = 0; i < probes_before.size(); ++i) {
      CHECK(index_probe_equal(make_index_probe(doc, probe_times[i], 80, 80), probes_before[i]));
    }
  };

  NotationNote tap = note_with_id(1, 240, 3);
  NotationNote unknown = note_with_id(99, 0, 0);
  CHECK(!doc.apply_note_updates({{1, tap}, {99, unknown}}));
  CHECK(!doc.find_note(99).has_value());
  assert_unchanged();

  NotationNote again = note_with_id(1, 720, 2);
  CHECK(!doc.apply_note_updates({{1, tap}, {1, again}}));
  assert_unchanged();
}

void test_apply_note_updates_forces_note_id() {
  ChartDocument doc;
  seed_apply_note_updates_fixture(doc);
  const uint64_t gen = doc.content_generation();
  NotationNote spoofed = note_with_id(99, 240, 3);
  CHECK(doc.apply_note_updates({{2, spoofed}}));
  CHECK_EQ(doc.content_generation(), gen + 1);
  const auto found = doc.find_note(2);
  CHECK(found.has_value());
  CHECK_EQ(found->id, 2);
  CHECK_EQ(found->start_tick, 240);
  CHECK_EQ(found->lane, 3);
  CHECK(!doc.find_note(99).has_value());
}

void test_apply_note_updates_incremental_index_without_hold_rebuild() {
  ChartDocument sequential;
  seed_apply_note_updates_fixture(sequential);
  add_split_to_fixture(sequential, 6, 960);
  CHECK(!sequential.index().hold_span_stale());
  const int64_t span0 = sequential.index().max_hold_span_ms();
  CHECK(span0 > 0);

  ChartDocument batched;
  batched.load_from_chart(sequential.to_notation_chart(), sequential.edit_mode());
  CHECK(!batched.index().hold_span_stale());
  CHECK_EQ(batched.index().max_hold_span_ms(), span0);

  // Move taps only, including a start-time swap. Longest hold (id 4) is untouched so
  // hold_span_stale stays false and a batch-end rebuild_index would be the only way
  // to "fix" a broken incremental on_note_updated — that path must not run.
  const auto tap1 = *sequential.find_note(1);
  const auto tap2 = *sequential.find_note(2);
  const auto tap3 = *sequential.find_note(3);
  NotationNote swap1 = tap1;
  swap1.start_tick = tap3.start_tick;
  swap1.end_tick = tap3.start_tick;
  NotationNote swap3 = tap3;
  swap3.start_tick = tap1.start_tick;
  swap3.end_tick = tap1.start_tick;
  NotationNote move2 = tap2;
  move2.start_tick = 960;
  move2.end_tick = 960;
  move2.lane = 3;
  const std::vector<NoteUpdate> updates = {{1, swap1}, {2, move2}, {3, swap3}};

  const int64_t old_ms[] = {tap1.start_ms(sequential.timing()), tap2.start_ms(sequential.timing()),
                            tap3.start_ms(sequential.timing())};
  const int32_t moved_ids[] = {1, 2, 3};

  for (const auto& update : updates) {
    CHECK(sequential.update_note(update.id, update.note));
  }
  const uint64_t gen_before = batched.content_generation();
  CHECK(batched.apply_note_updates(updates));
  CHECK_EQ(batched.content_generation(), gen_before + 1);

  CHECK(!sequential.index().hold_span_stale());
  CHECK(!batched.index().hold_span_stale());
  CHECK_EQ(sequential.index().max_hold_span_ms(), span0);
  CHECK_EQ(batched.index().max_hold_span_ms(), span0);
  CHECK(notes_match_by_id(sequential, batched));
  CHECK(concurrent_lines_match(sequential, batched));

  const int64_t new_ms[] = {batched.find_note(1)->start_ms(batched.timing()),
                            batched.find_note(2)->start_ms(batched.timing()),
                            batched.find_note(3)->start_ms(batched.timing())};
  for (int i = 0; i < 3; ++i) {
    CHECK(index_query_ids_match(sequential, batched, old_ms[i], 40, 40));
    CHECK(index_query_ids_match(sequential, batched, new_ms[i], 40, 40));
    CHECK(split_query_ids_match(sequential, batched, old_ms[i]));
    CHECK(split_query_ids_match(sequential, batched, new_ms[i]));

    std::vector<int32_t> at_old;
    std::vector<int32_t> at_new;
    sorted_query_candidates(batched, old_ms[i], 40, 40, at_old);
    sorted_query_candidates(batched, new_ms[i], 40, 40, at_new);
    if (old_ms[i] != new_ms[i]) {
      CHECK(std::find(at_old.begin(), at_old.end(), moved_ids[i]) == at_old.end());
    }
    CHECK(std::find(at_new.begin(), at_new.end(), moved_ids[i]) != at_new.end());
  }
  CHECK(split_query_ids_match(sequential, batched, 2000));
  std::vector<int32_t> splits;
  sorted_split_query(batched, 2000, splits);
  CHECK(std::find(splits.begin(), splits.end(), 6) != splits.end());
}

void test_apply_note_updates_read_only() {
  ChartDocument doc;
  seed_apply_note_updates_fixture(doc);
  doc.set_edit_mode(ChartEditMode::OfficialPreviewOnly);
  const uint64_t gen = doc.content_generation();
  NotationNote tap = note_with_id(1, 240, 3);
  CHECK(!doc.apply_note_updates({{1, tap}}));
  CHECK_EQ(doc.content_generation(), gen);
  CHECK_EQ(doc.find_note(1)->start_tick, 0);
  CHECK_EQ(doc.find_note(1)->lane, 0);
}

void test_apply_note_updates_hold_span_index_and_concurrent() {
  ChartDocument doc;
  seed_apply_note_updates_fixture(doc);
  CHECK(doc.index().max_hold_span_ms() > 0);
  CHECK(!doc.index().hold_span_stale());
  CHECK_EQ(static_cast<int32_t>(doc.concurrent_lines().size()), 0);

  const auto updates = make_batch_move_and_shorten();
  CHECK(doc.apply_note_updates(updates));
  CHECK(!doc.index().hold_span_stale());
  CHECK(doc.index().max_hold_span_ms() > 0);
  // Long hold 0–1920 shortened to 0–480; remaining short hold is also 480 ticks.
  CHECK_EQ(doc.find_note(4)->end_tick, 480);
  CHECK_EQ(doc.index().max_hold_span_ms(), doc.find_note(5)->end_ms(doc.timing()) -
                                               doc.find_note(5)->start_ms(doc.timing()));

  // Taps 1 and 3 now share tick 0 → multi-press sync line (hold tail + tap 2 may
  // add another line at the shortened hold end).
  bool found_start_line = false;
  for (const auto& line : doc.concurrent_lines()) {
    if (line.milliseconds != 0) {
      continue;
    }
    found_start_line = true;
    CHECK_EQ(line.start_lane, 1);
    CHECK_EQ(line.width, 5);  // lanes 1..5
  }
  CHECK(found_start_line);

  std::vector<int32_t> at_start;
  doc.index().query_candidates(0, 50, 50, at_start);
  std::sort(at_start.begin(), at_start.end());
  CHECK(std::find(at_start.begin(), at_start.end(), 1) != at_start.end());
  CHECK(std::find(at_start.begin(), at_start.end(), 3) != at_start.end());
  CHECK(std::find(at_start.begin(), at_start.end(), 4) != at_start.end());

  std::vector<int32_t> at_old_c;
  doc.index().query_candidates(1500, 50, 50, at_old_c);
  CHECK(std::find(at_old_c.begin(), at_old_c.end(), 3) == at_old_c.end());
}

void test_update_notes_command_undo_redo_generation() {
  ChartDocument doc;
  seed_apply_note_updates_fixture(doc);
  NotationNote before_a = *doc.find_note(1);
  NotationNote before_c = *doc.find_note(3);
  NotationNote after_a = before_a;
  after_a.lane = 1;
  NotationNote after_c = before_c;
  after_c.start_tick = 0;
  after_c.lane = 5;

  std::unordered_map<int32_t, UpdateNotesCommand::NotePair> changes;
  changes[1] = {before_a, after_a};
  changes[3] = {before_c, after_c};

  EditHistory history;
  const uint64_t g0 = doc.content_generation();
  CHECK(history.execute(std::make_unique<UpdateNotesCommand>(changes, "Batch move"), doc));
  CHECK_EQ(doc.content_generation(), g0 + 1);
  CHECK_EQ(doc.find_note(1)->lane, 1);
  CHECK_EQ(doc.find_note(3)->start_tick, 0);
  CHECK_EQ(doc.find_note(3)->lane, 5);

  CHECK(history.undo(doc));
  CHECK_EQ(doc.content_generation(), g0 + 2);
  CHECK_EQ(doc.find_note(1)->lane, before_a.lane);
  CHECK_EQ(doc.find_note(3)->start_tick, before_c.start_tick);
  CHECK_EQ(doc.find_note(3)->lane, before_c.lane);

  CHECK(history.redo(doc));
  CHECK_EQ(doc.content_generation(), g0 + 3);
  CHECK_EQ(doc.find_note(1)->lane, 1);
  CHECK_EQ(doc.find_note(3)->start_tick, 0);
  CHECK_EQ(doc.find_note(3)->lane, 5);

  // Failed apply must not mutate (pre-validation is atomic; no per-item rollback).
  ChartDocument rejected;
  seed_apply_note_updates_fixture(rejected);
  std::unordered_map<int32_t, UpdateNotesCommand::NotePair> bad;
  bad[1] = {before_a, after_a};
  NotationNote ghost = note_with_id(99, 0, 0);
  bad[99] = {ghost, ghost};
  const uint64_t rejected_gen = rejected.content_generation();
  UpdateNotesCommand bad_cmd(bad, "Bad batch");
  CHECK(!bad_cmd.execute(rejected));
  CHECK_EQ(rejected.content_generation(), rejected_gen);
  CHECK_EQ(rejected.find_note(1)->lane, 0);
  CHECK(!rejected.find_note(99).has_value());
}

// Naive oracle for hold-body / preview-combo hits. Must stay a double full-table
// scan so optimized collect_* can be checked against the original semantics.
bool reference_lanes_overlap(const NotationNote& a, const NotationNote& b) noexcept {
  return a.lane <= b.end_lane() && b.lane <= a.end_lane();
}

bool reference_is_combo_head_note(const NotationNote& note) noexcept {
  if (is_split_lane_gimmick(note.gimmick_type)) {
    return false;
  }
  if (is_hold_mid_star(note.note_type) || is_hold_body(note.note_type)) {
    return false;
  }
  switch (note.note_type) {
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::Flick:
    case NoteType::BlueTap:
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return true;
    default:
      return false;
  }
}

bool reference_mid_star_on_hold(const NotationNote& star, const NotationNote& hold) noexcept {
  if (!is_hold_mid_star(star.note_type)) return false;
  if (star.parent_hold_id >= 0) return star.parent_hold_id == hold.id;
  return reference_lanes_overlap(hold, star);
}

void reference_collect_hold_body_judge_times(const NotationNote& hold,
                                             const std::vector<NotationNote>& notes,
                                             const MusicTiming& timing,
                                             std::vector<int64_t>& out_sorted_unique) {
  out_sorted_unique.clear();
  if (!is_hold_body(hold.note_type) || is_hold_mid_star(hold.note_type)) {
    return;
  }

  const int64_t start = hold.start_ms(timing);
  const int64_t end = hold.end_ms(timing);
  if (end <= start) {
    return;
  }

  std::vector<int64_t> times;
  for (const auto& note : notes) {
    if (!is_hold_mid_star(note.note_type)) {
      continue;
    }
    const int64_t ms = note.start_ms(timing);
    if (ms <= start || ms >= end) {
      continue;
    }
    if (!reference_mid_star_on_hold(note, hold)) {
      continue;
    }
    times.push_back(ms);
  }

  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  out_sorted_unique = std::move(times);
}

void reference_collect_preview_combo_hits(const std::vector<NotationNote>& notes,
                                          const MusicTiming& timing,
                                          std::vector<int64_t>& out_sorted_hits) {
  out_sorted_hits.clear();
  std::vector<int64_t> hold_times;
  std::vector<char> star_consumed(notes.size(), 0);

  for (size_t i = 0; i < notes.size(); ++i) {
    const auto& note = notes[i];
    if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
      reference_collect_hold_body_judge_times(note, notes, timing, hold_times);
      out_sorted_hits.insert(out_sorted_hits.end(), hold_times.begin(), hold_times.end());
      const int64_t start = note.start_ms(timing);
      const int64_t end = note.end_ms(timing);
      for (size_t j = 0; j < notes.size(); ++j) {
        if (!is_hold_mid_star(notes[j].note_type)) {
          continue;
        }
        const int64_t ms = notes[j].start_ms(timing);
        if (ms > start && ms < end && reference_mid_star_on_hold(notes[j], note)) {
          star_consumed[j] = 1;
        }
      }
      if (is_hold_with_tail(note.note_type) && end > start) {
        out_sorted_hits.push_back(end);
      }
      continue;
    }

    if (reference_is_combo_head_note(note)) {
      out_sorted_hits.push_back(note.start_ms(timing));
    }
  }

  for (size_t i = 0; i < notes.size(); ++i) {
    if (star_consumed[i] || !is_hold_mid_star(notes[i].note_type)) {
      continue;
    }
    if (is_split_lane_gimmick(notes[i].gimmick_type)) {
      continue;
    }
    out_sorted_hits.push_back(notes[i].start_ms(timing));
  }

  std::sort(out_sorted_hits.begin(), out_sorted_hits.end());
}

void sabotaged_inclusive_hold_body_judge_times(const NotationNote& hold,
                                               const std::vector<NotationNote>& notes,
                                               const MusicTiming& timing,
                                               std::vector<int64_t>& out_sorted_unique) {
  out_sorted_unique.clear();
  if (!is_hold_body(hold.note_type) || is_hold_mid_star(hold.note_type)) {
    return;
  }
  const int64_t start = hold.start_ms(timing);
  const int64_t end = hold.end_ms(timing);
  if (end <= start) {
    return;
  }
  std::vector<int64_t> times;
  for (const auto& note : notes) {
    if (!is_hold_mid_star(note.note_type)) {
      continue;
    }
    const int64_t ms = note.start_ms(timing);
    if (ms < start || ms > end) {
      continue;
    }
    if (!reference_lanes_overlap(hold, note)) {
      continue;
    }
    times.push_back(ms);
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  out_sorted_unique = std::move(times);
}

void sabotaged_combo_without_consume(const std::vector<NotationNote>& notes,
                                     const MusicTiming& timing,
                                     std::vector<int64_t>& out_sorted_hits) {
  reference_collect_preview_combo_hits(notes, timing, out_sorted_hits);
  for (const auto& note : notes) {
    if (is_hold_mid_star(note.note_type) && !is_split_lane_gimmick(note.gimmick_type)) {
      out_sorted_hits.push_back(note.start_ms(timing));
    }
  }
  std::sort(out_sorted_hits.begin(), out_sorted_hits.end());
}

NotationNote combo_note(int32_t start_tick, int32_t end_tick, int32_t lane, int32_t width,
                        NoteType type, GimmickType gimmick = GimmickType::None) {
  NotationNote note;
  note.start_tick = start_tick;
  note.end_tick = end_tick;
  note.lane = lane;
  note.width = width;
  note.note_type = type;
  note.gimmick_type = gimmick;
  return note;
}

MusicTiming combo_test_timing() {
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  return timing;
}

void check_ms_lists_equal(const std::vector<int64_t>& got, const std::vector<int64_t>& ref,
                          const char* label) {
  if (got != ref) {
    std::fprintf(stderr, "combo ms mismatch (%s): got %zu vs ref %zu\n", label, got.size(),
                 ref.size());
    const size_t n = std::min(got.size(), ref.size());
    for (size_t i = 0; i < n; ++i) {
      if (got[i] != ref[i]) {
        std::fprintf(stderr, "  first diff i=%zu got=%lld ref=%lld\n", i,
                     static_cast<long long>(got[i]), static_cast<long long>(ref[i]));
        break;
      }
    }
  }
  CHECK(got == ref);
}

void expect_hold_and_combo_match(const std::vector<NotationNote>& notes, const MusicTiming& timing,
                                 const char* label) {
  std::vector<int64_t> got_hold;
  std::vector<int64_t> ref_hold;
  for (const auto& note : notes) {
    collect_hold_body_judge_times(note, notes, timing, got_hold);
    reference_collect_hold_body_judge_times(note, notes, timing, ref_hold);
    check_ms_lists_equal(got_hold, ref_hold, label);
  }
  std::vector<int64_t> got_combo;
  std::vector<int64_t> ref_combo;
  collect_preview_combo_hits(notes, timing, got_combo);
  reference_collect_preview_combo_hits(notes, timing, ref_combo);
  check_ms_lists_equal(got_combo, ref_combo, label);
}

void test_hold_combo_reference_fixed_cases() {
  const MusicTiming timing = combo_test_timing();

  // Duplicate ms: two overlapping stars at the same tick collapse to one judge.
  {
    const auto hold = combo_note(0, 1920, 0, 3, NoteType::Hold);
    const auto star_a = combo_note(480, 480, 0, 1, NoteType::Sound);
    const auto star_b = combo_note(480, 480, 1, 1, NoteType::HoldEighth);
    const auto star_c = combo_note(960, 960, 0, 1, NoteType::ScratchSound);
    const std::vector<NotationNote> notes{hold, star_a, star_b, star_c};
    std::vector<int64_t> times;
    reference_collect_hold_body_judge_times(hold, notes, timing, times);
    CHECK_EQ(static_cast<int>(times.size()), 2);
    CHECK_EQ(times[0], star_a.start_ms(timing));
    CHECK_EQ(times[1], star_c.start_ms(timing));
    CHECK_EQ(times[0], star_b.start_ms(timing));

    std::vector<int64_t> combo;
    reference_collect_preview_combo_hits(notes, timing, combo);
    CHECK_EQ(static_cast<int>(combo.size()), 3);
    CHECK_EQ(combo[0], times[0]);
    CHECK_EQ(combo[1], times[1]);
    CHECK_EQ(combo[2], hold.end_ms(timing));
    expect_hold_and_combo_match(notes, timing, "duplicate-ms");
  }

  // Wide hold covers every playable lane.
  {
    const auto hold = combo_note(0, 1920, 0, 12, NoteType::Hold);
    const auto s0 = combo_note(240, 240, 0, 1, NoteType::HoldEighth);
    const auto s5 = combo_note(480, 480, 5, 2, NoteType::Sound);
    const auto s11 = combo_note(720, 720, 11, 1, NoteType::ScratchSound);
    const std::vector<NotationNote> notes{hold, s0, s5, s11};
    std::vector<int64_t> times;
    reference_collect_hold_body_judge_times(hold, notes, timing, times);
    CHECK_EQ(static_cast<int>(times.size()), 3);
    CHECK_EQ(times[0], s0.start_ms(timing));
    CHECK_EQ(times[1], s5.start_ms(timing));
    CHECK_EQ(times[2], s11.start_ms(timing));
    expect_hold_and_combo_match(notes, timing, "wide-lane");
  }

  // Overlapping holds each emit the shared star; star is consumed once globally.
  {
    const auto hold_a = combo_note(0, 1920, 0, 3, NoteType::Hold);
    const auto hold_b = combo_note(480, 2400, 2, 3, NoteType::CriticalHold);
    const auto star = combo_note(960, 960, 2, 1, NoteType::Sound);
    const std::vector<NotationNote> notes{hold_a, hold_b, star};
    std::vector<int64_t> times_a;
    std::vector<int64_t> times_b;
    reference_collect_hold_body_judge_times(hold_a, notes, timing, times_a);
    reference_collect_hold_body_judge_times(hold_b, notes, timing, times_b);
    CHECK_EQ(static_cast<int>(times_a.size()), 1);
    CHECK_EQ(static_cast<int>(times_b.size()), 1);
    CHECK_EQ(times_a[0], star.start_ms(timing));
    CHECK_EQ(times_b[0], star.start_ms(timing));

    std::vector<int64_t> combo;
    reference_collect_preview_combo_hits(notes, timing, combo);
    int shared = 0;
    for (const int64_t ms : combo) {
      if (ms == star.start_ms(timing)) {
        ++shared;
      }
    }
    CHECK_EQ(shared, 2);
    CHECK_EQ(static_cast<int>(combo.size()), 4);
    expect_hold_and_combo_match(notes, timing, "overlapping-holds");
  }

  // Bound star counts only for its parent, even when lanes overlap another hold.
  {
    auto hold_a = combo_note(0, 1920, 0, 3, NoteType::Hold);
    hold_a.id = 1;
    auto hold_b = combo_note(480, 2400, 2, 3, NoteType::CriticalHold);
    hold_b.id = 2;
    auto star = combo_note(960, 960, 2, 1, NoteType::Sound);
    star.id = 3;
    star.parent_hold_id = 1;
    const std::vector<NotationNote> notes{hold_a, hold_b, star};
    std::vector<int64_t> times_a;
    std::vector<int64_t> times_b;
    collect_hold_body_judge_times(hold_a, notes, timing, times_a);
    collect_hold_body_judge_times(hold_b, notes, timing, times_b);
    CHECK_EQ(static_cast<int>(times_a.size()), 1);
    CHECK_EQ(static_cast<int>(times_b.size()), 0);
    expect_hold_and_combo_match(notes, timing, "bound-star-parent-only");
  }

  // Stars exactly on hold start/end are exclusive; only the interior star counts.
  {
    const auto hold = combo_note(480, 1440, 1, 2, NoteType::Hold);
    const auto at_start = combo_note(480, 480, 1, 1, NoteType::Sound);
    const auto at_end = combo_note(1440, 1440, 1, 1, NoteType::Sound);
    const auto before = combo_note(0, 0, 1, 1, NoteType::HoldEighth);
    const auto inside = combo_note(960, 960, 2, 1, NoteType::ScratchSound);
    const std::vector<NotationNote> notes{hold, at_start, at_end, before, inside};
    std::vector<int64_t> times;
    reference_collect_hold_body_judge_times(hold, notes, timing, times);
    CHECK_EQ(static_cast<int>(times.size()), 1);
    CHECK_EQ(times[0], inside.start_ms(timing));
    expect_hold_and_combo_match(notes, timing, "boundary-star");
  }

  // Isolated split mid-star is not combo; absorbed split star is a hold judge.
  {
    const auto isolated_split = combo_note(240, 240, 8, 1, NoteType::Sound, GimmickType::Split3);
    const auto hold = combo_note(0, 1920, 3, 2, NoteType::Hold);
    const auto absorbed_split = combo_note(960, 960, 3, 1, NoteType::Sound, GimmickType::Split3);
    const auto isolated = combo_note(2400, 2400, 5, 1, NoteType::HoldEighth);
    const std::vector<NotationNote> notes{isolated_split, hold, absorbed_split, isolated};
    std::vector<int64_t> times;
    reference_collect_hold_body_judge_times(hold, notes, timing, times);
    CHECK_EQ(static_cast<int>(times.size()), 1);
    CHECK_EQ(times[0], absorbed_split.start_ms(timing));

    std::vector<int64_t> combo;
    reference_collect_preview_combo_hits(notes, timing, combo);
    CHECK_EQ(static_cast<int>(combo.size()), 3);
    CHECK_EQ(combo[0], absorbed_split.start_ms(timing));
    CHECK_EQ(combo[1], hold.end_ms(timing));
    CHECK_EQ(combo[2], isolated.start_ms(timing));
    expect_hold_and_combo_match(notes, timing, "split-mid-star");
  }

  // Zero-width hold still uses lanes_overlap (end_lane = lane-1).
  {
    const auto hold = combo_note(0, 1920, 5, 0, NoteType::Hold);
    const auto star = combo_note(480, 480, 4, 2, NoteType::Sound);
    const auto miss = combo_note(720, 720, 8, 1, NoteType::Sound);
    const std::vector<NotationNote> notes{hold, star, miss};
    std::vector<int64_t> times;
    reference_collect_hold_body_judge_times(hold, notes, timing, times);
    CHECK_EQ(static_cast<int>(times.size()), 1);
    CHECK_EQ(times[0], star.start_ms(timing));
    expect_hold_and_combo_match(notes, timing, "zero-width-hold");
  }

  // Negative-lane star can still overlap; production must not OOB.
  {
    const auto hold = combo_note(0, 1920, 0, 2, NoteType::NontailHold);
    const auto star = combo_note(480, 480, -3, 5, NoteType::Sound);
    const std::vector<NotationNote> notes{hold, star};
    std::vector<int64_t> times;
    reference_collect_hold_body_judge_times(hold, notes, timing, times);
    CHECK_EQ(static_cast<int>(times.size()), 1);
    std::vector<int64_t> combo;
    reference_collect_preview_combo_hits(notes, timing, combo);
    CHECK_EQ(static_cast<int>(combo.size()), 1);
    expect_hold_and_combo_match(notes, timing, "negative-lane-star");
  }
}

void test_hold_combo_sabotage_detects_wrong_oracle() {
  const MusicTiming timing = combo_test_timing();
  const auto hold = combo_note(480, 1440, 1, 2, NoteType::Hold);
  const auto at_start = combo_note(480, 480, 1, 1, NoteType::Sound);
  const auto at_end = combo_note(1440, 1440, 1, 1, NoteType::Sound);
  const auto inside = combo_note(960, 960, 1, 1, NoteType::HoldEighth);
  const std::vector<NotationNote> notes{hold, at_start, at_end, inside};

  std::vector<int64_t> ref_times;
  std::vector<int64_t> broken_times;
  reference_collect_hold_body_judge_times(hold, notes, timing, ref_times);
  sabotaged_inclusive_hold_body_judge_times(hold, notes, timing, broken_times);
  CHECK_EQ(static_cast<int>(ref_times.size()), 1);
  CHECK(broken_times != ref_times);
  CHECK_EQ(static_cast<int>(broken_times.size()), 3);

  std::vector<int64_t> ref_combo;
  std::vector<int64_t> broken_combo;
  reference_collect_preview_combo_hits(notes, timing, ref_combo);
  sabotaged_combo_without_consume(notes, timing, broken_combo);
  CHECK(broken_combo != ref_combo);
}

void test_hold_combo_production_matches_reference_random() {
  const MusicTiming timing = combo_test_timing();
  expect_hold_and_combo_match({}, timing, "empty");

  const NoteType hold_types[] = {NoteType::Hold, NoteType::CriticalHold, NoteType::ScratchHold,
                                 NoteType::NontailHold, NoteType::NontailScratchHold};
  const NoteType star_types[] = {NoteType::HoldEighth, NoteType::Sound, NoteType::ScratchSound};
  const NoteType head_types[] = {NoteType::Normal, NoteType::Critical, NoteType::Flick,
                                 NoteType::HoldStart, NoteType::BlueTap};

  for (uint32_t seed = 1; seed <= 48; ++seed) {
    uint32_t state = seed * 747796405u + 2891336453u;
    auto next = [&]() {
      state = state * 1664525u + 1013904223u;
      return state;
    };
    auto pick = [&](int32_t n) { return static_cast<int32_t>(next() % static_cast<uint32_t>(n)); };

    std::vector<NotationNote> notes;
    notes.reserve(96);
    for (int32_t i = 0; i < 80; ++i) {
      NotationNote note;
      note.id = i;
      note.start_tick = pick(64) * 120;
      note.lane = pick(14) - 1;
      note.width = pick(8);
      const int32_t kind = pick(10);
      if (kind < 3) {
        note.note_type = hold_types[static_cast<size_t>(pick(5))];
        note.end_tick = note.start_tick + 240 + pick(16) * 120;
      } else if (kind < 7) {
        note.note_type = star_types[static_cast<size_t>(pick(3))];
        note.end_tick = note.start_tick;
        if (pick(7) == 0) {
          note.gimmick_type = GimmickType::Split3;
        }
        if (pick(5) == 0 && !notes.empty()) {
          note.start_tick = notes[static_cast<size_t>(pick(static_cast<int32_t>(notes.size())))]
                                .start_tick;
          note.end_tick = note.start_tick;
        }
      } else {
        note.note_type = head_types[static_cast<size_t>(pick(5))];
        note.end_tick = (note.note_type == NoteType::HoldStart) ? note.start_tick + 480
                                                               : note.start_tick;
        if (pick(11) == 0) {
          note.gimmick_type = GimmickType::Split6;
        }
      }
      if (pick(19) == 0) {
        note.width = 12;
        note.lane = 0;
      }
      notes.push_back(note);
    }

    char label[64];
    std::snprintf(label, sizeof(label), "random-seed-%u", seed);
    expect_hold_and_combo_match(notes, timing, label);
  }
}

MusicTiming make_curve_timing_4_4() {
  MusicTiming timing;
  timing.ticks_per_quarter = 480;
  timing.bpm = 120.0;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  normalize_timing_points(timing);
  return timing;
}

void test_easing_formulas_and_directions() {
  const double half_sqrt2 = 0.5 * std::sqrt(2.0);

  CHECK(apply_easing(0.3, EasingAlgorithm::Linear, EasingDirection::In, 7.0) == 0.3);
  CHECK(apply_easing(0.3, EasingAlgorithm::Linear, EasingDirection::Out, 7.0) == 0.3);
  CHECK(apply_easing(0.3, EasingAlgorithm::Linear, EasingDirection::InOut, 7.0) == 0.3);
  CHECK(apply_easing(0.3, EasingAlgorithm::Linear, EasingDirection::OutIn, 7.0) == 0.3);

  CHECK(std::fabs(apply_easing(0.5, EasingAlgorithm::Poly, EasingDirection::In, 1.0) - 0.25) <
        1e-15);
  CHECK(std::fabs(apply_easing(0.5, EasingAlgorithm::Exp, EasingDirection::In, 1.0) -
                  (std::expm1(0.5) / std::expm1(1.0))) < 1e-15);
  CHECK(std::fabs(apply_easing(0.5, EasingAlgorithm::Sine, EasingDirection::In, 9.0) -
                  (1.0 - half_sqrt2)) < 1e-12);

  CHECK(std::fabs(apply_easing(0.5, EasingAlgorithm::Poly, EasingDirection::Out, 1.0) - 0.75) <
        1e-15);
  CHECK(std::fabs(apply_easing(0.25, EasingAlgorithm::Poly, EasingDirection::Out, 1.0) - 0.4375) <
        1e-15);
  CHECK(std::fabs(apply_easing(0.5, EasingAlgorithm::Sine, EasingDirection::Out, 0.0) - half_sqrt2) <
        1e-12);
  CHECK(std::fabs(apply_easing(0.5, EasingAlgorithm::Exp, EasingDirection::Out, 1.0) -
                  (1.0 - std::expm1(0.5) / std::expm1(1.0))) < 1e-15);

  CHECK(std::fabs(apply_easing(0.25, EasingAlgorithm::Poly, EasingDirection::InOut, 1.0) - 0.125) <
        1e-15);
  CHECK(std::fabs(apply_easing(0.75, EasingAlgorithm::Poly, EasingDirection::InOut, 1.0) - 0.875) <
        1e-15);
  CHECK(std::fabs(apply_easing(0.25, EasingAlgorithm::Sine, EasingDirection::InOut, 0.0) -
                  0.5 * (1.0 - half_sqrt2)) < 1e-12);
  CHECK(std::fabs(apply_easing(0.75, EasingAlgorithm::Sine, EasingDirection::InOut, 0.0) -
                  (1.0 - 0.5 * (1.0 - half_sqrt2))) < 1e-12);

  CHECK(std::fabs(apply_easing(0.25, EasingAlgorithm::Poly, EasingDirection::OutIn, 1.0) - 0.375) <
        1e-15);
  CHECK(std::fabs(apply_easing(0.75, EasingAlgorithm::Poly, EasingDirection::OutIn, 1.0) - 0.625) <
        1e-15);
  CHECK(std::fabs(apply_easing(0.25, EasingAlgorithm::Sine, EasingDirection::OutIn, 0.0) -
                  0.5 * half_sqrt2) < 1e-12);
  CHECK(std::fabs(apply_easing(0.75, EasingAlgorithm::Sine, EasingDirection::OutIn, 0.0) -
                  (0.5 + 0.5 * (1.0 - half_sqrt2))) < 1e-12);
}

void test_easing_p0_and_invalid_parameters() {
  const double t = 0.37;
  const EasingDirection directions[] = {EasingDirection::In, EasingDirection::Out,
                                        EasingDirection::InOut, EasingDirection::OutIn};
  for (const auto direction : directions) {
    CHECK(apply_easing(t, EasingAlgorithm::Poly, direction, 0.0) == t);
    CHECK(apply_easing(t, EasingAlgorithm::Exp, direction, 0.0) == t);
    CHECK(apply_easing(t, EasingAlgorithm::Linear, direction, 0.0) == t);
  }

  CHECK(apply_easing(t, EasingAlgorithm::Poly, EasingDirection::In,
                     std::numeric_limits<double>::quiet_NaN()) == t);
  CHECK(apply_easing(t, EasingAlgorithm::Exp, EasingDirection::Out,
                     std::numeric_limits<double>::infinity()) == t);
  CHECK(apply_easing(t, EasingAlgorithm::Poly, EasingDirection::InOut,
                     -std::numeric_limits<double>::infinity()) == t);
  CHECK(apply_easing(t, EasingAlgorithm::Exp, EasingDirection::OutIn, -4.0) == t);

  const double at_20 = apply_easing(0.4, EasingAlgorithm::Poly, EasingDirection::In, 20.0);
  CHECK(apply_easing(0.4, EasingAlgorithm::Poly, EasingDirection::In, 99.0) == at_20);
  CHECK(apply_easing(0.4, EasingAlgorithm::Exp, EasingDirection::In, 25.0) ==
        apply_easing(0.4, EasingAlgorithm::Exp, EasingDirection::In, 20.0));
}

void test_easing_endpoints_and_monotonicity() {
  const EasingAlgorithm algorithms[] = {EasingAlgorithm::Linear, EasingAlgorithm::Poly,
                                        EasingAlgorithm::Exp, EasingAlgorithm::Sine};
  const EasingDirection directions[] = {EasingDirection::In, EasingDirection::Out,
                                        EasingDirection::InOut, EasingDirection::OutIn};
  const double parameters[] = {0.0, 1.0, 3.0, 20.0};
  for (const auto algorithm : algorithms) {
    for (const auto direction : directions) {
      for (const double p : parameters) {
        CHECK(apply_easing(0.0, algorithm, direction, p) == 0.0);
        CHECK(apply_easing(1.0, algorithm, direction, p) == 1.0);
        CHECK(apply_easing(-2.0, algorithm, direction, p) == 0.0);
        CHECK(apply_easing(3.0, algorithm, direction, p) == 1.0);
        CHECK(apply_easing(std::numeric_limits<double>::quiet_NaN(), algorithm, direction, p) ==
              0.0);
        double prev = apply_easing(0.0, algorithm, direction, p);
        for (int i = 1; i <= 100; ++i) {
          const double y = apply_easing(static_cast<double>(i) / 100.0, algorithm, direction, p);
          CHECK(y + 1e-12 >= prev);
          CHECK(y >= 0.0 && y <= 1.0);
          prev = y;
        }
      }
    }
  }
}

void test_scratch_hold_curve_empty_ranges() {
  const MusicTiming timing = make_curve_timing_4_4();
  ScratchHoldCurveRequest req;
  req.start_center = 2.0;
  req.end_center = 6.0;
  req.width = 1;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;

  req.start_tick = 100;
  req.end_tick = 100;
  CHECK(generate_scratch_hold_curve(req, timing).empty());
  CHECK(scratch_hold_curve_boundaries(req.start_tick, req.end_tick, timing,
                                      req.subdivisions_per_beat)
            .empty());

  req.end_tick = 40;
  CHECK(generate_scratch_hold_curve(req, timing).empty());
  CHECK(scratch_hold_curve_boundaries(100, 40, timing, 4).empty());
}

void test_scratch_hold_curve_meter_aware_boundaries() {
  MusicTiming timing = make_curve_timing_4_4();
  const auto on_grid = scratch_hold_curve_boundaries(0, 480, timing, 4);
  CHECK_EQ(static_cast<int32_t>(on_grid.size()), 5);
  CHECK_EQ(on_grid.front(), 0);
  CHECK_EQ(on_grid[1], 120);
  CHECK_EQ(on_grid[2], 240);
  CHECK_EQ(on_grid[3], 360);
  CHECK_EQ(on_grid.back(), 480);

  const auto off_grid = scratch_hold_curve_boundaries(0, 200, timing, 4);
  CHECK_EQ(off_grid.front(), 0);
  CHECK_EQ(off_grid.back(), 200);
  CHECK(std::find(off_grid.begin(), off_grid.end(), 120) != off_grid.end());
  CHECK(std::find(off_grid.begin(), off_grid.end(), 240) == off_grid.end());

  // Denominator 8 halves beat length (480 → 240). Numerator-only 3/4 would not.
  const auto flat = scratch_hold_curve_boundaries(1800, 2200, timing, 4);
  CHECK_EQ(static_cast<int32_t>(flat.size()), 5);
  CHECK(flat == (std::vector<int32_t>{1800, 1920, 2040, 2160, 2200}));
  CHECK(std::find(flat.begin(), flat.end(), 1980) == flat.end());

  timing.points.push_back(TimingPoint{1920, 120.0, 4, 8, false, true});
  normalize_timing_points(timing);
  const std::vector<int32_t> meter_expected{1800, 1920, 1980, 2040, 2100, 2160, 2200};
  const auto meter = scratch_hold_curve_boundaries(1800, 2200, timing, 4);
  CHECK(meter == meter_expected);
  CHECK(meter != flat);
  CHECK(std::find(meter.begin(), meter.end(), 1980) != meter.end());
  CHECK(std::find(meter.begin(), meter.end(), 2100) != meter.end());
  CHECK(scratch_hold_curve_boundaries(1800, 2200, timing, 4) == meter);

  ScratchHoldCurveRequest req;
  req.start_tick = 1800;
  req.end_tick = 2200;
  req.start_center = 0.0;
  req.end_center = 6.0;
  req.width = 1;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;
  const auto generated = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(static_cast<int32_t>(generated.size()), 6);
  CHECK_EQ(generated[0].start_tick, 1800);
  CHECK_EQ(generated[1].start_tick, 1920);
  CHECK_EQ(generated[2].start_tick, 1980);
  CHECK_EQ(generated.back().end_tick, 2200);
  CHECK(generate_scratch_hold_curve(req, timing).size() == generated.size());
}

void test_scratch_hold_curve_lane_clamping() {
  CHECK_EQ(scratch_hold_curve_left_lane(0.0, 1.5, 8.5, 2, 12, EasingAlgorithm::Linear,
                                       EasingDirection::In, 0.0),
           1);
  CHECK_EQ(scratch_hold_curve_left_lane(1.0, 1.5, 8.5, 2, 12, EasingAlgorithm::Linear,
                                       EasingDirection::In, 0.0),
           8);
  CHECK_EQ(scratch_hold_curve_left_lane(1.0, 1.5, 20.0, 2, 12, EasingAlgorithm::Linear,
                                       EasingDirection::In, 0.0),
           10);
  CHECK_EQ(scratch_hold_curve_left_lane(0.0, -4.0, 3.0, 3, 12, EasingAlgorithm::Linear,
                                       EasingDirection::In, 0.0),
           0);
  CHECK_EQ(scratch_hold_curve_left_lane(0.5, 0.0, 4.0, 1, 12, EasingAlgorithm::Poly,
                                       EasingDirection::In, 1.0),
           1);
}

void test_scratch_hold_curve_one_and_multiple_segments() {
  const MusicTiming timing = make_curve_timing_4_4();
  ScratchHoldCurveRequest req;
  req.start_tick = 15;
  req.end_tick = 90;
  req.start_center = 2.5;
  req.end_center = 6.5;
  req.width = 2;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;
  req.algorithm = EasingAlgorithm::Linear;
  req.direction = EasingDirection::In;

  const auto one = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(static_cast<int32_t>(one.size()), 1);
  CHECK_EQ(one[0].start_tick, 15);
  CHECK_EQ(one[0].end_tick, 90);
  CHECK_EQ(one[0].lane, 6);
  CHECK_EQ(one[0].width, 2);
  CHECK_EQ(one[0].scratch_length, 0);
  const auto one_cover = get_scratch_end_lane_range(one[0]);
  CHECK_EQ(one_cover.first, 6);
  CHECK_EQ(one_cover.second, 7);

  req.start_tick = 0;
  req.end_tick = 480;
  req.start_center = 0.0;
  req.end_center = 4.0;
  req.width = 1;
  const auto many = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(static_cast<int32_t>(many.size()), 4);
  CHECK_EQ(many[0].lane, 0);
  CHECK_EQ(many[1].lane, 1);
  CHECK_EQ(many[2].lane, 3);
  CHECK_EQ(many[3].lane, 4);
  CHECK_EQ(many[0].start_tick, 0);
  CHECK_EQ(many[0].end_tick, 120);
  CHECK_EQ(many[3].end_tick, 480);
  CHECK_EQ(many[3].scratch_length, 0);
  for (size_t i = 0; i + 1 < many.size(); ++i) {
    CHECK_EQ(many[i].end_tick, many[i + 1].start_tick);
    NotationNote prev = many[i];
    prev.scratch_length = 0;
    NotationNote next = many[i + 1];
    next.scratch_length = 0;
    sync_scratch_chain_joint(prev, next);
    CHECK_EQ(many[i].scratch_length, prev.scratch_length);
  }

  req.start_center = 2.5;
  req.end_center = 6.5;
  req.width = 2;
  const auto wide = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(static_cast<int32_t>(wide.size()), 4);
  CHECK_EQ(wide[0].lane, 2);
  CHECK_EQ(wide[1].lane, 3);
  CHECK_EQ(wide[2].lane, 5);
  CHECK_EQ(wide[3].lane, 6);
  CHECK_EQ(wide.back().scratch_length, 0);
  for (size_t i = 0; i + 1 < wide.size(); ++i) {
    NotationNote prev = wide[i];
    prev.scratch_length = 0;
    NotationNote next = wide[i + 1];
    next.scratch_length = 0;
    sync_scratch_chain_joint(prev, next);
    CHECK_EQ(wide[i].scratch_length, prev.scratch_length);
  }
}

bool note_fields_eq(const NotationNote& a, const NotationNote& b) {
  return a.id == b.id && a.start_tick == b.start_tick && a.end_tick == b.end_tick &&
         a.note_type == b.note_type && a.lane == b.lane && a.width == b.width &&
         a.gimmick_type == b.gimmick_type && a.scratch_length == b.scratch_length;
}

bool note_vectors_eq(const std::vector<NotationNote>& a, const std::vector<NotationNote>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!note_fields_eq(a[i], b[i])) return false;
  }
  return true;
}

int count_type_in(const std::vector<NotationNote>& notes, NoteType type) {
  int n = 0;
  for (const auto& note : notes) {
    if (note.note_type == type) ++n;
  }
  return n;
}

NotationNote make_scratch_body(int32_t id, int32_t start, int32_t end, int32_t lane,
                               int32_t width) {
  NotationNote note;
  note.id = id;
  note.start_tick = start;
  note.end_tick = end;
  note.lane = lane;
  note.width = width;
  note.note_type = NoteType::ScratchHold;
  return note;
}

void test_scratch_hold_curve_commit_rejects_empty() {
  ScratchHoldCurveCommitInput input;
  input.ticks_per_quarter = 480;
  CHECK(!build_scratch_hold_curve_commit(input).has_value());
}

void test_scratch_hold_curve_commit_new_chain_head_and_tail() {
  const MusicTiming timing = make_curve_timing_4_4();
  ScratchHoldCurveRequest req;
  req.start_tick = 0;
  req.end_tick = 480;
  req.start_center = 0.0;
  req.end_center = 4.0;
  req.width = 1;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;
  const auto bodies = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(static_cast<int32_t>(bodies.size()), 4);
  CHECK_EQ(bodies.back().end_tick, 480);

  ScratchHoldCurveCommitInput input;
  input.generated_bodies = bodies;
  input.include_head = true;
  input.ticks_per_quarter = 480;
  const auto commit = build_scratch_hold_curve_commit(input);
  CHECK(commit.has_value());
  CHECK(commit->before.empty());
  CHECK_EQ(count_type_in(commit->after, NoteType::ScratchHold), 4);
  CHECK_EQ(count_type_in(commit->after, NoteType::ScratchHoldStart), 1);
  int32_t last_end = -1;
  for (const auto& note : commit->after) {
    if (note.note_type != NoteType::ScratchHold) continue;
    last_end = std::max(last_end, note.end_tick);
    CHECK(note.end_tick <= 480);
  }
  CHECK_EQ(last_end, 480);

  ChartDocument doc;
  doc.set_timing(timing);
  EditHistory history;
  CHECK(history.execute(
      std::make_unique<SetNotesCommand>(commit->before, commit->after, "Place curve hold"), doc));
  CHECK_EQ(history.can_undo(), true);
  CHECK_EQ(history.can_redo(), false);
  const auto after_notes = doc.notes();
  CHECK(history.undo(doc));
  CHECK(note_vectors_eq(doc.notes(), commit->before));
  CHECK_EQ(history.can_undo(), false);
  CHECK_EQ(history.can_redo(), true);
  CHECK(history.redo(doc));
  CHECK(note_vectors_eq(doc.notes(), after_notes));
}

void test_scratch_hold_curve_commit_extend_updates_prev_no_second_head() {
  const MusicTiming timing = make_curve_timing_4_4();
  NotationNote prev = make_scratch_body(1, 0, 480, 2, 1);
  NotationNote head;
  head.id = 2;
  head.start_tick = 0;
  head.end_tick = 0;
  head.lane = 2;
  head.width = 1;
  head.note_type = NoteType::ScratchHoldStart;

  ScratchHoldCurveRequest req;
  req.start_tick = 480;
  req.end_tick = 960;
  req.start_center = 2.0;
  req.end_center = 6.0;
  req.width = 1;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;
  const auto bodies = generate_scratch_hold_curve(req, timing);
  CHECK(!bodies.empty());
  CHECK_EQ(bodies.back().end_tick, 960);

  ScratchHoldCurveCommitInput input;
  input.notes = {head, prev};
  input.generated_bodies = bodies;
  input.previous_body_id = 1;
  input.previous_body_original = prev;
  input.include_head = false;
  input.ticks_per_quarter = 480;
  const auto commit = build_scratch_hold_curve_commit(input);
  CHECK(commit.has_value());
  CHECK_EQ(count_type_in(commit->after, NoteType::ScratchHoldStart), 1);
  CHECK_EQ(count_type_in(commit->after, NoteType::ScratchHold),
           1 + static_cast<int>(bodies.size()));

  const NotationNote* updated_prev = nullptr;
  for (const auto& note : commit->after) {
    if (note.id == 1) updated_prev = &note;
  }
  CHECK(updated_prev != nullptr);
  NotationNote expected_prev = prev;
  sync_scratch_chain_joint(expected_prev, bodies.front());
  CHECK_EQ(updated_prev->scratch_length, expected_prev.scratch_length);
  CHECK(updated_prev->gimmick_type == expected_prev.gimmick_type);
  const auto cover = get_scratch_end_lane_range(*updated_prev);
  CHECK_EQ(cover.first, std::min(prev.lane, bodies.front().lane));
  CHECK_EQ(cover.second, std::max(prev.end_lane(), bodies.front().end_lane()));

  ChartDocument doc;
  doc.set_timing(timing);
  CHECK(doc.set_notes({head, prev}));
  auto seeded = with_recomputed_hold_eighths(doc.notes(), prev, 480);
  CHECK(count_type_in(seeded, NoteType::HoldEighth) > 0);
  CHECK(doc.set_notes(seeded));
  ScratchHoldCurveRequest exec_req = req;
  exec_req.subdivisions_per_beat = 1;
  input.notes = doc.notes();
  input.generated_bodies = generate_scratch_hold_curve(exec_req, timing);
  CHECK_EQ(static_cast<int32_t>(input.generated_bodies.size()), 1);
  const auto exec_commit = build_scratch_hold_curve_commit(input);
  CHECK(exec_commit.has_value());
  CHECK(count_type_in(exec_commit->after, NoteType::HoldEighth) >
        count_type_in(exec_commit->before, NoteType::HoldEighth));
  EditHistory history;
  CHECK(history.execute(std::make_unique<SetNotesCommand>(
      exec_commit->before, exec_commit->after, "Place curve hold"),
                        doc));
  CHECK_EQ(history.can_undo(), true);
  const auto after_exec = doc.notes();
  CHECK(history.undo(doc));
  CHECK(note_vectors_eq(doc.notes(), exec_commit->before));
  CHECK_EQ(count_type_in(doc.notes(), NoteType::HoldEighth),
           count_type_in(exec_commit->before, NoteType::HoldEighth));
  CHECK(history.redo(doc));
  CHECK(note_vectors_eq(doc.notes(), after_exec));
  CHECK_EQ(count_type_in(doc.notes(), NoteType::HoldEighth),
           count_type_in(after_exec, NoteType::HoldEighth));
}

void test_scratch_hold_curve_commit_hold_eighths_one_command() {
  const MusicTiming timing = make_curve_timing_4_4();
  ScratchHoldCurveRequest req;
  req.start_tick = 0;
  req.end_tick = 960;
  req.start_center = 0.0;
  req.end_center = 0.0;
  req.width = 1;
  req.lane_count = 12;
  req.subdivisions_per_beat = 1;
  const auto bodies = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(static_cast<int32_t>(bodies.size()), 2);

  ScratchHoldCurveCommitInput input;
  input.generated_bodies = bodies;
  input.include_head = true;
  input.ticks_per_quarter = 480;
  const auto commit = build_scratch_hold_curve_commit(input);
  CHECK(commit.has_value());
  CHECK(count_type_in(commit->after, NoteType::HoldEighth) > 0);

  ChartDocument doc;
  doc.set_timing(timing);
  EditHistory history;
  CHECK(history.execute(
      std::make_unique<SetNotesCommand>(commit->before, commit->after, "Place curve hold"), doc));
  const auto after_notes = doc.notes();
  const int eighths = count_note_type(doc, NoteType::HoldEighth);
  CHECK(eighths > 0);
  CHECK(history.undo(doc));
  CHECK_EQ(count_note_type(doc, NoteType::HoldEighth), 0);
  CHECK(note_vectors_eq(doc.notes(), commit->before));
  CHECK(history.redo(doc));
  CHECK_EQ(count_note_type(doc, NoteType::HoldEighth), eighths);
  CHECK(note_vectors_eq(doc.notes(), after_notes));
}

void test_scratch_hold_curve_normalizes_hold_top_centers_not_mouse() {
  const MusicTiming timing = make_curve_timing_4_4();
  ScratchHoldCurveRequest req;
  req.start_tick = 0;
  req.end_tick = 480;
  req.start_center = 0.0;
  req.end_center = 5.4;  // raw mouse; last hold snaps to lane 5
  req.width = 1;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;
  req.algorithm = EasingAlgorithm::Linear;
  req.direction = EasingDirection::In;

  const auto notes = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(static_cast<int32_t>(notes.size()), 4);
  CHECK_EQ(notes.front().end_tick, 120);
  CHECK_EQ(notes.back().end_tick, 480);
  CHECK_EQ(notes.front().lane, 0);
  CHECK_EQ(notes.back().lane, 5);

  const double first_top_center = scratch_hold_lane_center(notes.front().lane, req.width);
  const double last_top_center = scratch_hold_lane_center(notes.back().lane, req.width);
  CHECK(std::abs(last_top_center - req.end_center) > 1e-6);

  const int32_t first_top = notes.front().end_tick;
  const int32_t last_top = notes.back().end_tick;
  for (const auto& note : notes) {
    const double t =
        last_top == first_top
            ? 1.0
            : static_cast<double>(note.end_tick - first_top) /
                  static_cast<double>(last_top - first_top);
    CHECK_EQ(note.lane, scratch_hold_curve_left_lane(t, first_top_center, last_top_center,
                                                    req.width, req.lane_count, req.algorithm,
                                                    req.direction, req.parameter));
  }

  // Mouse-aimed lerp 0→5.4 puts body 2 at lane 4; hold-top lerp 0→5 puts it at 3.
  CHECK_EQ(notes[1].lane, 2);
  CHECK_EQ(notes[2].lane, 3);
}

void test_scratch_hold_curve_terminal_end_cap() {
  const MusicTiming timing = make_curve_timing_4_4();
  ScratchHoldCurveRequest req;
  req.start_tick = 0;
  req.end_tick = 480;
  req.start_center = 2.5;
  req.end_center = 6.5;
  req.width = 2;
  req.lane_count = 12;
  req.subdivisions_per_beat = 4;
  req.algorithm = EasingAlgorithm::Linear;
  req.direction = EasingDirection::In;

  const auto notes = generate_scratch_hold_curve(req, timing);
  CHECK_EQ(notes.back().end_tick, 480);
  CHECK_EQ(notes.back().scratch_length, 0);
  const int32_t end_left =
      scratch_hold_curve_left_lane(1.0, req.start_center, req.end_center, req.width, req.lane_count,
                                   req.algorithm, req.direction, req.parameter);
  CHECK_EQ(end_left, 6);
  CHECK_EQ(notes.back().lane, 6);
  const auto cover = get_scratch_end_lane_range(notes.back());
  CHECK_EQ(cover.first, 6);
  CHECK_EQ(cover.second, 7);
}

void test_public_mutation_generation_semantics_unchanged() {
  ChartDocument doc;
  doc.mark_saved();
  const uint64_t g0 = doc.content_generation();
  CHECK_EQ(doc.add_note(note_with_id(1, 0, 0)), 1);
  CHECK_EQ(doc.content_generation(), g0 + 2);

  doc.mark_saved();
  const uint64_t g1 = doc.content_generation();
  CHECK(doc.remove_note(1));
  CHECK_EQ(doc.content_generation(), g1 + 2);

  CHECK_EQ(doc.add_note(note_with_id(2, 0, 1)), 2);
  doc.mark_saved();
  const uint64_t g2 = doc.content_generation();
  MusicTiming timing = doc.timing();
  timing.bpm = 140.0;
  timing.points = {TimingPoint{0, 140.0, 4, 4, true, true}};
  CHECK(doc.set_timing(std::move(timing)));
  CHECK_EQ(doc.content_generation(), g2 + 2);

  doc.mark_saved();
  const uint64_t g3 = doc.content_generation();
  CHECK(doc.set_notes({note_with_id(3, 480, 2)}));
  CHECK_EQ(doc.content_generation(), g3 + 2);

  doc.mark_saved();
  const uint64_t g4 = doc.content_generation();
  doc.rebuild_concurrent_lines();
  CHECK(doc.is_dirty());
  CHECK_EQ(doc.content_generation(), g4 + 1);
}

int main() {
  test_auto_note_id_starts_at_zero();
  test_concurrent_lines_multi_press_only();
  test_hold_head_pairs_but_attached_excludes_head();
  test_scratch_hold_end_lane_encoding();
  test_resolve_end_lane_span_matches_scratch_and_jump();
  test_scratch_hold_jump_scratch_stays_in_lane_bounds();
  test_snap_scratch_chain_next_lane_splits_illegal_zone();
  test_snap_scratch_hold_segment_lane_avoids_both_side_cover();
  test_sync_scratch_chain_joint_exact_union();
  test_easing_formulas_and_directions();
  test_easing_p0_and_invalid_parameters();
  test_easing_endpoints_and_monotonicity();
  test_scratch_hold_curve_empty_ranges();
  test_scratch_hold_curve_meter_aware_boundaries();
  test_scratch_hold_curve_lane_clamping();
  test_scratch_hold_curve_one_and_multiple_segments();
  test_scratch_hold_curve_normalizes_hold_top_centers_not_mouse();
  test_scratch_hold_curve_terminal_end_cap();
  test_scratch_hold_curve_commit_rejects_empty();
  test_scratch_hold_curve_commit_new_chain_head_and_tail();
  test_scratch_hold_curve_commit_extend_updates_prev_no_second_head();
  test_scratch_hold_curve_commit_hold_eighths_one_command();
  test_scratch_hold_segment_horizontal_move_keeps_chain();
  test_scratch_chain_joint_direction();
  test_scratch_hold_chain_requires_exact_jump_scratch_cover();
  test_hold_chain_family_predicates();
  test_occupied_lane_span_includes_hold_jump_scratch();
  test_apply_hold_chain_gimmick_encodes_regular_joint();
  test_regular_hold_chain_and_cross_family_negative();
  test_convert_note_type_preserves_hold_chain_gimmick();
  test_official_and_wdschart_roundtrip_hold_chain_fragment();
  test_generate_scratch_hold_curve_regular_hold_gimmick();
  test_regular_hold_jump_scratch_tail_covers_next_head();
  test_hold_head_suppressed_by_non_body_overlap_not_by_hold_body();
  test_hold_head_partial_overlap_single_free_run();
  test_hold_head_partial_overlap_multiple_free_runs_skipped();
  test_hold_head_partial_overlap_with_prior_hold_tail();
  test_hold_head_ignores_body_eighth_and_star_overlap();
  test_scratch_hold_auto_head_is_scratch_hold_start();
  test_repair_legacy_scratch_hold_heads();
  test_hold_head_pairs_exact_span_and_family();
  test_resolve_convert_target_is_identity();
  test_convert_notes_in_selection_hold_stars();
  test_explicit_note_id_zero();
  test_normalize_for_save_reassigns_zero_based();
  test_normalize_remaps_and_rebinds_star_parents();
  test_save_reload_normalizes_and_reloads();
  test_save_success_preserves_history_and_ids();
  test_replace_file_atomic_preserves_target_on_failure();
  test_load_rejects_huge_notes_count();
  test_load_rejects_lane_width_overflow();
  test_load_rejects_nonfinite_ticks();
  test_measure_ticks_no_hang_near_int_max();
  test_load_rejects_invalid_tpq_and_nonfinite_bpm();
  test_set_timing_rejects_illegal_tpq_atomically();
  test_construct_normalize_fallback_illegal_tpq();
  test_tick_ms_saturates_nan_inf_and_extremes();
  test_negative_offset_legal_tick_and_violation_query();
  test_int32_tick_range_measure_and_snap_no_hang();
  test_note_id_max_and_auto_exhaust_are_atomic();
  test_set_notes_auto_explicit_collision_is_atomic();
  test_load_rejects_explicit_int32_max_note_id();
  test_load_from_chart_renormalizes_unsafe_ids_keeps_sparse();
  test_seconds_to_ticks_large_negative_finite();
  test_set_timing_rejects_negative_tick_atomically();
  test_normalize_clamps_negative_ticks_before_merge();
  test_load_rejects_negative_timing_tick();
  test_legacy_and_missing_tpq_remain_compatible();
  test_tick_ms_hits_saturation_gates();
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
  test_snapshot_sub_ms_timeline_us_survives_rebuild_and_patch();
  test_split_gimmick_range_and_combo_includes_heads();
  test_hold_combo_reference_fixed_cases();
  test_hold_combo_sabotage_detects_wrong_oracle();
  test_hold_combo_production_matches_reference_random();
  test_split_appear_phase_before_start_ms();
  test_official_playfield_visual_lanes_are_six();
  test_official_judge_sprite_pink_peaks_sit_on_track_edges();
  test_official_note_visual_width_subtracts_margin();
  test_official_concurrent_line_is_full_notation_sliced();
  test_official_playfield_judge_ndc_and_perspective();
  test_official_calculate_position_y_matches_il2cpp();
  test_official_setting_value_ranges();
  test_official_hidden_line_and_note_height_defaults();
  test_official_split_tip_span_matches_sprite_cap();
  test_split_fadein_direction_from_linehight_z180();
  test_official_split_fade_in_cubic_and_zero_length();
  test_official_split_fade_in_visible_hits_screen_before_clip_end();
  test_official_split_fade_in_scale_is_playfield_coverage();
  test_official_split_fade_out_is_300ms_smoothstep_from_end();
  test_split_color_slot_mirrors_linehight_z180();
  test_hold_start_visible_with_zero_end_tick();
  test_load_legacy_v1_wdschart();
  test_official_chart_import_and_roundtrip();
  test_official_chart_load_auto();
  test_load_repo_test_official_charts();
  test_wdsproject_format_roundtrip_and_relative_paths();
  test_edit_grid_and_note_operations();
  test_selection_drag_snaps_only_anchor_tick();
  test_timing_bpm_meter_split_and_prune();
  test_truncated_wdschart_rejected();
  test_wdschart_omits_and_ignores_eighths();
  test_official_keeps_file_eighth_and_export_generates();
  test_official_export_sanitizes_eighths_and_sorts_by_time();
  test_official_export_jump_scratch_eighths_stay_plain();
  test_sus_ignores_file_eighth_and_export_generates();
  test_official_csv_tempo_map_export();
  test_official_csv_flick_scratch_length_encodes_width();
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
  test_star_hold_bind_legacy_uniqueness_and_attachment();
  test_load_v5_rebinds_dangling_and_unbound_star_parents();
  test_timing_tick_ms_roundtrip_multi_bpm();
  test_timing_first_bpm_is_song_start_without_tick0();
  test_hold_span_stale_skips_rebuild_without_holds();
  test_apply_note_updates_empty_does_not_change_generation();
  test_apply_note_updates_batch_equivalent_and_generation_plus_one();
  test_apply_note_updates_rejects_duplicate_and_unknown_atomically();
  test_apply_note_updates_forces_note_id();
  test_apply_note_updates_incremental_index_without_hold_rebuild();
  test_apply_note_updates_read_only();
  test_apply_note_updates_hold_span_index_and_concurrent();
  test_update_notes_command_undo_redo_generation();
  test_public_mutation_generation_semantics_unchanged();
  test_sus_channel_reuse_two_holds();
  test_sus_cross_measure_hold_at_bar_head();
  test_sus_spec_example_hold_14002400();
  test_sus_export_headless_hold_body();
  test_sus_export_covered_hold_skips_damage();
  test_sus_export_partial_hold_head_pairs_body();
  test_sus_hold_mid_star_roundtrip();
  test_sus_overlapping_full_width_mids_keep_distinct_parents();
  test_sus_slide_export_not_orphan_hold_start();
  test_sus_standalone_directional_keeps_width();
  test_sus_leftover_air_becomes_flick();
  test_sus_tap_plus_directional_becomes_flick();
  test_sus_ched_slide_air_distinguishes_hold_family();
  test_sus_ched_export_slide_and_end_pair();
  test_sus_ched_mid_flick_splits_jump_scratch();
  test_sus_start_air_suppresses_pink_head();
  test_sus_left_jump_air_binds_scratch_hold();
  test_sus_jump_scratch_chain_joint_no_pink_head();
  test_sus_mid_air_only_splits_jump_scratch();
  test_sus_mid_air_right_span_is_jump_scratch();
  test_sus_start_air_only_unscratches_to_blue_hold();
  test_sus_til01_split_roundtrip();
  test_wdschart_export_import_preserves_chart();
  test_sus_slide_invisible_mid_not_sound();
  test_sus_type3_does_not_copy_to_sibling_hold();
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

#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_renderer.hpp"
#include "wds/ui/regions/status/status_bar.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>
#include <wds/core/types.hpp>
#include <wds/interaction/ui_painter.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __FILE__, __LINE__)

using wds::chart_editor::ChartEditorEngine;
using wds::chart_editor::EditGridConfig;
using wds::chart_editor::MusicTiming;
using wds::chart_editor::NotationNote;
using wds::chart_editor::NoteType;
using wds::chart_editor::PreviewConfig;
using wds::chart_editor::TimingPoint;
using wds::chart_editor::tick_to_milliseconds;
using wds::ui::ChartEditPanel;
using wds::ui::ChartEditRenderer;
using wds::ui::EditViewport;
using wds::ui::StatusLevel;
using wds::ui::UiManager;
using wds::ui::kChartErrorMarkerColor;
using wds::ui::kChartErrorMarkerThickness;

NotationNote make_tap(int32_t tick, int32_t lane, int32_t width = 1) {
  NotationNote note;
  note.start_tick = tick;
  note.lane = lane;
  note.width = width;
  note.note_type = NoteType::Normal;
  return note;
}

bool color_near(const wds::interaction::Color& a, const wds::interaction::Color& b) {
  return std::fabs(a.r - b.r) < 0.02f && std::fabs(a.g - b.g) < 0.02f &&
         std::fabs(a.b - b.b) < 0.02f && std::fabs(a.a - b.a) < 0.05f;
}

int count_error_marker_rects(const wds::interaction::UiPainter& painter, float expected_y) {
  int n = 0;
  for (const auto& rect : painter.rects()) {
    if (!color_near(rect.color, kChartErrorMarkerColor)) continue;
    if (std::fabs(rect.bounds.h - kChartErrorMarkerThickness) > 0.01f) continue;
    const float mid = rect.bounds.y + rect.bounds.h * 0.5f;
    if (std::fabs(mid - expected_y) < 0.75f) ++n;
  }
  return n;
}

struct PanelHarness {
  ChartEditorEngine engine;
  ChartEditPanel panel;

  PanelHarness() : panel(engine) {
    panel.set_bounds({0.0f, 0.0f, 800.0f, 1000.0f});
    EditGridConfig grid;
    grid.ticks_per_quarter = 480;
    grid.visible_hectoms = 40;
    grid.subdivisions_per_beat = 4;
    grid.lane_count = 12;
    panel.set_grid(grid);
    MusicTiming timing;
    timing.bpm = 120.0;
    timing.ticks_per_quarter = 480;
    timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
    engine.document().set_timing(timing);
    panel.sync_to_timeline_ms(0.0);
  }
};

void test_panel_replaces_markers_and_clears_on_generation() {
  PanelHarness h;
  const uint64_t gen = h.engine.document().content_generation();
  h.panel.set_error_ticks({480, 960}, gen);
  CHECK_EQ(static_cast<int>(h.panel.error_ticks().size()), 2);
  CHECK_EQ(h.panel.error_ticks()[0], 480);

  h.panel.set_error_ticks({240}, gen);
  CHECK_EQ(static_cast<int>(h.panel.error_ticks().size()), 1);
  CHECK_EQ(h.panel.error_ticks()[0], 240);

  h.panel.sync_to_timeline_ms(1500.0);
  h.panel.update(0.016f);
  CHECK_EQ(static_cast<int>(h.panel.error_ticks().size()), 1);

  h.panel.update(8.0f);
  CHECK_EQ(static_cast<int>(h.panel.error_ticks().size()), 1);

  h.engine.add_note(make_tap(0, 0));
  h.panel.update(0.016f);
  CHECK(h.panel.error_ticks().empty());
}

void test_panel_empty_check_clears_markers() {
  PanelHarness h;
  const uint64_t gen = h.engine.document().content_generation();
  h.panel.set_error_ticks({480}, gen);
  h.panel.set_error_ticks({}, gen);
  CHECK(h.panel.error_ticks().empty());
}

void test_renderer_draws_visible_markers_without_expiry() {
  EditViewport viewport;
  viewport.set_bounds({0.0f, 0.0f, 240.0f, 480.0f});
  EditGridConfig grid;
  grid.ticks_per_quarter = 480;
  grid.visible_hectoms = 20;
  grid.subdivisions_per_beat = 4;
  grid.lane_count = 12;
  viewport.set_grid(grid);
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  viewport.set_timing(timing);
  viewport.sync_scroll_to_playhead(0);

  ChartEditRenderer renderer;
  PreviewConfig preview;
  std::unordered_set<int32_t> selected;
  const std::vector<int32_t> ticks = {0, 480};
  wds::interaction::UiPainter painter;
  renderer.paint(painter, viewport, timing, {}, preview, selected, std::nullopt, {}, std::nullopt,
                 nullptr, true, -1, ticks);

  CHECK_EQ(count_error_marker_rects(painter, viewport.y_at(0)), 1);
  CHECK_EQ(count_error_marker_rects(painter, viewport.y_at(480)), 1);

  const std::vector<int32_t> offscreen_ticks = {48000};
  wds::interaction::UiPainter offscreen;
  renderer.paint(offscreen, viewport, timing, {}, preview, selected, std::nullopt, {}, std::nullopt,
                 nullptr, true, -1, offscreen_ticks);
  int yellow = 0;
  for (const auto& rect : offscreen.rects()) {
    if (color_near(rect.color, kChartErrorMarkerColor) &&
        std::fabs(rect.bounds.h - kChartErrorMarkerThickness) < 0.01f) {
      ++yellow;
    }
  }
  CHECK_EQ(yellow, 0);
}

void test_check_no_errors_info_and_empty_markers() {
  UiManager ui;
  ui.resize(1600, 900, 1600, 900);
  auto* panel = ui.edit_panel();
  CHECK(panel != nullptr);
  const uint64_t gen = ui.session().engine().document().content_generation();
  panel->set_error_ticks({480}, gen);
  const bool undo_before = ui.session().engine().history().can_undo();
  const bool redo_before = ui.session().engine().history().can_redo();
  const auto notes_before = ui.session().engine().document().notes();

  ui.check_chart_errors();

  CHECK(panel->error_ticks().empty());
  CHECK(ui.status_bar() != nullptr);
  CHECK_EQ(static_cast<int>(ui.status_bar()->level()), static_cast<int>(StatusLevel::Info));
  CHECK(!ui.status_bar()->message().empty());
  CHECK_EQ(ui.session().engine().document().content_generation(), gen);
  CHECK_EQ(ui.session().engine().history().can_undo(), undo_before);
  CHECK_EQ(ui.session().engine().history().can_redo(), redo_before);
  CHECK_EQ(static_cast<int>(ui.session().engine().document().notes().size()),
           static_cast<int>(notes_before.size()));
}

void test_check_errors_pause_seek_warn_and_replace() {
  UiManager ui;
  ui.resize(1600, 900, 1600, 900);
  auto& engine = ui.session().engine();
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  engine.document().set_timing(timing);
  engine.add_note(make_tap(960, 0, 2));
  engine.add_note(make_tap(960, 1, 1));
  engine.add_note(make_tap(240, 4, 2));
  engine.add_note(make_tap(240, 5, 1));

  const uint64_t gen = engine.document().content_generation();
  ui.edit_panel()->set_error_ticks({0}, gen);
  engine.play();
  CHECK(engine.playback_state() == wds::common::PlaybackState::Playing);

  const bool undo_before = engine.history().can_undo();
  ui.check_chart_errors();

  CHECK_EQ(engine.document().content_generation(), gen);
  CHECK_EQ(engine.history().can_undo(), undo_before);
  CHECK(engine.playback_state() == wds::common::PlaybackState::Paused);
  const int64_t expected_ms = tick_to_milliseconds(240, engine.document().timing());
  CHECK_EQ(engine.timeline_ms(), expected_ms);
  CHECK(ui.status_bar() != nullptr);
  CHECK_EQ(static_cast<int>(ui.status_bar()->level()), static_cast<int>(StatusLevel::Warning));
  CHECK(ui.status_bar()->message().find("2") != std::string::npos ||
        ui.status_bar()->message().find("240") != std::string::npos);

  auto* panel = ui.edit_panel();
  CHECK_EQ(static_cast<int>(panel->error_ticks().size()), 2);
  CHECK_EQ(panel->error_ticks()[0], 240);
  CHECK_EQ(panel->error_ticks()[1], 960);

  engine.document().set_notes({make_tap(480, 0, 2), make_tap(480, 1, 1)});
  const uint64_t gen2 = engine.document().content_generation();
  ui.check_chart_errors();
  CHECK_EQ(engine.document().content_generation(), gen2);
  CHECK_EQ(static_cast<int>(panel->error_ticks().size()), 1);
  CHECK_EQ(panel->error_ticks()[0], 480);
}

void test_check_warning_counts_unique_ticks_as_locations() {
  UiManager ui;
  ui.resize(1600, 900, 1600, 900);
  auto& engine = ui.session().engine();
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  engine.document().set_timing(timing);
  engine.add_note(make_tap(100, 2, 3));
  engine.add_note(make_tap(100, 1, 3));
  engine.add_note(make_tap(100, 3, 2));

  ui.check_chart_errors();
  CHECK(ui.status_bar() != nullptr);
  const std::string msg = ui.status_bar()->message();
  CHECK(msg.find("1 处") != std::string::npos);
  CHECK(msg.find("3 处") == std::string::npos);
  CHECK(msg.find("100") != std::string::npos);
  CHECK_EQ(static_cast<int>(ui.edit_panel()->error_ticks().size()), 1);
}

void test_same_tick_multiple_notes_draw_one_yellow_line() {
  UiManager ui;
  ui.resize(1600, 900, 1600, 900);
  auto& engine = ui.session().engine();
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
  engine.document().set_timing(timing);
  engine.add_note(make_tap(100, 2, 3));
  engine.add_note(make_tap(100, 1, 3));
  engine.add_note(make_tap(100, 3, 2));
  ui.check_chart_errors();

  auto* panel = ui.edit_panel();
  CHECK(panel != nullptr);
  CHECK_EQ(static_cast<int>(panel->error_ticks().size()), 1);
  CHECK_EQ(panel->error_ticks()[0], 100);

  wds::interaction::UiPainter painter;
  panel->paint(painter);
  const float y = panel->viewport().y_at(100);
  CHECK_EQ(count_error_marker_rects(painter, y), 1);
}

}  // namespace

int main() {
  test_panel_replaces_markers_and_clears_on_generation();
  test_panel_empty_check_clears_markers();
  test_renderer_draws_visible_markers_without_expiry();
  test_check_no_errors_info_and_empty_markers();
  test_check_errors_pause_seek_warn_and_replace();
  test_check_warning_counts_unique_ticks_as_locations();
  test_same_tick_multiple_notes_draw_one_yellow_line();

  if (g_failures == 0) {
    std::printf("All chart_validation UI tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", g_failures);
  return 1;
}

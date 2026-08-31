#include "wds/common/log.hpp"
#include "wds/ui/frame_diag.hpp"
#include "wds/ui/layout/editor_layout.hpp"
#include "wds/ui/regions/edit/edit_viewport.hpp"

#include <wds/core/official_playfield.hpp>
#include "wds/ui/window.hpp"
#include "wds/ui/regions/preview/preview_hit_widget.hpp"
#define WDS_UI_PLAYBACK_PREVIEW_HELPERS_ONLY
#include "wds/ui/regions/preview/playback_preview.hpp"
#include "wds/ui/timeline_wheel.hpp"

#include "wds/interaction/widget_root.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <unordered_set>

int main() {
  wds::ui::EditViewport viewport;
  viewport.set_bounds({0, 0, 120, 240});
  wds::chart_editor::EditGridConfig grid;
  grid.ticks_per_quarter = 480;
  // 10 hectoms = 1000 ms. At 120 BPM / TPQ 480 → 1 beat = 500 ms → 2 beats = 960 ticks.
  grid.visible_hectoms = 10;
  grid.subdivisions_per_beat = 4;
  grid.lane_count = 12;
  viewport.set_grid(grid);

  wds::chart_editor::MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {{0, 120.0, 4, 4}};
  viewport.set_timing(timing);

  // Mid-screen maps to mid-range in time; top is later, bottom is earlier.
  assert(viewport.tick_at(120) == 480);
  assert(viewport.tick_at(0) == 960);
  assert(viewport.tick_at(240) == 0);
  assert(std::abs(viewport.y_at(960) - 0.0f) < 0.01f);
  assert(std::abs(viewport.y_at(0) - 240.0f) < 0.01f);

  // Width-1: snap to nearest lane center (lane i centered at i+0.5 in edge coords).
  assert(viewport.lane_at(55) == 5);
  assert(viewport.lane_at(59) == 5);
  assert(viewport.lane_at(65) == 6);
  assert(viewport.lane_at(60, 3) == 5);
  assert(viewport.lane_at(65, 3) == 5);
  assert(viewport.lane_at(119, 3) == 9);

  // Official tap/hold visual width: inset follows (notation − visual) / 2, not 1px.
  // Always-on (Release strips assert).
  {
    using wds::chart_editor::official_hold_line_visual_width;
    using wds::chart_editor::official_note_width;
    using wds::chart_editor::official_tap_visual_width;
    auto require = [](bool ok, const char* msg) {
      if (!ok) {
        std::fprintf(stderr, "FAIL edit visual width: %s\n", msg);
        std::abort();
      }
    };
    const float one = viewport.lane_width(1);
    const float notation = official_note_width(1);
    const float tap_visual = official_tap_visual_width(notation);
    const float hold_visual = official_hold_line_visual_width(notation);
    const float tap_inset = (one - one * (tap_visual / notation)) * 0.5f;
    const float hold_inset = (one - one * (hold_visual / notation)) * 0.5f;
    require(std::abs(viewport.note_inset_px(1) - tap_inset) < 1e-4f, "1-lane tap inset");
    require(std::abs(viewport.hold_inset_px(1) - hold_inset) < 1e-4f, "1-lane hold inset");
    require(viewport.hold_inset_px(1) < viewport.note_inset_px(1), "hold inset < tap inset");
    require(std::abs(viewport.tap_visual_world_width(1) - tap_visual) < 1e-6f, "tap world");
    require(std::abs(viewport.hold_visual_world_width(1) - hold_visual) < 1e-6f, "hold world");

    const float four = viewport.lane_width(4);
    const float n4 = official_note_width(4);
    const float v4 = official_tap_visual_width(n4);
    require(std::abs(viewport.note_inset_px(4) - (four - four * (v4 / n4)) * 0.5f) < 1e-4f,
            "4-lane tap inset");
    require(viewport.note_inset_px(4) / four < viewport.note_inset_px(1) / one,
            "4-lane inset fraction smaller");
  }

  // Scroll forward by 250 ms → +240 ticks at 120 BPM.
  viewport.scroll_by_ms(250.0f);
  assert(viewport.tick_at(0) == 1200);
  assert(viewport.tick_at(240) == 240);

  // Playhead at tick 480 (500 ms) sits on the judgment line.
  viewport.sync_scroll_to_playhead(480);
  assert(std::abs(viewport.y_at(480) - viewport.judgeline_y()) < 0.01f);
  assert(std::abs(viewport.judgeline_y() - 216.0f) < 0.01f);

  // At chart start: tick 0 sits on the judgeline; blank is below the line.
  viewport.sync_scroll_to_playhead(0);
  assert(viewport.scroll_ms() < 0.0f);
  assert(std::abs(viewport.scroll_ms() - viewport.min_scroll_ms()) < 0.01f);
  assert(std::abs(viewport.y_at(0) - viewport.judgeline_y()) < 0.01f);
  viewport.set_scroll_ms(viewport.min_scroll_ms() - 1000.0f);
  assert(std::abs(viewport.scroll_ms() - viewport.min_scroll_ms()) < 0.01f);
  assert(viewport.tick_at(240) == 0);

  // No time remapping / ease: 1:1 scroll, identity preview mapping.
  {
    using wds::chart_editor::EditLeadIn;
    const int64_t visible = viewport.visible_ms();
    assert(EditLeadIn::judgeline_chart_ms(0.0, visible) == 0.0);
    assert(EditLeadIn::preview_chart_ms(500, visible) == 500);
    assert(EditLeadIn::transport_ms_for_chart_ms(0, visible) == 0);
    assert(EditLeadIn::transport_ms_for_chart_ms(100, visible) == 100);

    const float s0 = viewport.scroll_ms_for_playhead(0.0f);
    const float s1 = viewport.scroll_ms_for_playhead(1.0f);
    assert(std::abs(s1 - (s0 + 1.0f)) < 0.01f);
    viewport.sync_scroll_to_playhead_ms(250.0);
    assert(std::abs(viewport.y_at_ms(250.0f) - viewport.judgeline_y()) < 0.01f);
  }

  // Higher BPM packs the same tick gaps denser (less vertical px per tick).
  viewport.set_scroll_ms(0.0f);
  const float y_gap_120 = std::abs(viewport.y_at(480) - viewport.y_at(0));
  timing.bpm = 240.0;
  timing.points = {{0, 240.0, 4, 4}};
  viewport.set_timing(timing);
  const float y_gap_240 = std::abs(viewport.y_at(480) - viewport.y_at(0));
  assert(y_gap_240 < y_gap_120 * 0.55f);
  assert(y_gap_240 > y_gap_120 * 0.45f);

  // Official preview canvas is 16:9 (PlayerSettings 1280×720).
  {
    wds::ui::EditorLayouter layouter;
    const auto official = layouter.compute(1280, 720);
    assert(std::fabs(wds::ui::EditorLayouter::kPreviewAspect - 16.0f / 9.0f) < 1e-6f);
    assert(std::fabs(static_cast<float>(official.preview_content.width) /
                         std::max(1, official.preview_content.height) -
                     16.0f / 9.0f) < 0.03f);
  }

  assert(wds::ui::kDefaultWindowWidth == 1280);
  assert(wds::ui::kDefaultWindowHeight == 800);
  {
    namespace th = wds::interaction::theme;
    wds::ui::EditorLayouter layouter;
    const auto def = layouter.compute(1280, 800);
    assert(def.regions.preview.x == 0.0f);
    assert(def.regions.toolbar.y + 0.01f >= def.regions.settings.y + def.regions.settings.h);
    assert(def.regions.edit.x + 0.01f >= def.regions.preview.w);
    assert(def.regions.status.y + 0.01f >= def.regions.edit.h);
    const float left_w = def.regions.toolbar.w;
    const float icon = wds::ui::estimate_toolbar_icon_px(left_w);
    assert(icon > th::kMinIconPx + 0.5f);
    const float icon_block = icon * 2.0f + th::kUiGap * 2.0f;
    assert(def.regions.toolbar.h + 0.01f >= icon_block + th::kControlHeight * 4.0f);
    assert(def.regions.toolbar.h + def.regions.settings.h + 0.01f >=
           icon_block + th::kUiPad * 4.0f + th::kControlHeight * 7.0f);
    const wds::ui::LeftColumnMetrics col =
        wds::ui::compute_left_column_metrics(left_w, def.regions.settings.h + def.regions.toolbar.h);
    assert(std::fabs(col.icon - icon) < 0.01f);
    assert(std::fabs(col.icon_block_h - icon_block) < 0.01f);
  }
  {
    wds::ui::EditorLayouter layouter;
    const auto crushed = layouter.compute(1280, 480);
    assert(crushed.regions.toolbar.h > 1.0f);
    assert(crushed.regions.settings.h > 1.0f);
    assert(crushed.regions.preview.bottom() <= crushed.regions.settings.y + 0.01f);
    assert(crushed.regions.settings.bottom() <= crushed.regions.toolbar.y + 0.01f);
  }

  // Shared wheel math: 20 hectoms / 1x / +1 notch → -100 ms; scales with range and speed.
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, 1.0f) == -100);
  assert(wds::ui::timeline_scrub_delta_ms(-1.0f, 20, 1.0f) == 100);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 40, 1.0f) == -200);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 10, 1.0f) == -50);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, 2.0f) == -200);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, 0.5f) == -50);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, 0.25f) == -25);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, 3.0f) == -300);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, -1.0f) == -25);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, 0.0f) == -25);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, std::numeric_limits<float>::quiet_NaN()) ==
         -100);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 20, std::numeric_limits<float>::infinity()) ==
         -100);
  assert(wds::ui::timeline_scrub_delta_ms(0.4f, 20, 1.0f) == -40);
  assert(wds::ui::timeline_scrub_delta_ms(0.25f, 20, 1.0f) == -25);
  assert(wds::ui::timeline_scrub_delta_ms(1.0f, 0, 1.0f) == -5);
  assert(wds::ui::timeline_scrub_delta_ms(0.0f, 20, 1.0f) == 0);

  // Primary+wheel visible range: undo global invert, apply dedicated invert, min 1 step.
  assert(wds::ui::visible_range_after_wheel(1.0f, 20, false, false) == 19);
  assert(wds::ui::visible_range_after_wheel(-1.0f, 20, false, false) == 21);
  assert(wds::ui::visible_range_after_wheel(2.6f, 20, false, false) == 17);
  assert(wds::ui::visible_range_after_wheel(-2.6f, 20, false, false) == 23);
  assert(wds::ui::visible_range_after_wheel(0.25f, 20, false, false) == 19);
  assert(wds::ui::visible_range_after_wheel(-0.25f, 20, false, false) == 21);
  assert(wds::ui::visible_range_after_wheel(0.0f, 20, false, false) == 20);
  assert(wds::ui::visible_range_after_wheel(1e-7f, 20, false, false) == 20);
  assert(wds::ui::visible_range_after_wheel(1.0f, 20, true, false) == 21);
  assert(wds::ui::visible_range_after_wheel(1.0f, 20, false, true) == 21);
  assert(wds::ui::visible_range_after_wheel(1.0f, 20, true, true) == 19);
  assert(wds::ui::visible_range_after_wheel(1.0f, 1, false, false) == 1);
  assert(wds::ui::visible_range_after_wheel(-1.0f, 1000, false, false) == 1000);
  assert(wds::ui::visible_range_after_wheel(10.0f, 5, false, false) == 1);

  // PreviewHitWidget: WidgetRoot routes scroll inside bounds; misses outside.
  {
    using wds::interaction::ScrollEvent;
    using wds::interaction::WidgetRoot;
    using wds::ui::PreviewHitWidget;

    WidgetRoot root;
    root.set_bounds({0, 0, 400, 300});
    auto preview = std::make_unique<PreviewHitWidget>();
    PreviewHitWidget* hit = preview.get();
    hit->set_bounds({10, 10, 200, 100});
    int calls = 0;
    ScrollEvent last{};
    hit->set_scroll_handler([&](const ScrollEvent& event) {
      ++calls;
      last = event;
    });
    root.add_child(std::move(preview));

    const ScrollEvent inside{{50, 50}, 0.25f, 1.5f, {true, true, false, false}};
    root.process_frame(0.016f, {inside});
    assert(calls == 1);
    assert(last.position.x == inside.position.x);
    assert(last.position.y == inside.position.y);
    assert(last.delta_x == inside.delta_x);
    assert(last.delta_y == inside.delta_y);
    assert(last.mods == inside.mods);

    root.process_frame(0.016f, {ScrollEvent{{210, 50}, 0.0f, 1.0f, {}}});
    root.process_frame(0.016f, {ScrollEvent{{50, 110}, 0.0f, 1.0f, {}}});
    root.process_frame(0.016f, {ScrollEvent{{5, 50}, 0.0f, 1.0f, {}}});
    root.process_frame(0.016f, {ScrollEvent{{300, 200}, 0.0f, 1.0f, {}}});
    assert(calls == 1);

    hit->set_visible(false);
    root.process_frame(0.016f, {inside});
    assert(calls == 1);
    hit->set_visible(true);
    hit->set_enabled(false);
    root.process_frame(0.016f, {inside});
    assert(calls == 1);
  }

  // Frame diag: only exact "1" enables. Independent of WDS_ENABLE_LOGGING.
  {
    using wds::ui::frame_diag_enable_contract;
    using wds::ui::frame_diag_env_enabled;

    assert(frame_diag_env_enabled("1"));
    const auto enabled = frame_diag_enable_contract("1");
    assert(enabled.sample_timing);
    assert(enabled.open_log_file);
    assert(enabled.emit_output);

    const char* rejected[] = {
        nullptr, "",  "0",   "2",    "01",  "10",   "1 ",   " 1",    "1\n",  "1\t",
        "1\r",   "+", "1.0", "-1",   "true", "TRUE", "True", "yes",   "YES",  "Yes",
        "on",    "ON", "On",  "false", "FALSE", "one", "enabled", "debug", " 1 ",
    };
    for (const char* value : rejected) {
      assert(!frame_diag_env_enabled(value));
      const auto contract = frame_diag_enable_contract(value);
      assert(!contract.sample_timing);
      assert(!contract.open_log_file);
      assert(!contract.emit_output);
    }

    // Compile-time logging must not imply frame diagnostics.
    (void)WDS_ENABLE_LOGGING;
    assert(!frame_diag_env_enabled(nullptr));
    assert(frame_diag_env_enabled("1"));
  }

  // Music-clock SFX: schedule factory runs only on first mark; failure erases for retry.
  {
    using wds::ui::commit_hit_sfx_schedule;
    std::unordered_set<uint64_t> played;
    const uint64_t key = 0x100000002ull;
    int arms = 0;

    assert(!commit_hit_sfx_schedule(played, key, [&] {
      ++arms;
      return false;
    }));
    assert(arms == 1);
    assert(played.count(key) == 0);

    assert(commit_hit_sfx_schedule(played, key, [&] {
      ++arms;
      return true;
    }));
    assert(arms == 2);
    assert(played.count(key) == 1);

    assert(!commit_hit_sfx_schedule(played, key, [&] {
      ++arms;
      return true;
    }));
    assert(arms == 2);
    assert(played.count(key) == 1);
  }
}

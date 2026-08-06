#include "wds/ui/regions/edit/edit_viewport.hpp"

#include <cassert>
#include <cmath>

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
}

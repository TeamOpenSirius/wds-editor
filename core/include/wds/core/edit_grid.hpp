#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wds::chart_editor {

struct EditGridConfig {
  int32_t ticks_per_quarter = 480;
  // Visible edit window height in units of 100 ms (integer; no fractional hectoms).
  int32_t visible_hectoms = 20;
  int32_t subdivisions_per_beat = 4;
  int32_t lane_count = 12;
};

// Edit/preview time geometry. The playhead is always on the judgeline at 1:1
// wall-clock speed — no lead-in ease (eases change scroll speed and desync).
// At transport t=0, chart t=0 sits on the judgeline; blank is the spatial band
// between the judgeline and the window bottom.
struct EditLeadIn {
  static constexpr float kJudgelineMarginBottom = 0.10f;
  static constexpr int32_t kMsPerHectom = 100;

  static int32_t visible_ms_from_hectoms(int32_t visible_hectoms) noexcept {
    return std::max(1, visible_hectoms) * kMsPerHectom;
  }

  // Identity: no time remapping (kept so call sites stay stable).
  static double judgeline_chart_ms(double now_ms, int64_t /*visible_ms*/) noexcept {
    return now_ms;
  }

  static int64_t preview_chart_ms(int64_t now_ms, int64_t visible_ms) noexcept {
    return static_cast<int64_t>(
        std::llround(judgeline_chart_ms(static_cast<double>(now_ms), visible_ms)));
  }

  static int64_t preview_chart_us(int64_t now_us, int64_t visible_ms) noexcept {
    const double now_ms = static_cast<double>(now_us) / 1000.0;
    return static_cast<int64_t>(std::llround(judgeline_chart_ms(now_ms, visible_ms) * 1000.0));
  }

  static int64_t transport_ms_for_chart_ms(int64_t chart_ms, int64_t /*visible_ms*/) noexcept {
    return chart_ms;
  }
};

int32_t round_to_int_tick(float tick);
// Approximate floor(TPQ/subdivs) for nudge / min-hold duration.
int32_t subdivision_tick_step(const EditGridConfig& cfg);
// Snap to nearest in-beat subdivision using (i*TPQ)/subdivs (exact on beat edges).
int32_t snap_tick(float tick, const EditGridConfig& cfg);
int32_t snap_lane(float lane_center, const EditGridConfig& cfg);

// Tick delta for a multi-note drag: snap the grabbed note onto the subdivision
// grid, then add the pointer's grid delta. Apply this same value to every note
// so followers keep their offset from the grabbed note.
int32_t selection_drag_tick_delta(int32_t anchor_start_tick, int32_t pointer_delta_tick,
                                  const EditGridConfig& cfg) noexcept;
int32_t clamp_lane_for_width(int32_t lane, int32_t width, int32_t lane_count);
bool lane_in_bounds(int32_t lane, int32_t width, int32_t lane_count);
std::vector<int32_t> beat_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                         const EditGridConfig& cfg);
std::vector<int32_t> subdivision_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                                const EditGridConfig& cfg);

}  // namespace wds::chart_editor

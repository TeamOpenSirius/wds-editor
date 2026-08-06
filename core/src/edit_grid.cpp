#include <wds/core/edit_grid.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {

int32_t round_to_int_tick(float tick) {
  return static_cast<int32_t>(std::lround(tick));
}

int32_t subdivision_tick_step(const EditGridConfig& cfg) {
  // Approximate average step for nudge / min-hold. Exact in-beat positions use
  // multiply-then-divide (see snap_tick / subdivision_ticks_in_range).
  if (cfg.ticks_per_quarter <= 0 || cfg.subdivisions_per_beat <= 0) {
    return 1;
  }
  return std::max(1, cfg.ticks_per_quarter / cfg.subdivisions_per_beat);
}

int32_t snap_tick(float tick, const EditGridConfig& cfg) {
  if (cfg.ticks_per_quarter <= 0 || cfg.subdivisions_per_beat <= 0) {
    return std::max(0, round_to_int_tick(tick));
  }
  const int32_t beat = std::max(1, cfg.ticks_per_quarter);
  const int32_t subdivs = std::max(1, cfg.subdivisions_per_beat);
  const int32_t raw = std::max(0, round_to_int_tick(tick));
  const int32_t beat_start = (raw / beat) * beat;
  const int32_t within = raw - beat_start;
  const int32_t i = static_cast<int32_t>(
      (static_cast<int64_t>(within) * subdivs + beat / 2) / beat);
  return beat_start + static_cast<int32_t>(
                          (static_cast<int64_t>(std::min(i, subdivs)) * beat) / subdivs);
}

int32_t snap_lane(float lane_center, const EditGridConfig& cfg) {
  if (cfg.lane_count <= 0) {
    return 0;
  }
  return std::clamp(round_to_int_tick(lane_center), 0, cfg.lane_count - 1);
}

bool lane_in_bounds(int32_t lane, int32_t width, int32_t lane_count) {
  return width > 0 && lane >= 0 && lane_count > 0 && lane <= lane_count - width;
}

int32_t clamp_lane_for_width(int32_t lane, int32_t width, int32_t lane_count) {
  if (lane_count <= 0 || width <= 0 || width > lane_count) {
    return 0;
  }
  return std::clamp(lane, 0, lane_count - width);
}

std::vector<int32_t> beat_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                         const EditGridConfig& cfg) {
  std::vector<int32_t> ticks;
  if (end_tick < start_tick || cfg.ticks_per_quarter <= 0) {
    return ticks;
  }
  const int32_t step = cfg.ticks_per_quarter;
  const int64_t first = static_cast<int64_t>(
      std::ceil(static_cast<double>(start_tick) / static_cast<double>(step))) *
                        step;
  for (int64_t tick = first; tick <= end_tick; tick += step) {
    ticks.push_back(static_cast<int32_t>(tick));
  }
  return ticks;
}

std::vector<int32_t> subdivision_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                                const EditGridConfig& cfg) {
  std::vector<int32_t> ticks;
  if (end_tick < start_tick || cfg.ticks_per_quarter <= 0 ||
      cfg.subdivisions_per_beat <= 0) {
    return ticks;
  }
  const int32_t beat = std::max(1, cfg.ticks_per_quarter);
  const int32_t subdivs = std::max(1, cfg.subdivisions_per_beat);
  int64_t beat_start = (static_cast<int64_t>(std::max(0, start_tick)) / beat) * beat;
  if (beat_start > start_tick && beat_start >= beat) beat_start -= beat;
  for (; beat_start <= end_tick; beat_start += beat) {
    for (int32_t i = 1; i < subdivs; ++i) {
      const int64_t tick = beat_start + (static_cast<int64_t>(i) * beat) / subdivs;
      if (tick < start_tick) continue;
      if (tick > end_tick) return ticks;
      ticks.push_back(static_cast<int32_t>(tick));
    }
  }
  return ticks;
}

}  // namespace wds::chart_editor

#include <wds/core/edit_grid.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {

int32_t round_to_int_tick(float tick) {
  return static_cast<int32_t>(std::lround(tick));
}

int32_t subdivision_tick_step(const EditGridConfig& cfg) {
  if (cfg.ticks_per_quarter <= 0 || cfg.subdivisions_per_beat <= 0) {
    return 1;
  }
  return std::max(1, round_to_int_tick(
                         static_cast<float>(cfg.ticks_per_quarter) / cfg.subdivisions_per_beat));
}

int32_t snap_tick(float tick, const EditGridConfig& cfg) {
  const int32_t step = subdivision_tick_step(cfg);
  return round_to_int_tick(tick / step) * step;
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
  const int32_t first = static_cast<int32_t>(
      std::ceil(static_cast<double>(start_tick) / static_cast<double>(step))) * step;
  for (int32_t tick = first; tick <= end_tick; tick += step) {
    ticks.push_back(tick);
  }
  return ticks;
}

std::vector<int32_t> subdivision_ticks_in_range(int32_t start_tick, int32_t end_tick,
                                                const EditGridConfig& cfg) {
  std::vector<int32_t> ticks;
  if (end_tick < start_tick) {
    return ticks;
  }
  const int32_t step = subdivision_tick_step(cfg);
  const int32_t first = static_cast<int32_t>(
      std::ceil(static_cast<double>(start_tick) / static_cast<double>(step))) * step;
  for (int32_t tick = first; tick <= end_tick; tick += step) {
    if (cfg.ticks_per_quarter <= 0 || tick % cfg.ticks_per_quarter != 0) {
      ticks.push_back(tick);
    }
  }
  return ticks;
}

}  // namespace wds::chart_editor

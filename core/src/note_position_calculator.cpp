#include <wds/core/note_position_calculator.hpp>

#include <wds/core/official_playfield.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {

NotePositionCalculator::NotePositionCalculator(PreviewConfig config) : config_(config) {
  recalculate_move_seconds();
}

void NotePositionCalculator::set_config(PreviewConfig config) {
  config_ = config;
  recalculate_move_seconds();
}

float NotePositionCalculator::move_seconds() const noexcept { return move_seconds_; }

float NotePositionCalculator::speed_rate() const noexcept { return speed_rate_; }

void NotePositionCalculator::recalculate_move_seconds() {
  speed_rate_ = official_speed_rate(config_.note_speed);
  move_seconds_ = official_move_seconds(config_.note_speed);
  if (move_seconds_ <= 0.0f) {
    move_seconds_ = 0.001f;
  }
}

float NotePositionCalculator::calculate_position_y(int64_t target_ms, int64_t passed_ms) const {
  if (unit_speed_only_) {
    return official_note_local_y(target_ms, passed_ms, config_.note_speed, config_.offset_value,
                                 config_.position_pow3_rate, config_.position_pow1_rate);
  }
  const double delta_ms = scroll_distance_ms(static_cast<double>(target_ms),
                                             static_cast<double>(passed_ms));
  const double time = static_cast<double>(speed_rate_) * delta_ms / 1000.0;
  return static_cast<float>(config_.offset_value +
                            config_.position_pow3_rate * time * time * time +
                            config_.position_pow1_rate * time);
}

float NotePositionCalculator::calculate_hold_length(int64_t start_ms, int64_t end_ms,
                                                    int64_t passed_ms) const {
  if (end_ms <= start_ms) {
    return 0.0f;
  }

  const float start_y = calculate_position_y(start_ms, passed_ms);
  const float end_y = calculate_position_y(end_ms, passed_ms);
  return std::max(0.0f, end_y - start_y);
}

float NotePositionCalculator::note_width(int32_t lane_count, int32_t note_width_lanes) const {
  const float full_width =
      official_note_width(lane_count, config_.note_width_per_lane, config_.lane_border_width);
  const float lane_span = official_note_width(note_width_lanes, config_.note_width_per_lane,
                                              config_.lane_border_width);
  if (config_.compatibility_profile == ChartCompatibilityProfile::SiriusExtended) {
    return lane_span;
  }
  return std::min(lane_span, full_width);
}

float NotePositionCalculator::note_position_x(int32_t lane, int32_t lane_count,
                                              int32_t note_width_lanes) const {
  const float width = note_width(lane_count, note_width_lanes);
  return official_note_position_x(lane + 1, width, config_.note_width_per_lane,
                                  config_.lane_border_width);
}

void NotePositionCalculator::set_scroll_speed_points(std::vector<ScrollSpeedPoint> points,
                                                      const MusicTiming& timing) {
  speed_segments_.clear();
  unit_speed_only_ = true;
  std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
    return a.tick < b.tick;
  });
  points.erase(std::unique(points.begin(), points.end(), [](const auto& a, const auto& b) {
                 return a.tick == b.tick;
               }), points.end());
  double prefix = 0.0;
  double previous_ms = 0.0;
  double previous_multiplier = 1.0;
  for (const auto& point : points) {
    if (point.tick < 0 || !std::isfinite(point.multiplier) || !(point.multiplier > 0.0)) {
      continue;
    }
    const double start_ms = static_cast<double>(tick_to_milliseconds(point.tick, timing));
    prefix += (start_ms - previous_ms) * previous_multiplier;
    speed_segments_.push_back({start_ms, point.multiplier, prefix});
    unit_speed_only_ = unit_speed_only_ && point.multiplier == 1.0;
    previous_ms = start_ms;
    previous_multiplier = point.multiplier;
  }
}

void NotePositionCalculator::clear_scroll_speed_points() {
  speed_segments_.clear();
  unit_speed_only_ = true;
}

double NotePositionCalculator::scroll_distance_ms(double target_ms, double passed_ms) const noexcept {
  if (speed_segments_.empty()) return target_ms - passed_ms;
  const auto scroll_at = [&](double t) {
    if (t < speed_segments_.front().start_ms) return t;
    const auto it = std::upper_bound(
        speed_segments_.begin(), speed_segments_.end(), t,
        [](double value, const SpeedSegment& segment) { return value < segment.start_ms; });
    const auto& segment = *(it == speed_segments_.begin() ? it : std::prev(it));
    return segment.prefix_scroll_ms + (t - segment.start_ms) * segment.multiplier;
  };
  return scroll_at(target_ms) - scroll_at(passed_ms);
}

}  // namespace wds::chart_editor

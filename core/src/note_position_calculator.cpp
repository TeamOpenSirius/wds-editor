#include <wds/core/note_position_calculator.hpp>

#include <wds/core/official_playfield.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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
  // A negative SV segment can put the tail above the head (end_y < start_y);
  // the hold is still drawn between the two endpoints, so report its length.
  return std::fabs(end_y - start_y);
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
  has_negative_scroll_speed_ = false;
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
    if (point.tick < 0 || !is_valid_scroll_multiplier(point.multiplier)) {
      continue;
    }
    const double start_ms = static_cast<double>(tick_to_milliseconds(point.tick, timing));
    prefix += (start_ms - previous_ms) * previous_multiplier;
    speed_segments_.push_back({start_ms, point.multiplier, prefix});
    unit_speed_only_ = unit_speed_only_ && point.multiplier == 1.0;
    has_negative_scroll_speed_ = has_negative_scroll_speed_ || point.multiplier < 0.0;
    previous_ms = start_ms;
    previous_multiplier = point.multiplier;
  }
}

void NotePositionCalculator::clear_scroll_speed_points() {
  speed_segments_.clear();
  unit_speed_only_ = true;
  has_negative_scroll_speed_ = false;
}

double NotePositionCalculator::scroll_coordinate_at(double time_ms) const noexcept {
  if (speed_segments_.empty()) return time_ms;
  if (time_ms < speed_segments_.front().start_ms) return time_ms;
  const auto it = std::upper_bound(
      speed_segments_.begin(), speed_segments_.end(), time_ms,
      [](double value, const SpeedSegment& segment) { return value < segment.start_ms; });
  const auto& segment = *(it == speed_segments_.begin() ? it : std::prev(it));
  return segment.prefix_scroll_ms + (time_ms - segment.start_ms) * segment.multiplier;
}

double NotePositionCalculator::scroll_distance_ms(double target_ms,
                                                  double passed_ms) const noexcept {
  return scroll_coordinate_at(target_ms) - scroll_coordinate_at(passed_ms);
}

double NotePositionCalculator::scroll_distance_for_position_y(double y) const {
  const double speed_rate = std::max(static_cast<double>(speed_rate_), 1e-9);
  const double pow3 = static_cast<double>(config_.position_pow3_rate);
  const double pow1 = static_cast<double>(config_.position_pow1_rate);
  const double target = y - static_cast<double>(config_.offset_value);
  const auto position_for_distance = [&](double distance_ms) {
    const double time = speed_rate * distance_ms / 1000.0;
    return pow3 * time * time * time + pow1 * time;
  };

  if (pow3 == 0.0 && pow1 == 0.0) {
    return 0.0;
  }

  double lo = -1000.0;
  double hi = 1000.0;
  for (int i = 0; i < 80 && position_for_distance(lo) > target; ++i) {
    lo *= 2.0;
  }
  for (int i = 0; i < 80 && position_for_distance(hi) < target; ++i) {
    hi *= 2.0;
  }
  if (!(position_for_distance(lo) <= target && position_for_distance(hi) >= target)) {
    return 0.0;
  }
  for (int i = 0; i < 120; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (position_for_distance(mid) < target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

bool NotePositionCalculator::visible_target_time_bounds(double passed_ms, double y_low,
                                                        double y_high, double* out_first_ms,
                                                        double* out_last_ms) const {
  if (out_first_ms == nullptr || out_last_ms == nullptr) return false;
  if (y_low > y_high) std::swap(y_low, y_high);
  std::vector<ScrollTimeRange> ranges;
  scroll_time_ranges_for_distance(passed_ms, scroll_distance_for_position_y(y_low),
                                  scroll_distance_for_position_y(y_high), ranges);
  if (ranges.empty()) return false;
  *out_first_ms = ranges.front().first_ms;
  *out_last_ms = ranges.back().last_ms;
  return true;
}

void NotePositionCalculator::scroll_time_ranges_for_distance(
    double passed_ms, double distance_lo, double distance_hi,
    std::vector<ScrollTimeRange>& out) const {
  out.clear();
  if (!std::isfinite(distance_lo) || !std::isfinite(distance_hi)) return;
  if (distance_lo > distance_hi) std::swap(distance_lo, distance_hi);

  // s(t) is piecewise linear. Each active piece contributes at most one
  // interval to the preimage of [lower_coord, upper_coord].
  const double base = scroll_coordinate_at(passed_ms);
  const double lower_coord = base + distance_lo;
  const double upper_coord = base + distance_hi;
  const auto include = [&](double lo, double hi) {
    if (!(lo <= hi)) return;
    if (!out.empty() && lo <= out.back().last_ms + 1e-6) {
      out.back().last_ms = std::max(out.back().last_ms, hi);
    } else {
      out.push_back({lo, hi});
    }
  };

  if (speed_segments_.empty()) {
    include(lower_coord, upper_coord);
    return;
  }

  const double first_start = speed_segments_.front().start_ms;
  // Before the first SV point the scroll coordinate is the chart clock itself.
  if (lower_coord <= first_start) {
    include(lower_coord, std::min(upper_coord, first_start));
  }
  for (size_t i = 0; i < speed_segments_.size(); ++i) {
    const SpeedSegment& segment = speed_segments_[i];
    const double start = segment.start_ms;
    const double end = (i + 1 < speed_segments_.size())
                           ? speed_segments_[i + 1].start_ms
                           : std::numeric_limits<double>::infinity();
    if (segment.multiplier == 0.0) {
      // Frozen scroll: every target time in this segment shares one coordinate.
      if (segment.prefix_scroll_ms >= lower_coord &&
          segment.prefix_scroll_ms <= upper_coord) {
        include(start, end);
      }
      continue;
    }
    double at_lower = start + (lower_coord - segment.prefix_scroll_ms) / segment.multiplier;
    double at_upper = start + (upper_coord - segment.prefix_scroll_ms) / segment.multiplier;
    if (at_lower > at_upper) std::swap(at_lower, at_upper);
    at_lower = std::max(at_lower, start);
    at_upper = std::min(at_upper, end);
    include(at_lower, at_upper);
  }
}

bool NotePositionCalculator::scroll_time_last_for_distance(double passed_ms, double distance_lo,
                                                          double distance_hi,
                                                          double* out_last_ms) const {
  if (out_last_ms == nullptr) return false;
  if (!std::isfinite(distance_lo) || !std::isfinite(distance_hi)) return false;
  if (distance_lo > distance_hi) std::swap(distance_lo, distance_hi);

  const double base = scroll_coordinate_at(passed_ms);
  const double lower_coord = base + distance_lo;
  const double upper_coord = base + distance_hi;
  if (speed_segments_.empty()) {
    *out_last_ms = upper_coord;
    return true;
  }

  // Walk backwards: the first segment that intersects the band owns the global
  // maximum, so typical charts stop immediately instead of scanning all SV.
  for (size_t i = speed_segments_.size(); i > 0; --i) {
    const SpeedSegment& segment = speed_segments_[i - 1];
    const double start = segment.start_ms;
    const double end = (i < speed_segments_.size())
                           ? speed_segments_[i].start_ms
                           : std::numeric_limits<double>::infinity();
    if (segment.multiplier == 0.0) {
      if (segment.prefix_scroll_ms >= lower_coord &&
          segment.prefix_scroll_ms <= upper_coord) {
        *out_last_ms = end;
        return true;
      }
      continue;
    }
    double at_lower = start + (lower_coord - segment.prefix_scroll_ms) / segment.multiplier;
    double at_upper = start + (upper_coord - segment.prefix_scroll_ms) / segment.multiplier;
    if (at_lower > at_upper) std::swap(at_lower, at_upper);
    at_lower = std::max(at_lower, start);
    at_upper = std::min(at_upper, end);
    if (at_lower <= at_upper) {
      *out_last_ms = at_upper;
      return true;
    }
  }

  // No SV segment intersects; the identity prefix may still do so.
  if (!speed_segments_.empty()) {
    const double first_start = speed_segments_.front().start_ms;
    if (lower_coord <= first_start) {
      const double hi = std::min(upper_coord, first_start);
      if (lower_coord <= hi) {
        *out_last_ms = hi;
        return true;
      }
    }
  }
  return false;
}

}  // namespace wds::chart_editor

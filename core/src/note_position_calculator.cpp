#include <wds/core/note_position_calculator.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {

namespace {

constexpr double kNoteSpeedAdjustValue = 10.0;
constexpr float kSpeedCorrectValue = 0.6f;

float ease_blend(float t, float pow1_rate, float pow3_rate) {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  const float cubic = clamped * clamped * clamped;
  return pow1_rate * clamped + pow3_rate * cubic;
}

}  // namespace

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
  // Mirrors NotePositionCalculator.CalculateMoveSeconds(noteSpeed) behavior at a high level.
  const double adjusted = config_.note_speed / kNoteSpeedAdjustValue;
  speed_rate_ = static_cast<float>(adjusted);
  move_seconds_ = static_cast<float>(config_.note_approach_seconds * std::pow(adjusted, kSpeedCorrectValue));
  if (move_seconds_ <= 0.0f) {
    move_seconds_ = 0.001f;
  }
}

float NotePositionCalculator::calculate_position_y(int64_t target_ms, int64_t passed_ms) const {
  const float diff_sec = static_cast<float>(target_ms - passed_ms) / 1000.0f;
  if (diff_sec <= 0.0f) {
    return config_.judge_line_y;
  }

  const float normalized = diff_sec / move_seconds_;
  const float eased = ease_blend(normalized, config_.position_pow1_rate, config_.position_pow3_rate);
  const float travel = config_.spawn_y - config_.judge_line_y;
  return config_.judge_line_y + eased * travel;
}

float NotePositionCalculator::calculate_hold_length(int64_t start_ms, int64_t end_ms,
                                                    int64_t passed_ms) const {
  if (end_ms <= start_ms) {
    return 0.0f;
  }

  const float start_y = calculate_position_y(start_ms, passed_ms);
  const float end_y = calculate_position_y(end_ms, passed_ms);
  return std::max(0.0f, start_y - end_y);
}

float NotePositionCalculator::note_width(int32_t lane_count, int32_t note_width_lanes) const {
  const float full_width = static_cast<float>(lane_count) * config_.note_width_per_lane +
                           static_cast<float>(std::max(0, lane_count - 1)) * config_.lane_border_width;
  const float lane_span = static_cast<float>(note_width_lanes) * config_.note_width_per_lane +
                          static_cast<float>(std::max(0, note_width_lanes - 1)) * config_.lane_border_width;
  return std::min(lane_span, full_width);
}

float NotePositionCalculator::note_position_x(int32_t lane, int32_t lane_count,
                                              int32_t note_width_lanes) const {
  const float lane_origin =
      static_cast<float>(lane) * (config_.note_width_per_lane + config_.lane_border_width);
  const float width = note_width(lane_count, note_width_lanes);
  const float full_width = note_width(lane_count, lane_count);
  const float centered_offset = (full_width - width) * 0.5f;
  return lane_origin - centered_offset;
}

}  // namespace wds::chart_editor

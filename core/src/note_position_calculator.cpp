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
  return official_note_local_y(target_ms, passed_ms, config_.note_speed, config_.offset_value,
                               config_.position_pow3_rate, config_.position_pow1_rate);
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
  return std::min(lane_span, full_width);
}

float NotePositionCalculator::note_position_x(int32_t lane, int32_t lane_count,
                                              int32_t note_width_lanes) const {
  const float width = note_width(lane_count, note_width_lanes);
  return official_note_position_x(lane + 1, width, config_.note_width_per_lane,
                                  config_.lane_border_width);
}

}  // namespace wds::chart_editor

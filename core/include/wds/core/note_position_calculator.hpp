#pragma once

#include <wds/core/preview_config.hpp>

#include <cstdint>

namespace wds::chart_editor {

// Port of Sirius.Game.NotePositionCalculator.
class NotePositionCalculator {
 public:
  explicit NotePositionCalculator(PreviewConfig config = {});

  void set_config(PreviewConfig config);

  float move_seconds() const noexcept;
  float speed_rate() const noexcept;

  float calculate_position_y(int64_t target_ms, int64_t passed_ms) const;
  float calculate_hold_length(int64_t start_ms, int64_t end_ms, int64_t passed_ms) const;

  float note_width(int32_t lane_count, int32_t note_width_lanes) const;
  float note_position_x(int32_t lane, int32_t lane_count, int32_t note_width_lanes) const;

 private:
  void recalculate_move_seconds();

  PreviewConfig config_;
  float move_seconds_ = 1.8f;
  float speed_rate_ = 1.0f;
};

}  // namespace wds::chart_editor

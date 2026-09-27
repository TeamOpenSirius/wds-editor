#pragma once

#include <wds/core/preview_config.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <vector>

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

  // Configure positive chart SV events. Events are immutable after this call
  // and are only read by position queries, so callers can safely share the
  // calculator with render/update workers.
  void set_scroll_speed_points(std::vector<ScrollSpeedPoint> points,
                               const MusicTiming& timing);
  void clear_scroll_speed_points();
  void set_compatibility_profile(ChartCompatibilityProfile profile) noexcept {
    config_.compatibility_profile = profile;
  }
  double scroll_distance_ms(double target_ms, double passed_ms) const noexcept;

 private:
  void recalculate_move_seconds();

  PreviewConfig config_;
  float move_seconds_ = 1.8f;
  float speed_rate_ = 1.0f;
  struct SpeedSegment {
    double start_ms = 0.0;
    double multiplier = 1.0;
    double prefix_scroll_ms = 0.0;
  };
  std::vector<SpeedSegment> speed_segments_;
  bool unit_speed_only_ = true;
};

}  // namespace wds::chart_editor

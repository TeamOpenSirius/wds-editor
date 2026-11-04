#pragma once

#include <wds/core/preview_config.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <vector>

namespace wds::chart_editor {

// A target-time interval whose position lies inside a requested y band.
struct ScrollTimeRange {
  double first_ms = 0.0;
  double last_ms = 0.0;
};

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

  // Configure chart SV events (0 freezes, < 0 scrolls backwards). Events are
  // immutable after this call and are only read by position queries, so callers
  // can safely share the calculator with render/update workers.
  void set_scroll_speed_points(std::vector<ScrollSpeedPoint> points,
                               const MusicTiming& timing);
  void clear_scroll_speed_points();
  void set_compatibility_profile(ChartCompatibilityProfile profile) noexcept {
    config_.compatibility_profile = profile;
  }
  double scroll_distance_ms(double target_ms, double passed_ms) const noexcept;

  // True when any configured SV point has a negative multiplier. Callers use
  // this to switch from the monotonic "spawn lead" window to a spatial query.
  bool has_negative_scroll_speed() const noexcept { return has_negative_scroll_speed_; }

  // Hull of the target times whose on-screen position y lies in [y_low, y_high]
  // at passed_ms. With a negative multiplier the scroll coordinate is not
  // monotonic, so this walks every SV segment and returns the first/last time
  // in the preimage (the interval is a superset of all visible target times).
  // Returns false when no target time maps into the band.
  bool visible_target_time_bounds(double passed_ms, double y_low, double y_high,
                                  double* out_first_ms, double* out_last_ms) const;

  // Distance from `passed_ms` whose position_y equals y. The position curve is
  // monotonically increasing, so this is a fixed-iteration bisection.
  double scroll_distance_for_position_y(double y) const;

  // Exact preimage intervals of a scroll-distance band. The returned ranges are
  // sorted and merged; with negative multipliers there can be many of them.
  void scroll_time_ranges_for_distance(double passed_ms, double distance_lo,
                                       double distance_hi,
                                       std::vector<ScrollTimeRange>& out) const;

  // Last target time in the same preimage. Scans segments backwards and stops at
  // the first hit, which is usually O(1) for charts whose latest visible time is
  // near the query point.
  bool scroll_time_last_for_distance(double passed_ms, double distance_lo,
                                     double distance_hi, double* out_last_ms) const;

  size_t scroll_segment_count() const noexcept { return speed_segments_.size(); }

 private:
  void recalculate_move_seconds();
  double scroll_coordinate_at(double time_ms) const noexcept;

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
  bool has_negative_scroll_speed_ = false;
};

}  // namespace wds::chart_editor

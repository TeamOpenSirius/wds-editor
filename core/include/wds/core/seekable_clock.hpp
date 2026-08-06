#pragma once

#include <wds/core/types.hpp>

#include <wds/common/timeline.hpp>

#include <cstdint>

namespace wds::chart_editor {

// Compatibility wrapper around wds::common::Timeline for chart preview.
// Prefer TimelineSnapshot / apply_timeline() at module boundaries.
class SeekableClock {
 public:
  SeekableClock() = default;

  int64_t current_time_ms() const noexcept { return timeline_.position_ms(); }
  PreviewPlaybackState playback_state() const noexcept {
    return timeline_.state();
  }

  wds::common::Timeline& timeline() noexcept { return timeline_; }
  const wds::common::Timeline& timeline() const noexcept { return timeline_; }

  wds::common::TimelineSnapshot snapshot() const noexcept {
    return timeline_.snapshot();
  }

  void apply(const wds::common::TimelineSnapshot& snap) { timeline_.apply(snap); }

  void seek(int64_t time_ms) { timeline_.seek_ms(time_ms); }
  void play() { timeline_.play(); }

  // Advance time when playing. No-op while paused.
  void tick(int64_t delta_ms) { timeline_.tick_ms(delta_ms); }

  void reset() { timeline_.reset(); }

 private:
  wds::common::Timeline timeline_;
};

}  // namespace wds::chart_editor

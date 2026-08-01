#pragma once

#include "wds/common/time.hpp"

#include <algorithm>

namespace wds::common {

// Shared seekable timeline used by core preview, audio transport, and UI.
// Precision is microseconds; millisecond helpers remain for existing call sites.
class Timeline {
 public:
  Timeline() = default;

  Microseconds position() const noexcept { return position_; }
  PlaybackState state() const noexcept { return state_; }
  TimelineSnapshot snapshot() const noexcept { return {position_, state_}; }

  int64_t position_ms() const noexcept { return us_to_ms_floor(position_); }
  bool playing() const noexcept { return state_ == PlaybackState::Playing; }

  void seek(Microseconds time) {
    position_ = std::max(Microseconds{0}, time);
  }

  void seek_ms(int64_t time_ms) { seek(ms_to_us(std::max<int64_t>(0, time_ms))); }

  void play() { state_ = PlaybackState::Playing; }
  void pause() { state_ = PlaybackState::Paused; }

  void toggle_playback() {
    state_ = (state_ == PlaybackState::Playing) ? PlaybackState::Paused
                                                : PlaybackState::Playing;
  }

  void advance(Microseconds delta) {
    if (state_ != PlaybackState::Playing || delta.count() <= 0) {
      return;
    }
    position_ += delta;
  }

  void tick_ms(int64_t delta_ms) {
    if (delta_ms <= 0) {
      return;
    }
    advance(ms_to_us(delta_ms));
  }

  void reset() {
    position_ = Microseconds{0};
    state_ = PlaybackState::Paused;
  }

  void apply(const TimelineSnapshot& snap) {
    position_ = std::max(Microseconds{0}, snap.position);
    state_ = snap.state;
  }

 private:
  Microseconds position_{0};
  PlaybackState state_ = PlaybackState::Paused;
};

}  // namespace wds::common

#pragma once

#include <chrono>
#include <cstdint>

namespace wds::common {

using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;

inline constexpr Microseconds ms_to_us(int64_t ms) noexcept {
  return Microseconds{ms * 1000};
}

inline constexpr int64_t us_to_ms_floor(Microseconds us) noexcept {
  return us.count() / 1000;
}

inline constexpr int64_t us_to_ms_round(Microseconds us) noexcept {
  if (us.count() >= 0) {
    return (us.count() + 500) / 1000;
  }
  return (us.count() - 500) / 1000;
}

enum class PlaybackState {
  Paused,
  Playing,
};

struct TimelineSnapshot {
  Microseconds position{0};
  PlaybackState state = PlaybackState::Paused;

  int64_t position_ms() const noexcept { return us_to_ms_floor(position); }
};

}  // namespace wds::common

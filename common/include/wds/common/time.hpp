#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace wds::common {

using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;

inline constexpr Microseconds ms_to_us(int64_t ms) noexcept {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMaxMs = kMax / 1000;
  // C++ integer `/` truncates toward zero, not toward -inf. INT64_MIN/1000 is
  // therefore the last ms whose *1000 still fits; kMinMs-1 would overflow.
  constexpr int64_t kMinMs = kMin / 1000;
  if (ms > kMaxMs) {
    return Microseconds{kMax};
  }
  if (ms < kMinMs) {
    return Microseconds{kMin};
  }
  return Microseconds{ms * 1000};
}

inline constexpr int64_t us_to_ms_floor(Microseconds us) noexcept {
  return us.count() / 1000;
}

inline constexpr int64_t us_to_ms_round(Microseconds us) noexcept {
  const int64_t count = us.count();
  const int64_t q = count / 1000;
  const int64_t r = count % 1000;
  if (r >= 500) {
    return q + 1;
  }
  if (r <= -500) {
    return q - 1;
  }
  return q;
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

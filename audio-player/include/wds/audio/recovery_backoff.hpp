#pragma once

#include <cstdint>
#include <limits>

namespace wds::audio {

enum class StreamHealth {
  Playing,
  Paused,
  Stopped,
  Stalled,
};

enum class StreamRecoveryKind {
  None,
  NaturalEnd,
  Recover,
};

inline StreamRecoveryKind classify_stream_recovery(StreamHealth health, bool near_end) noexcept {
  if (health == StreamHealth::Stopped && near_end) {
    return StreamRecoveryKind::NaturalEnd;
  }
  if (health == StreamHealth::Stopped || health == StreamHealth::Stalled ||
      health == StreamHealth::Paused) {
    return StreamRecoveryKind::Recover;
  }
  return StreamRecoveryKind::None;
}

// Poll-accumulated wall-clock backoff. First attempt is immediate; subsequent
// waits are 50 → 100 → 200 → 400 → 800 → 1000 ms (capped).
class RecoveryBackoff {
 public:
  static constexpr int64_t kDelaysUs[6] = {50000, 100000, 200000, 400000, 800000, 1000000};

  void add_elapsed(int64_t wall_delta_us) noexcept {
    if (wall_delta_us <= 0) {
      return;
    }
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    if (elapsed_us_ > kMax - wall_delta_us) {
      elapsed_us_ = kMax;
      return;
    }
    elapsed_us_ += wall_delta_us;
  }

  bool try_acquire() noexcept {
    const int64_t need = next_delay_us();
    if (elapsed_us_ < need) {
      return false;
    }
    elapsed_us_ = 0;
    ++attempt_count_;
    pending_ = true;
    return true;
  }

  bool allow_attempt(int64_t wall_delta_us) noexcept {
    add_elapsed(wall_delta_us);
    return try_acquire();
  }

  void on_success() noexcept { reset(); }

  void on_failure() noexcept { pending_ = true; }

  void reset() noexcept {
    elapsed_us_ = 0;
    attempt_count_ = 0;
    pending_ = false;
  }

  int attempt_count() const noexcept { return attempt_count_; }
  bool pending() const noexcept { return pending_; }

  int64_t next_delay_us() const noexcept {
    if (attempt_count_ <= 0) {
      return 0;
    }
    const int idx = attempt_count_ - 1;
    if (idx >= 5) {
      return kDelaysUs[5];
    }
    return kDelaysUs[idx];
  }

 private:
  int64_t elapsed_us_ = 0;
  int attempt_count_ = 0;
  bool pending_ = false;
};

}  // namespace wds::audio

#pragma once

#include "hit_sfx.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wds::audio {

// GameSePlayer.IgnorePlayMilliseconds — same (target_ms, cue) only replays
// when last_play + 25 < this play time (Auto preview: target_ms == play time).
inline constexpr int64_t kIgnorePlayMilliseconds = 25;
// GameSePlayer._entryPlaySeCountPerFrame > 24 drops the rest of that LateTick.
// Preview arms on the music clock, so the cap is per target millisecond.
inline constexpr int kMaxEntryPlaySePerInstant = 25;

struct HoldInterval {
  int64_t start_ms = 0;
  int64_t end_ms = 0;
};

struct SfxHistoryKey {
  int64_t ms = 0;
  HitSfxClip clip = HitSfxClip::Count;

  bool operator==(const SfxHistoryKey& other) const noexcept {
    return ms == other.ms && clip == other.clip;
  }
};

struct SfxHistoryKeyHash {
  std::size_t operator()(const SfxHistoryKey& key) const noexcept {
    const auto h = std::hash<int64_t>{}(key.ms);
    return h ^ (static_cast<std::size_t>(key.clip) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
  }
};

// Replica of GameSePlayer._seHistory / TrySetCue (Auto: diffMs = 0).
class SfxPlayHistory {
 public:
  bool can_admit(int64_t target_ms, HitSfxClip clip, bool bypass) const noexcept {
    if (bypass || clip == HitSfxClip::Count || clip == HitSfxClip::Hold) {
      return true;
    }
    const auto it = last_play_ms_.find(SfxHistoryKey{target_ms, clip});
    if (it == last_play_ms_.end()) {
      return true;
    }
    return it->second + kIgnorePlayMilliseconds < target_ms;
  }

  void record(int64_t target_ms, HitSfxClip clip, bool bypass) {
    if (bypass || clip == HitSfxClip::Count || clip == HitSfxClip::Hold) {
      return;
    }
    last_play_ms_[SfxHistoryKey{target_ms, clip}] = target_ms;
  }

  void clear() { last_play_ms_.clear(); }

 private:
  std::unordered_map<SfxHistoryKey, int64_t, SfxHistoryKeyHash> last_play_ms_;
};

class SfxInstantBudget {
 public:
  bool can_admit(int64_t target_ms) const noexcept {
    const auto it = counts_.find(target_ms);
    return it == counts_.end() || it->second < kMaxEntryPlaySePerInstant;
  }

  void record(int64_t target_ms) { ++counts_[target_ms]; }

  void clear() { counts_.clear(); }

 private:
  std::unordered_map<int64_t, int> counts_;
};

inline void merge_hold_intervals(std::vector<HoldInterval>& intervals) {
  if (intervals.size() < 2) {
    return;
  }
  std::sort(intervals.begin(), intervals.end(), [](const HoldInterval& a, const HoldInterval& b) {
    return a.start_ms < b.start_ms || (a.start_ms == b.start_ms && a.end_ms < b.end_ms);
  });
  std::size_t write = 0;
  for (std::size_t i = 1; i < intervals.size(); ++i) {
    if (intervals[i].start_ms <= intervals[write].end_ms) {
      intervals[write].end_ms = std::max(intervals[write].end_ms, intervals[i].end_ms);
    } else {
      ++write;
      intervals[write] = intervals[i];
    }
  }
  intervals.resize(write + 1);
}

inline bool hold_covers_ms(const std::vector<HoldInterval>& intervals, int64_t clock_ms) noexcept {
  for (const auto& interval : intervals) {
    if (clock_ms >= interval.start_ms && clock_ms <= interval.end_ms) {
      return true;
    }
  }
  return false;
}

}  // namespace wds::audio

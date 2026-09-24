#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace wds::audio {

inline constexpr int64_t kSfxSyncHorizonUs = 10'000'000;
inline constexpr size_t kMaxPendingSfxSyncs = 4096;

enum class SfxSyncAdmit : uint8_t {
  PastOrDue,
  WithinWindow,
  TooFar,
  AtCapacity,
};

enum class SfxSyncPostArm : uint8_t {
  KeepArmed,
  PlayDue,
  RetryLater,
};

// Resolution after BASS_ChannelSetSync returns (handle may still be 0 in the
// window where a MIXTIME callback or clear/shutdown already ran).
enum class SfxSyncArmResult : uint8_t {
  KeepArmed,
  PlayDue,
  RetryLater,
  CallbackConsumed,
  CancelledDuringArm,
};

// Unique-winner claim on a scheduled POS sync.
enum class SfxSyncClaim : uint8_t {
  Pending = 0,
  Fired = 1,
  Cancelled = 2,
};

enum class SfxSyncKind : uint8_t {
  OneShot = 0,
  HoldOn,
  HoldOff,
};

enum class SfxSyncPlayClaim : uint8_t {
  PlayNow,
  AlreadyFired,
  Cancelled,
};

// max(0, target - heard) without signed overflow.
inline constexpr int64_t saturating_future_delta_us(int64_t target_us, int64_t heard_us) noexcept {
  if (target_us <= heard_us) {
    return 0;
  }
  if (heard_us < 0) {
    const int64_t headroom = std::numeric_limits<int64_t>::max() + heard_us;
    if (target_us > headroom) {
      return std::numeric_limits<int64_t>::max();
    }
  }
  return target_us - heard_us;
}

inline constexpr SfxSyncAdmit admit_sfx_sync(int64_t target_us, int64_t heard_us,
                                             size_t pending_count) noexcept {
  if (target_us <= heard_us) {
    return SfxSyncAdmit::PastOrDue;
  }
  if (saturating_future_delta_us(target_us, heard_us) > kSfxSyncHorizonUs) {
    return SfxSyncAdmit::TooFar;
  }
  if (pending_count >= kMaxPendingSfxSyncs) {
    return SfxSyncAdmit::AtCapacity;
  }
  return SfxSyncAdmit::WithinWindow;
}

inline constexpr SfxSyncPostArm decide_after_set_sync(bool set_sync_ok,
                                                      bool target_already_due) noexcept {
  if (target_already_due) {
    return SfxSyncPostArm::PlayDue;
  }
  return set_sync_ok ? SfxSyncPostArm::KeepArmed : SfxSyncPostArm::RetryLater;
}

inline bool claim_sfx_sync_fire(std::atomic<SfxSyncClaim>& state) noexcept {
  auto expected = SfxSyncClaim::Pending;
  return state.compare_exchange_strong(expected, SfxSyncClaim::Fired, std::memory_order_acq_rel,
                                       std::memory_order_acquire);
}

inline bool claim_sfx_sync_cancel(std::atomic<SfxSyncClaim>& state) noexcept {
  auto expected = SfxSyncClaim::Pending;
  return state.compare_exchange_strong(expected, SfxSyncClaim::Cancelled, std::memory_order_acq_rel,
                                       std::memory_order_acquire);
}

// Callback play claim: only while the payload is still in pending. Retired
// lookup must keep the pointer alive but must not CAS Fired or Cancelled.
inline bool claim_sfx_sync_callback_play(std::atomic<SfxSyncClaim>& state,
                                         bool found_in_pending) noexcept {
  if (!found_in_pending) {
    return false;
  }
  return claim_sfx_sync_fire(state);
}

inline SfxSyncPlayClaim finish_due_play_claim(std::atomic<SfxSyncClaim>& state) noexcept {
  if (claim_sfx_sync_fire(state)) {
    return SfxSyncPlayClaim::PlayNow;
  }
  return state.load(std::memory_order_acquire) == SfxSyncClaim::Fired
             ? SfxSyncPlayClaim::AlreadyFired
             : SfxSyncPlayClaim::Cancelled;
}

inline constexpr SfxSyncArmResult decide_arm_resolution(bool set_sync_ok, bool found_in_pending,
                                                        SfxSyncClaim claim,
                                                        bool target_already_due) noexcept {
  if (claim == SfxSyncClaim::Cancelled) {
    return SfxSyncArmResult::CancelledDuringArm;
  }
  if (claim == SfxSyncClaim::Fired) {
    return SfxSyncArmResult::CallbackConsumed;
  }
  if (!found_in_pending) {
    // Pending but already unlinked: do not pretend this is a cancel (that
    // dropped the UI mark while the callback could still play). Retry later.
    return SfxSyncArmResult::RetryLater;
  }
  if (!set_sync_ok) {
    return target_already_due ? SfxSyncArmResult::PlayDue : SfxSyncArmResult::RetryLater;
  }
  if (target_already_due) {
    return SfxSyncArmResult::PlayDue;
  }
  return SfxSyncArmResult::KeepArmed;
}

inline constexpr bool arm_result_keeps_hit_mark(SfxSyncArmResult result) noexcept {
  return result == SfxSyncArmResult::KeepArmed || result == SfxSyncArmResult::PlayDue ||
         result == SfxSyncArmResult::CallbackConsumed;
}

// handle==0 is not an identity — several arms share that window.
inline constexpr bool pending_sync_identifies(unsigned long long stored_handle,
                                              unsigned long long callback_handle,
                                              bool payload_equal) noexcept {
  if (payload_equal) {
    return true;
  }
  return stored_handle != 0 && stored_handle == callback_handle;
}

// UI music-clock arm: mark first, drop the key unless schedule_at accepted so
// TooFar / AtCapacity / SetSyncFailure stay retryable on the next tick.
inline bool commit_hit_sfx_schedule(std::unordered_set<uint64_t>& played, uint64_t key,
                                    bool schedule_accepted) {
  if (!played.insert(key).second) {
    return false;
  }
  if (!schedule_accepted) {
    played.erase(key);
  }
  return schedule_accepted;
}

}  // namespace wds::audio

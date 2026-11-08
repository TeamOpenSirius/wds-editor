#pragma once

#include <cstdint>

namespace wds::ui {

// Paused previews present at an idle cadence instead of every vsync. A cap hit
// must never eat a forced present (resize settle, re-expose, explicit
// request_frame): at that moment the window still shows a drawable of the old
// size, so skipping the frame leaves the preview stretched or black at the new
// aspect ratio until something else happens to request one.
inline constexpr bool idle_cap_defers_frame(bool forced_frame, bool startup, bool has_input,
                                            bool bypass_cap, bool idle_cap_enabled,
                                            int64_t pending_us, int64_t interval_us) noexcept {
  // 2 ms slack: at exactly one vsync per interval the accumulator lands a hair
  // under the cap and every other frame would be skipped (60 -> 30 fps).
  return idle_cap_enabled && !forced_frame && !startup && !has_input && !bypass_cap &&
         pending_us + 2000 < interval_us;
}

// The platform WSI drops the first present into a freshly (re)built swapchain:
// MoltenVK reports VK_SUBOPTIMAL_KHR and leaves the drawable blank, so a paused
// preview keeps the empty surface until another frame is presented. One more
// present is needed. The streak cap stops a wedged swapchain from pinning the
// paused preview to the idle redraw cadence forever.
inline constexpr bool needs_follow_up_present(bool drew, bool rebuilt, int streak,
                                              int max_streak = 2) noexcept {
  return drew && rebuilt && streak < max_streak;
}

// Before a paused preview stops presenting, every image of the current
// swapchain must hold the latest picture: the driver/compositor may show the
// image acquired by the next (play) frame before the GPU has drawn into it.
// Coverage is tracked by acquired image index, not by a present count (MAILBOX
// may cycle through only some images). Every frame the host asked for as a
// fill counts toward the cap, whatever happened to the picture during it, so
// no per-frame source of change can keep the fill loop alive.
inline constexpr std::uint32_t kSwapchainFillMaxImages = 64;

struct SwapchainFillState {
  std::uint64_t serial = 0;
  std::uint32_t image_count = 0;
  // Bit i: image i holds the latest picture.
  std::uint64_t covered = 0;
  // Fill-requested frames since the present gate last opened.
  std::uint32_t fill_presents = 0;
  bool active = false;
};

// One rendered frame. `requested_fill`: the host rendered it only because the
// fill was pending (present gate closed). `fill_frame`: requested, and the
// picture was still unchanged when it was drawn.
struct SwapchainFrameFacts {
  bool requested_fill = false;
  bool fill_frame = false;
  bool presented = false;
  std::uint64_t present_serial = 0;
  std::uint32_t image_index = 0;
  std::uint64_t swapchain_serial = 0;
  std::uint32_t image_count = 0;
};

enum class SwapchainFillOutcome : std::uint8_t {
  Idle = 0,
  Pending,
  Covered,
  CapReached,
};

inline constexpr std::uint64_t swapchain_fill_full_mask(std::uint32_t image_count) noexcept {
  return image_count >= kSwapchainFillMaxImages ? ~std::uint64_t{0}
                                                : ((std::uint64_t{1} << image_count) - 1u);
}

inline constexpr std::uint32_t swapchain_fill_cap(std::uint32_t image_count) noexcept {
  return (image_count < kSwapchainFillMaxImages ? image_count : kSwapchainFillMaxImages) * 2u;
}

inline constexpr std::uint32_t swapchain_fill_covered_count(const SwapchainFillState& s) noexcept {
  std::uint64_t bits = s.covered & swapchain_fill_full_mask(s.image_count);
  std::uint32_t n = 0;
  while (bits != 0) {
    bits &= bits - 1u;
    ++n;
  }
  return n;
}

// Playing frames change the picture every frame; skip the per-image bookkeeping
// and just leave the next paused frame owing a full fill.
inline constexpr void swapchain_fill_invalidate(SwapchainFillState& s) noexcept {
  s.covered = 0;
  s.fill_presents = 0;
  s.active = true;
}

inline constexpr bool swapchain_fill_pending(const SwapchainFillState& s) noexcept {
  return s.active;
}

// Microseconds a fill frame must still wait. Fill frames start only after the
// picture has been still for one paused interval, so they never compete with
// content frames for a vsync while scrubbing.
inline constexpr int64_t swapchain_fill_wait_us(int64_t since_content_us,
                                                int64_t interval_us) noexcept {
  return since_content_us >= interval_us ? 0 : interval_us - since_content_us;
}

inline constexpr SwapchainFillOutcome note_swapchain_frame(SwapchainFillState& s,
                                                           const SwapchainFrameFacts& f) noexcept {
  if (f.swapchain_serial != s.serial || f.image_count != s.image_count) {
    // Images of a new swapchain start with undefined contents. fill_presents
    // is kept so a swapchain rebuilt on every present still hits the cap.
    s.serial = f.swapchain_serial;
    s.image_count = f.image_count;
    s.covered = 0;
    s.active = true;
  }
  if (!f.requested_fill) {
    s.covered = 0;
    s.fill_presents = 0;
    s.active = true;
  } else if (!s.active) {
    return SwapchainFillOutcome::Idle;
  } else {
    ++s.fill_presents;
    if (!f.fill_frame) {
      s.covered = 0;
    }
  }
  if (s.image_count == 0 || s.image_count > kSwapchainFillMaxImages) {
    s.active = false;
    return SwapchainFillOutcome::Idle;
  }
  if (f.presented && f.present_serial == s.serial && f.image_index < s.image_count) {
    s.covered |= std::uint64_t{1} << f.image_index;
  }
  const std::uint64_t full = swapchain_fill_full_mask(s.image_count);
  if ((s.covered & full) == full) {
    s.active = false;
    return s.fill_presents > 0 ? SwapchainFillOutcome::Covered : SwapchainFillOutcome::Idle;
  }
  if (s.fill_presents >= swapchain_fill_cap(s.image_count)) {
    s.active = false;
    return SwapchainFillOutcome::CapReached;
  }
  return SwapchainFillOutcome::Pending;
}

}  // namespace wds::ui

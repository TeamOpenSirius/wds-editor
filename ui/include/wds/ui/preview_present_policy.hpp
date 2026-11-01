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

}  // namespace wds::ui

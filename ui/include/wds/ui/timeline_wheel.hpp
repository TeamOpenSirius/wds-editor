#pragma once

#include <algorithm>
#include <cstdint>

namespace wds::ui {

// Earliest music time the playhead / scrubbers may sit on (chart start when
// offset is negative, otherwise music t=0).
inline int64_t timeline_origin_ms(int64_t offset_ms) noexcept {
  return std::min<int64_t>(0, offset_ms);
}

inline int64_t clamp_scrub_ms(int64_t time_ms, int64_t offset_ms) noexcept {
  return std::max(time_ms, timeline_origin_ms(offset_ms));
}

int64_t timeline_scrub_delta_ms(float delta_y, int32_t visible_hectoms, float speed) noexcept;

// Alt/Option+wheel is delivered on the horizontal axis: Qt's Windows pointer
// handler copies the notch into angleDelta.x when Alt is down, and macOS
// AppKit does the same swap for Option. The larger component is the gesture.
float visible_range_wheel_delta(float delta_x, float delta_y) noexcept;

// `delta_y` is already the Qt-delivered delta after global invert_scroll_wheel.
// invert_scroll_wheel undoes that adapter flip; invert_visible_range_scroll is then
// applied independently (same order as ChartEditPanel Option+wheel).
int32_t visible_range_after_wheel(float delta_y, int32_t current, bool invert_scroll_wheel,
                                  bool invert_visible_range_scroll) noexcept;

}  // namespace wds::ui

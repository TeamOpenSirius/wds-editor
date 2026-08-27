#pragma once

#include <cstdint>

namespace wds::ui {

int64_t timeline_scrub_delta_ms(float delta_y, int32_t visible_hectoms, float speed) noexcept;

// `delta_y` is already the GLFW-delivered delta after global invert_scroll_wheel.
// invert_scroll_wheel undoes that adapter flip; invert_visible_range_scroll is then
// applied independently (same order as ChartEditPanel Primary+wheel).
int32_t visible_range_after_wheel(float delta_y, int32_t current, bool invert_scroll_wheel,
                                  bool invert_visible_range_scroll) noexcept;

}  // namespace wds::ui

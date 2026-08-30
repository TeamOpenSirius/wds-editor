#include "wds/ui/timeline_wheel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wds::ui {

int64_t timeline_scrub_delta_ms(float delta_y, int32_t visible_hectoms, float speed) noexcept {
  if (!std::isfinite(speed)) {
    speed = 1.0f;
  } else {
    speed = std::clamp(speed, 0.25f, 3.0f);
  }
  constexpr float kScrollMsPerNotchAtVisible20 = 100.0f;
  constexpr float kScrollVisibleRefHectoms = 20.0f;
  const float ms_per_notch_at_1x =
      kScrollMsPerNotchAtVisible20 *
      (static_cast<float>(std::max(1, visible_hectoms)) / kScrollVisibleRefHectoms);
  const float delta_ms = -delta_y * ms_per_notch_at_1x * speed;
  return static_cast<int64_t>(std::lround(delta_ms));
}

int32_t visible_range_after_wheel(float delta_y, int32_t current, bool invert_scroll_wheel,
                                  bool invert_visible_range_scroll) noexcept {
  if (std::abs(delta_y) < 1e-6f) {
    return current;
  }
  float dy = delta_y;
  if (invert_scroll_wheel) {
    dy = -dy;
  }
  if (invert_visible_range_scroll) {
    dy = -dy;
  }
  int steps = static_cast<int>(std::lround(std::abs(dy)));
  if (steps < 1) {
    steps = 1;
  }
  const int32_t delta = (dy > 0.0f) ? -steps : steps;
  return std::clamp(current + delta, 1, 1000);
}

}  // namespace wds::ui

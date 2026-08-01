#pragma once

#include "theme.hpp"
#include "types.hpp"
#include "ui_painter.hpp"

#include <cmath>

namespace wds::interaction::caret {

// Bright warm white — reads clearly on both Surface and SurfaceVariant fills.
inline constexpr Color kColor{1.0f, 0.96f, 0.72f, 1.0f};
inline constexpr float kWidth = 3.0f;
// Full blink cycle (on + off). ~530 ms visible matches common editor carets.
inline constexpr float kBlinkPeriod = 1.06f;

inline bool blink_visible(float phase_seconds) noexcept {
  const float cycle = std::fmod(std::max(0.0f, phase_seconds), kBlinkPeriod);
  return cycle < kBlinkPeriod * 0.5f;
}

// Draw a caret at the trailing edge of centered (or measured) text inside `field`.
inline void paint(UiPainter& painter, const Rect& field, float text_end_x, float z,
                  float blink_phase) {
  if (!blink_visible(blink_phase)) {
    return;
  }
  painter.fill_rect_front({text_end_x, field.y + field.h * 0.18f, kWidth, field.h * 0.64f},
                          kColor, 0.0f, z);
}

}  // namespace wds::interaction::caret

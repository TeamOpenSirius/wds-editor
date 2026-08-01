#include "wds/interaction/widgets/slider.hpp"

#include "wds/interaction/theme.hpp"

#include <algorithm>
#include <cmath>

namespace wds::interaction {

Slider::Slider() = default;

void Slider::set_range(float min_value, float max_value) noexcept {
  min_ = min_value;
  max_ = std::max(max_value, min_value + 1e-6f);
  value_ = std::min(std::max(value_, min_), max_);
}

void Slider::set_value(float value) noexcept {
  value_ = std::min(std::max(value, min_), max_);
}

void Slider::set_from_position(Vec2 position) {
  const Rect abs = absolute_bounds();
  const float t = std::min(1.0f, std::max(0.0f, (position.x - abs.x) / std::max(abs.w, 1.0f)));
  const float next = min_ + t * (max_ - min_);
  if (std::fabs(next - value_) > 1e-5f) {
    value_ = next;
    if (on_change_) {
      on_change_(value_);
    }
  }
}

void Slider::paint(UiPainter& painter) const {
  if (!visible_) {
    return;
  }
  const Rect abs = absolute_bounds();
  const float track_h = std::min(theme::px(6.0f), abs.h);
  Rect track{abs.x, abs.y + (abs.h - track_h) * 0.5f, abs.w, track_h};
  painter.fill_rect(track, theme::kSurfaceVariant, track_h * 0.5f);

  const float t = (value_ - min_) / (max_ - min_);
  Rect fill{track.x, track.y, track.w * t, track.h};
  painter.fill_rect(fill, theme::kPrimary, track_h * 0.5f);
}

void Slider::on_pointer_down(const PointerDownEvent& event) {
  if (!enabled_ || mode_ != SliderMode::Interactive || event.button != PointerButton::Left) {
    return;
  }
  const bool started = !dragging_;
  dragging_ = true;
  set_visual_state(WidgetState::Pressed);
  if (started && on_drag_begin_) {
    on_drag_begin_();
  }
  set_from_position(event.position);
}

void Slider::on_pointer_up(const PointerUpEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) {
    return;
  }
  const bool ended = dragging_;
  dragging_ = false;
  set_visual_state(WidgetState::Normal);
  if (ended && on_drag_end_) {
    on_drag_end_();
  }
}

void Slider::on_pointer_move(const PointerMoveEvent& event) {
  if (!enabled_ || !dragging_) {
    return;
  }
  set_from_position(event.position);
}

}  // namespace wds::interaction

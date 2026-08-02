#include "wds/interaction/widgets/stepper.hpp"

#include "wds/interaction/theme.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

namespace wds::interaction {

Stepper::Stepper() = default;

void Stepper::set_range(int min_value, int max_value) noexcept {
  min_ = min_value;
  max_ = std::max(max_value, min_value);
  value_ = std::min(std::max(value_, min_), max_);
}

void Stepper::set_step(int step) noexcept { step_ = std::max(step, 1); }

void Stepper::set_value(int value) noexcept {
  value_ = std::min(std::max(value, min_), max_);
}

void Stepper::layout(const Rect& parent_bounds) {
  Widget::layout(parent_bounds);
  const Rect abs = absolute_bounds();
  const float btn_w = std::min(theme::px(18.0f), abs.w * 0.25f);
  minus_bounds_ = {abs.x, abs.y, btn_w, abs.h};
  plus_bounds_ = {abs.right() - btn_w, abs.y, btn_w, abs.h};
}

void Stepper::paint(UiPainter& painter) const {
  if (!visible_) {
    return;
  }
  const Rect abs = absolute_bounds();
  painter.fill_rect(abs, theme::kSurfaceVariant, theme::kCornerRadiusSm);
  painter.fill_rect(minus_bounds_, theme::kSurface, theme::kCornerRadiusSm);
  painter.fill_rect(plus_bounds_, theme::kSurface, theme::kCornerRadiusSm);
  painter.label(minus_bounds_, "-", theme::kOnSurface);
  painter.label(plus_bounds_, "+", theme::kOnSurface);

  Rect value_rect{minus_bounds_.right(), abs.y, plus_bounds_.x - minus_bounds_.right(), abs.h};
  painter.label(value_rect, std::to_string(value_), theme::kOnSurface);
}

void Stepper::bump(int delta) {
  const int next = std::min(std::max(value_ + delta, min_), max_);
  if (next != value_) {
    value_ = next;
    if (on_change_) {
      on_change_(value_);
    }
  }
}

void Stepper::on_pointer_down(const PointerDownEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) {
    return;
  }
  if (minus_bounds_.contains(event.position)) {
    bump(-step_);
    set_visual_state(WidgetState::Pressed);
  } else if (plus_bounds_.contains(event.position)) {
    bump(step_);
    set_visual_state(WidgetState::Pressed);
  }
}

void Stepper::on_click(const ClickEvent& event) {
  (void)event;  // Root already delivered the matching pointer-down event.
}

void FloatStepper::set_range(double min_value, double max_value) noexcept {
  min_ = min_value;
  max_ = std::max(max_value, min_value);
  set_value(value_);
}

void FloatStepper::set_step(double step) noexcept { step_ = std::max(step, 0.000001); }

void FloatStepper::set_value(double value) noexcept {
  value_ = std::min(std::max(value, min_), max_);
}

void FloatStepper::layout(const Rect& parent_bounds) {
  Widget::layout(parent_bounds);
  const Rect abs = absolute_bounds();
  const float button_width = std::min(theme::px(18.0f), abs.w * 0.25f);
  minus_bounds_ = {abs.x, abs.y, button_width, abs.h};
  plus_bounds_ = {abs.right() - button_width, abs.y, button_width, abs.h};
}

void FloatStepper::paint(UiPainter& painter) const {
  if (!visible_) {
    return;
  }
  const Rect abs = absolute_bounds();
  painter.fill_rect(abs, theme::kSurfaceVariant, theme::kCornerRadiusSm);
  painter.fill_rect(minus_bounds_, theme::kSurface, theme::kCornerRadiusSm);
  painter.fill_rect(plus_bounds_, theme::kSurface, theme::kCornerRadiusSm);
  painter.label(minus_bounds_, "-", theme::kOnSurface);
  painter.label(plus_bounds_, "+", theme::kOnSurface);
  std::ostringstream value_text;
  value_text << std::fixed << std::setprecision(1) << value_;
  painter.label({minus_bounds_.right(), abs.y, plus_bounds_.x - minus_bounds_.right(), abs.h},
                value_text.str(), theme::kOnSurface);
}

void FloatStepper::bump(double delta) {
  const double next = std::min(std::max(value_ + delta, min_), max_);
  if (next != value_) {
    value_ = next;
    if (on_change_) {
      on_change_(value_);
    }
  }
}

void FloatStepper::on_pointer_down(const PointerDownEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) {
    return;
  }
  if (minus_bounds_.contains(event.position)) {
    bump(-step_);
  } else if (plus_bounds_.contains(event.position)) {
    bump(step_);
  }
}

}  // namespace wds::interaction

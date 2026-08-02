#include "wds/interaction/widgets/button.hpp"

#include "wds/interaction/theme.hpp"

namespace wds::interaction {
namespace {

Color tone_base(ButtonTone tone) noexcept {
  switch (tone) {
    case ButtonTone::Confirm:
      return {0.20f, 0.58f, 0.36f, 1.0f};
    case ButtonTone::Cancel:
      return {0.32f, 0.34f, 0.38f, 1.0f};
    case ButtonTone::Default:
    default:
      return theme::kSurfaceVariant;
  }
}

}  // namespace

Button::Button(std::string label) : label_(std::move(label)) {}

void Button::paint(UiPainter& painter) const { paint_at(painter, 0.9f); }

void Button::paint_at(UiPainter& painter, float z) const {
  if (!visible_) {
    return;
  }
  constexpr float kZMax = 0.999f;
  const float z_fill = std::min(z, kZMax);
  const float z_text = std::min(z + 0.01f, kZMax);

  const Rect abs = absolute_bounds();
  const float scale = interaction_scale();
  const float dw = abs.w * (1.0f - scale) * 0.5f;
  const float dh = abs.h * (1.0f - scale) * 0.5f;
  Rect draw = {abs.x + dw, abs.y + dh, abs.w - 2.0f * dw, abs.h - 2.0f * dh};

  Color fill = tone_base(tone_);
  if (!enabled_) {
    fill = theme::kSurface;
  } else if (visual_state_ == WidgetState::Pressed) {
    fill = fill.lerp(theme::kOnSurface, 0.18f);
  } else if (visual_state_ == WidgetState::Hovered) {
    fill = fill.lerp(theme::kOnSurface, 0.10f);
  }

  painter.fill_rect(draw, fill, theme::kCornerRadiusMd, z_fill);
  if (!label_.empty()) {
    painter.label(draw, label_, theme::kOnSurface, z_text);
  }
}

void Button::on_pointer_down(const PointerDownEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) {
    return;
  }
  pressed_ = true;
  pointer_inside_ = absolute_bounds().contains(event.position);
  set_visual_state(WidgetState::Pressed);
}

void Button::on_pointer_up(const PointerUpEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) {
    return;
  }
  const bool inside = absolute_bounds().contains(event.position);
  pressed_ = false;
  pointer_inside_ = inside;
  set_visual_state(inside ? WidgetState::Hovered : WidgetState::Normal);
}

void Button::on_pointer_move(const PointerMoveEvent& event) {
  if (!enabled_) {
    return;
  }
  pointer_inside_ = absolute_bounds().contains(event.position);
  if (pressed_) {
    set_visual_state(WidgetState::Pressed);
  } else {
    set_visual_state(pointer_inside_ ? WidgetState::Hovered : WidgetState::Normal);
  }
}

void Button::on_click(const ClickEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) {
    return;
  }
  if (on_click_) {
    on_click_();
  }
}

}  // namespace wds::interaction

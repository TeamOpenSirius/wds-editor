#include "wds/interaction/widgets/checkbox.hpp"

#include "wds/interaction/theme.hpp"

#include <algorithm>

namespace wds::interaction {

Checkbox::Checkbox(std::string label) : label_(std::move(label)) {}

void Checkbox::paint(UiPainter& painter) const { paint_at(painter, 0.9f); }

void Checkbox::paint_at(UiPainter& painter, float z) const {
  if (!visible_) return;
  const Rect abs = absolute_bounds();
  const float side = std::clamp(abs.h * 0.72f, theme::px(9.0f), theme::px(14.0f));
  const float radius = side * 0.5f;
  const Vec2 center{abs.x + radius, abs.y + abs.h * 0.5f};
  if (checked_) {
    painter.fill_circle(center, radius, theme::kPrimary, z);
  } else {
    painter.fill_circle_outline(center, radius, theme::kSurfaceVariant, theme::kOutline,
                                theme::px(0.875f), z);
  }
  // Left-align tip text next to the box so the pair reads as one control.
  const float box_text_gap = theme::px(5.0f);
  painter.label({abs.x + side + box_text_gap, abs.y, std::max(0.0f, abs.w - side - box_text_gap), abs.h},
                label_, enabled_ ? theme::kOnSurface : theme::kOutline, z + 0.01f, false, 0.0f,
                true);
}

void Checkbox::on_click(const ClickEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) return;
  checked_ = !checked_;
  if (on_change_) on_change_(checked_);
}
}  // namespace wds::interaction

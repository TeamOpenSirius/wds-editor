#include "wds/interaction/widgets/modal.hpp"

#include "wds/interaction/theme.hpp"

namespace wds::interaction {

void Modal::close() {
  if (!visible_) return;
  visible_ = false;
  if (on_close_) on_close_();
}

void Modal::layout(const Rect& parent_bounds) {
  if (bounds_.w <= 0.0f || bounds_.h <= 0.0f) {
    bounds_ = {0.0f, 0.0f, parent_bounds.w, parent_bounds.h};
  }
  Widget::layout(parent_bounds);
}

void Modal::paint(UiPainter& painter) const {
  if (!visible_) return;
  const Rect abs = absolute_bounds();
  painter.fill_rect(abs, {0.0f, 0.0f, 0.0f, 0.55f}, 0.0f, 0.97f);
  Rect content = content_bounds_;
  content.x += abs.x;
  content.y += abs.y;
  painter.fill_rect(content, theme::kSurface, theme::kCornerRadiusMd, 0.98f);
  Widget::paint(painter);
}

Widget* Modal::hit_test(Vec2 point) {
  if (!visible_ || !enabled_ || !absolute_bounds().contains(point)) return nullptr;
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test(point)) return hit;
  }
  return this;
}

void Modal::on_click(const ClickEvent& event) {
  Rect content = content_bounds_;
  const Rect abs = absolute_bounds();
  content.x += abs.x;
  content.y += abs.y;
  if (close_on_backdrop_ && event.button == PointerButton::Left && !content.contains(event.position)) {
    close();
  }
}
}  // namespace wds::interaction

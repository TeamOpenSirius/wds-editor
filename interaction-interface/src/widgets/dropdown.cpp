#include "wds/interaction/widgets/dropdown.hpp"

#include "wds/interaction/popup_menu.hpp"
#include "wds/interaction/theme.hpp"
#include "wds/interaction/widget_root.hpp"

#include <algorithm>
#include <cmath>

namespace wds::interaction {
namespace {

float chevron_slot_w() noexcept { return std::max(18.0f, theme::px(11.0f)); }

void paint_chevron(UiPainter& painter, const Rect& abs, bool open, float z = 0.9f) {
  const float slot = chevron_slot_w();
  const int cx = static_cast<int>(std::lround(abs.right() - slot * 0.5f));
  const int cy = static_cast<int>(std::lround(abs.y + abs.h * 0.5f));
  // Filled ▼/▲ via 1px scanlines — axis-aligned quads cannot draw diagonals as an L/T.
#if defined(_WIN32)
  const int half_w = std::max(5, static_cast<int>(std::lround(theme::px(5.0f))));
  const int half_h = std::max(4, static_cast<int>(std::lround(theme::px(3.5f))));
#else
  const int half_w = std::max(4, static_cast<int>(std::lround(theme::px(4.0f))));
  const int half_h = std::max(3, static_cast<int>(std::lround(theme::px(2.5f))));
#endif
  const int h = half_h * 2 + 1;
  const int top = cy - half_h;
  for (int row = 0; row < h; ++row) {
    const float t = (h <= 1) ? 0.0f : static_cast<float>(row) / static_cast<float>(h - 1);
    const float span = open ? t : (1.0f - t);  // open = ▲, closed = ▼
    const int hw = std::max(0, static_cast<int>(std::lround(static_cast<float>(half_w) * span)));
    painter.fill_rect({static_cast<float>(cx - hw), static_cast<float>(top + row),
                       static_cast<float>(hw * 2 + 1), 1.0f},
                      theme::kOnSurfaceMuted, 0.0f, z);
  }
}

popup_menu::Placement placement_of(bool opens_upward) noexcept {
  return opens_upward ? popup_menu::Placement::Up : popup_menu::Placement::Down;
}

}  // namespace

Dropdown::Dropdown() = default;

void Dropdown::set_items(std::vector<std::string> items) {
  items_ = std::move(items);
  if (selected_index_ >= static_cast<int>(items_.size())) {
    selected_index_ = items_.empty() ? -1 : 0;
  }
}

void Dropdown::set_selected_index(int index) noexcept {
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    selected_index_ = items_.empty() ? -1 : 0;
    return;
  }
  selected_index_ = index;
}

const std::string& Dropdown::selected_label() const {
  static const std::string kEmpty;
  if (selected_index_ < 0 || selected_index_ >= static_cast<int>(items_.size())) {
    return kEmpty;
  }
  return items_[static_cast<std::size_t>(selected_index_)];
}

void Dropdown::paint(UiPainter& painter) const {
  if (!visible_) {
    return;
  }
  // While open, paint_popup_layer owns the host chrome so text is not alpha-stacked.
  if (open_) {
    return;
  }
  const Rect abs = absolute_bounds();
  painter.fill_rect_outline(abs, theme::kSurfaceVariant, theme::kOutline, theme::kCornerRadiusSm);
  const Rect text_bounds{abs.x + 4.0f, abs.y, std::max(0.0f, abs.w - chevron_slot_w() - 4.0f),
                         abs.h};
  painter.label(text_bounds, selected_label(), theme::kOnSurface);
  paint_chevron(painter, abs, open_);
}

void Dropdown::paint_popup_layer(UiPainter& painter) const {
  if (!visible_ || !open_ || items_.empty()) {
    return;
  }
  const Rect abs = absolute_bounds();
  const auto geom =
      popup_menu::layout(this, abs, items_.size(), menu_scroll_, placement_of(opens_upward_));
  popup_menu::paint_items(painter, geom, items_, selected_index_, hover_index_);

  // Redraw host field above menu contents (scrolled rows must not cover the value).
  painter.fill_rect_outline(abs, theme::kSurfaceVariant, theme::kOutline, theme::kCornerRadiusSm,
                            popup_menu::kHostZ);
  const Rect text_bounds{abs.x + 4.0f, abs.y, std::max(0.0f, abs.w - chevron_slot_w() - 4.0f),
                         abs.h};
  painter.label(text_bounds, selected_label(), theme::kOnSurface, popup_menu::kHostTextZ);
  paint_chevron(painter, abs, open_, popup_menu::kHostTextZ);
}

Widget* Dropdown::hit_test_popup(Vec2 point) {
  if (!visible_ || !open_ || items_.empty()) {
    return nullptr;
  }
  const auto geom = popup_menu::layout(this, absolute_bounds(), items_.size(), menu_scroll_,
                                       placement_of(opens_upward_));
  return geom.rect.contains(point) ? this : nullptr;
}

Widget* Dropdown::hit_test_popup_host(Vec2 point) {
  if (!visible_ || !enabled_) {
    return nullptr;
  }
  return absolute_bounds().contains(point) ? this : nullptr;
}

Widget* Dropdown::hit_test(Vec2 point) {
  if (!visible_ || !enabled_) {
    return nullptr;
  }
  if (hit_test_popup(point) != nullptr) {
    return this;
  }
  return absolute_bounds().contains(point) ? this : nullptr;
}

void Dropdown::close_own_popup() {
  if (!open_) {
    return;
  }
  open_ = false;
  menu_scroll_ = 0.0f;
  hover_index_ = -1;
  if (WidgetRoot* root = find_root()) {
    root->note_popup_closed(this);
  }
}

bool Dropdown::dismiss_popups(Vec2 point) {
  bool closed = false;
  const Rect abs = absolute_bounds();
  const auto geom =
      (open_ && !items_.empty())
          ? popup_menu::layout(this, abs, items_.size(), menu_scroll_, placement_of(opens_upward_))
          : popup_menu::Geometry{};
  if (open_ && !abs.contains(point) && !geom.rect.contains(point)) {
    close_own_popup();
    closed = true;
  }
  return Widget::dismiss_popups(point) || closed;
}

void Dropdown::on_pointer_down(const PointerDownEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left) {
    return;
  }
  const Rect abs = absolute_bounds();
  if (open_ && !items_.empty()) {
    const auto geom =
        popup_menu::layout(this, abs, items_.size(), menu_scroll_, placement_of(opens_upward_));
    if (geom.rect.contains(event.position)) {
      const int index = popup_menu::index_at_point(geom, event.position, items_.size());
      if (index >= 0) {
        selected_index_ = index;
        if (on_select_) {
          on_select_(index, items_[static_cast<std::size_t>(index)]);
        }
      }
      close_own_popup();
      return;
    }
  }

  if (!abs.contains(event.position)) {
    return;
  }

  if (open_) {
    close_own_popup();
    return;
  }
  open_ = true;
  menu_scroll_ = 0.0f;
  hover_index_ = -1;
  if (WidgetRoot* root = find_root()) {
    root->note_popup_opened(this);
  } else {
    close_sibling_popups();
  }
}

void Dropdown::on_pointer_move(const PointerMoveEvent& event) {
  if (!enabled_ || !open_ || items_.empty()) {
    hover_index_ = -1;
    return;
  }
  const auto geom = popup_menu::layout(this, absolute_bounds(), items_.size(), menu_scroll_,
                                       placement_of(opens_upward_));
  hover_index_ = popup_menu::index_at_point(geom, event.position, items_.size());
}

void Dropdown::on_click(const ClickEvent& /*event*/) {
  // Selection / toggle are handled on pointer-down so the menu closes before
  // ClickEvent would hit-test through to widgets underneath.
}

void Dropdown::on_scroll(const ScrollEvent& event) {
  if (!enabled_ || !open_ || items_.empty()) {
    return;
  }
  const auto geom = popup_menu::layout(this, absolute_bounds(), items_.size(), menu_scroll_,
                                       placement_of(opens_upward_));
  if (!geom.rect.contains(event.position)) {
    return;
  }
  menu_scroll_ = popup_menu::clamp_scroll(geom, menu_scroll_ - event.delta_y * geom.row_h);
  const auto scrolled = popup_menu::layout(this, absolute_bounds(), items_.size(), menu_scroll_,
                                           placement_of(opens_upward_));
  hover_index_ = popup_menu::index_at_point(scrolled, event.position, items_.size());
}

}  // namespace wds::interaction

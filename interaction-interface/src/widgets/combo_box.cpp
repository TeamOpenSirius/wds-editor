#include "wds/interaction/widgets/combo_box.hpp"

#include "wds/interaction/caret.hpp"
#include "wds/interaction/popup_menu.hpp"
#include "wds/interaction/theme.hpp"
#include "wds/interaction/widget_root.hpp"

#include <algorithm>
#include <cmath>

namespace wds::interaction {
namespace {

void pop_utf8_codepoint(std::string& text) {
  if (text.empty()) return;
  size_t i = text.size();
  do {
    --i;
  } while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80);
  text.erase(i);
}

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

ComboBox::ComboBox() = default;

void ComboBox::set_items(std::vector<std::string> items) { items_ = std::move(items); }

void ComboBox::set_text(std::string text) {
  text_ = std::move(text);
  committed_text_ = text_;
}

void ComboBox::accept_text(std::string text) {
  text_ = std::move(text);
  committed_text_ = text_;
  if (on_commit_) {
    on_commit_(text_);
  }
}

void ComboBox::commit_or_revert() {
  if (dropdown_only_) {
    text_ = committed_text_;
    return;
  }
  if (validator_ && !validator_(text_)) {
    text_ = committed_text_;
    return;
  }
  if (text_ == committed_text_) {
    return;
  }
  committed_text_ = text_;
  if (on_commit_) {
    on_commit_(text_);
  }
}

int ComboBox::selected_item_index() const noexcept {
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (items_[i] == text_) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void ComboBox::update(float delta_seconds) {
  Widget::update(delta_seconds);
  if (!dropdown_only_ && visual_state_ == WidgetState::Focused) {
    caret_blink_t_ += delta_seconds;
  }
}

void ComboBox::paint(UiPainter& painter) const { paint_at(painter, 0.9f); }

void ComboBox::paint_at(UiPainter& painter, float z) const {
  if (!visible_) {
    return;
  }
  // While open, paint_popup_layer owns the host chrome so text is not alpha-stacked.
  if (open_) {
    return;
  }
  constexpr float kZMax = 0.999f;
  const float z_fill = std::min(z, kZMax);
  const float z_text = std::min(z + 0.01f, kZMax);
  const float z_caret = std::min(z + 0.02f, kZMax);

  const Rect abs = absolute_bounds();
  const Color outline =
      visual_state_ == WidgetState::Focused ? theme::kPrimary : theme::kOutline;
  painter.fill_rect_outline(abs, theme::kSurfaceVariant, outline, theme::kCornerRadiusSm, z_fill);

  const Rect text_bounds{abs.x + 4.0f, abs.y, std::max(0.0f, abs.w - chevron_slot_w() - 4.0f),
                         abs.h};
  painter.label(text_bounds, text_, theme::kOnSurface, z_text);

  if (!dropdown_only_ && visual_state_ == WidgetState::Focused) {
    const float px = theme::kFontSizeMd;
    const Vec2 size = painter.measure_text(text_, px);
    const float text_x = text_bounds.x + std::max(0.0f, (text_bounds.w - size.x) * 0.5f);
    caret::paint(painter, abs, text_x + size.x, z_caret, caret_blink_t_);
  }

  paint_chevron(painter, abs, open_, z_text);
}

void ComboBox::paint_popup_layer(UiPainter& painter) const {
  if (!visible_ || !open_ || items_.empty()) {
    return;
  }
  const Rect abs = absolute_bounds();
  const auto geom =
      popup_menu::layout(this, abs, items_.size(), menu_scroll_, placement_of(opens_upward_));
  popup_menu::paint_items(painter, geom, items_, selected_item_index(), hover_index_);

  const Color outline =
      visual_state_ == WidgetState::Focused ? theme::kPrimary : theme::kOutline;
  painter.fill_rect_outline(abs, theme::kSurfaceVariant, outline, theme::kCornerRadiusSm,
                            popup_menu::kHostZ);
  const Rect text_bounds{abs.x + 4.0f, abs.y, std::max(0.0f, abs.w - chevron_slot_w() - 4.0f),
                         abs.h};
  painter.label(text_bounds, text_, theme::kOnSurface, popup_menu::kHostTextZ);
  paint_chevron(painter, abs, open_, popup_menu::kHostTextZ);
}

Widget* ComboBox::hit_test_popup(Vec2 point) {
  if (!visible_ || !open_ || items_.empty()) {
    return nullptr;
  }
  const auto geom = popup_menu::layout(this, absolute_bounds(), items_.size(), menu_scroll_,
                                       placement_of(opens_upward_));
  return geom.rect.contains(point) ? this : nullptr;
}

Widget* ComboBox::hit_test_popup_host(Vec2 point) {
  if (!visible_ || !enabled_) {
    return nullptr;
  }
  return absolute_bounds().contains(point) ? this : nullptr;
}

Widget* ComboBox::hit_test(Vec2 point) {
  if (!visible_ || !enabled_) {
    return nullptr;
  }
  if (hit_test_popup(point) != nullptr) {
    return this;
  }
  return absolute_bounds().contains(point) ? this : nullptr;
}

void ComboBox::close_own_popup() {
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

bool ComboBox::dismiss_popups(Vec2 point) {
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

void ComboBox::on_pointer_down(const PointerDownEvent& event) {
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
        accept_text(items_[static_cast<std::size_t>(index)]);
      }
      close_own_popup();
      return;
    }
  }

  if (!abs.contains(event.position)) {
    return;
  }

  if (dropdown_only_ || event.position.x > abs.right() - chevron_slot_w()) {
    if (open_) {
      close_own_popup();
    } else {
      open_ = true;
      menu_scroll_ = 0.0f;
      hover_index_ = -1;
      if (WidgetRoot* root = find_root()) {
        root->note_popup_opened(this);
      } else {
        close_sibling_popups();
      }
    }
    set_visual_state(WidgetState::Focused);
  } else {
    close_own_popup();
    set_visual_state(WidgetState::Focused);
  }
  reset_caret_blink();
}

void ComboBox::on_pointer_move(const PointerMoveEvent& event) {
  if (!enabled_ || !open_ || items_.empty()) {
    hover_index_ = -1;
    return;
  }
  const auto geom = popup_menu::layout(this, absolute_bounds(), items_.size(), menu_scroll_,
                                       placement_of(opens_upward_));
  hover_index_ = popup_menu::index_at_point(geom, event.position, items_.size());
}

void ComboBox::on_key_down(const KeyDownEvent& event) {
  if (!enabled_ || visual_state_ != WidgetState::Focused) {
    return;
  }
  if (event.key == KeyCode::Escape) {
    text_ = committed_text_;
    close_own_popup();
    set_visual_state(WidgetState::Normal);
    return;
  }
  if (event.key == KeyCode::Enter) {
    close_own_popup();
    // Blur (and commit/revert) is handled by WidgetRoot when Focused is cleared.
    set_visual_state(WidgetState::Normal);
    return;
  }
  if (dropdown_only_) {
    return;
  }
  if (event.key == KeyCode::Backspace && !text_.empty()) {
    pop_utf8_codepoint(text_);
    reset_caret_blink();
  }
}

void ComboBox::on_focus() { reset_caret_blink(); }

void ComboBox::on_blur() {
  commit_or_revert();
  close_own_popup();
}

void ComboBox::on_text_input(const TextInputEvent& event) {
  if (!enabled_ || dropdown_only_ || visual_state_ != WidgetState::Focused) {
    return;
  }
  for (const char c : event.text) {
    if (c >= 32 && c < 127) {
      text_.push_back(c);
    }
  }
  reset_caret_blink();
}

void ComboBox::on_scroll(const ScrollEvent& event) {
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

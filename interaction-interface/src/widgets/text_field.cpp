#include "wds/interaction/widgets/text_field.hpp"

#include "wds/interaction/caret.hpp"
#include "wds/interaction/theme.hpp"
#include "wds/interaction/utf8_edit.hpp"

#include <algorithm>

namespace wds::interaction {

TextField::TextField(std::string placeholder) : placeholder_(std::move(placeholder)) {}

void TextField::set_text(std::string text) {
  text_ = std::move(text);
  committed_text_ = text_;
  caret_ = text_.size();
}

void TextField::commit_or_revert() {
  if (validator_ && !validator_(text_)) {
    text_ = committed_text_;
    caret_ = text_.size();
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

void TextField::update(float delta_seconds) {
  Widget::update(delta_seconds);
  if (error_flash_remaining_ > 0.0f) {
    error_flash_remaining_ = std::max(0.0f, error_flash_remaining_ - delta_seconds);
  }
  if (visual_state_ == WidgetState::Focused) {
    caret_blink_t_ += delta_seconds;
  }
}

void TextField::paint(UiPainter& painter) const {
  paint_at(painter, 0.9f);
}

void TextField::paint_at(UiPainter& painter, float z) const {
  if (!visible_) {
    return;
  }
  // Ortho far plane is 1.0 — z must stay strictly below it or the quad is clipped.
  constexpr float kZMax = 0.999f;
  const float z_fill = std::min(z, kZMax);
  const float z_text = std::min(z + 0.01f, kZMax);
  const float z_caret = std::min(z + 0.02f, kZMax);

  const Rect abs = absolute_bounds();
  const bool focused = enabled_ && visual_state_ == WidgetState::Focused;
  const Color washed = theme::kSurfaceVariant.lerp(Color{0.92f, 0.92f, 0.94f, 1.0f}, 0.42f);
  const Color fill = !enabled_ ? washed : (focused ? theme::kSurface : theme::kSurfaceVariant);
  const Color outline = (text_invalid() || error_flashing()) ? theme::kError
                       : !enabled_    ? washed.lerp(theme::kOutline, 0.35f)
                       : focused      ? theme::kPrimary
                                      : theme::kOutline;
  painter.fill_rect_outline(abs, fill, outline, theme::kCornerRadiusSm, z_fill);
  // When focused, skip placeholder so the caret is not covered by muted hint text.
  const bool show_placeholder = text_.empty() && !focused;
  const std::string& shown = show_placeholder ? placeholder_ : text_;
  const Color color = !enabled_          ? theme::kOnSurfaceMuted.lerp(Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.35f)
                    : show_placeholder ? theme::kOnSurfaceMuted
                                       : theme::kOnSurface;
  painter.label(abs, shown, color, z_text);

  if (focused) {
    const float px = theme::kFontSizeMd;
    const Vec2 size = painter.measure_text(text_, px);
    const std::string prefix = text_.substr(0, std::min(caret_, text_.size()));
    const Vec2 prefix_size = painter.measure_text(prefix, px);
    const float text_x = abs.x + std::max(0.0f, (abs.w - size.x) * 0.5f);
    caret::paint(painter, abs, text_x + prefix_size.x, z_caret, caret_blink_t_);
  }
}

void TextField::on_pointer_down(const PointerDownEvent&) {
  if (!enabled_) {
    return;
  }
  set_visual_state(WidgetState::Focused);
  reset_caret_blink();
}

void TextField::on_focus() { reset_caret_blink(); }

void TextField::on_key_down(const KeyDownEvent& event) {
  if (!enabled_ || visual_state_ != WidgetState::Focused) {
    return;
  }
  if (event.key == KeyCode::Enter) {
    // Blur (and commit/revert) is handled by WidgetRoot when Focused is cleared.
    set_visual_state(WidgetState::Normal);
    return;
  }
  if (event.key == KeyCode::Escape) {
    text_ = committed_text_;
    set_visual_state(WidgetState::Normal);
    return;
  }
  if (event.key == KeyCode::Left) {
    caret_ = utf8_edit::prev_offset(text_, caret_);
    reset_caret_blink();
    return;
  }
  if (event.key == KeyCode::Right) {
    caret_ = utf8_edit::next_offset(text_, caret_);
    reset_caret_blink();
    return;
  }
  if (event.key == KeyCode::Backspace && caret_ > 0) {
    utf8_edit::erase_prev(text_, caret_);
    reset_caret_blink();
    if (on_change_) {
      on_change_(text_);
    }
    return;
  }
  if (event.key == KeyCode::Delete && caret_ < text_.size()) {
    utf8_edit::erase_next(text_, caret_);
    reset_caret_blink();
    if (on_change_) {
      on_change_(text_);
    }
  }
}

void TextField::on_blur() { commit_or_revert(); }

void TextField::on_text_input(const TextInputEvent& event) {
  if (!enabled_ || visual_state_ != WidgetState::Focused) {
    return;
  }
  utf8_edit::insert(text_, caret_, event.text);
  reset_caret_blink();
  if (on_change_) {
    on_change_(text_);
  }
}

}  // namespace wds::interaction

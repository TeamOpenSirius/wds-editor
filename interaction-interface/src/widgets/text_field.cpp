#include "wds/interaction/widgets/text_field.hpp"

#include "wds/interaction/caret.hpp"
#include "wds/interaction/theme.hpp"

#include <algorithm>

namespace wds::interaction {

TextField::TextField(std::string placeholder) : placeholder_(std::move(placeholder)) {}

void TextField::set_text(std::string text) {
  text_ = std::move(text);
  committed_text_ = text_;
}

void TextField::commit_or_revert() {
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

void TextField::update(float delta_seconds) {
  Widget::update(delta_seconds);
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
  const bool focused = visual_state_ == WidgetState::Focused;
  const Color fill = focused ? theme::kSurface : theme::kSurfaceVariant;
  const Color outline = focused ? theme::kPrimary : theme::kOutline;
  painter.fill_rect_outline(abs, fill, outline, theme::kCornerRadiusSm, z_fill);
  // When focused, skip placeholder so the caret is not covered by muted hint text.
  const bool show_placeholder = text_.empty() && !focused;
  const std::string& shown = show_placeholder ? placeholder_ : text_;
  const Color color = show_placeholder ? theme::kOnSurfaceMuted : theme::kOnSurface;
  painter.label(abs, shown, color, z_text);

  if (focused) {
    const float px = theme::kFontSizeMd;
    const Vec2 size = painter.measure_text(text_, px);
    const float text_x = abs.x + std::max(0.0f, (abs.w - size.x) * 0.5f);
    caret::paint(painter, abs, text_x + size.x, z_caret, caret_blink_t_);
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
  if (event.key == KeyCode::Backspace && !text_.empty()) {
    text_.pop_back();
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
  text_ += event.text;
  reset_caret_blink();
  if (on_change_) {
    on_change_(text_);
  }
}

}  // namespace wds::interaction

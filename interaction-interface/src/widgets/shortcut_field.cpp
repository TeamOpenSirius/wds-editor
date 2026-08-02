#include "wds/interaction/widgets/shortcut_field.hpp"

#include "wds/interaction/editor_shortcuts.hpp"
#include "wds/interaction/platform.hpp"
#include "wds/interaction/theme.hpp"

#include <algorithm>

namespace wds::interaction {

ShortcutField::ShortcutField() {
  committed_ = {};
  draft_ = committed_;
  capture_label_ = format_shortcut_chord(committed_);
}

void ShortcutField::set_chord(ShortcutChord chord) {
  chord.mods = normalize_primary(chord.mods);
  committed_ = chord;
  draft_ = chord;
  capturing_ = false;
  conflict_highlight_ = false;
  reject_flash_t_ = 0.0f;
  capture_label_ = format_shortcut_chord(committed_);
}

void ShortcutField::clear_chord() {
  committed_ = {};
  draft_ = {};
  capturing_ = false;
  reject_flash_t_ = 0.0f;
  capture_label_ = format_shortcut_chord(committed_);
  set_visual_state(WidgetState::Normal);
  if (on_change_) on_change_(committed_);
}

void ShortcutField::begin_capture() {
  capturing_ = true;
  draft_ = committed_;
  capture_label_.clear();
  reject_flash_t_ = 0.0f;
}

void ShortcutField::cancel_capture() {
  capturing_ = false;
  draft_ = committed_;
  capture_label_ = format_shortcut_chord(committed_);
  set_visual_state(WidgetState::Normal);
}

void ShortcutField::flash_reject() {
  reject_flash_t_ = 0.85f;
  draft_ = committed_;
  capture_label_ = format_shortcut_chord(committed_);
}

void ShortcutField::refresh_capture_label(const Modifiers& mods) {
  capture_label_ = format_shortcut_modifiers(mods);
}

void ShortcutField::try_commit(ShortcutChord chord) {
  chord.mods = normalize_primary(chord.mods);
  if (is_forbidden_shortcut_key(chord.key) || !is_completing_shortcut_key(chord.key)) {
    flash_reject();
    return;
  }
  // Conflicts are allowed temporarily; the settings dialog marks collisions in red.
  committed_ = chord;
  draft_ = chord;
  capturing_ = false;
  capture_label_ = format_shortcut_chord(committed_);
  set_visual_state(WidgetState::Normal);
  if (on_change_) on_change_(committed_);
}

std::string ShortcutField::display_text() const {
  if (capturing_) {
    if (!capture_label_.empty()) return capture_label_;
    return "按下快捷键";
  }
  return format_shortcut_chord(committed_);
}

void ShortcutField::update(float delta_seconds) {
  Widget::update(delta_seconds);
  if (reject_flash_t_ > 0.0f) {
    reject_flash_t_ = std::max(0.0f, reject_flash_t_ - delta_seconds);
  }
}

void ShortcutField::paint(UiPainter& painter) const { paint_at(painter, 0.9f); }

void ShortcutField::paint_at(UiPainter& painter, float z) const {
  if (!visible_) return;
  constexpr float kZMax = 0.999f;
  const float z_fill = std::min(z, kZMax);
  const float z_text = std::min(z + 0.01f, kZMax);

  const Rect abs = absolute_bounds();
  const bool focused = visual_state_ == WidgetState::Focused;
  const bool conflict = conflict_highlight_ || reject_flash_t_ > 0.0f;
  const Color fill = focused ? theme::kSurface : theme::kSurfaceVariant;
  Color outline = focused ? theme::kPrimary : theme::kOutline;
  if (conflict) outline = theme::kError;
  painter.fill_rect_outline(abs, fill, outline, theme::kCornerRadiusSm, z_fill);

  const std::string shown = display_text();
  const bool placeholder = capturing_ && capture_label_.empty();
  const Color color = placeholder ? theme::kOnSurfaceMuted
                                  : (conflict ? theme::kError : theme::kOnSurface);
  painter.label(abs, shown, color, z_text);
}

void ShortcutField::on_pointer_down(const PointerDownEvent&) {
  if (!enabled_) return;
  set_visual_state(WidgetState::Focused);
  begin_capture();
}

void ShortcutField::on_focus() { begin_capture(); }

void ShortcutField::on_blur() {
  // Click-away / focus loss: discard in-progress capture.
  capturing_ = false;
  draft_ = committed_;
  capture_label_ = format_shortcut_chord(committed_);
}

void ShortcutField::on_key_down(const KeyDownEvent& event) {
  if (!enabled_ || visual_state_ != WidgetState::Focused) return;

  if (event.key == KeyCode::Escape || event.key == KeyCode::Enter) {
    cancel_capture();
    return;
  }

  // Modifier-only (or unmapped) keys: show mods, keep capturing.
  if (!is_completing_shortcut_key(event.key)) {
    if (is_forbidden_shortcut_key(event.key)) {
      flash_reject();
      return;
    }
    refresh_capture_label(event.mods);
    return;
  }

  ShortcutChord next{event.key, event.mods};
  try_commit(next);
}

void ShortcutField::on_key_up(const KeyUpEvent& event) {
  if (!enabled_ || visual_state_ != WidgetState::Focused || !capturing_) return;
  // After releasing a modifier, keep showing remaining held mods.
  refresh_capture_label(event.mods);
}

}  // namespace wds::interaction

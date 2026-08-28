#include "wds/interaction/editor_input.hpp"

#include "wds/interaction/platform.hpp"

#include <algorithm>
#include <cmath>

namespace wds::interaction {
namespace {

bool is_left(PointerButton button) noexcept { return button == PointerButton::Left; }
bool is_right(PointerButton button) noexcept { return button == PointerButton::Right; }
bool is_middle(PointerButton button) noexcept { return button == PointerButton::Middle; }

}  // namespace

bool is_primary_modifier(const Modifiers& mods) noexcept {
  return primary_modifier_down(mods);
}

bool is_visible_range_wheel_modifiers(const Modifiers& mods) noexcept {
  if (!is_primary_modifier(mods) || mods.shift || mods.alt) {
    return false;
  }
#ifdef __APPLE__
  return !mods.control;
#else
  return !mods.super;
#endif
}

bool is_toggle_select(const PointerDownEvent& event) noexcept {
  return is_left(event.button) && is_primary_modifier(event.mods);
}

bool is_marquee_select(const PointerDownEvent& event) noexcept {
  return is_left(event.button) && is_primary_modifier(event.mods);
}

bool is_select_or_edit_note(const PointerDownEvent& event) noexcept {
  return is_left(event.button) && !is_primary_modifier(event.mods);
}

bool is_clear_selection_click(const PointerUpEvent& event) noexcept {
  return is_left(event.button) && !is_primary_modifier(event.mods);
}

bool is_delete_single_note(const PointerDownEvent& event) noexcept {
  return is_middle(event.button);
}

bool is_cancel_placement(const PointerDownEvent& event) noexcept {
  return is_middle(event.button);
}

bool is_place_hold_star(const PointerDownEvent& event, bool scratch_hold) noexcept {
  // Normal: Shift+Right; ScratchHold: Shift+Left.
  const bool side = scratch_hold ? is_left(event.button) : is_right(event.button);
  return side && event.mods.shift;
}

bool is_chain_hold_body(const PointerDownEvent& event, bool scratch_hold) noexcept {
  // Continuous chain is ScratchHold-only (left click). Normal Hold does not chain.
  if (!scratch_hold) return false;
  return is_left(event.button) && !event.mods.shift;
}

bool is_finish_hold_body(PointerButton button, bool scratch_hold) noexcept {
  // Normal: release Left; ScratchHold: release Right.
  return scratch_hold ? is_right(button) : is_left(button);
}

bool is_begin_place_left(const PointerDownEvent& event) noexcept {
  return is_left(event.button) && !is_primary_modifier(event.mods);
}

bool is_begin_place_right(const PointerDownEvent& event) noexcept {
  return is_right(event.button);
}

bool is_place_button(PointerButton button) noexcept {
  return is_left(button) || is_right(button);
}

bool is_left_button(PointerButton button) noexcept { return is_left(button); }
bool is_right_button(PointerButton button) noexcept { return is_right(button); }

bool is_curve_fill_modifiers(const Modifiers& mods) noexcept {
  return mods.shift && is_primary_modifier(mods) && !mods.alt;
}

bool is_curve_fill_modifier_press(const KeyDownEvent& event) noexcept {
  return !event.repeat && is_curve_fill_modifiers(event.mods);
}

bool is_curve_fill_placement_allowed(bool place_hold_body, bool scratch_hold) noexcept {
  return place_hold_body && scratch_hold;
}

bool is_curve_fill_confirm(const PointerDownEvent& event) noexcept {
  return is_left(event.button) && is_curve_fill_modifiers(event.mods);
}

bool is_curve_fill_confirm(const PointerUpEvent& event) noexcept {
  return is_right(event.button) && is_curve_fill_modifiers(event.mods);
}

bool suppress_idle_placement_ghost(const Modifiers& mods, bool note_drawing) noexcept {
  return is_primary_modifier(mods) && !note_drawing;
}

bool is_vertical_swipe(SwipeDirection swipe) noexcept {
  return swipe == SwipeDirection::Up || swipe == SwipeDirection::Down;
}

bool is_horizontal_swipe(SwipeDirection swipe) noexcept {
  return swipe == SwipeDirection::Left || swipe == SwipeDirection::Right;
}

PlaceIntent resolve_place_intent(PointerButton button, SwipeDirection swipe,
                                 bool was_click) noexcept {
  const SwipeDirection effective =
      (was_click || swipe == SwipeDirection::None) ? SwipeDirection::None : swipe;

  if (is_left(button)) {
    // Down is not a place gesture: treat as resting Tap (same as no swipe).
    if (effective == SwipeDirection::None || effective == SwipeDirection::Down) {
      return PlaceIntent::Tap;
    }
    if (effective == SwipeDirection::Left) return PlaceIntent::ExTap;
    if (effective == SwipeDirection::Right) return PlaceIntent::HoldStart;
    // Hold body: upward only (later time).
    if (effective == SwipeDirection::Up) return PlaceIntent::HoldBody;
    return PlaceIntent::None;
  }
  if (is_right(button)) {
    if (effective == SwipeDirection::None || effective == SwipeDirection::Down) {
      return PlaceIntent::Flick;
    }
    if (effective == SwipeDirection::Left) return PlaceIntent::FlickLeft;
    if (effective == SwipeDirection::Right) return PlaceIntent::FlickRight;
    if (effective == SwipeDirection::Up) return PlaceIntent::ScratchHoldBody;
    return PlaceIntent::None;
  }
  return PlaceIntent::None;
}

bool is_delete_selection_key(KeyCode key) noexcept { return key == KeyCode::Delete; }

namespace {

std::array<int, 6>& slots_storage() noexcept {
  static std::array<int, 6> slots{1, 2, 3, 4, 6, 12};
  return slots;
}

bool& invert_scroll_storage() noexcept {
  static bool invert = false;
  return invert;
}

bool& invert_visible_range_scroll_storage() noexcept {
  static bool invert = false;
  return invert;
}

float& scroll_wheel_speed_storage() noexcept {
  static float speed = 1.0f;
  return speed;
}

}  // namespace

std::array<int, 6>& width_slot_values() noexcept { return slots_storage(); }

const std::array<int, 6>& width_slot_values_const() noexcept { return slots_storage(); }

bool set_width_slot_values(const std::array<int, 6>& values) noexcept {
  for (int v : values) {
    if (v < 1 || v > 12) return false;
  }
  slots_storage() = values;
  return true;
}

bool invert_scroll_wheel() noexcept { return invert_scroll_storage(); }

void set_invert_scroll_wheel(bool enabled) noexcept { invert_scroll_storage() = enabled; }

bool invert_visible_range_scroll() noexcept { return invert_visible_range_scroll_storage(); }

void set_invert_visible_range_scroll(bool enabled) noexcept {
  invert_visible_range_scroll_storage() = enabled;
}

float scroll_wheel_speed() noexcept { return scroll_wheel_speed_storage(); }

void set_scroll_wheel_speed(float speed) noexcept {
  if (!std::isfinite(speed)) {
    speed = 1.0f;
  } else {
    if (speed < 0.25f) speed = 0.25f;
    if (speed > 3.0f) speed = 3.0f;
  }
  scroll_wheel_speed_storage() = speed;
}

std::optional<int> default_width_for_key(KeyCode key) noexcept {
  // Match configured width-slot chords (bare key, no modifiers).
  const auto& slots = slots_storage();
  static constexpr EditorShortcut kIds[6] = {
      EditorShortcut::WidthSlot0, EditorShortcut::WidthSlot1, EditorShortcut::WidthSlot2,
      EditorShortcut::WidthSlot3, EditorShortcut::WidthSlot4, EditorShortcut::WidthSlot5,
  };
  for (int i = 0; i < 6; ++i) {
    const ShortcutChord& chord = editor_shortcut(kIds[i]);
    if (chord.key == key && !chord.mods.any()) return slots[static_cast<std::size_t>(i)];
  }
  return std::nullopt;
}

EditKeyResult resolve_edit_key(const KeyDownEvent& event) noexcept {
  const ShortcutChord chord{event.key, normalize_primary(event.mods)};
  if (chord == editor_shortcut(EditorShortcut::DeleteSelection)) {
    return {EditKeyAction::DeleteSelection, 0};
  }
  static constexpr EditorShortcut kWidthIds[6] = {
      EditorShortcut::WidthSlot0, EditorShortcut::WidthSlot1, EditorShortcut::WidthSlot2,
      EditorShortcut::WidthSlot3, EditorShortcut::WidthSlot4, EditorShortcut::WidthSlot5,
  };
  for (int i = 0; i < 6; ++i) {
    if (chord == editor_shortcut(kWidthIds[i])) {
      return {EditKeyAction::SetDefaultWidth, slots_storage()[static_cast<std::size_t>(i)]};
    }
  }
  return {};
}

ShortcutChord chord_toggle_playback() noexcept {
  return editor_shortcut(EditorShortcut::TogglePlayback);
}
ShortcutChord chord_pause_playback() noexcept {
  return editor_shortcut(EditorShortcut::PausePlayback);
}
ShortcutChord chord_save() noexcept { return editor_shortcut(EditorShortcut::Save); }
ShortcutChord chord_open() noexcept { return editor_shortcut(EditorShortcut::Open); }
ShortcutChord chord_undo() noexcept { return editor_shortcut(EditorShortcut::Undo); }
ShortcutChord chord_redo() noexcept { return editor_shortcut(EditorShortcut::Redo); }
ShortcutChord chord_copy() noexcept { return editor_shortcut(EditorShortcut::Copy); }
ShortcutChord chord_paste() noexcept { return editor_shortcut(EditorShortcut::Paste); }
ShortcutChord chord_mirror() noexcept { return editor_shortcut(EditorShortcut::Mirror); }
ShortcutChord chord_mirror_about_center() noexcept {
  return editor_shortcut(EditorShortcut::MirrorAboutCenter);
}
ShortcutChord chord_nudge_up() noexcept { return editor_shortcut(EditorShortcut::NudgeUp); }
ShortcutChord chord_nudge_down() noexcept { return editor_shortcut(EditorShortcut::NudgeDown); }
ShortcutChord chord_nudge_left() noexcept { return editor_shortcut(EditorShortcut::NudgeLeft); }
ShortcutChord chord_nudge_right() noexcept { return editor_shortcut(EditorShortcut::NudgeRight); }
ShortcutChord chord_delete_selection() noexcept {
  return editor_shortcut(EditorShortcut::DeleteSelection);
}

// Ctrl+Shift+F11 (Cmd+Shift+F11 on macOS) — avoids bare F11 (macOS Show Desktop /
// browser fullscreen) and Ctrl+Cmd+F (system Enter Full Screen).
ShortcutChord chord_toggle_fullscreen() noexcept {
  return editor_shortcut(EditorShortcut::ToggleFullscreen);
}

ShortcutChord chord_width_slot(int slot_index) noexcept {
  static constexpr EditorShortcut kIds[6] = {
      EditorShortcut::WidthSlot0, EditorShortcut::WidthSlot1, EditorShortcut::WidthSlot2,
      EditorShortcut::WidthSlot3, EditorShortcut::WidthSlot4, EditorShortcut::WidthSlot5,
  };
  const int idx = std::clamp(slot_index, 0, 5);
  return editor_shortcut(kIds[idx]);
}

ShortcutChord chord_playback_rate_slot(int slot_index) noexcept {
  static constexpr EditorShortcut kIds[4] = {
      EditorShortcut::PlaybackRate0, EditorShortcut::PlaybackRate1, EditorShortcut::PlaybackRate2,
      EditorShortcut::PlaybackRate3,
  };
  const int idx = std::clamp(slot_index, 0, 3);
  return editor_shortcut(kIds[idx]);
}

std::optional<float> playback_rate_for_slot(int slot_index) noexcept {
  static constexpr float kRates[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  if (slot_index < 0 || slot_index > 3) return std::nullopt;
  return kRates[slot_index];
}

ShortcutChord chord_toggle_sfx_mute() noexcept {
  return editor_shortcut(EditorShortcut::ToggleSfxMute);
}

}  // namespace wds::interaction

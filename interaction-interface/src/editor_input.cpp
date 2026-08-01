#include "wds/interaction/editor_input.hpp"

#include <algorithm>

namespace wds::interaction {
namespace {

bool is_left(PointerButton button) noexcept { return button == PointerButton::Left; }
bool is_right(PointerButton button) noexcept { return button == PointerButton::Right; }
bool is_middle(PointerButton button) noexcept { return button == PointerButton::Middle; }

}  // namespace

bool is_primary_modifier(const Modifiers& mods) noexcept {
  return primary_modifier_down(mods);
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

float scroll_wheel_speed() noexcept { return scroll_wheel_speed_storage(); }

void set_scroll_wheel_speed(float speed) noexcept {
  if (speed < 0.25f) speed = 0.25f;
  if (speed > 3.0f) speed = 3.0f;
  scroll_wheel_speed_storage() = speed;
}

std::optional<int> default_width_for_key(KeyCode key) noexcept {
  // Letter values match ASCII (GLFW maps A–Z → 65–90). Compare as int so
  // Q/W/E/S/D work even though KeyCode only enumerates A and Z explicitly.
  const auto& slots = slots_storage();
  switch (static_cast<int>(key)) {
    case 'Q':
      return slots[0];
    case 'W':
      return slots[1];
    case 'E':
      return slots[2];
    case 'A':
      return slots[3];
    case 'S':
      return slots[4];
    case 'D':
      return slots[5];
    default:
      return std::nullopt;
  }
}

EditKeyResult resolve_edit_key(const KeyDownEvent& event) noexcept {
  if (is_delete_selection_key(event.key)) {
    return {EditKeyAction::DeleteSelection, 0};
  }
  if (const auto width = default_width_for_key(event.key)) {
    return {EditKeyAction::SetDefaultWidth, *width};
  }
  return {};
}

ShortcutChord chord_toggle_playback() noexcept { return {KeyCode::Space, {}}; }
ShortcutChord chord_pause_playback() noexcept {
  Modifiers mods;
  mods.shift = true;
  return {KeyCode::Space, mods};
}
ShortcutChord chord_save() noexcept { return chord_primary(static_cast<KeyCode>('S')); }
ShortcutChord chord_open() noexcept { return chord_primary(static_cast<KeyCode>('O')); }
ShortcutChord chord_undo() noexcept { return chord_primary(static_cast<KeyCode>('Z')); }
ShortcutChord chord_redo() noexcept { return chord_primary(static_cast<KeyCode>('Y')); }
ShortcutChord chord_copy() noexcept { return chord_primary(static_cast<KeyCode>('C')); }
ShortcutChord chord_paste() noexcept { return chord_primary(static_cast<KeyCode>('V')); }
ShortcutChord chord_mirror() noexcept { return chord_primary(static_cast<KeyCode>('M')); }
ShortcutChord chord_mirror_about_center() noexcept {
  return chord_primary(static_cast<KeyCode>('M'), true);
}
ShortcutChord chord_nudge_up() noexcept { return {KeyCode::Up, {}}; }
ShortcutChord chord_nudge_down() noexcept { return {KeyCode::Down, {}}; }
ShortcutChord chord_nudge_left() noexcept { return {KeyCode::Left, {}}; }
ShortcutChord chord_nudge_right() noexcept { return {KeyCode::Right, {}}; }
ShortcutChord chord_delete_selection() noexcept { return {KeyCode::Delete, {}}; }

// Ctrl+Shift+F11 (Cmd+Shift+F11 on macOS) — avoids bare F11 (macOS Show Desktop /
// browser fullscreen) and Ctrl+Cmd+F (system Enter Full Screen).
ShortcutChord chord_toggle_fullscreen() noexcept {
  return chord_primary(KeyCode::F11, /*shift=*/true);
}

ShortcutChord chord_width_slot(int slot_index) noexcept {
  static constexpr KeyCode kKeys[6] = {
      static_cast<KeyCode>('Q'), static_cast<KeyCode>('W'), static_cast<KeyCode>('E'),
      KeyCode::A,                static_cast<KeyCode>('S'), static_cast<KeyCode>('D'),
  };
  const int idx = std::clamp(slot_index, 0, 5);
  return {kKeys[idx], {}};
}

}  // namespace wds::interaction

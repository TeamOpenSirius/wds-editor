#pragma once

#include "events.hpp"
#include "editor_shortcuts.hpp"
#include "gesture.hpp"
#include "platform.hpp"
#include "shortcuts.hpp"

#include <array>
#include <optional>

namespace wds::interaction {

// Chart-editor input semantics (requests/2). UI consumes these helpers only —
// do not re-encode PointerButton / KeyCode / swipe → action maps elsewhere.

inline constexpr float kEditorSwipeMinDistancePx = 16.0f;

// --- Modifiers / pointer predicates -----------------------------------------

bool is_primary_modifier(const Modifiers& mods) noexcept;

bool is_toggle_select(const PointerDownEvent& event) noexcept;
bool is_marquee_select(const PointerDownEvent& event) noexcept;
bool is_select_or_edit_note(const PointerDownEvent& event) noexcept;
bool is_clear_selection_click(const PointerUpEvent& event) noexcept;
bool is_delete_single_note(const PointerDownEvent& event) noexcept;
// Middle button during PlaceGesture / PlaceHoldBody cancels the in-progress draw.
bool is_cancel_placement(const PointerDownEvent& event) noexcept;
// Hold placement: normal Hold uses Shift+Right for stars and left-up to finish
// (no chain). ScratchHold swaps left/right and supports chain on Left click.
bool is_place_hold_star(const PointerDownEvent& event, bool scratch_hold = false) noexcept;
bool is_chain_hold_body(const PointerDownEvent& event, bool scratch_hold = false) noexcept;
bool is_finish_hold_body(PointerButton button, bool scratch_hold) noexcept;
bool is_begin_place_left(const PointerDownEvent& event) noexcept;
bool is_begin_place_right(const PointerDownEvent& event) noexcept;
bool is_place_button(PointerButton button) noexcept;
bool is_left_button(PointerButton button) noexcept;
bool is_right_button(PointerButton button) noexcept;

// --- Placement gestures -----------------------------------------------------

enum class PlaceIntent {
  None,
  Tap,
  ExTap,
  HoldStart,
  HoldBody,
  Flick,
  FlickLeft,
  FlickRight,
  ScratchHoldBody,
};

bool is_vertical_swipe(SwipeDirection swipe) noexcept;
bool is_horizontal_swipe(SwipeDirection swipe) noexcept;

// Maps button + swipe (+ click) to a placement intent. was_click true forces
// the click branch even if a weak swipe was classified.
PlaceIntent resolve_place_intent(PointerButton button, SwipeDirection swipe,
                                 bool was_click) noexcept;

// --- Key intents ------------------------------------------------------------

enum class EditKeyAction { None, DeleteSelection, SetDefaultWidth };

struct EditKeyResult {
  EditKeyAction action = EditKeyAction::None;
  int width = 0;
};

bool is_delete_selection_key(KeyCode key) noexcept;
// Q/W/E/A/S/D → configurable place-width slots (default 1/2/3/4/6/12).
std::array<int, 6>& width_slot_values() noexcept;
const std::array<int, 6>& width_slot_values_const() noexcept;
bool set_width_slot_values(const std::array<int, 6>& values) noexcept;  // each in [1,12]
// When true, GlfwInputAdapter negates scroll deltas before enqueue.
bool invert_scroll_wheel() noexcept;
void set_invert_scroll_wheel(bool enabled) noexcept;
// Edit-panel wheel timeline scrub multiplier (at visible range 20, 1x ≈ 100ms/notch).
float scroll_wheel_speed() noexcept;
void set_scroll_wheel_speed(float speed) noexcept;
std::optional<int> default_width_for_key(KeyCode key) noexcept;
EditKeyResult resolve_edit_key(const KeyDownEvent& event) noexcept;

// --- Named shortcut chords (UiManager binds these) --------------------------

ShortcutChord chord_toggle_playback() noexcept;
// Shift+Space — paired with chord_toggle_playback; UiManager swaps pause behaviors.
ShortcutChord chord_pause_playback() noexcept;
ShortcutChord chord_save() noexcept;
ShortcutChord chord_open() noexcept;
ShortcutChord chord_undo() noexcept;
ShortcutChord chord_redo() noexcept;
ShortcutChord chord_copy() noexcept;
ShortcutChord chord_paste() noexcept;
ShortcutChord chord_mirror() noexcept;
ShortcutChord chord_mirror_about_center() noexcept;
ShortcutChord chord_nudge_up() noexcept;
ShortcutChord chord_nudge_down() noexcept;
ShortcutChord chord_nudge_left() noexcept;
ShortcutChord chord_nudge_right() noexcept;
ShortcutChord chord_delete_selection() noexcept;
// Ctrl+Shift+F11 (Cmd+Shift+F11 on macOS).
ShortcutChord chord_toggle_fullscreen() noexcept;
// Default note width slots (Q/W/E/A/S/D → 1/2/3/4/6/12).
ShortcutChord chord_width_slot(int slot_index) noexcept;  // 0..5
// Playback rate presets: F1–F4 → 0.25x / 0.5x / 0.75x / 1x.
ShortcutChord chord_playback_rate_slot(int slot_index) noexcept;  // 0..3
std::optional<float> playback_rate_for_slot(int slot_index) noexcept;
// Default X — toggle SFX mute on the preview settings panel.
ShortcutChord chord_toggle_sfx_mute() noexcept;

}  // namespace wds::interaction

#pragma once

#include "events.hpp"
#include "shortcuts.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace wds::interaction {

// All user-configurable editor chords (UiManager binds these).
enum class EditorShortcut : int {
  TogglePlayback = 0,
  PausePlayback,
  Save,
  Open,
  Undo,
  Redo,
  Copy,
  Paste,
  Mirror,
  MirrorAboutCenter,
  NudgeUp,
  NudgeDown,
  NudgeLeft,
  NudgeRight,
  DeleteSelection,
  ToggleFullscreen,
  WidthSlot0,
  WidthSlot1,
  WidthSlot2,
  WidthSlot3,
  WidthSlot4,
  WidthSlot5,
  PlaybackRate0,
  PlaybackRate1,
  PlaybackRate2,
  PlaybackRate3,
  ToggleSfxMute,
  PlaceType0,
  PlaceType1,
  PlaceType2,
  PlaceType3,
  PlaceType4,
  PlaceType5,
  PlaceType6,
  PlaceType7,
  Count
};

inline constexpr std::size_t kEditorShortcutCount =
    static_cast<std::size_t>(EditorShortcut::Count);

const char* editor_shortcut_id(EditorShortcut id) noexcept;
// `pause_at_current` swaps the Shift-Space pause-target label (toolbar checkbox).
const char* editor_shortcut_label(EditorShortcut id, bool pause_at_current = false) noexcept;
ShortcutChord default_editor_shortcut(EditorShortcut id) noexcept;

const ShortcutChord& editor_shortcut(EditorShortcut id) noexcept;
void set_editor_shortcut(EditorShortcut id, ShortcutChord chord) noexcept;
void set_editor_shortcuts(const std::array<ShortcutChord, kEditorShortcutCount>& chords) noexcept;
void reset_editor_shortcuts() noexcept;
std::array<ShortcutChord, kEditorShortcutCount> editor_shortcuts_snapshot() noexcept;

// Period and other text-field typables cannot be shortcut keys; digits are allowed.
bool is_forbidden_shortcut_key(KeyCode key) noexcept;
bool is_completing_shortcut_key(KeyCode key) noexcept;

// On-screen label. macOS matches Qt NativeText (⌘S, ⇧⌘C); elsewhere Ctrl+S.
std::string format_shortcut_chord(const ShortcutChord& chord);
// Modifiers only while capturing (e.g. ⇧⌘ / Shift+Ctrl).
std::string format_shortcut_modifiers(const Modifiers& mods);
// Config I/O: Qt PortableText tokens, same on every OS ("Ctrl+S", "Ctrl+Shift+M").
std::string format_shortcut_chord_portable(const ShortcutChord& chord);
std::string format_shortcut_modifiers_portable(const Modifiers& mods);
// Parse portable, legacy (Cmd/Option), and native symbols (⌘S). Empty → nullopt.
std::optional<ShortcutChord> parse_shortcut_chord(const std::string& text);

// True when `chord` matches another binding (excluding `self`).
bool editor_shortcut_conflicts(EditorShortcut self, const ShortcutChord& chord) noexcept;

}  // namespace wds::interaction

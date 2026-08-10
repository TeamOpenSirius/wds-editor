#pragma once

#include <wds/interaction/editor_shortcuts.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace wds::ui {

// Editor preferences persisted as config/config.yml under the OS data dir:
//   Windows: %LOCALAPPDATA%\WDS\config\config.yml
//   macOS:   ~/Library/Application Support/WDS/config/config.yml
//   Linux:   ${XDG_CONFIG_HOME:-~/.config}/WDS/config/config.yml
// Legacy fallback / one-shot migrate: <exe_dir>/config/config.yml
struct EditorUiConfig {
  double note_speed = 5.0;
  int32_t visible_hectoms = 20;
  float music_volume = 1.0f;
  bool music_muted = false;
  float sfx_volume = 1.0f;
  bool sfx_muted = false;
  bool pause_at_current = false;
  bool split_width_follow = false;
  // Q/W/E/A/S/D place-width slots (each in [1, 12]).
  std::array<int, 6> width_slots{{1, 2, 3, 4, 6, 12}};
  // When true, mute looping Hold-body SFX (head/tail/JumpScratch/stars unchanged).
  bool mute_hold_body_sfx = false;
  // When true, show TimingEffect Auto judgment text during preview auto-hit.
  bool show_judgment_text = false;
  // When true, importing .sus creates an editable in-memory WDS project.
  bool sus_auto_convert = false;
  // When true, negate timeline-scrub wheel deltas (not Ctrl/Cmd+wheel visible range).
  bool invert_scroll_wheel = false;
  // When true, invert Ctrl/Cmd+wheel visible-range adjust direction.
  // Independent of invert_scroll_wheel (timeline scrub).
  bool invert_visible_range_scroll = false;
  // Multiplier for edit-panel timeline scrub only (not Ctrl/Cmd+wheel visible range).
  // At visible_hectoms=20, 1x = 100ms/notch (scales proportionally with range).
  // Legacy hardcoded scrub was 50ms/notch at range 20 (= 0.5x). Default is 1x.
  float scroll_wheel_speed = 1.0f;
  // User-configurable editor chords (defaults match built-in bindings).
  std::array<wds::interaction::ShortcutChord, wds::interaction::kEditorShortcutCount> shortcuts{};
  bool shortcuts_initialized = false;
};

// Resolves the platform config path (creates nothing; save may create dirs).
// Prefers the OS data directory so app updates do not wipe preferences; if that
// file is missing, may one-shot migrate from a legacy next-to-exe config.
std::string resolve_editor_config_path(const char* argv0);

// Returns false if the file is missing or unreadable; `out` left unchanged on hard failure
// of open, but partially parsed keys still apply when the file exists.
bool load_editor_ui_config(const std::string& path, EditorUiConfig& out);
bool save_editor_ui_config(const std::string& path, const EditorUiConfig& cfg);

}  // namespace wds::ui

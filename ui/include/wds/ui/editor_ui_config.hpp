#pragma once

#include "wds/ui/curve_template.hpp"

#include <wds/interaction/editor_shortcuts.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wds::ui {

enum class EditSpectrumMode : std::uint8_t {
  None = 0,
  Envelope = 1,
  Spectrogram = 2,
};

inline EditSpectrumMode clamp_spectrum_display(int value) noexcept {
  if (value <= 0) return EditSpectrumMode::None;
  if (value >= 2) return EditSpectrumMode::Spectrogram;
  return EditSpectrumMode::Envelope;
}

inline const char* spectrum_display_key(EditSpectrumMode mode) noexcept {
  switch (mode) {
    case EditSpectrumMode::None:
      return "none";
    case EditSpectrumMode::Spectrogram:
      return "spectrogram";
    case EditSpectrumMode::Envelope:
    default:
      return "envelope";
  }
}

inline EditSpectrumMode spectrum_display_from_key(std::string_view text) noexcept {
  if (text == "none" || text == "0") return EditSpectrumMode::None;
  if (text == "spectrogram" || text == "2") return EditSpectrumMode::Spectrogram;
  if (text == "envelope" || text == "1") return EditSpectrumMode::Envelope;
  return EditSpectrumMode::Envelope;
}

// Editor preferences persisted as config/config.yml under the OS data dir:
//   Windows: %LOCALAPPDATA%\WDS\config\config.yml
//   macOS:   ~/Library/Application Support/WDS/config/config.yml
//   Linux:   ${XDG_CONFIG_HOME:-~/.config}/WDS/config/config.yml
// Legacy fallback / one-shot migrate: <exe_dir>/config/config.yml
struct EditorUiConfig {
  double note_speed = 5.0;
  // Official NoteStartOffset (0..100 step 5).
  int note_start_offset = 0;
  // Official NoteHeight / GetNoteHeight level (1..10).
  int note_height_level = 8;
  // Official SplitEffectLineOpacity (10..100 step 10).
  int split_line_opacity = 100;
  // Preferred Vulkan MSAA samples: 1 (低) / 2 (中) / 4 (高). Default matches app 2×.
  int msaa_samples = 2;
  // Edit-panel audio backdrop: none / envelope / frequency spectrogram.
  EditSpectrumMode spectrum_display = EditSpectrumMode::Envelope;
  int32_t visible_hectoms = 20;
  // Edit-grid beat subdivisions (toolbar; each in [1, 64]).
  int32_t subdivisions_per_beat = 4;
  float music_volume = 1.0f;
  bool music_muted = false;
  float sfx_volume = 1.0f;
  bool sfx_muted = false;
  // Preview transport rate (0.25..2). Discrete combo labels snap on apply.
  float playback_rate = 1.0f;
  bool pause_at_current = false;
  bool split_width_follow = false;
  // Q/W/E/A/S/D place-width slots (each in [1, 12]).
  std::array<int, 6> width_slots{{1, 2, 3, 4, 6, 12}};
  // When true, mute looping Hold-body SFX (head/tail/JumpScratch/stars unchanged).
  bool mute_hold_body_sfx = false;
  // When true, show TimingEffect Auto judgment text during preview auto-hit.
  bool show_judgment_text = false;
  // Preview stage lane count (1..32).
  int lane_count = 12;
  // New-style toolbox flow: convert buttons also lock the left-click place type.
  bool new_note_place_logic = false;
  // When true, importing .sus creates an editable in-memory WDS project.
  bool sus_auto_convert = false;
  // When true, negate timeline-scrub wheel deltas (not Option+wheel visible range).
  bool invert_scroll_wheel = false;
  // When true, invert Option+wheel visible-range adjust direction.
  // Independent of invert_scroll_wheel (timeline scrub).
  bool invert_visible_range_scroll = false;
  // When true, crash reports may include typed text and full file paths.
  bool allow_crash_log_sensitive = false;
  // Multiplier for edit-panel timeline scrub only (not Option+wheel visible range).
  // At visible_hectoms=20, 1x = 100ms/notch (scales proportionally with range).
  // Legacy hardcoded scrub was 50ms/notch at range 20 (= 0.5x). Default is 1x.
  float scroll_wheel_speed = 1.0f;
  // User-configurable editor chords (defaults match built-in bindings).
  std::array<wds::interaction::ShortcutChord, wds::interaction::kEditorShortcutCount> shortcuts{};
  bool shortcuts_initialized = false;
  // Curve-fill templates (config.yml only; chart files are unchanged). ID 0 = empty.
  std::vector<CurveTemplate> curve_templates;
  std::uint64_t curve_selected_template_id = 0;
  wds::chart_editor::EasingDirection curve_selected_direction =
      wds::chart_editor::EasingDirection::In;
};

// Matches VulkanRenderer::set_preferred_msaa: ≤1 → 1, ≤2 → 2, else 4.
inline int clamp_msaa_samples(int samples) noexcept {
  return samples <= 1 ? 1 : (samples <= 2 ? 2 : 4);
}

inline int clamp_subdivisions_per_beat(int value) noexcept {
  return std::clamp(value, 1, 64);
}

inline float clamp_playback_rate(float rate) noexcept {
  return std::clamp(rate, 0.25f, 2.0f);
}

// Resolves the platform config path (creates nothing; save may create dirs).
// Prefers the OS data directory so app updates do not wipe preferences; if that
// file is missing, may one-shot migrate from a legacy next-to-exe config.
std::string resolve_editor_config_path(const char* argv0);

// Returns false if the file is missing or unreadable; `out` left unchanged on hard failure
// of open, but partially parsed keys still apply when the file exists.
// Legacy shortcut spellings (Cmd/Option/Shift+Ctrl) are rewritten to PortableText.
bool load_editor_ui_config(const std::string& path, EditorUiConfig& out);
bool save_editor_ui_config(const std::string& path, const EditorUiConfig& cfg);

// Copy curve-fill fields between persisted config and the live UiManager state
// used by later dialog/toolbar code. Settings/toolbar capture must call apply
// before save so templates, selected ID, and direction are not replaced by defaults.
void capture_curve_template_state(const EditorUiConfig& cfg, CurveTemplateUiState& state);
void apply_curve_template_state(EditorUiConfig& cfg, const CurveTemplateUiState& state);

}  // namespace wds::ui

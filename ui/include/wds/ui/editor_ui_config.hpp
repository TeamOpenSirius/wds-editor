#pragma once

#include "wds/ui/curve_template.hpp"

#include <wds/interaction/editor_shortcuts.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wds::ui {

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

// Resolves the platform config path (creates nothing; save may create dirs).
// Prefers the OS data directory so app updates do not wipe preferences; if that
// file is missing, may one-shot migrate from a legacy next-to-exe config.
std::string resolve_editor_config_path(const char* argv0);

// Returns false if the file is missing or unreadable; `out` left unchanged on hard failure
// of open, but partially parsed keys still apply when the file exists.
bool load_editor_ui_config(const std::string& path, EditorUiConfig& out);
bool save_editor_ui_config(const std::string& path, const EditorUiConfig& cfg);

// Copy curve-fill fields between persisted config and the live UiManager state
// used by later dialog/toolbar code. Settings/toolbar capture must call apply
// before save so templates, selected ID, and direction are not replaced by defaults.
void capture_curve_template_state(const EditorUiConfig& cfg, CurveTemplateUiState& state);
void apply_curve_template_state(EditorUiConfig& cfg, const CurveTemplateUiState& state);

}  // namespace wds::ui

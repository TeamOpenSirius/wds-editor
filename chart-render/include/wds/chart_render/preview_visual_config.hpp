#pragma once

#include <wds/core/official_playfield.hpp>
#include <wds/core/split_fade.hpp>

#include <cstdint>
#include <string>

namespace wds::renderer {

// Official playfield: LaneGroup Rx=60° + FOV 50, content 16:9.
struct PreviewVisualConfig {
  // Options.NoteSpeed equivalent; appearTime = official CalculateMoveSeconds.
  float note_speed = 5.0f;

  // Stage / camera
  float target_aspect_ratio = wds::chart_editor::kOfficialPreviewAspect;
  bool lock_aspect_ratio = true;
  float extra_width = 1.0f;
  float stage_opacity = 0.8f;
  float stage_top_overscan = 0.0f;

  // Judgment line thickness in JudgeArea Y (img_ingame_judgment_area3 = 0.72).
  float judgeline_height = wds::chart_editor::kOfficialJudgeSpriteHeight;
  float judgline_move_length = 0.01f;

  // Note / lane visuals
  int32_t lane_count = 12;
  float note_height = 85.0f / 640.0f;
  float note_move_length = 0.02f;
  float note_border_percent = 0.02f;
  // Official prefab ScratchNote/Notes/* localPosition.z (height toward camera = -z).
  float note_unity_local_z_bottom = wds::chart_editor::kOfficialNoteLocalZBottom;
  float note_unity_local_z_top = wds::chart_editor::kOfficialNoteLocalZTop;
  // GetNoteHeight level (1..10). Default 8 → Rx = -15°.
  int note_height_level = wds::chart_editor::kOfficialDefaultNoteHeightLevel;
  // NoteStartOffset (0..100 step 5). Default 0 → visible Y=58.
  int note_start_offset = wds::chart_editor::kOfficialDefaultNoteStartOffset;
  float tick_width = 168.0f / 640.0f;
  float tick_height = 112.0f / 640.0f;
  float arrow_width = 80.0f / 640.0f;
  float arrow_height = 240.0f / 640.0f;
  float arrow_percent = 1.6f;
  // Sirius constants.cpp: alpha = 1 - 0.8 * Mod(i + times.now * arrowSpeed, num) / num
  float arrow_speed = 20.0f;
  float sync_line_height = 5.0f / 640.0f;
  // Slightly wider than stock so the gaussian glow skirts have screen-space room.
  float split_line_length = 0.048f;
  // Official fadeIn 1.0s; fadeOut alpha 1→0 in 0.3s (Light: lines only).
  float split_line_animation_start = 1.0f;
  float split_line_animation_end = 0.3f;
  // Official Initialize: RGB *= settings/100. GameSettings default is 100.
  float split_line_opacity = 1.00f;
  // Mild additive body glow so overlaps with the judgeline brighten (beam, not matte).
  float split_line_body_glow = 0.18f;
  // Traveling semi-transparent band (SplitEffect_all SplitLine particle streak).
  // Period: EndTimeSecond (LaneEffect / VFX default 0.5s). Travel uses the baked
  // position curve (slow near judgeline, accelerates toward tip).
  float split_line_pulse_period = 0.5f;
  // Full width in note-percent: official OffSet 5 / Height 45.
  float split_line_pulse_band = 5.0f / 45.0f;
  // Peak darkening. SpriteRenderer alpha stays LineColor.a; the streak is a
  // VFX overlay (CheckAlpha 0.5 on an already ~0.4 line ≈ 0.2 extra hole).
  float split_line_pulse_dip = 0.2f;
  // Official SplitLine bright-head: identity uses 45/256 of the visible ribbon;
  // z=180 projects the same 12.15 wu from the mesh tip (judge end).
  float split_line_tip_whiten = wds::chart_editor::kOfficialSplitLineSpriteTipFrac;
  // Official SplitLine output is (1,1,1,1) × expr-68 alpha. No 0.55 gate.
  float split_line_tip_glow = 1.0f;
  // Visible 挡板: LaneNoteStartLine.Initialize size.y = rect.height/100
  // (start_line_500 = 4.98 at offset 0), not prefab m_Size.y=1.
  float hidden_line_height = wds::chart_editor::kOfficialStartLineSpriteHeight;
  // Official StartLine renderer alpha is 1; the PNG carries 0.50–0.80.
  float hidden_line_alpha = 1.0f;

  // Note / hold alphas (sonolus-sirius-engine/engine/play/utils.cpp Draw args)
  // drawTick → 0.5; drawHoldEighth → 0.8 / 0.85 while holding; flat notes → 1.0
  float tick_alpha = 0.5f;
  float hold_body_alpha = 0.8f;
  float hold_body_holding_alpha = 0.85f;

  // Hit VFX — official Bomb Light (~0.7s). Sonolus linear heights kept as fallback.
  float effect_linear_height = 0.4375f;
  float effect_circular_height = 0.3125f;
  float effect_duration = 0.7f;
  float effect_distance = 0.03125f;
  // Hold-body eighth pulses — Default Light Square matches HoldBomb (isStrong only
  // scales disabled Light modules; keep full opacity).
  float hold_body_effect_alpha = 1.0f;
  // TimingEffect Auto (TimingEffect_anime m_StopTime ≈ 0.417s).
  float judge_text_height = 0.15f;
  float judge_auto_ratio = 216.0f / 76.0f;
  float judge_text_duration = 0.1f;  // combo number pop window
  float timing_effect_duration = 0.417f;
  // Centered on the stage (PreviewUI's local X=-0.87 is relative to an offset parent).
  float timing_effect_unity_x = 0.0f;
  float timing_effect_unity_half_width = 5.55f;  // LaneWidth 0.925 * 12 / 2
  // Y: fraction from judgeline toward tip (lower → sits closer to judgeline).
  float timing_effect_y_fraction = 0.34f;
  float timing_effect_root_scale = 0.8f;

  // Combo (Sirius Stage.cpp drawCombo AP branch)
  float combo_scale = 1.12f;
  float combo_alpha = 1.0f;
  float combo_ap_number_height = 0.238f;
  float combo_ap_number_distance = -0.048f;
  float combo_ap_text_height = 0.066f;
  float combo_ap_text_distance = 0.003f;
  float combo_ap_digit_ratio = 118.0f / 148.0f;
  float combo_ap_text_ratio = 168.0f / 48.0f;
  // Sirius drawCombo: cx = screen.w * factor (origin at screen center) → track right side.
  float combo_center_x_factor = 0.4f;
  // Lower baseline (toward judgeline) than stock 0.2.
  float combo_baseline_y = 0.12f;

  // Official CalculateMoveSeconds = 7.4167 / note_speed.
  float appear_time_base = 7.4166667f;

  // Directory containing skin PNGs (project-root skins/ by default)
  std::string skins_directory = "skins";
  // Directory containing Sirius effect clips (project-root effects/ by default)
  std::string effects_directory = "effects";
  // Optional BGM path (OGG/WAV preferred). Empty → wall-clock transport fallback.
  std::string bgm_path;

  // Preferred MSAA samples (unified 2× across platforms).
  int msaa_samples = 2;

  float appear_time() const noexcept {
    return wds::chart_editor::official_move_seconds(static_cast<double>(note_speed));
  }
};

}  // namespace wds::renderer

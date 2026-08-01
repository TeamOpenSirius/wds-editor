#pragma once

#include <cstdint>
#include <string>

namespace wds::renderer {

// Visual parameters mirrored from sonolus-sirius-engine/engine/shared/constants.cpp
// (play / watch mode). Geometry uses a trapezoid stage with top narrowed by high_width.
struct PreviewVisualConfig {
  // Options.NoteSpeed equivalent; appearTime = 7.4 / note_speed
  float note_speed = 5.0f;

  // Stage / camera
  float target_aspect_ratio = 1115.0f / 640.0f;
  bool lock_aspect_ratio = true;
  float extra_width = 1.0f;
  float stage_opacity = 0.8f;
  float high_width = 0.1f;  // top width / bottom width ratio
  // Push stage tip above the visible panel so notes spawn off-screen and scroll in.
  // Fraction of pre-extend stage height; taller preview raises the tip accordingly.
  float stage_top_overscan = 0.38f;

  // Judgment line — slightly lower than Sirius default to use more of the tall panel.
  float judgeline_margin_bottom = 0.16f;
  float judgeline_height = 0.133f;
  float judgline_move_length = 0.01f;

  // Note / lane visuals
  int32_t lane_count = 12;  // Sirius playfield uses 12 lanes
  float note_height = 85.0f / 640.0f;
  float note_move_length = 0.02f;
  float note_border_percent = 0.02f;
  float tick_width = 168.0f / 640.0f;
  float tick_height = 112.0f / 640.0f;
  float arrow_width = 80.0f / 640.0f;
  float arrow_height = 240.0f / 640.0f;
  float arrow_percent = 1.6f;
  // Sirius constants.cpp: alpha = 1 - 0.8 * Mod(i + times.now * arrowSpeed, num) / num
  float arrow_speed = 20.0f;
  float sync_line_height = 5.0f / 640.0f;
  float split_line_length = 0.02f;
  float split_line_animation_start = 0.75f;
  float split_line_animation_end = 0.20f;
  // Multiply snapshot split_line_alpha when drawing (1 = full; lower = more see-through).
  float split_line_opacity = 0.72f;

  // Note / hold alphas (sonolus-sirius-engine/engine/play/utils.cpp Draw args)
  // drawTick → 0.5; drawHoldEighth → 0.8 / 0.85 while holding; flat notes → 1.0
  float tick_alpha = 0.5f;
  float hold_body_alpha = 0.8f;
  float hold_body_holding_alpha = 0.85f;

  // Hit VFX (sonolus-sirius-engine/engine/shared/constants.cpp)
  float effect_linear_height = 0.4375f;
  float effect_circular_height = 0.3125f;
  float effect_duration = 0.5f;
  float effect_distance = 0.03125f;
  // Hold-body eighth pulses (more transparent than head/tail hits).
  float hold_body_effect_alpha = 0.35f;
  float judge_text_height = 0.15f;
  float judge_auto_ratio = 216.0f / 76.0f;
  float judge_text_duration = 0.1f;

  // Combo (Sirius Stage.cpp drawCombo AP branch)
  float combo_scale = 1.0f;
  float combo_alpha = 1.0f;
  float combo_ap_number_height = 0.238f;
  float combo_ap_number_distance = -0.048f;
  float combo_ap_text_height = 0.066f;
  float combo_ap_text_distance = 0.003f;
  float combo_ap_digit_ratio = 118.0f / 148.0f;
  float combo_ap_text_ratio = 168.0f / 48.0f;
  // Sirius drawCombo: cx = screen.w * factor (origin at screen center) → track right side.
  float combo_center_x_factor = 0.4f;
  float combo_baseline_y = 0.2f;

  // Longer approach than stock Sirius so notes spawn earlier and cover the taller
  // tip→judgeline path after lowering the judgment line.
  float appear_time_base = 8.5f;

  // Directory containing skin PNGs (project-root skins/ by default)
  std::string skins_directory = "skins";
  // Directory containing Sirius effect clips (project-root effects/ by default)
  std::string effects_directory = "effects";
  // Optional BGM path (OGG/WAV preferred). Empty → wall-clock transport fallback.
  std::string bgm_path;

  // Preferred MSAA samples (unified 2× across platforms).
  int msaa_samples = 2;

  float appear_time() const noexcept { return appear_time_base / note_speed; }
};

}  // namespace wds::renderer

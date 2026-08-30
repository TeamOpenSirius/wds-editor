#pragma once

#include <cstdint>

namespace wds::chart_editor {

// Mirrors key fields from GameSettings / GameDetailSettings / NotePositionCalculator.
struct PreviewConfig {
  // Note speed slider (game uses note speed with NoteSpeedAdjustValue = 10).
  double note_speed = 5.0;

  // Lane layout (RecoveredGameConfigValues).
  int32_t lane_count = 6;
  float lane_border_width = 0.01f;
  float note_width_per_lane = 0.915f;

  // Spatial layout in JudgeArea units (map to your renderer).
  float judge_line_y = 0.0f;
  float spawn_y = 58.0f;
  double offset_value = 0.0;

  // Timing windows in milliseconds (from GameDetailSettings equivalents).
  int64_t perfect_window_ms = 25;
  int64_t great_window_ms = 50;
  int64_t good_window_ms = 80;
  int64_t miss_window_ms = 120;

  // How early notes become visible before their judgment time.
  // Official CalculateMoveSeconds ≈ 7.4167 / note_speed (default 5 → ~1.483s).
  float note_approach_seconds = 1.48334f;

  // How long notes stay visible after passing judge line in auto preview.
  int64_t post_miss_visible_ms = 250;

  // Official GameConfig PositionPow1Rate / PositionPow3Rate.
  float position_pow1_rate = 10.0f;
  float position_pow3_rate = 0.2f;

  // Auto preview only: whether to simulate hold body while between start/end.
  bool simulate_hold_body = true;

  // After judgment time, note body is gone; AutoHit window drives hit VFX
  // (mirrors Sirius terminate() particle lifetime = effectDurationTime).
  bool show_auto_hit_feedback = true;
  int64_t auto_hit_feedback_ms = 700;  // official Bomb _animationTime ≈ 0.7s

  // Fallback tail when hold-span cache has not been reduced after deletions.
  int64_t preview_tail_fallback_ms = 8000;

  // Split-line / STAGE_COVER fade windows (seconds).
  // Official fadeIn clip 1.0s; fadeOut alpha keys 0→0.3s (OnExit is 1.0s).
  float split_line_animation_start_sec = 1.0f;
  float split_line_animation_end_sec = 0.3f;

  // Incremental snapshot update policy (see PreviewSnapshotBuilder::estimate_diff).
  // Prefer FullRebuild when |timeline delta| exceeds this (large seek / scrub).
  int64_t incremental_max_time_delta_ms = 500;
  // Prefer FullRebuild when estimated_churn / max(prev, candidates) exceeds this.
  float incremental_churn_ratio_threshold = 0.35f;
};

}  // namespace wds::chart_editor

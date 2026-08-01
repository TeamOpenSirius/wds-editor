#pragma once

#include <cstdint>

namespace wds::chart_editor {

// Mirrors key fields from GameSettings / GameDetailSettings / NotePositionCalculator.
struct PreviewConfig {
  // Note speed slider (game uses note speed with NoteSpeedAdjustValue = 10).
  double note_speed = 5.0;

  // Lane layout
  int32_t lane_count = 6;
  float lane_border_width = 0.05f;
  float note_width_per_lane = 1.0f;

  // Spatial layout in world units (map to your renderer).
  float judge_line_y = 0.0f;
  float spawn_y = 8.0f;

  // Timing windows in milliseconds (from GameDetailSettings equivalents).
  int64_t perfect_window_ms = 25;
  int64_t great_window_ms = 50;
  int64_t good_window_ms = 80;
  int64_t miss_window_ms = 120;

  // How early notes become visible before their judgment time.
  // Sirius appearTime ≈ 7.4 / note_speed (default 5 → ~1.48s).
  float note_approach_seconds = 1.48f;

  // How long notes stay visible after passing judge line in auto preview.
  int64_t post_miss_visible_ms = 250;

  // Position easing mix from NotePositionUpdater (_positionPow1Rate / _positionPow3Rate).
  float position_pow1_rate = 0.35f;
  float position_pow3_rate = 0.65f;

  // Auto preview only: whether to simulate hold body while between start/end.
  bool simulate_hold_body = true;

  // After judgment time, note body is gone; AutoHit window drives hit VFX
  // (mirrors Sirius terminate() particle lifetime = effectDurationTime).
  bool show_auto_hit_feedback = true;
  int64_t auto_hit_feedback_ms = 500;  // effectDurationTime = 0.5s

  // Fallback tail when hold-span cache has not been reduced after deletions.
  int64_t preview_tail_fallback_ms = 8000;

  // Split-line / STAGE_COVER fade windows (seconds). Keep both equal so gray
  // lane dividers and effect lines ease in/out together.
  float split_line_animation_start_sec = 0.75f;
  float split_line_animation_end_sec = 0.20f;

  // Incremental snapshot update policy (see PreviewSnapshotBuilder::estimate_diff).
  // Prefer FullRebuild when |timeline delta| exceeds this (large seek / scrub).
  int64_t incremental_max_time_delta_ms = 500;
  // Prefer FullRebuild when estimated_churn / max(prev, candidates) exceeds this.
  float incremental_churn_ratio_threshold = 0.35f;
};

}  // namespace wds::chart_editor

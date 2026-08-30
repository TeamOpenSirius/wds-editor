#pragma once

#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/types.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wds::chart_editor {

// Decides how the next preview frame should be produced.
enum class SnapshotUpdateStrategy : uint8_t {
  IncrementalPatch,  // Mutate previous snapshot object instances in place
  FullRebuild,       // Recycle old contents (keep capacity) and rebuild
};

// Lightweight estimate of how much the next frame differs from the previous snapshot.
// Covers notes, split lanes, and concurrent lines.
struct SnapshotDiffEstimate {
  int64_t previous_timeline_ms = 0;
  int64_t next_timeline_ms = 0;
  int64_t time_delta_ms = 0;

  size_t previous_note_count = 0;
  size_t previous_split_count = 0;
  size_t previous_concurrent_count = 0;
  size_t previous_visible_count = 0;

  size_t note_candidate_count = 0;
  size_t split_candidate_count = 0;
  size_t concurrent_candidate_count = 0;
  size_t candidate_count = 0;

  // Lower-bound |enter| + |leave| proxy from per-type set-size differences.
  size_t estimated_churn = 0;
  float estimated_churn_ratio = 1.0f;

  bool revision_changed = false;
  bool prefer_incremental = false;
  SnapshotUpdateStrategy strategy = SnapshotUpdateStrategy::FullRebuild;
};

struct PreviewNoteInstance {
  int32_t note_id = 0;
  NoteType note_type = NoteType::None;
  GimmickType gimmick_type = GimmickType::None;
  int32_t lane = 0;
  int32_t width = 1;
  int32_t end_lane = 0;

  float pos_x = 0.0f;
  float pos_y = 0.0f;
  float width_world = 0.0f;
  float height_world = 0.0f;
  float hold_body_length = 0.0f;

  int64_t start_ms = 0;
  int64_t end_ms = 0;
  PreviewNoteVisualState visual_state = PreviewNoteVisualState::Hidden;
  bool is_grayed_out = false;

  // JumpScratch / OneDirection gimmick preview hints
  bool uses_jump_scratch_position = false;
  int32_t jump_scratch_lane_from = 0;
  int32_t jump_scratch_lane_to = 0;
  // Official scratchLength: Flick/ScratchHoldEnd arrow direction (+ right, - left, 0 both).
  int32_t scratch_length = 0;

  void apply_identity(int32_t id, NoteType type, GimmickType gimmick, int32_t note_lane,
                      int32_t note_width, int32_t note_end_lane, int64_t note_start_ms,
                      int64_t note_end_ms, int32_t note_scratch_length = 0) noexcept;

  void apply_layout(float x, float y, float world_width, float world_height,
                    float hold_length) noexcept;

  void apply_visual(PreviewNoteVisualState state, bool grayed_out) noexcept;

  void apply_jump_scratch(bool enabled, int32_t lane_from, int32_t lane_to) noexcept;

  void assign(const PreviewNoteInstance& other) noexcept;
};

struct PreviewSplitLaneInstance {
  int32_t source_note_id = 0;
  int32_t split_count = 0;
  SplitLaneType split_lane_type = SplitLaneType::BothEnds;
  int32_t scratch_length = 0;
  bool should_show = false;
  bool is_continued = false;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  int32_t effective_lane_count = 0;

  // Official SplitEffect: fadeIn = LineHight scale.y grow; fadeOut = alpha only.
  float split_line_alpha = 1.0f;     // disappear fade (SpriteRenderer.a)
  float split_percent_start = 0.0f;  // appear: judge-grow 1-scale, tip-grow 0
  float split_percent_end = 1.0f;    // appear: judge-grow 1, tip-grow scale
  // STAGE_COVER opacity: inverse of split presence (1 before appear, 0 while steady, …).
  float stage_cover_alpha = 1.0f;
  // 0=appear, 1=steady, 2=disappear (texture always base soft line in Light preview).
  int32_t split_anim_phase = 1;

  void apply_identity(int32_t source_id, int32_t count, SplitLaneType type, int32_t value,
                      int64_t note_start_ms, int64_t note_end_ms) noexcept;

  void apply_state(bool show, bool continued, int32_t lane_count) noexcept;

  void apply_animation(float line_alpha, float percent_start, float percent_end,
                       float cover_alpha, int32_t anim_phase) noexcept;

  void assign(const PreviewSplitLaneInstance& other) noexcept;
};

struct PreviewConcurrentLineInstance {
  int64_t milliseconds = 0;
  int32_t start_lane = 0;
  int32_t width = 1;
  float pos_y = 0.0f;

  void apply_identity(int64_t ms, int32_t lane, int32_t line_width) noexcept;

  void apply_layout(float y) noexcept;

  void assign(const PreviewConcurrentLineInstance& other) noexcept;
};

struct PreviewSnapshot {
  int64_t timeline_ms = 0;
  // Sub-ms clock for smooth preview motion (ms helpers floor µs).
  int64_t timeline_us = 0;
  double bpm = 120.0;
  int32_t ticks_per_quarter = 480;
  PreviewPlaybackState playback_state = PreviewPlaybackState::Paused;
  uint64_t revision = 0;

  std::vector<PreviewNoteInstance> notes;
  std::vector<PreviewConcurrentLineInstance> concurrent_lines;
  std::vector<PreviewSplitLaneInstance> split_lanes;
  int32_t active_lane_count = 0;

  // Auto-preview combo (seek-safe); updated each snapshot rebuild.
  int32_t combo_count = 0;
  int64_t last_judge_ms = -1;

  SnapshotUpdateStrategy last_update_strategy = SnapshotUpdateStrategy::FullRebuild;

  void clear_keep_capacity() noexcept;
  void reserve(size_t note_capacity, size_t line_capacity = 0,
               size_t split_capacity = 0);

  PreviewNoteInstance* find_note(int32_t note_id) noexcept;
  const PreviewNoteInstance* find_note(int32_t note_id) const noexcept;
  PreviewNoteInstance& upsert_note(const PreviewNoteInstance& instance);
  bool remove_note(int32_t note_id) noexcept;
  bool remove_note_at(size_t index) noexcept;

  PreviewSplitLaneInstance* find_split_lane(int32_t source_note_id) noexcept;
  const PreviewSplitLaneInstance* find_split_lane(int32_t source_note_id) const noexcept;
  PreviewSplitLaneInstance& upsert_split_lane(const PreviewSplitLaneInstance& instance);
  bool remove_split_lane(int32_t source_note_id) noexcept;
  bool remove_split_lane_at(size_t index) noexcept;

  PreviewConcurrentLineInstance* find_concurrent_line(int64_t milliseconds,
                                                      int32_t start_lane) noexcept;
  const PreviewConcurrentLineInstance* find_concurrent_line(int64_t milliseconds,
                                                            int32_t start_lane) const noexcept;
  PreviewConcurrentLineInstance& upsert_concurrent_line(
      const PreviewConcurrentLineInstance& instance);
  bool remove_concurrent_line(int64_t milliseconds, int32_t start_lane) noexcept;
  bool remove_concurrent_line_at(size_t index) noexcept;

  void clear_notes_keep_capacity() noexcept;
  void clear_concurrent_lines_keep_capacity() noexcept;
  void clear_split_lanes_keep_capacity() noexcept;

  void rebuild_note_index();
  void rebuild_split_lane_index();
  void rebuild_concurrent_line_index();

 private:
  static uint64_t concurrent_line_key(int64_t milliseconds, int32_t start_lane) noexcept;

  std::unordered_map<int32_t, size_t> note_index_;
  std::unordered_map<int32_t, size_t> split_lane_index_;
  std::unordered_map<uint64_t, size_t> concurrent_line_index_;
};

}  // namespace wds::chart_editor

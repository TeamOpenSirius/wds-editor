#pragma once

#include <wds/core/auto_judge_simulator.hpp>
#include <wds/core/chart_index.hpp>
#include <wds/core/note_position_calculator.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>
#include <wds/core/preview_snapshot.hpp>
#include <wds/core/split_lane_simulator.hpp>
#include <wds/core/types.hpp>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace wds::chart_editor {

// Deterministic snapshot builder with optional incremental patching.
// Unlike original NoteObjectScheduler/NoteObjectManager (forward-only),
// evaluation is still derived from (chart, time) and supports rollback.
class PreviewSnapshotBuilder {
 public:
  PreviewSnapshotBuilder(PreviewConfig config = {});

  void set_config(PreviewConfig config);
  void set_scroll_speed_points(std::vector<ScrollSpeedPoint> points,
                               const MusicTiming& timing);
  void set_compatibility_profile(ChartCompatibilityProfile profile);

  PreviewSnapshot build(const NotationChart& chart, const ChartNoteIndex& index,
                        int64_t preview_time_ms, PreviewPlaybackState playback_state,
                        uint64_t revision) const;

  PreviewSnapshot build(const std::vector<NotationNote>& notes, const MusicTiming& timing,
                        const std::vector<ConcurrentLineNote>& concurrent_lines,
                        const ChartNoteIndex& index, int64_t preview_time_ms,
                        PreviewPlaybackState playback_state, uint64_t revision) const;

  // Lightweight pre-redraw estimate across notes / split lanes / concurrent lines.
  SnapshotDiffEstimate estimate_diff(
      const PreviewSnapshot& previous, const ChartNoteIndex& index,
      const std::vector<ConcurrentLineNote>& concurrent_lines, int64_t preview_time_ms,
      uint64_t revision) const;

  SnapshotUpdateStrategy choose_strategy(const SnapshotDiffEstimate& estimate) const;

  void build_into(PreviewSnapshot& out, const std::vector<NotationNote>& notes,
                  const MusicTiming& timing,
                  const std::vector<ConcurrentLineNote>& concurrent_lines,
                  const ChartNoteIndex& index, int64_t preview_time_ms,
                  PreviewPlaybackState playback_state, uint64_t revision) const;

  void update_incremental(PreviewSnapshot& inout, const std::vector<NotationNote>& notes,
                          const MusicTiming& timing,
                          const std::vector<ConcurrentLineNote>& concurrent_lines,
                          const ChartNoteIndex& index, int64_t preview_time_ms,
                          PreviewPlaybackState playback_state, uint64_t revision) const;

  void rebuild_or_update(PreviewSnapshot& inout, const std::vector<NotationNote>& notes,
                         const MusicTiming& timing,
                         const std::vector<ConcurrentLineNote>& concurrent_lines,
                         const ChartNoteIndex& index, int64_t preview_time_ms,
                         PreviewPlaybackState playback_state, uint64_t revision) const;

 private:
  void apply_gimmick_position(PreviewNoteInstance& instance,
                              const NotationNote& note) const;

  int32_t resolve_active_lane_count(const std::vector<PreviewSplitLaneInstance>& splits,
                                    int32_t base_lane_count) const;
  bool is_spawn_visible(const NotationNote& note, int64_t preview_time_ms,
                        const MusicTiming& timing) const;

  bool is_expired(const NotationNote& note, int64_t preview_time_ms,
                  PreviewNoteVisualState state, const MusicTiming& timing) const;

  bool is_concurrent_line_visible(const ConcurrentLineNote& line,
                                  int64_t preview_time_ms) const;

  void ensure_note_lookup(const std::vector<NotationNote>& notes, uint64_t revision) const;

  void ensure_combo_hits(const std::vector<NotationNote>& notes, const MusicTiming& timing,
                         uint64_t revision) const;

  const NotationNote* lookup_note(int32_t note_id) const;

  void fill_note_instance(PreviewNoteInstance& instance, const NotationNote& note,
                          int64_t preview_time_ms, const MusicTiming& timing,
                          int32_t lane_count) const;

  void fill_concurrent_line_instance(PreviewConcurrentLineInstance& instance,
                                     const ConcurrentLineNote& line,
                                     int64_t preview_time_ms) const;

  void rebuild_split_lanes(PreviewSnapshot& out, const std::vector<NotationNote>& notes,
                           const MusicTiming& timing, const ChartNoteIndex& index,
                           int64_t preview_time_ms) const;

  void update_split_lanes_incremental(PreviewSnapshot& inout,
                                      const std::vector<NotationNote>& notes,
                                      const MusicTiming& timing, const ChartNoteIndex& index,
                                      int64_t preview_time_ms) const;

  // Lane-border spans use the official hide/restore window [start-1s, end+2s],
  // wider than the pooled split instance lifetime. Rebuilt every update.
  void rebuild_split_borders(PreviewSnapshot& out, const MusicTiming& timing,
                             const ChartNoteIndex& index, int64_t preview_time_ms) const;

  void rebuild_concurrent_lines(PreviewSnapshot& out,
                                const std::vector<ConcurrentLineNote>& concurrent_lines,
                                int64_t preview_time_ms, uint64_t revision) const;

  void update_concurrent_lines_incremental(
      PreviewSnapshot& inout, const std::vector<ConcurrentLineNote>& concurrent_lines,
      int64_t preview_time_ms, uint64_t revision) const;

  void update_notes_incremental(PreviewSnapshot& inout, int64_t preview_time_ms,
                                const MusicTiming& timing, const ChartNoteIndex& index,
                                int32_t lane_count, uint64_t revision) const;

  void rebuild_notes(PreviewSnapshot& out, int64_t preview_time_ms, const MusicTiming& timing,
                     const ChartNoteIndex& index, int32_t lane_count,
                     size_t concurrent_capacity, uint64_t revision) const;

  static size_t size_delta(size_t a, size_t b) noexcept;

  int64_t spawn_lead_ms() const;
  int64_t tail_ms(const ChartNoteIndex& index) const;
  // Lead derived from the last visible target time (hull fallback).
  int64_t query_lead_ms(int64_t preview_time_ms) const;
  int64_t lead_ms_from_last(int64_t preview_time_ms, double last_ms) const;
  bool note_visible_spatially(const NotationNote& note, int64_t preview_time_ms,
                              const MusicTiming& timing) const;
  double visible_bottom_y() const;
  double visible_top_y() const;
  void visible_distance_bounds(double& out_lo, double& out_hi) const;

  // Fills candidate_buffer_ for (now, index, revision). Reused by estimate_diff
  // and the following rebuild/update, so the expensive query runs once.
  void ensure_note_candidates(int64_t preview_time_ms, const ChartNoteIndex& index,
                              uint64_t revision) const;
  // Per-frame cache for concurrent-line visibility, shared by estimate and the
  // following rebuild/update.
  void ensure_concurrent_visibility(
      const std::vector<ConcurrentLineNote>& concurrent_lines, int64_t preview_time_ms,
      uint64_t revision) const;

  static constexpr size_t kRangeScanSegmentsMax = 1024;
  static constexpr size_t kMaxCandidateRanges = 512;

  PreviewConfig config_;
  NotePositionCalculator position_calculator_;
  AutoJudgeSimulator auto_judge_;
  SplitLaneSimulator split_lane_simulator_;
  mutable std::vector<int32_t> candidate_buffer_;
  mutable std::vector<int32_t> split_buffer_;
  mutable std::vector<uint8_t> visited_buffer_;
  mutable std::unordered_map<int32_t, size_t> note_id_to_chart_index_;
  mutable uint64_t cached_lookup_revision_ = ~uint64_t{0};
  mutable const std::vector<NotationNote>* cached_notes_ = nullptr;
  mutable uint64_t cached_combo_revision_ = ~uint64_t{0};
  mutable const std::vector<NotationNote>* cached_combo_notes_ = nullptr;
  mutable std::vector<int64_t> cached_combo_hits_;
  double minimum_scroll_multiplier_ = 1.0;
  bool has_negative_scroll_ = false;
  // Bumped on config / SV changes; invalidates derived distance and query caches.
  uint64_t scroll_generation_ = 0;
  mutable uint64_t cached_distance_generation_ = ~uint64_t{0};
  mutable double cached_distance_lo_ = 0.0;
  mutable double cached_distance_hi_ = 0.0;
  mutable int64_t candidate_cache_now_ = std::numeric_limits<int64_t>::min();
  mutable uint64_t candidate_cache_revision_ = ~uint64_t{0};
  mutable const ChartNoteIndex* candidate_cache_index_ = nullptr;
  mutable int64_t candidate_cache_tail_ = -1;
  mutable uint64_t candidate_cache_scroll_generation_ = ~uint64_t{0};
  mutable std::vector<ScrollTimeRange> visible_ranges_;
  mutable std::vector<ScrollTimeRange> query_ranges_;
  mutable int64_t concurrent_cache_now_ = std::numeric_limits<int64_t>::min();
  mutable uint64_t concurrent_cache_revision_ = ~uint64_t{0};
  mutable const void* concurrent_cache_data_ = nullptr;
  mutable size_t concurrent_cache_size_ = 0;
  mutable uint64_t concurrent_cache_scroll_generation_ = ~uint64_t{0};
  mutable std::vector<uint8_t> concurrent_visibility_;
};

}  // namespace wds::chart_editor

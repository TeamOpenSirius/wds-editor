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

  void rebuild_concurrent_lines(PreviewSnapshot& out,
                                const std::vector<ConcurrentLineNote>& concurrent_lines,
                                int64_t preview_time_ms) const;

  void update_concurrent_lines_incremental(
      PreviewSnapshot& inout, const std::vector<ConcurrentLineNote>& concurrent_lines,
      int64_t preview_time_ms) const;

  void update_notes_incremental(PreviewSnapshot& inout, int64_t preview_time_ms,
                                const MusicTiming& timing, const ChartNoteIndex& index,
                                int32_t lane_count) const;

  void rebuild_notes(PreviewSnapshot& out, int64_t preview_time_ms, const MusicTiming& timing,
                     const ChartNoteIndex& index, int32_t lane_count,
                     size_t concurrent_capacity) const;

  static size_t size_delta(size_t a, size_t b) noexcept;

  int64_t spawn_lead_ms() const;
  int64_t tail_ms(const ChartNoteIndex& index) const;

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
};

}  // namespace wds::chart_editor

#pragma once

#include <wds/core/detail/start_ms_avl_index.hpp>

#include <cstdint>
#include <vector>

namespace wds::chart_editor {

struct NotationNote;
struct MusicTiming;

// Custom AVL index keyed by cached start_ms.
//
// Stores note IDs (not vector indices) so ChartDocument may reorder/sort freely.
//
// Optimized for:
// - N <= ~10000
// - Single-note edits: O(log N) insert/remove/update
// - Paste / bulk replace: O(N log N) rebuild
// - Preview range query: O(log N + k)
class ChartNoteIndex {
 public:
  void rebuild(const std::vector<NotationNote>& notes, const MusicTiming& timing);
  void clear();

  void on_note_added(const NotationNote& note, const MusicTiming& timing);
  void on_note_removed(const NotationNote& note, const MusicTiming& timing);
  void on_note_updated(const NotationNote& old_note, const NotationNote& new_note,
                       const MusicTiming& timing);

  void query_candidates(int64_t time_ms, int64_t lead_ms, int64_t tail_ms,
                        std::vector<int32_t>& out_note_ids) const;

  void query_split_lanes_up_to(int64_t time_ms, std::vector<int32_t>& out_note_ids) const;

  int64_t max_hold_span_ms() const noexcept { return max_hold_span_ms_; }

 private:
  void insert_id(int32_t note_id, int64_t start_ms);
  void remove_id(int32_t note_id, int64_t start_ms);
  void update_hold_span(const NotationNote& note, const MusicTiming& timing);

  detail::StartMsAvlIndex by_start_ms_;
  detail::StartMsAvlIndex split_lane_by_start_ms_;
  int64_t max_hold_span_ms_ = 0;
};

}  // namespace wds::chart_editor

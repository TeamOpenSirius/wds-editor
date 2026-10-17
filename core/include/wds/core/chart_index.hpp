#pragma once

#include <wds/core/detail/start_ms_avl_index.hpp>

#include <cstdint>
#include <limits>
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

  // lead_ms / tail_ms are clamped to >= 0. Window bounds saturate at int64
  // limits so time_ms == INT64_MAX/MIN cannot overflow, and lower <= upper.
  void query_candidates(int64_t time_ms, int64_t lead_ms, int64_t tail_ms,
                        std::vector<int32_t>& out_note_ids) const;

  void query_split_lanes_up_to(int64_t time_ms, std::vector<int32_t>& out_note_ids) const;
  void query_split_lanes_in_range(int64_t lower_ms, int64_t upper_ms,
                                  std::vector<int32_t>& out_note_ids) const;

  // End-time tree: ids of notes whose type has a judged tail (is_hold_with_tail),
  // keyed by tick_to_milliseconds(note.end_tick, timing). The key is the RAW end
  // tick, not NotationNote::end_ms(): it is not clamped to start_ms for degenerate
  // tails (end_tick <= start_tick), so every tailed note is found by its exact
  // end_tick. For ordinary holds (end_tick > start_tick) the key equals end_ms().
  // Ids with ends in [lower_ms, upper_ms] (inclusive), ascending by key; within one
  // key the order is unspecified. out_note_ids is cleared first; lower > upper is
  // an empty result. O(log N + k).
  void query_ends_in_range(int64_t lower_ms, int64_t upper_ms,
                           std::vector<int32_t>& out_note_ids) const;

  // Allocation-free visitors over the start / end trees: fn(int32_t note_id) is
  // called for each id with key in [lower_ms, upper_ms] (inclusive), ascending by
  // key. lower > upper visits nothing. fn must not mutate the index.
  template <typename Fn>
  void for_each_start_in_range(int64_t lower_ms, int64_t upper_ms, Fn&& fn) const {
    if (lower_ms > upper_ms) return;
    by_start_ms_.for_each_in_range(lower_ms, upper_ms,
                                   [&fn](int64_t /*key*/, int32_t note_id) { fn(note_id); });
  }
  template <typename Fn>
  void for_each_end_in_range(int64_t lower_ms, int64_t upper_ms, Fn&& fn) const {
    if (lower_ms > upper_ms) return;
    by_end_ms_.for_each_in_range(lower_ms, upper_ms,
                                 [&fn](int64_t /*key*/, int32_t note_id) { fn(note_id); });
  }

  int64_t max_hold_span_ms() const noexcept { return max_hold_span_ms_; }
  int64_t max_split_span_ms() const noexcept { return max_split_span_ms_; }
  // True when cached span metadata may be stale: longest hold removed/shortened,
  // or the current max split removed/shortened/changed to non-split. Shared so
  // ChartDocument's existing hold_span_stale() rebuild path also refreshes
  // max_split_span_ms(). Distinct from max==0 ("no holds/splits").
  bool hold_span_stale() const noexcept { return hold_span_stale_; }

 private:
  static int64_t sat_add_i64(int64_t a, int64_t b) noexcept {
    if (b >= 0) {
      if (a > std::numeric_limits<int64_t>::max() - b) {
        return std::numeric_limits<int64_t>::max();
      }
    } else if (a < std::numeric_limits<int64_t>::min() - b) {
      return std::numeric_limits<int64_t>::min();
    }
    return a + b;
  }

  static int64_t sat_sub_i64(int64_t a, int64_t b) noexcept {
    if (b >= 0) {
      if (a < std::numeric_limits<int64_t>::min() + b) {
        return std::numeric_limits<int64_t>::min();
      }
    } else if (a > std::numeric_limits<int64_t>::max() + b) {
      return std::numeric_limits<int64_t>::max();
    }
    return a - b;
  }

  void insert_id(int32_t note_id, int64_t start_ms);
  void remove_id(int32_t note_id, int64_t start_ms);
  void update_hold_span(const NotationNote& note, const MusicTiming& timing);
  void update_split_span(const NotationNote& note, const MusicTiming& timing);

  detail::StartMsAvlIndex by_start_ms_;
  detail::StartMsAvlIndex split_lane_by_start_ms_;
  detail::StartMsAvlIndex by_end_ms_;
  int64_t max_hold_span_ms_ = 0;
  int64_t max_split_span_ms_ = 0;
  bool hold_span_stale_ = false;
};

}  // namespace wds::chart_editor

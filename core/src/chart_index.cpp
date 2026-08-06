#include <wds/core/chart_index.hpp>

#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>

#include <algorithm>

namespace wds::chart_editor {

void ChartNoteIndex::clear() {
  by_start_ms_.clear();
  split_lane_by_start_ms_.clear();
  max_hold_span_ms_ = 0;
}

void ChartNoteIndex::rebuild(const std::vector<NotationNote>& notes, const MusicTiming& timing) {
  clear();

  std::vector<std::pair<int64_t, int32_t>> start_entries;
  std::vector<std::pair<int64_t, int32_t>> split_entries;
  start_entries.reserve(notes.size());
  split_entries.reserve(notes.size() / 4);

  for (const auto& note : notes) {
    const int64_t start_ms = note.start_ms(timing);
    start_entries.emplace_back(start_ms, note.id);
    update_hold_span(note, timing);

    if (is_split_lane_gimmick(note.gimmick_type)) {
      split_entries.emplace_back(start_ms, note.id);
    }
  }

  by_start_ms_.build(start_entries);
  split_lane_by_start_ms_.build(split_entries);
}

void ChartNoteIndex::on_note_added(const NotationNote& note, const MusicTiming& timing) {
  const int64_t start_ms = note.start_ms(timing);
  insert_id(note.id, start_ms);
  update_hold_span(note, timing);

  if (is_split_lane_gimmick(note.gimmick_type)) {
    split_lane_by_start_ms_.insert(start_ms, note.id);
  }
}

void ChartNoteIndex::on_note_removed(const NotationNote& note, const MusicTiming& timing) {
  const int64_t start_ms = note.start_ms(timing);
  remove_id(note.id, start_ms);

  if (is_split_lane_gimmick(note.gimmick_type)) {
    split_lane_by_start_ms_.erase(start_ms, note.id);
  }

  const int64_t span = std::max<int64_t>(0, note.end_ms(timing) - note.start_ms(timing));
  if (span > 0 && span >= max_hold_span_ms_) {
    // Max may now be stale; ChartDocument rebuilds when this is zeroed.
    max_hold_span_ms_ = 0;
  }
}

void ChartNoteIndex::on_note_updated(const NotationNote& old_note, const NotationNote& new_note,
                                     const MusicTiming& timing) {
  const int64_t old_ms = old_note.start_ms(timing);
  const int64_t new_ms = new_note.start_ms(timing);

  if (old_ms != new_ms) {
    remove_id(old_note.id, old_ms);
    insert_id(new_note.id, new_ms);
  }

  if (is_split_lane_gimmick(old_note.gimmick_type)) {
    split_lane_by_start_ms_.erase(old_ms, old_note.id);
  }
  if (is_split_lane_gimmick(new_note.gimmick_type)) {
    split_lane_by_start_ms_.insert(new_ms, new_note.id);
  }

  const int64_t old_span =
      std::max<int64_t>(0, old_note.end_ms(timing) - old_note.start_ms(timing));
  const int64_t new_span =
      std::max<int64_t>(0, new_note.end_ms(timing) - new_note.start_ms(timing));
  if (old_span >= max_hold_span_ms_ && new_span < old_span) {
    max_hold_span_ms_ = 0;
  } else {
    update_hold_span(new_note, timing);
  }
}

void ChartNoteIndex::insert_id(int32_t note_id, int64_t start_ms) {
  by_start_ms_.insert(start_ms, note_id);
}

void ChartNoteIndex::remove_id(int32_t note_id, int64_t start_ms) {
  by_start_ms_.erase(start_ms, note_id);
}

void ChartNoteIndex::update_hold_span(const NotationNote& note, const MusicTiming& timing) {
  const int64_t span = std::max<int64_t>(0, note.end_ms(timing) - note.start_ms(timing));
  max_hold_span_ms_ = std::max(max_hold_span_ms_, span);
}

void ChartNoteIndex::query_candidates(int64_t time_ms, int64_t lead_ms, int64_t tail_ms,
                                      std::vector<int32_t>& out_note_ids) const {
  out_note_ids.clear();

  const int64_t lower = time_ms - tail_ms;
  const int64_t upper = time_ms + lead_ms;

  by_start_ms_.for_each_in_range(lower, upper, [&](int64_t /*key*/, int32_t note_id) {
    out_note_ids.push_back(note_id);
  });
}

void ChartNoteIndex::query_split_lanes_up_to(int64_t time_ms,
                                             std::vector<int32_t>& out_note_ids) const {
  out_note_ids.clear();

  split_lane_by_start_ms_.for_each_up_to(time_ms, [&](int64_t /*key*/, int32_t note_id) {
    out_note_ids.push_back(note_id);
  });
}

}  // namespace wds::chart_editor

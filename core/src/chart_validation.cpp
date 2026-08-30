#include <wds/core/chart_validation.hpp>

#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/types.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace wds::chart_editor {
namespace {

struct OccupancyEvent {
  int32_t tick = 0;
  int32_t lo = 0;
  int32_t hi = 0;
  int32_t note_id = 0;
  size_t index = 0;
  bool is_tail = false;
};

bool skip_note(const NotationNote& note) noexcept {
  if (is_split_lane_gimmick(note.gimmick_type)) return true;
  return note.note_type == NoteType::HiSpeed || note.note_type == NoteType::None;
}

bool contributes_start(const NotationNote& note) noexcept {
  if (skip_note(note)) return false;
  if (is_hold_mid_star(note.note_type)) return false;
  if (is_hold_body(note.note_type)) return false;
  return true;
}

bool contributes_tail(const NotationNote& note) noexcept {
  if (skip_note(note)) return false;
  if (!is_hold_with_tail(note.note_type)) return false;
  return note.end_tick > note.start_tick;
}

int32_t clamped_width(int32_t width) noexcept { return width < 1 ? 1 : width; }

int32_t closed_hi(int32_t lane, int32_t width) noexcept {
  const int32_t w = clamped_width(width);
  const int64_t hi = static_cast<int64_t>(lane) + static_cast<int64_t>(w) - 1;
  if (hi > std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
  if (hi < std::numeric_limits<int32_t>::min()) return std::numeric_limits<int32_t>::min();
  return static_cast<int32_t>(hi);
}

void span_from_lane_width(int32_t lane, int32_t width, int32_t& lo, int32_t& hi) noexcept {
  lo = lane;
  hi = closed_hi(lane, width);
  if (hi < lo) std::swap(lo, hi);
}

void span_from_end_helper(const NotationNote& note, int32_t& lo, int32_t& hi) noexcept {
  const auto span = resolve_end_lane_span(note);
  span_from_lane_width(span.first, span.second, lo, hi);
}

bool closed_intersect(int32_t a_lo, int32_t a_hi, int32_t b_lo, int32_t b_hi) noexcept {
  return a_lo <= b_hi && b_lo <= a_hi;
}

bool scratch_chain_joint(const NotationNote& prev, const NotationNote& next) noexcept {
  if (!is_scratch_hold_body(prev.note_type) || !is_scratch_hold_body(next.note_type)) {
    return false;
  }
  if (prev.end_tick != next.start_tick || prev.end_tick <= prev.start_tick) return false;
  const int32_t union_left = std::min(prev.lane, next.lane);
  const int32_t union_right = std::max(prev.end_lane(), next.end_lane());
  const auto [cover_lo, cover_hi] = get_scratch_end_lane_range(prev);
  return cover_lo == union_left && cover_hi == union_right;
}

bool is_legal_joint(const OccupancyEvent& a, const OccupancyEvent& b,
                    const std::vector<NotationNote>& notes) noexcept {
  const NotationNote& na = notes[a.index];
  const NotationNote& nb = notes[b.index];
  if (hold_head_pairs_with_body(na, nb) || hold_head_pairs_with_body(nb, na)) {
    return true;
  }
  if (a.is_tail && scratch_chain_joint(na, nb)) return true;
  if (b.is_tail && scratch_chain_joint(nb, na)) return true;
  return false;
}

}  // namespace

ChartValidationResult find_note_overlaps(const std::vector<NotationNote>& notes) {
  std::vector<OccupancyEvent> events;
  events.reserve(notes.size() * 2);
  for (size_t i = 0; i < notes.size(); ++i) {
    const NotationNote& note = notes[i];
    if (contributes_start(note)) {
      OccupancyEvent ev;
      ev.tick = note.start_tick;
      span_from_lane_width(note.lane, note.width, ev.lo, ev.hi);
      ev.note_id = note.id;
      ev.index = i;
      ev.is_tail = false;
      events.push_back(ev);
    }
    if (contributes_tail(note)) {
      OccupancyEvent ev;
      ev.tick = note.end_tick;
      span_from_end_helper(note, ev.lo, ev.hi);
      ev.note_id = note.id;
      ev.index = i;
      ev.is_tail = true;
      events.push_back(ev);
    }
  }

  std::sort(events.begin(), events.end(), [](const OccupancyEvent& a, const OccupancyEvent& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    if (a.index != b.index) return a.index < b.index;
    return a.note_id < b.note_id;
  });

  ChartValidationResult result;
  for (size_t begin = 0; begin < events.size();) {
    size_t end = begin + 1;
    while (end < events.size() && events[end].tick == events[begin].tick) ++end;
    for (size_t i = begin; i < end; ++i) {
      for (size_t j = i + 1; j < end; ++j) {
        const OccupancyEvent& a = events[i];
        const OccupancyEvent& b = events[j];
        if (a.index == b.index) continue;
        if (!closed_intersect(a.lo, a.hi, b.lo, b.hi)) continue;
        if (is_legal_joint(a, b, notes)) continue;
        NoteOverlapPair pair;
        pair.tick = a.tick;
        pair.first_note_id = std::min(a.note_id, b.note_id);
        pair.second_note_id = std::max(a.note_id, b.note_id);
        result.pairs.push_back(pair);
      }
    }
    begin = end;
  }

  std::sort(result.pairs.begin(), result.pairs.end(),
            [](const NoteOverlapPair& a, const NoteOverlapPair& b) {
              if (a.tick != b.tick) return a.tick < b.tick;
              if (a.first_note_id != b.first_note_id) return a.first_note_id < b.first_note_id;
              return a.second_note_id < b.second_note_id;
            });
  result.pairs.erase(std::unique(result.pairs.begin(), result.pairs.end(),
                                 [](const NoteOverlapPair& a, const NoteOverlapPair& b) {
                                   return a.tick == b.tick && a.first_note_id == b.first_note_id &&
                                          a.second_note_id == b.second_note_id;
                                 }),
                     result.pairs.end());

  result.error_ticks.reserve(result.pairs.size());
  for (const auto& pair : result.pairs) {
    if (result.error_ticks.empty() || result.error_ticks.back() != pair.tick) {
      result.error_ticks.push_back(pair.tick);
    }
  }
  return result;
}

}  // namespace wds::chart_editor

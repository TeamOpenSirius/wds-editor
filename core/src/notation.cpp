#include <wds/core/notation.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace wds::chart_editor {
namespace {

bool timing_points_are_normalized(const MusicTiming& timing) noexcept {
  if (timing.ticks_per_quarter <= 0 || timing.points.empty()) {
    return false;
  }
  if (timing.points.front().tick != 0) {
    return false;
  }
  for (size_t i = 1; i < timing.points.size(); ++i) {
    if (timing.points[i].tick <= timing.points[i - 1].tick) {
      return false;
    }
  }
  return true;
}

void ensure_timing_prefix(const MusicTiming& timing) {
  if (timing.prefix_ms.size() == timing.points.size() && !timing.points.empty()) {
    return;
  }
  rebuild_timing_prefix_ms(timing);
}

size_t last_index_tick_le(const std::vector<TimingPoint>& pts, int32_t target) {
  size_t lo = 0;
  size_t hi = pts.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (pts[mid].tick <= target) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo == 0 ? 0 : lo - 1;
}

size_t last_index_prefix_le(const std::vector<double>& prefix, double remain) {
  size_t lo = 0;
  size_t hi = prefix.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (prefix[mid] <= remain) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo == 0 ? 0 : lo - 1;
}

int64_t tick_to_milliseconds_normalized(int32_t tick, const MusicTiming& timing) {
  if (timing.ticks_per_quarter <= 0 || timing.points.empty()) {
    return timing.offset_ms;
  }
  ensure_timing_prefix(timing);
  const auto& pts = timing.points;
  const int32_t target = std::max(0, tick);
  const size_t i = last_index_tick_le(pts, target);
  const double tpq = static_cast<double>(timing.ticks_per_quarter);
  const double bpm = pts[i].bpm > 0.0 ? pts[i].bpm : 120.0;
  const double rate = 60000.0 / (bpm * tpq);
  const double ms = static_cast<double>(timing.offset_ms) + timing.prefix_ms[i] +
                    static_cast<double>(target - pts[i].tick) * rate;
  return static_cast<int64_t>(std::llround(ms));
}

int32_t milliseconds_to_tick_normalized(int64_t ms, const MusicTiming& timing) {
  if (timing.ticks_per_quarter <= 0 || timing.points.empty()) {
    return 0;
  }
  const double remain = static_cast<double>(ms - timing.offset_ms);
  if (remain <= 0.0) {
    return 0;
  }
  ensure_timing_prefix(timing);
  const auto& pts = timing.points;
  const size_t i = last_index_prefix_le(timing.prefix_ms, remain);
  const double tpq = static_cast<double>(timing.ticks_per_quarter);
  const double bpm = pts[i].bpm > 0.0 ? pts[i].bpm : 120.0;
  const double ms_per_tick = 60000.0 / (bpm * tpq);
  const double into = remain - timing.prefix_ms[i];
  return std::max(0, static_cast<int32_t>(std::llround(static_cast<double>(pts[i].tick) +
                                                       into / ms_per_tick)));
}

}  // namespace

int64_t tick_to_milliseconds(int32_t tick, const MusicTiming& timing) {
  if (timing_points_are_normalized(timing)) {
    return tick_to_milliseconds_normalized(tick, timing);
  }
  MusicTiming t = timing;
  normalize_timing_points(t);
  return tick_to_milliseconds_normalized(tick, t);
}

int32_t milliseconds_to_tick(int64_t ms, const MusicTiming& timing) {
  if (timing_points_are_normalized(timing)) {
    return milliseconds_to_tick_normalized(ms, timing);
  }
  MusicTiming t = timing;
  normalize_timing_points(t);
  return milliseconds_to_tick_normalized(ms, t);
}

int64_t NotationNote::start_ms(const MusicTiming& timing) const {
  return tick_to_milliseconds(start_tick, timing);
}

int64_t NotationNote::end_ms(const MusicTiming& timing) const {
  // Instantaneous notes (official endTime -1 → end_tick 0, or end <= start):
  // use start as end so hold heads / taps never expire against a bogus early end_ms.
  if (end_tick <= 0 || end_tick < start_tick) {
    return start_ms(timing);
  }
  return tick_to_milliseconds(end_tick, timing);
}

bool is_hold_family(NoteType type) noexcept {
  switch (type) {
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
    case NoteType::Hold:
    case NoteType::CriticalHold:
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailHold:
    case NoteType::NontailCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::HoldEighth:
      return true;
    default:
      return false;
  }
}

bool is_hold_start(NoteType type) noexcept {
  switch (type) {
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return true;
    default:
      return false;
  }
}

bool is_hold_body(NoteType type) noexcept {
  switch (type) {
    case NoteType::Hold:
    case NoteType::CriticalHold:
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailHold:
    case NoteType::NontailCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::HoldEighth:
      return true;
    default:
      return false;
  }
}

bool is_hold_with_tail(NoteType type) noexcept {
  switch (type) {
    case NoteType::Hold:
    case NoteType::CriticalHold:
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
      return true;
    default:
      return false;
  }
}

bool is_nontail_hold_body(NoteType type) noexcept {
  switch (type) {
    case NoteType::NontailHold:
    case NoteType::NontailCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
      return true;
    default:
      return false;
  }
}

bool is_scratch_hold_body(NoteType type) noexcept {
  switch (type) {
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
      return true;
    default:
      return false;
  }
}

bool is_tap_family(NoteType type) noexcept {
  switch (type) {
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::Sound:
    case NoteType::ScratchSound:
    case NoteType::BlueTap:
      return true;
    default:
      return false;
  }
}

bool is_hold_mid_star(NoteType type) noexcept {
  switch (type) {
    case NoteType::HoldEighth:
    case NoteType::Sound:
    case NoteType::ScratchSound:
      return true;
    default:
      return false;
  }
}

namespace {

bool lanes_overlap(const NotationNote& a, const NotationNote& b) noexcept {
  return a.lane <= b.end_lane() && b.lane <= a.end_lane();
}

bool is_combo_head_note(const NotationNote& note) noexcept {
  if (is_split_lane_gimmick(note.gimmick_type)) {
    return false;
  }
  if (is_hold_mid_star(note.note_type) || is_hold_body(note.note_type)) {
    return false;
  }
  switch (note.note_type) {
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::Flick:
    case NoteType::BlueTap:
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return true;
    default:
      return false;
  }
}

}  // namespace

void collect_hold_body_judge_times(const NotationNote& hold,
                                   const std::vector<NotationNote>& notes,
                                   const MusicTiming& timing,
                                   std::vector<int64_t>& out_sorted_unique) {
  out_sorted_unique.clear();
  if (!is_hold_body(hold.note_type) || is_hold_mid_star(hold.note_type)) {
    return;
  }

  const int64_t start = hold.start_ms(timing);
  const int64_t end = hold.end_ms(timing);
  if (end <= start) {
    return;
  }

  // Sirius: soft body judges are chart mid-stars only (HoldEighth / Sound /
  // ScratchSound). Do not synthesize eighths — HoldEighth already is that beat.
  std::vector<int64_t> times;
  for (const auto& note : notes) {
    if (!is_hold_mid_star(note.note_type)) {
      continue;
    }
    const int64_t ms = note.start_ms(timing);
    if (ms <= start || ms >= end) {
      continue;
    }
    if (!lanes_overlap(hold, note)) {
      continue;
    }
    times.push_back(ms);
  }

  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  out_sorted_unique = std::move(times);
}

void collect_preview_combo_hits(const std::vector<NotationNote>& notes, const MusicTiming& timing,
                                std::vector<int64_t>& out_sorted_hits) {
  out_sorted_hits.clear();
  std::vector<int64_t> hold_times;
  std::vector<char> star_consumed(notes.size(), 0);

  for (size_t i = 0; i < notes.size(); ++i) {
    const auto& note = notes[i];
    if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
      collect_hold_body_judge_times(note, notes, timing, hold_times);
      out_sorted_hits.insert(out_sorted_hits.end(), hold_times.begin(), hold_times.end());
      const int64_t start = note.start_ms(timing);
      const int64_t end = note.end_ms(timing);
      for (size_t j = 0; j < notes.size(); ++j) {
        if (!is_hold_mid_star(notes[j].note_type)) {
          continue;
        }
        const int64_t ms = notes[j].start_ms(timing);
        if (ms > start && ms < end && lanes_overlap(note, notes[j])) {
          star_consumed[j] = 1;
        }
      }
      if (is_hold_with_tail(note.note_type) && end > start) {
        out_sorted_hits.push_back(end);
      }
      continue;
    }

    if (is_combo_head_note(note)) {
      out_sorted_hits.push_back(note.start_ms(timing));
    }
  }

  for (size_t i = 0; i < notes.size(); ++i) {
    if (star_consumed[i] || !is_hold_mid_star(notes[i].note_type)) {
      continue;
    }
    if (is_split_lane_gimmick(notes[i].gimmick_type)) {
      continue;
    }
    out_sorted_hits.push_back(notes[i].start_ms(timing));
  }

  std::sort(out_sorted_hits.begin(), out_sorted_hits.end());
}

PreviewComboState combo_from_sorted_hits(const std::vector<int64_t>& sorted_hits,
                                         int64_t preview_time_ms) {
  PreviewComboState state;
  const auto it =
      std::upper_bound(sorted_hits.begin(), sorted_hits.end(), preview_time_ms);
  state.combo = static_cast<int32_t>(it - sorted_hits.begin());
  if (state.combo > 0) {
    state.last_judge_ms = *(it - 1);
  }
  return state;
}

PreviewComboState compute_preview_combo(const std::vector<NotationNote>& notes,
                                        const MusicTiming& timing,
                                        int64_t preview_time_ms) {
  std::vector<int64_t> hits;
  collect_preview_combo_hits(notes, timing, hits);
  return combo_from_sorted_hits(hits, preview_time_ms);
}

bool contributes_to_concurrent_at_start(NoteType type) noexcept {
  // Mirrors sonolus-sirius-engine/levelData.cpp addSyncLine call sites.
  // Hold mid-stars (HoldEighth / Sound / ScratchSound) are excluded.
  switch (type) {
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::Flick:
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
    case NoteType::BlueTap:
      return true;
    default:
      return false;
  }
}

bool contributes_to_concurrent_at_end(NoteType type) noexcept {
  // Tailed hold ends contribute; nontail ends do not.
  return is_hold_with_tail(type);
}

std::vector<ConcurrentLineNote> build_concurrent_lines(
    const std::vector<NotationNote>& notes, const MusicTiming& timing) {
  struct Span {
    int32_t left = 0;
    int32_t right = 0;
    int32_t count = 0;  // distinct hit events at this ms (need ≥2 for multi-press)
  };
  std::unordered_map<int64_t, Span> by_ms;
  by_ms.reserve(notes.size());

  auto absorb = [&](int64_t ms, int32_t lane, int32_t width) {
    const int32_t right = lane + std::max(1, width) - 1;
    Span& span = by_ms[ms];
    if (span.count == 0) {
      span.left = lane;
      span.right = right;
    } else {
      span.left = std::min(span.left, lane);
      span.right = std::max(span.right, right);
    }
    ++span.count;
  };

  for (const auto& note : notes) {
    if (is_split_lane_gimmick(note.gimmick_type)) {
      continue;
    }
    // Exclude hold bodies / HoldEighth / mid-stars; heads, flats, and hold tails join.
    if (contributes_to_concurrent_at_start(note.note_type)) {
      absorb(note.start_ms(timing), note.lane, note.width);
    }
    if (contributes_to_concurrent_at_end(note.note_type) && note.end_tick > note.start_tick) {
      absorb(note.end_ms(timing), note.lane, note.width);
    }
  }

  std::vector<ConcurrentLineNote> out;
  out.reserve(by_ms.size());
  for (const auto& [ms, span] : by_ms) {
    // Multi-press sync line only when ≥2 notes share the judgment time.
    if (span.count < 2 || span.right < span.left) {
      continue;
    }
    ConcurrentLineNote line;
    line.milliseconds = ms;
    line.start_lane = span.left;
    line.width = span.right - span.left + 1;
    out.push_back(line);
  }
  std::sort(out.begin(), out.end(),
            [](const ConcurrentLineNote& a, const ConcurrentLineNote& b) {
              return a.milliseconds < b.milliseconds;
            });
  return out;
}

ChartDocument::ChartDocument(MusicTiming timing) : timing_(std::move(timing)) {
  normalize_timing_points(timing_);
}

bool ChartDocument::set_timing(MusicTiming timing) {
  if (is_read_only()) {
    return false;
  }
  normalize_timing_points(timing);
  timing_ = std::move(timing);
  rebuild_index();
  rebuild_concurrent_lines();
  mark_dirty();
  return true;
}

bool ChartDocument::set_offset_ms(int64_t offset_ms) {
  if (offset_ms < 0) {
    return false;
  }
  if (timing_.offset_ms == offset_ms) {
    return true;
  }
  timing_.offset_ms = offset_ms;
  rebuild_index();
  rebuild_concurrent_lines();
  ++content_generation_;
  if (!is_read_only()) {
    is_dirty_ = true;
  }
  return true;
}

int32_t ChartDocument::add_note(NotationNote note) {
  if (is_read_only()) {
    return -1;
  }
  if (note.id < 0) {
    note.id = next_id_++;
  } else {
    if (id_to_index_.find(note.id) != id_to_index_.end()) {
      return -1;  // explicit id already present
    }
    next_id_ = std::max(next_id_, note.id + 1);
  }

  index_.on_note_added(note, timing_);
  notes_.push_back(note);
  sort_notes_for_display();
  rebuild_id_index();
  rebuild_concurrent_lines();
  mark_dirty();
  return note.id;
}

bool ChartDocument::update_note(int32_t id, const NotationNote& note) {
  if (is_read_only()) {
    return false;
  }
  const auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) {
    return false;
  }

  const NotationNote old_note = notes_[it->second];
  NotationNote updated = note;
  updated.id = id;

  index_.on_note_updated(old_note, updated, timing_);
  notes_[it->second] = updated;
  sort_notes_for_display();
  rebuild_id_index();
  if (index_.hold_span_stale()) {
    rebuild_index();
  }
  rebuild_concurrent_lines();
  mark_dirty();
  return true;
}

bool ChartDocument::remove_note(int32_t id) {
  if (is_read_only()) {
    return false;
  }
  const auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) {
    return false;
  }

  const NotationNote removed = notes_[it->second];
  index_.on_note_removed(removed, timing_);
  notes_.erase(notes_.begin() + static_cast<std::ptrdiff_t>(it->second));
  sort_notes_for_display();
  rebuild_id_index();
  if (index_.hold_span_stale()) {
    rebuild_index();
  }
  rebuild_concurrent_lines();
  mark_dirty();
  return true;
}

std::optional<NotationNote> ChartDocument::find_note(int32_t id) const {
  const auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) {
    return std::nullopt;
  }
  return notes_[it->second];
}

bool ChartDocument::set_notes(std::vector<NotationNote> notes) {
  if (is_read_only()) {
    return false;
  }
  std::unordered_set<int32_t> seen;
  seen.reserve(notes.size());
  for (const auto& note : notes) {
    if (note.id < 0) continue;
    if (!seen.insert(note.id).second) {
      return false;  // duplicate explicit id
    }
  }
  notes_ = std::move(notes);
  for (auto& note : notes_) {
    if (note.id < 0) {
      note.id = next_id_++;
    } else {
      next_id_ = std::max(next_id_, note.id + 1);
    }
  }
  sort_notes_for_display();
  rebuild_id_index();
  rebuild_index();
  rebuild_concurrent_lines();
  mark_dirty();
  return true;
}

bool ChartDocument::set_concurrent_lines(std::vector<ConcurrentLineNote> lines) {
  if (is_read_only()) {
    return false;
  }
  concurrent_lines_ = std::move(lines);
  std::sort(concurrent_lines_.begin(), concurrent_lines_.end(),
            [](const ConcurrentLineNote& a, const ConcurrentLineNote& b) {
              return a.milliseconds < b.milliseconds;
            });
  mark_dirty();
  return true;
}

void ChartDocument::rebuild_concurrent_lines() {
  concurrent_lines_ = build_concurrent_lines(notes_, timing_);
  std::sort(concurrent_lines_.begin(), concurrent_lines_.end(),
            [](const ConcurrentLineNote& a, const ConcurrentLineNote& b) {
              return a.milliseconds < b.milliseconds;
            });
  if (is_editable()) {
    mark_dirty();
  }
}

NotationChart ChartDocument::to_notation_chart() const {
  NotationChart chart;
  chart.timing = timing_;
  chart.notes = notes_;
  chart.concurrent_lines = concurrent_lines_;
  return chart;
}

NotationChart ChartDocument::normalized_chart() const {
  NotationChart chart = to_notation_chart();
  normalize_notes_inplace(chart.notes);
  chart.concurrent_lines = build_concurrent_lines(chart.notes, chart.timing);
  return chart;
}

void ChartDocument::load_from_chart(const NotationChart& chart, ChartEditMode mode) {
  timing_ = chart.timing;
  normalize_timing_points(timing_);
  notes_ = chart.notes;
  concurrent_lines_ = chart.concurrent_lines;
  edit_mode_ = mode;

  next_id_ = 0;
  for (auto& note : notes_) {
    if (note.id < 0) {
      note.id = next_id_++;
    } else {
      next_id_ = std::max(next_id_, note.id + 1);
    }
  }

  sort_notes_for_display();
  rebuild_id_index();
  rebuild_index();
  rebuild_concurrent_lines();
  is_dirty_ = false;
  ++content_generation_;
}

bool ChartDocument::normalize_for_save() {
  if (is_read_only()) {
    return false;
  }
  normalize_notes_inplace(notes_);
  next_id_ = static_cast<int32_t>(notes_.size());
  rebuild_id_index();
  rebuild_index();
  return true;
}

void ChartDocument::normalize_notes_inplace(std::vector<NotationNote>& notes) {
  std::sort(notes.begin(), notes.end(), compare_notes_for_save);
  int32_t next = 0;
  for (auto& note : notes) {
    note.id = next++;
  }
}

int32_t ChartDocument::next_note_id() const { return next_id_; }

bool ChartDocument::compare_notes_for_save(const NotationNote& a,
                                           const NotationNote& b) noexcept {
  if (a.start_tick != b.start_tick) {
    return a.start_tick < b.start_tick;
  }
  if (a.end_tick != b.end_tick) {
    return a.end_tick < b.end_tick;
  }
  if (a.lane != b.lane) {
    return a.lane < b.lane;
  }
  if (a.width != b.width) {
    return a.width < b.width;
  }
  return static_cast<int>(a.note_type) < static_cast<int>(b.note_type);
}

void ChartDocument::sort_notes_for_display() {
  std::sort(notes_.begin(), notes_.end(), [](const NotationNote& a, const NotationNote& b) {
    if (a.start_tick == b.start_tick) {
      return a.id < b.id;
    }
    return a.start_tick < b.start_tick;
  });
}

void ChartDocument::rebuild_id_index() {
  id_to_index_.clear();
  for (size_t i = 0; i < notes_.size(); ++i) {
    id_to_index_[notes_[i].id] = i;
  }
}

void ChartDocument::rebuild_index() { index_.rebuild(notes_, timing_); }

}  // namespace wds::chart_editor

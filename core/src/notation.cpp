#include <wds/core/notation.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>

namespace wds::chart_editor {

int64_t tick_to_milliseconds(float tick, const MusicTiming& timing) {
  MusicTiming t = timing;
  normalize_timing_points(t);
  if (t.ticks_per_quarter <= 0 || t.points.empty()) {
    return t.offset_ms;
  }
  const double tpq = static_cast<double>(t.ticks_per_quarter);
  double ms = static_cast<double>(t.offset_ms);
  const float target = std::max(0.0f, tick);
  for (size_t i = 0; i < t.points.size(); ++i) {
    const float t0 = static_cast<float>(t.points[i].tick);
    const float t1 = (i + 1 < t.points.size()) ? static_cast<float>(t.points[i + 1].tick)
                                               : std::numeric_limits<float>::infinity();
    if (target <= t0) break;
    const float use = std::min(target, t1);
    const double bpm = t.points[i].bpm > 0.0 ? t.points[i].bpm : 120.0;
    ms += (use - t0) * (60000.0 / (bpm * tpq));
    if (target <= t1) break;
  }
  return static_cast<int64_t>(std::llround(ms));
}

float milliseconds_to_tick(int64_t ms, const MusicTiming& timing) {
  MusicTiming t = timing;
  normalize_timing_points(t);
  if (t.ticks_per_quarter <= 0 || t.points.empty()) {
    return 0.0f;
  }
  const double tpq = static_cast<double>(t.ticks_per_quarter);
  double remain = static_cast<double>(ms - t.offset_ms);
  if (remain <= 0.0) return 0.0f;
  for (size_t i = 0; i < t.points.size(); ++i) {
    const float t0 = static_cast<float>(t.points[i].tick);
    const float t1 = (i + 1 < t.points.size()) ? static_cast<float>(t.points[i + 1].tick)
                                               : std::numeric_limits<float>::infinity();
    const double bpm = t.points[i].bpm > 0.0 ? t.points[i].bpm : 120.0;
    const double ms_per_tick = 60000.0 / (bpm * tpq);
    if (!std::isfinite(t1)) {
      return static_cast<float>(t0 + remain / ms_per_tick);
    }
    const double seg_ms = (t1 - t0) * ms_per_tick;
    if (remain <= seg_ms) {
      return static_cast<float>(t0 + remain / ms_per_tick);
    }
    remain -= seg_ms;
  }
  return static_cast<float>(t.points.back().tick);
}

int64_t NotationNote::start_ms(const MusicTiming& timing) const {
  return tick_to_milliseconds(start_tick, timing);
}

int64_t NotationNote::end_ms(const MusicTiming& timing) const {
  // Instantaneous notes (official endTime -1 → end_tick 0, or end <= start):
  // use start as end so hold heads / taps never expire against a bogus early end_ms.
  if (end_tick <= 0.0f || end_tick < start_tick) {
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

PreviewComboState compute_preview_combo(const std::vector<NotationNote>& notes,
                                        const MusicTiming& timing,
                                        int64_t preview_time_ms) {
  PreviewComboState state;
  std::vector<int64_t> hold_times;
  std::vector<char> star_consumed(notes.size(), 0);

  auto register_hit = [&](int64_t ms) {
    if (ms > preview_time_ms) {
      return;
    }
    state.combo += 1;
    state.last_judge_ms = std::max(state.last_judge_ms, ms);
  };

  for (size_t i = 0; i < notes.size(); ++i) {
    const auto& note = notes[i];
    if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
      collect_hold_body_judge_times(note, notes, timing, hold_times);
      for (int64_t t : hold_times) {
        register_hit(t);
      }
      // Mark mid-stars absorbed by this hold so orphans are not double-counted.
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
        register_hit(end);
      }
      continue;
    }

    if (is_combo_head_note(note)) {
      register_hit(note.start_ms(timing));
    }
  }

  for (size_t i = 0; i < notes.size(); ++i) {
    if (star_consumed[i] || !is_hold_mid_star(notes[i].note_type)) {
      continue;
    }
    if (is_split_lane_gimmick(notes[i].gimmick_type)) {
      continue;
    }
    register_hit(notes[i].start_ms(timing));
  }

  return state;
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
  if (!is_read_only()) {
    mark_dirty();
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
  is_dirty_ = false;
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

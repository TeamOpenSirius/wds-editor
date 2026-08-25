#include <wds/core/note_edit_ops.hpp>

#include <wds/core/gimmick.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace wds::chart_editor {
namespace {

bool overlaps(const NotationNote& a, const NotationNote& b) {
  return a.lane <= b.end_lane() && b.lane <= a.end_lane();
}

bool same_tick(int32_t a, int32_t b) noexcept { return a == b; }

std::vector<NotationNote> notes_with_recomputed_hold_eighths(std::vector<NotationNote> notes,
                                                            const NotationNote& hold,
                                                            int32_t ticks_per_quarter) {
  if (!is_hold_with_tail(hold.note_type) || hold.end_tick <= hold.start_tick) return notes;
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [&](const NotationNote& note) {
                               return note.note_type == NoteType::HoldEighth &&
                                      note.start_tick > hold.start_tick &&
                                      note.start_tick < hold.end_tick && overlaps(note, hold);
                             }),
              notes.end());
  const int32_t step = std::max(1, ticks_per_quarter / 2);
  for (int64_t tick64 = static_cast<int64_t>(hold.start_tick) + step; tick64 < hold.end_tick;
       tick64 += step) {
    const int32_t tick = static_cast<int32_t>(tick64);
    const bool occupied = std::any_of(notes.begin(), notes.end(), [&](const NotationNote& note) {
      return (note.note_type == NoteType::Sound || note.note_type == NoteType::ScratchSound) &&
             same_tick(note.start_tick, tick) && overlaps(note, hold);
    });
    if (!occupied) {
      NotationNote eighth = hold;
      eighth.id = kAutoNoteId;
      eighth.start_tick = tick;
      eighth.end_tick = tick;
      eighth.note_type = NoteType::HoldEighth;
      eighth.scratch_length = 0;
      notes.push_back(eighth);
    }
  }
  return notes;
}

}  // namespace

std::vector<NotationNote> with_recomputed_hold_eighths(std::vector<NotationNote> notes,
                                                       const NotationNote& hold,
                                                       int32_t ticks_per_quarter) {
  return notes_with_recomputed_hold_eighths(std::move(notes), hold, ticks_per_quarter);
}

NotationNote convert_note_type(NotationNote note, NoteType target, int32_t ticks_per_quarter) {
  const bool was_hold = is_hold_with_tail(note.note_type);
  const bool target_hold = is_hold_with_tail(target);
  const bool was_scratch_hold = is_scratch_hold_body(note.note_type);
  const bool target_scratch_hold = is_scratch_hold_body(target);
  const bool split = is_split_lane_gimmick(note.gimmick_type);

  if (!was_hold && target_hold) {
    note.end_tick = note.start_tick + std::max(1, ticks_per_quarter);
  } else if (was_hold && !target_hold) {
    note.end_tick = note.start_tick;
  }

  note.note_type = target;

  // JumpScratch / OneDirection are ScratchHold-family only.
  if (!target_scratch_hold && (is_jump_scratch(note.gimmick_type) || is_one_direction(note.gimmick_type))) {
    note.gimmick_type = GimmickType::None;
  }

  // scratch_length: flick/Scratch direction, ScratchHold end span, or split color.
  if (!split) {
    if (target == NoteType::Flick) {
      // ScratchHold stores ±width (or wider JumpScratch spans); collapse to flick ±1.
      if (was_scratch_hold) {
        if (note.scratch_length < 0) note.scratch_length = -1;
        else if (note.scratch_length > 0) note.scratch_length = 1;
        else note.scratch_length = 0;
      }
    } else if (target_scratch_hold) {
      // Flick/Scratch encode ±1; ScratchHold equal-width direction uses ±width.
      if (!was_scratch_hold) {
        if (note.scratch_length < 0) note.scratch_length = -std::max(1, note.width);
        else if (note.scratch_length > 0) note.scratch_length = std::max(1, note.width);
        else note.scratch_length = 0;
      }
    } else {
      note.scratch_length = 0;
    }
  }
  return note;
}

namespace {

// Flick / Scratch / ScratchHold / JumpScratch encode signed direction/span in
// scratch_length. Horizontal mirror flips left↔right, so the sign must flip.
// Split-lane gimmicks reuse scratch_length as a color id — leave those alone.
void mirror_scratch_direction(NotationNote& note) noexcept {
  if (is_split_lane_gimmick(note.gimmick_type)) return;
  note.scratch_length = -note.scratch_length;
}

}  // namespace

void mirror_notes(std::vector<NotationNote>& notes, int32_t lane_count) {
  for (auto& note : notes) {
    const int32_t w = std::max(1, note.width);
    note.lane = std::clamp(lane_count - note.lane - w, 0, std::max(0, lane_count - w));
    note.width = w;
    mirror_scratch_direction(note);
  }
}

void mirror_notes_about_center(std::vector<NotationNote>& notes) {
  if (notes.empty()) return;
  int32_t left = notes.front().lane;
  int32_t right = notes.front().end_lane();
  for (const auto& note : notes) {
    left = std::min(left, note.lane);
    right = std::max(right, note.end_lane());
  }
  for (auto& note : notes) {
    note.lane = left + right - note.end_lane();
    mirror_scratch_direction(note);
  }
}

bool nudge_notes_time(std::vector<NotationNote>& notes, int32_t delta_tick) {
  for (const auto& note : notes)
    if (note.start_tick + delta_tick < 0 ||
        (note.end_tick > 0 && note.end_tick + delta_tick < 0))
      return false;
  for (auto& note : notes) {
    note.start_tick += delta_tick;
    if (note.end_tick > 0) note.end_tick += delta_tick;
  }
  return true;
}

bool nudge_notes_lane(std::vector<NotationNote>& notes, int32_t delta_lane, int32_t lane_count) {
  for (const auto& note : notes) {
    const auto [occ_lane, occ_width] = occupied_lane_span(note);
    if (!lane_in_bounds(occ_lane + delta_lane, occ_width, lane_count)) return false;
  }
  for (auto& note : notes) note.lane += delta_lane;
  return true;
}

namespace {

// Older editor builds used Normal / Critical / BlueTap as ScratchHold auto-heads.
bool is_legacy_scratch_hold_head(const NotationNote& note) noexcept {
  if (note.end_tick > note.start_tick) return false;
  switch (note.note_type) {
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::BlueTap:
      return true;
    default:
      return false;
  }
}

NoteType auto_hold_head_type(NoteType body_type) noexcept {
  switch (body_type) {
    case NoteType::CriticalHold:
    case NoteType::NontailCriticalHold:
      return NoteType::CriticalHoldStart;
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailScratchCriticalHold:
      return NoteType::ScratchCriticalHoldStart;
    case NoteType::ScratchHold:
    case NoteType::NontailScratchHold:
      return NoteType::ScratchHoldStart;
    default:
      return NoteType::HoldStart;
  }
}

NoteType migrate_legacy_scratch_head_type(const NotationNote& head,
                                          const NotationNote& body) noexcept {
  // Critical tap art on a scratch body → ScratchCriticalHoldStart; otherwise
  // ScratchHoldStart (BlueTap / Normal).
  if (head.note_type == NoteType::Critical ||
      body.note_type == NoteType::ScratchCriticalHold ||
      body.note_type == NoteType::NontailScratchCriticalHold) {
    return NoteType::ScratchCriticalHoldStart;
  }
  return NoteType::ScratchHoldStart;
}

}  // namespace

bool is_hold_head_note(const NotationNote& note) noexcept {
  // Official AppConst hold heads: HoldStart / CriticalHoldStart /
  // ScratchHoldStart / ScratchCriticalHoldStart (instantaneous).
  if (note.end_tick > note.start_tick) return false;
  return is_hold_start(note.note_type);
}

std::optional<NotationNote> make_auto_hold_head(const ChartDocument& doc,
                                                const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type) || hold.width < 1) return std::nullopt;

  const auto mark_range = [&](std::vector<char>& occupied, int32_t lo, int32_t hi) {
    lo = std::max(lo, hold.lane);
    hi = std::min(hi, hold.end_lane());
    for (int32_t lane = lo; lane <= hi; ++lane) {
      occupied[static_cast<size_t>(lane - hold.lane)] = 1;
    }
  };

  // Occupied lanes at hold.start_tick:
  // - notes that start here, except hold bodies / HoldEighth / mid-stars
  // - hold tails of other holds that end here (ScratchHold uses end span)
  // Hold bodies (incl. nontail), eighths, and stars never force head shortening.
  std::vector<char> occupied(static_cast<size_t>(hold.width), 0);
  for (const auto& note : doc.notes()) {
    if (note.id == hold.id) continue;
    if (is_hold_with_tail(note.note_type)) {
      if (!same_tick(note.end_tick, hold.start_tick)) continue;
      if (is_scratch_hold_body(note.note_type)) {
        const auto [tail_lo, tail_hi] = get_scratch_end_lane_range(note);
        mark_range(occupied, tail_lo, tail_hi);
      } else {
        mark_range(occupied, note.lane, note.end_lane());
      }
      continue;
    }
    if (!same_tick(note.start_tick, hold.start_tick)) continue;
    if (is_hold_body(note.note_type) || is_hold_mid_star(note.note_type)) continue;
    mark_range(occupied, note.lane, note.end_lane());
  }

  // Collect continuous free runs inside the body span.
  std::vector<std::pair<int32_t, int32_t>> free_runs;  // (lane, width)
  int32_t run_start = -1;
  for (int32_t i = 0; i < hold.width; ++i) {
    if (!occupied[static_cast<size_t>(i)]) {
      if (run_start < 0) run_start = i;
    } else if (run_start >= 0) {
      free_runs.emplace_back(hold.lane + run_start, i - run_start);
      run_start = -1;
    }
  }
  if (run_start >= 0) {
    free_runs.emplace_back(hold.lane + run_start, hold.width - run_start);
  }
  // Zero free lanes, or more than one discontinuous free range → no head.
  if (free_runs.size() != 1) return std::nullopt;

  NotationNote head = hold;
  head.id = kAutoNoteId;
  head.start_tick = hold.start_tick;
  head.end_tick = hold.start_tick;
  head.lane = free_runs.front().first;
  head.width = free_runs.front().second;
  // Official AppConst: Hold → HoldStart (80); ScratchHold → ScratchHoldStart (82).
  head.note_type = auto_hold_head_type(hold.note_type);
  head.scratch_length = 0;
  head.gimmick_type = GimmickType::None;
  return head;
}

std::optional<NotationNote> paired_hold_head_for(const ChartDocument& doc,
                                                 const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type)) return std::nullopt;
  std::optional<NotationNote> legacy;
  for (const auto& note : doc.notes()) {
    if (note.id == hold.id || !same_tick(note.start_tick, hold.start_tick) ||
        !overlaps(note, hold)) {
      continue;
    }
    if (is_hold_head_note(note)) return note;
    if (!legacy && is_legacy_scratch_hold_head(note) && is_scratch_hold_body(hold.note_type)) {
      legacy = note;
    }
  }
  return legacy;
}

std::optional<NotationNote> paired_hold_body_for(const ChartDocument& doc,
                                                 const NotationNote& head) {
  // Accept official heads and legacy ScratchHold auto-heads so convert/sync
  // still finds the body before repair_legacy_hold_heads runs.
  if (!is_hold_head_note(head) && !is_legacy_scratch_hold_head(head)) return std::nullopt;
  for (const auto& note : doc.notes()) {
    if (!is_hold_with_tail(note.note_type) || !same_tick(note.start_tick, head.start_tick) ||
        !overlaps(note, head)) {
      continue;
    }
    if (is_legacy_scratch_hold_head(head) && !is_scratch_hold_body(note.note_type)) {
      continue;
    }
    return note;
  }
  return std::nullopt;
}

bool ensure_hold_head_if_needed(ChartDocument& doc, const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type)) return false;
  if (auto existing = paired_hold_head_for(doc, hold)) {
    if (is_legacy_scratch_hold_head(*existing) && is_scratch_hold_body(hold.note_type)) {
      NotationNote fixed = *existing;
      fixed.note_type = migrate_legacy_scratch_head_type(*existing, hold);
      return doc.update_note(fixed.id, fixed);
    }
    return true;
  }
  const auto head = make_auto_hold_head(doc, hold);
  if (!head) return false;
  return doc.add_note(*head) >= 0;
}

int repair_legacy_hold_heads(ChartDocument& doc) {
  int repaired = 0;
  // Snapshot bodies first — update_note mutates the note list.
  std::vector<NotationNote> bodies;
  for (const auto& note : doc.notes()) {
    if (is_scratch_hold_body(note.note_type) && is_hold_with_tail(note.note_type)) {
      bodies.push_back(note);
    }
  }
  for (const auto& body : bodies) {
    auto head = paired_hold_head_for(doc, body);
    if (!head || !is_legacy_scratch_hold_head(*head)) continue;
    NotationNote fixed = *head;
    fixed.note_type = migrate_legacy_scratch_head_type(*head, body);
    if (doc.update_note(fixed.id, fixed)) ++repaired;
  }
  return repaired;
}

NoteType resolve_convert_target(const ChartDocument& doc, const NotationNote& note,
                                NoteType target) noexcept {
  const bool scratch_body = is_scratch_hold_body(note.note_type);
  const bool hold_body = is_hold_with_tail(note.note_type);
  // Legacy Normal/Critical/BlueTap heads only count when paired with a ScratchHold body.
  // Lone taps share those types and must not be treated as hold heads (ConvertHold
  // would otherwise incorrectly resolve them to HoldStart).
  const bool legacy_paired_head =
      is_legacy_scratch_hold_head(note) && paired_hold_body_for(doc, note).has_value();
  const bool head = is_hold_head_note(note) || legacy_paired_head;
  const bool scratch_head = [&] {
    if (note.note_type == NoteType::ScratchHoldStart ||
        note.note_type == NoteType::ScratchCriticalHoldStart) {
      return true;
    }
    if (!legacy_paired_head) return false;
    if (auto body = paired_hold_body_for(doc, note)) {
      return is_scratch_hold_body(body->note_type);
    }
    return false;
  }();

  // Hold-head retints (head-only selection). Illegal targets keep the current type.
  if (head && !hold_body) {
    if (target == NoteType::Critical) {
      return scratch_head ? NoteType::ScratchCriticalHoldStart : NoteType::CriticalHoldStart;
    }
    if (target == NoteType::Normal || target == NoteType::HoldStart) {
      return scratch_head ? NoteType::ScratchHoldStart : NoteType::HoldStart;
    }
    if (target == NoteType::Hold) {
      // Scratch-family heads cannot become HoldStart while the body stays ScratchHold.
      return scratch_head ? note.note_type : NoteType::HoldStart;
    }
    if (target == NoteType::ScratchHold) {
      return scratch_head ? NoteType::ScratchHoldStart : note.note_type;
    }
    // Flick / other: not a legal head conversion.
    return note.note_type;
  }

  if (target == NoteType::Critical) {
    // Hold bodies collapse to Critical taps (not CriticalHold*).
    return NoteType::Critical;
  }

  if (target == NoteType::Normal) {
    return NoteType::Normal;
  }

  if (target == NoteType::HoldStart) {
    // Body collapses to a head at start (ScratchHold → ScratchHoldStart).
    if (scratch_body) return NoteType::ScratchHoldStart;
    return NoteType::HoldStart;
  }

  if (target == NoteType::Hold) {
    return NoteType::Hold;
  }

  if (target == NoteType::ScratchHold) {
    return NoteType::ScratchHold;
  }

  if (target == NoteType::Flick) {
    return NoteType::Flick;
  }

  return target;
}

bool recompute_hold_eighths(ChartDocument& doc, const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type) || hold.end_tick <= hold.start_tick) return false;
  auto notes =
      notes_with_recomputed_hold_eighths(doc.notes(), hold, doc.timing().ticks_per_quarter);
  return doc.set_notes(std::move(notes));
}

bool recompute_hold_eighths(ChartDocument& doc) {
  std::vector<NotationNote> holds;
  for (const auto& note : doc.notes()) if (is_hold_with_tail(note.note_type)) holds.push_back(note);
  bool changed = false;
  for (const auto& hold : holds) changed = recompute_hold_eighths(doc, hold) || changed;
  return changed;
}

std::vector<NotationNote> hold_eighths_for(const ChartDocument& doc, const NotationNote& hold) {
  std::vector<NotationNote> out;
  if (!is_hold_with_tail(hold.note_type) || hold.end_tick <= hold.start_tick) return out;
  for (const auto& note : doc.notes()) {
    if (note.note_type == NoteType::HoldEighth && note.start_tick > hold.start_tick &&
        note.start_tick < hold.end_tick && overlaps(note, hold)) {
      out.push_back(note);
    }
  }
  return out;
}

namespace {

bool is_visible_hold_mid_star(NoteType type) noexcept {
  return type == NoteType::Sound || type == NoteType::ScratchSound;
}

bool attached_to_hold_span(const NotationNote& note, const NotationNote& hold) noexcept {
  if (!is_hold_with_tail(hold.note_type) || hold.end_tick <= hold.start_tick) return false;
  if (note.start_tick <= hold.start_tick || note.start_tick >= hold.end_tick) return false;
  if (note.note_type == NoteType::HoldEighth || is_visible_hold_mid_star(note.note_type)) {
    return overlaps(note, hold) ||
           (note.lane == hold.lane && note.width == hold.width);
  }
  return false;
}

}  // namespace

std::vector<NotationNote> hold_attached_notes_for(const ChartDocument& doc,
                                                  const NotationNote& hold) {
  std::vector<NotationNote> out;
  if (!is_hold_with_tail(hold.note_type) || hold.end_tick <= hold.start_tick) return out;
  for (const auto& note : doc.notes()) {
    if (attached_to_hold_span(note, hold)) out.push_back(note);
  }
  return out;
}

std::optional<NotationNote> parent_hold_for(const ChartDocument& doc, const NotationNote& note) {
  if (!(note.note_type == NoteType::HoldEighth || is_visible_hold_mid_star(note.note_type))) {
    return std::nullopt;
  }
  for (const auto& hold : doc.notes()) {
    if (attached_to_hold_span(note, hold)) return hold;
  }
  return std::nullopt;
}

namespace {

bool same_chain_tick(int32_t a, int32_t b) noexcept { return a == b; }

// Prev's JumpScratch must cover the union of both bodies (may be wider on one side).
bool scratch_chain_lanes_connected(const NotationNote& prev, const NotationNote& next) noexcept {
  const int32_t union_left = std::min(prev.lane, next.lane);
  const int32_t union_right = std::max(prev.end_lane(), next.end_lane());
  const auto [cover_lo, cover_hi] = get_scratch_end_lane_range(prev);
  return cover_lo <= union_left && cover_hi >= union_right;
}

}  // namespace

std::optional<NotationNote> chained_next_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body) {
  if (!is_scratch_hold_body(body.note_type) || body.end_tick <= body.start_tick) {
    return std::nullopt;
  }
  for (const auto& note : doc.notes()) {
    if (note.id == body.id || !is_scratch_hold_body(note.note_type)) continue;
    if (!same_chain_tick(note.start_tick, body.end_tick)) continue;
    // A head at the next start means a new chain, not a continuation.
    if (paired_hold_head_for(doc, note)) continue;
    if (!scratch_chain_lanes_connected(body, note)) continue;
    return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> chained_prev_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body) {
  if (!is_scratch_hold_body(body.note_type) || body.end_tick <= body.start_tick) {
    return std::nullopt;
  }
  for (const auto& note : doc.notes()) {
    if (note.id == body.id || !is_scratch_hold_body(note.note_type)) continue;
    if (!same_chain_tick(note.end_tick, body.start_tick)) continue;
    // Prev may have its own head (first segment of a chain). Connection is decided
    // by time abutment + JumpScratch cover containing both bodies.
    if (!scratch_chain_lanes_connected(note, body)) continue;
    return note;
  }
  return std::nullopt;
}

bool prune_hold_mid_stars(ChartDocument& doc, const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type)) return false;
  std::vector<NotationNote> notes = doc.notes();
  const auto end = std::remove_if(notes.begin(), notes.end(), [&](const NotationNote& note) {
    if (!is_visible_hold_mid_star(note.note_type)) return false;
    // Remove if it used to belong to this hold's lanes but left the open interval,
    // or still overlaps lanes but is outside (start, end).
    const bool same_lanes = overlaps(note, hold) ||
                            (note.lane == hold.lane && note.width == hold.width);
    if (!same_lanes) return false;
    return note.start_tick <= hold.start_tick || note.start_tick >= hold.end_tick;
  });
  if (end == notes.end()) return false;
  notes.erase(end, notes.end());
  return doc.set_notes(std::move(notes));
}

std::vector<NotationNote> paste_notes_aligned(const std::vector<NotationNote>& clipboard,
                                              int32_t anchor_tick, const EditGridConfig& grid) {
  if (clipboard.empty()) return {};
  const int32_t earliest = std::min_element(clipboard.begin(), clipboard.end(),
                                            [](const auto& a, const auto& b) {
                                              return a.start_tick < b.start_tick;
                                            })
                               ->start_tick;
  int32_t snapped_anchor = snap_tick(static_cast<float>(anchor_tick), grid);
  // Keep all pasted notes at non-negative ticks.
  if (snapped_anchor < earliest) {
    snapped_anchor = earliest;
  }
  std::vector<NotationNote> result = clipboard;
  for (auto& note : result) {
    note.id = kAutoNoteId;
    note.start_tick += snapped_anchor - earliest;
    if (note.end_tick > 0) note.end_tick += snapped_anchor - earliest;
  }
  return result;
}

}  // namespace wds::chart_editor

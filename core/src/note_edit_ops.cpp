#include <wds/core/note_edit_ops.hpp>

#include <wds/core/gimmick.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wds::chart_editor {
namespace {

bool overlaps(const NotationNote& a, const NotationNote& b) {
  return a.lane <= b.end_lane() && b.lane <= a.end_lane();
}

bool same_tick(int32_t a, int32_t b) noexcept { return a == b; }

bool is_visible_hold_mid_star(NoteType type) noexcept {
  return type == NoteType::Sound || type == NoteType::ScratchSound;
}

bool star_type_matches_hold(NoteType star, NoteType hold) noexcept {
  if (!is_bindable_hold_body(hold)) return false;
  if (star == NoteType::Sound) return !is_scratch_hold_body(hold);
  if (star == NoteType::ScratchSound) return is_scratch_hold_body(hold);
  return false;
}

bool star_matches_hold_for_legacy_bind(const NotationNote& star,
                                       const NotationNote& hold) noexcept {
  if (!star_type_matches_hold(star.note_type, hold.note_type)) return false;
  if (hold.end_tick <= hold.start_tick) return false;
  if (star.start_tick <= hold.start_tick || star.start_tick >= hold.end_tick) return false;
  return star.lane == hold.lane && star.width == hold.width;
}

std::vector<NotationNote> notes_with_recomputed_hold_eighths(std::vector<NotationNote> notes,
                                                            const NotationNote& hold,
                                                            int32_t ticks_per_quarter) {
  if (!is_hold_with_tail(hold.note_type) || hold.end_tick <= hold.start_tick) return notes;
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [&](const NotationNote& note) {
                               if (note.note_type != NoteType::HoldEighth) return false;
                               if (hold.id >= 0 && note.parent_hold_id == hold.id) return true;
                               if (note.parent_hold_id >= 0) return false;
                               return note.start_tick > hold.start_tick &&
                                      note.start_tick < hold.end_tick && overlaps(note, hold);
                             }),
              notes.end());
  const int32_t step = std::max(1, ticks_per_quarter / 2);
  for (int64_t tick64 = static_cast<int64_t>(hold.start_tick) + step; tick64 < hold.end_tick;
       tick64 += step) {
    const int32_t tick = static_cast<int32_t>(tick64);
    const bool occupied = std::any_of(notes.begin(), notes.end(), [&](const NotationNote& note) {
      if (!is_visible_hold_mid_star(note.note_type) || !same_tick(note.start_tick, tick)) {
        return false;
      }
      if (hold.id >= 0 && note.parent_hold_id == hold.id) return true;
      return note.parent_hold_id < 0 && overlaps(note, hold);
    });
    if (!occupied) {
      NotationNote eighth = hold;
      eighth.id = kAutoNoteId;
      eighth.start_tick = tick;
      eighth.end_tick = tick;
      eighth.note_type = NoteType::HoldEighth;
      eighth.scratch_length = 0;
      eighth.parent_hold_id = hold.id >= 0 ? hold.id : kNoBoundHoldId;
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

void strip_hold_eighths(std::vector<NotationNote>& notes) noexcept {
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [](const NotationNote& note) {
                               return note.note_type == NoteType::HoldEighth;
                             }),
              notes.end());
}

std::vector<NotationNote> with_all_hold_eighths_recomputed(std::vector<NotationNote> notes,
                                                           int32_t ticks_per_quarter) {
  strip_hold_eighths(notes);
  std::vector<NotationNote> holds;
  for (const auto& note : notes) {
    if (is_hold_with_tail(note.note_type)) holds.push_back(note);
  }
  for (const auto& hold : holds) {
    notes = notes_with_recomputed_hold_eighths(std::move(notes), hold, ticks_per_quarter);
  }
  return notes;
}

NotationNote convert_note_type(NotationNote note, NoteType target, int32_t ticks_per_quarter) {
  const bool was_hold = is_bindable_hold_body(note.note_type) && note.end_tick > note.start_tick;
  const bool target_hold = is_bindable_hold_body(target);
  const bool was_chain = is_hold_chain_body(note.note_type);
  const bool target_chain = is_hold_chain_body(target);
  const bool split = is_split_lane_gimmick(note.gimmick_type);

  if (target_hold) {
    if (!was_hold) {
      note.end_tick = note.start_tick + std::max(1, ticks_per_quarter);
    }
  } else {
    note.end_tick = note.start_tick;
  }

  note.note_type = target;

  // JumpScratch / OneDirection stay only on hold-chain bodies.
  if (!target_chain &&
      (is_jump_scratch(note.gimmick_type) || is_one_direction(note.gimmick_type))) {
    note.gimmick_type = GimmickType::None;
  }

  // scratch_length: flick direction, hold-chain end span, or split color.
  // Hold tail direction is computed from chain geometry unless authored; converting
  // a non-chain note (Flick ±1 included) always starts as equal-width bidirectional.
  if (!split) {
    if (target == NoteType::Flick) {
      // Hold-chain stores ±width (or wider JumpScratch spans); collapse to flick ±1.
      if (was_chain) {
        if (note.scratch_length < 0) note.scratch_length = -1;
        else if (note.scratch_length > 0) note.scratch_length = 1;
        else note.scratch_length = 0;
      }
    } else if (target_chain) {
      if (!was_chain) note.scratch_length = 0;
    } else {
      note.scratch_length = 0;
    }
  }

  if (!is_hold_mid_star(target)) {
    note.parent_hold_id = kNoBoundHoldId;
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
  // Bounds must include hold-chain end-cap cover (JumpScratch / ScratchHold
  // tail). Body-only min/max leaves a right-extended tail on the wrong side
  // after scratch_length is negated.
  const auto [first_lo, first_w] = occupied_lane_span(notes.front());
  int32_t left = first_lo;
  int32_t right = first_lo + first_w - 1;
  for (const auto& note : notes) {
    const auto [lo, w] = occupied_lane_span(note);
    left = std::min(left, lo);
    right = std::max(right, lo + w - 1);
  }
  for (auto& note : notes) {
    note.lane = left + right - note.end_lane();
    mirror_scratch_direction(note);
  }
}

bool nudge_notes_time(std::vector<NotationNote>& notes, int32_t delta_tick,
                      int32_t min_tick) {
  auto time_floor = [&](const NotationNote& note) {
    if (note.note_type == NoteType::HiSpeed) return 0;
    if (note.note_type == NoteType::None && !is_split_lane_gimmick(note.gimmick_type)) {
      return 0;
    }
    return min_tick;
  };
  for (const auto& note : notes) {
    const int32_t floor = time_floor(note);
    if (note.start_tick + delta_tick < floor ||
        (note.end_tick > 0 && note.end_tick + delta_tick < floor)) {
      return false;
    }
  }
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

bool hold_head_pairs_with_body(const NotationNote& head, const NotationNote& body) noexcept {
  if (!is_hold_head_note(head)) return false;
  if (!is_hold_with_tail(body.note_type) && !is_nontail_hold_body(body.note_type)) {
    return false;
  }
  if (!same_tick(head.start_tick, body.start_tick) || head.lane != body.lane ||
      head.width != body.width) {
    return false;
  }
  const bool scratch_body = is_scratch_hold_body(body.note_type);
  switch (head.note_type) {
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
      return !scratch_body;
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return scratch_body;
    default:
      return false;
  }
}

namespace {

bool same_start_span(const NotationNote& a, const NotationNote& b) noexcept {
  return a.lane == b.lane && a.width == b.width;
}

std::optional<NotationNote> legacy_scratch_head_for(const ChartDocument& doc,
                                                    const NotationNote& hold) {
  if (!is_scratch_hold_body(hold.note_type) || !is_hold_with_tail(hold.note_type)) {
    return std::nullopt;
  }
  for (const auto& note : doc.notes()) {
    if (note.id == hold.id || !is_legacy_scratch_hold_head(note)) continue;
    if (!same_tick(note.start_tick, hold.start_tick) || !same_start_span(note, hold)) {
      continue;
    }
    return note;
  }
  return std::nullopt;
}

}  // namespace

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
      if (is_hold_chain_body(note.note_type)) {
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
  if (!is_hold_with_tail(hold.note_type) && !is_nontail_hold_body(hold.note_type)) {
    return std::nullopt;
  }
  for (const auto& note : doc.notes()) {
    if (note.id == hold.id) continue;
    if (hold_head_pairs_with_body(note, hold)) return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> paired_hold_body_for(const ChartDocument& doc,
                                                 const NotationNote& head) {
  if (!is_hold_head_note(head)) return std::nullopt;
  for (const auto& note : doc.notes()) {
    if (hold_head_pairs_with_body(head, note)) return note;
  }
  return std::nullopt;
}

bool ensure_hold_head_if_needed(ChartDocument& doc, const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type)) return false;
  if (paired_hold_head_for(doc, hold)) return true;
  if (auto legacy = legacy_scratch_head_for(doc, hold)) {
    NotationNote fixed = *legacy;
    fixed.note_type = migrate_legacy_scratch_head_type(*legacy, hold);
    return doc.update_note(fixed.id, fixed);
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
    auto head = legacy_scratch_head_for(doc, body);
    if (!head) continue;
    NotationNote fixed = *head;
    fixed.note_type = migrate_legacy_scratch_head_type(*head, body);
    if (doc.update_note(fixed.id, fixed)) ++repaired;
  }
  return repaired;
}

NoteType resolve_convert_target(const ChartDocument& doc, const NotationNote& note,
                                NoteType target) noexcept {
  (void)doc;
  (void)note;
  return target;
}

namespace {

NoteType visible_star_type_for_hold(NoteType hold_body) noexcept {
  return is_scratch_hold_body(hold_body) ? NoteType::ScratchSound : NoteType::Sound;
}

NoteType hold_head_type_for_body(NoteType current_head, NoteType body) noexcept {
  const bool gold = current_head == NoteType::CriticalHoldStart ||
                    current_head == NoteType::ScratchCriticalHoldStart;
  if (is_scratch_hold_body(body)) {
    return gold ? NoteType::ScratchCriticalHoldStart : NoteType::ScratchHoldStart;
  }
  return gold ? NoteType::CriticalHoldStart : NoteType::HoldStart;
}

void apply_convert_scratch_override(NotationNote& after,
                                    std::optional<int32_t> scratch_length) noexcept {
  if (!scratch_length.has_value()) return;
  const int32_t dir = *scratch_length;
  if (after.note_type == NoteType::Flick) {
    after.scratch_length = dir < 0 ? -1 : (dir > 0 ? 1 : 0);
  } else if (is_scratch_hold_body(after.note_type)) {
    after.scratch_length = dir < 0 ? -after.width : (dir > 0 ? after.width : 0);
  }
}

}  // namespace

ConvertNotesResult convert_notes_in_selection(const ChartDocument& doc,
                                              const std::unordered_set<int32_t>& selected,
                                              NoteType target,
                                              std::optional<int32_t> scratch_length) {
  ConvertNotesResult result;
  if (selected.empty()) return result;
  const int32_t tpq = doc.timing().ticks_per_quarter;
  std::unordered_set<int32_t> handled;
  std::unordered_set<int32_t> remove_ids;

  auto queue_remove = [&](const NotationNote& note) {
    if (remove_ids.insert(note.id).second) result.removals.push_back(note);
  };

  std::vector<int32_t> selected_bodies;
  selected_bodies.reserve(selected.size());
  for (const int32_t id : selected) {
    auto note = doc.find_note(id);
    if (note && is_bindable_hold_body(note->note_type)) selected_bodies.push_back(id);
  }

  for (const int32_t body_id : selected_bodies) {
    auto body = doc.find_note(body_id);
    if (!body) continue;
    NotationNote after = convert_note_type(*body, target, tpq);
    apply_convert_scratch_override(after, scratch_length);
    result.updates[body_id] = after;
    handled.insert(body_id);

    if (is_bindable_hold_body(after.note_type)) {
      const NoteType star_t = visible_star_type_for_hold(after.note_type);
      for (const auto& dep : hold_attached_notes_for(doc, *body)) {
        if (dep.note_type == NoteType::HoldEighth) {
          if (is_nontail_hold_body(after.note_type)) {
            queue_remove(dep);
            handled.insert(dep.id);
          }
          continue;
        }
        if (!is_visible_hold_mid_star(dep.note_type)) continue;
        NotationNote star_after = convert_note_type(dep, star_t, tpq);
        star_after.parent_hold_id = body->id;
        result.updates[dep.id] = star_after;
        handled.insert(dep.id);
      }
      if (auto head = paired_hold_head_for(doc, *body)) {
        const NoteType head_t = hold_head_type_for_body(head->note_type, after.note_type);
        result.updates[head->id] = convert_note_type(*head, head_t, tpq);
        handled.insert(head->id);
      }
    } else {
      for (const auto& dep : hold_attached_notes_for(doc, *body)) {
        if (dep.note_type == NoteType::HoldEighth) {
          queue_remove(dep);
          handled.insert(dep.id);
          continue;
        }
        if (!is_visible_hold_mid_star(dep.note_type)) continue;
        if (selected.count(dep.id)) {
          NotationNote star_after = convert_note_type(dep, target, tpq);
          apply_convert_scratch_override(star_after, scratch_length);
          result.updates[dep.id] = star_after;
        } else {
          queue_remove(dep);
        }
        handled.insert(dep.id);
      }
      if (auto head = paired_hold_head_for(doc, *body)) {
        queue_remove(*head);
        handled.insert(head->id);
      }
    }
  }

  for (const int32_t id : selected) {
    if (handled.count(id) || remove_ids.count(id)) continue;
    auto note = doc.find_note(id);
    if (!note) continue;
    NotationNote after = convert_note_type(*note, target, tpq);
    apply_convert_scratch_override(after, scratch_length);
    result.updates[id] = after;
  }

  for (const int32_t id : remove_ids) result.updates.erase(id);
  return result;
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
    if (note.note_type != NoteType::HoldEighth) continue;
    if (hold.id >= 0 && note.parent_hold_id == hold.id) {
      out.push_back(note);
      continue;
    }
    if (note.parent_hold_id >= 0) continue;
    if (note.start_tick > hold.start_tick && note.start_tick < hold.end_tick &&
        overlaps(note, hold)) {
      out.push_back(note);
    }
  }
  return out;
}

namespace {

bool attached_to_hold_by_bind(const NotationNote& note, const NotationNote& hold) noexcept {
  if (hold.id < 0 || note.parent_hold_id != hold.id) return false;
  return note.note_type == NoteType::HoldEighth || is_visible_hold_mid_star(note.note_type);
}

}  // namespace

std::vector<NotationNote> hold_attached_notes_for(const ChartDocument& doc,
                                                  const NotationNote& hold) {
  std::vector<NotationNote> out;
  if (!is_bindable_hold_body(hold.note_type) || hold.end_tick <= hold.start_tick) return out;
  for (const auto& note : doc.notes()) {
    if (attached_to_hold_by_bind(note, hold)) out.push_back(note);
  }
  return out;
}

std::optional<NotationNote> parent_hold_for(const ChartDocument& doc, const NotationNote& note) {
  if (!(note.note_type == NoteType::HoldEighth || is_visible_hold_mid_star(note.note_type))) {
    return std::nullopt;
  }
  if (note.parent_hold_id < 0) return std::nullopt;
  auto hold = doc.find_note(note.parent_hold_id);
  if (!hold || !is_bindable_hold_body(hold->note_type)) return std::nullopt;
  return hold;
}

bool hold_has_visible_star_at(const std::vector<NotationNote>& notes, int32_t hold_id,
                              int32_t tick, int32_t except_note_id) {
  if (hold_id < 0) return false;
  for (const auto& note : notes) {
    if (note.id == except_note_id) continue;
    if (!is_visible_hold_mid_star(note.note_type)) continue;
    if (note.parent_hold_id != hold_id) continue;
    if (note.start_tick == tick) return true;
  }
  return false;
}

bool hold_has_visible_star_at(const ChartDocument& doc, int32_t hold_id, int32_t tick,
                              int32_t except_note_id) {
  return hold_has_visible_star_at(doc.notes(), hold_id, tick, except_note_id);
}

bool visible_star_tick_conflicts(const std::vector<NotationNote>& notes) {
  std::unordered_map<int64_t, int32_t> seen;
  for (const auto& note : notes) {
    if (!is_visible_hold_mid_star(note.note_type) || note.parent_hold_id < 0) continue;
    const int64_t key = (static_cast<int64_t>(note.parent_hold_id) << 32) |
                        static_cast<uint32_t>(note.start_tick);
    auto [it, inserted] = seen.emplace(key, note.id);
    if (!inserted && it->second != note.id) return true;
  }
  return false;
}

void infer_legacy_star_hold_binds(std::vector<NotationNote>& notes) {
  std::unordered_map<int32_t, const NotationNote*> by_id;
  by_id.reserve(notes.size());
  for (const auto& note : notes) {
    if (note.id >= 0) by_id[note.id] = &note;
  }
  for (auto& star : notes) {
    if (!is_visible_hold_mid_star(star.note_type)) continue;
    if (star.parent_hold_id >= 0) {
      const auto it = by_id.find(star.parent_hold_id);
      if (it == by_id.end() || !is_bindable_hold_body(it->second->note_type)) {
        star.parent_hold_id = kNoBoundHoldId;
      }
    }
    if (star.parent_hold_id >= 0) continue;
    for (const auto& hold : notes) {
      if (hold.id < 0 || !star_matches_hold_for_legacy_bind(star, hold)) continue;
      star.parent_hold_id = hold.id;
      break;
    }
  }
}

namespace {

bool same_chain_tick(int32_t a, int32_t b) noexcept { return a == b; }

}  // namespace

std::optional<NotationNote> chained_next_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body) {
  if (!is_hold_chain_body(body.note_type) || body.end_tick <= body.start_tick) {
    return std::nullopt;
  }
  for (const auto& note : doc.notes()) {
    if (note.id == body.id || !is_hold_chain_body(note.note_type)) continue;
    if (!same_hold_chain_family(body.note_type, note.note_type)) continue;
    if (!same_chain_tick(note.start_tick, body.end_tick)) continue;
    // A head at the next start means a new chain, not a continuation.
    if (paired_hold_head_for(doc, note)) continue;
    if (!hold_chain_lanes_connected(body, note)) continue;
    return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> chained_prev_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body) {
  if (!is_hold_chain_body(body.note_type) || body.end_tick <= body.start_tick) {
    return std::nullopt;
  }
  // A head on this body starts a new chain; it is not a continuation.
  if (paired_hold_head_for(doc, body)) return std::nullopt;
  for (const auto& note : doc.notes()) {
    if (note.id == body.id || !is_hold_chain_body(note.note_type)) continue;
    if (!same_hold_chain_family(body.note_type, note.note_type)) continue;
    if (!same_chain_tick(note.end_tick, body.start_tick)) continue;
    // Prev may have its own head (first segment of a chain). Connection is decided
    // by time abutment + prev tail exactly covering both bodies.
    if (!hold_chain_lanes_connected(note, body)) continue;
    return note;
  }
  return std::nullopt;
}

bool prune_hold_mid_stars(ChartDocument& doc, const NotationNote& hold) {
  if (!is_bindable_hold_body(hold.note_type)) return false;
  std::vector<NotationNote> notes = doc.notes();
  const auto end = std::remove_if(notes.begin(), notes.end(), [&](const NotationNote& note) {
    if (!is_visible_hold_mid_star(note.note_type)) return false;
    if (note.parent_hold_id != hold.id) return false;
    const bool same_lanes = note.lane == hold.lane && note.width == hold.width;
    if (!same_lanes) return true;
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
    note.start_tick += snapped_anchor - earliest;
    if (note.end_tick > 0) note.end_tick += snapped_anchor - earliest;
  }
  return result;
}

}  // namespace wds::chart_editor

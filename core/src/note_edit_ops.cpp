#include <wds/core/note_edit_ops.hpp>

#include <wds/core/gimmick.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
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

// Stars bind by geometry only: Sound and ScratchSound may attach to either hold
// family (blue / purple). Do not reintroduce a color/family gate here — native
// load, official CSV import and save normalization all share this inference.
bool star_matches_hold_for_legacy_bind(const NotationNote& star,
                                       const NotationNote& hold) noexcept {
  if (!is_bindable_hold_body(hold.note_type)) return false;
  if (hold.end_tick <= hold.start_tick) return false;
  if (star.start_tick <= hold.start_tick || star.start_tick >= hold.end_tick) return false;
  return star.lane == hold.lane && star.width == hold.width;
}

int32_t hold_eighth_step(int32_t ticks_per_quarter) noexcept {
  return std::max(1, ticks_per_quarter / 2);
}

// Eighths are always derived: every HoldEighth bound to `hold` (and unbound ones
// inside its span / lanes) is dropped, then the tpq/2 grid is regenerated for any
// bindable hold body (tailed or nontail). Ticks occupied by a visible Sound /
// ScratchSound star stay empty.
// `max_new` clamps how many eighths this call may generate (see
// kMaxAutoHoldEighths); `generated` (optional) reports how many were added.
std::vector<NotationNote> notes_with_recomputed_hold_eighths(std::vector<NotationNote> notes,
                                                            const NotationNote& hold,
                                                            int32_t ticks_per_quarter,
                                                            std::size_t max_new,
                                                            std::size_t* generated = nullptr) {
  if (generated != nullptr) *generated = 0;
  if (!is_bindable_hold_body(hold.note_type) || hold.end_tick <= hold.start_tick) return notes;
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [&](const NotationNote& note) {
                               if (note.note_type != NoteType::HoldEighth) return false;
                               if (hold.id >= 0 && note.parent_hold_id == hold.id) return true;
                               if (note.parent_hold_id >= 0) return false;
                               return note.start_tick > hold.start_tick &&
                                      note.start_tick < hold.end_tick && overlaps(note, hold);
                             }),
              notes.end());
  const int32_t step = hold_eighth_step(ticks_per_quarter);

  // One pass over the existing notes builds the per-tick lookup, so each grid
  // step is O(1) instead of scanning every note (generated eighths are never
  // visible stars, so the lookup stays valid while new eighths are appended).
  std::unordered_set<int32_t> occupied_ticks;
  for (const auto& note : notes) {
    if (note.start_tick <= hold.start_tick || note.start_tick >= hold.end_tick) continue;
    if (is_visible_hold_mid_star(note.note_type)) {
      if ((hold.id >= 0 && note.parent_hold_id == hold.id) ||
          (note.parent_hold_id < 0 && overlaps(note, hold))) {
        occupied_ticks.insert(note.start_tick);
      }
    }
  }

  std::size_t added = 0;
  for (int64_t tick64 = static_cast<int64_t>(hold.start_tick) + step; tick64 < hold.end_tick;
       tick64 += step) {
    if (added >= max_new) break;
    const int32_t tick = static_cast<int32_t>(tick64);
    const bool occupied = occupied_ticks.count(tick) != 0;
    if (!occupied) {
      ++added;
      NotationNote eighth = hold;
      eighth.id = kAutoNoteId;
      eighth.start_tick = tick;
      eighth.end_tick = tick;
      eighth.note_type = NoteType::HoldEighth;
      // Official HoldEighth is gimmick 0 / scratch 0. Copying the parent hold
      // would leak JumpScratch onto type 900 and kill sequential judge.
      eighth.gimmick_type = GimmickType::None;
      eighth.scratch_length = 0;
      eighth.parent_hold_id = hold.id >= 0 ? hold.id : kNoBoundHoldId;
      notes.push_back(eighth);
    }
  }
  if (generated != nullptr) *generated = added;
  return notes;
}

}  // namespace

std::size_t hold_eighth_slot_count(const NotationNote& hold, int32_t ticks_per_quarter) noexcept {
  if (!is_bindable_hold_body(hold.note_type) || hold.end_tick <= hold.start_tick) return 0;
  const int64_t span = static_cast<int64_t>(hold.end_tick) - hold.start_tick;
  // Grid ticks start + k*step for k >= 1 with tick < end_tick.
  return static_cast<std::size_t>((span - 1) / hold_eighth_step(ticks_per_quarter));
}

bool hold_eighths_within_limit(const std::vector<NotationNote>& notes,
                               int32_t ticks_per_quarter) noexcept {
  std::size_t total = 0;
  for (const auto& note : notes) {
    total += hold_eighth_slot_count(note, ticks_per_quarter);
    if (total > kMaxAutoHoldEighths) return false;
  }
  return true;
}

std::vector<NotationNote> with_recomputed_hold_eighths(std::vector<NotationNote> notes,
                                                       const NotationNote& hold,
                                                       int32_t ticks_per_quarter) {
  return notes_with_recomputed_hold_eighths(std::move(notes), hold, ticks_per_quarter,
                                            kMaxAutoHoldEighths);
}

void strip_hold_eighths(std::vector<NotationNote>& notes) noexcept {
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [](const NotationNote& note) {
                               return note.note_type == NoteType::HoldEighth;
                             }),
              notes.end());
}

namespace {

// All holds have distinct non-negative ids (the precondition of the batched
// eighth generation in with_all_hold_eighths_recomputed).
bool holds_have_unique_bound_ids(const std::vector<NotationNote>& holds) {
  std::unordered_set<int32_t> seen;
  seen.reserve(holds.size());
  for (const auto& hold : holds) {
    if (hold.id < 0 || !seen.insert(hold.id).second) return false;
  }
  return true;
}

}  // namespace

std::vector<NotationNote> with_all_hold_eighths_recomputed(std::vector<NotationNote> notes,
                                                           int32_t ticks_per_quarter) {
  strip_hold_eighths(notes);
  std::vector<NotationNote> holds;
  for (const auto& note : notes) {
    if (is_bindable_hold_body(note.note_type)) holds.push_back(note);
  }
  // Total budget across all holds; callers that can report errors reject over-limit
  // charts up front via hold_eighths_within_limit, so clamping here only protects
  // paths with no error channel.
  std::size_t budget = kMaxAutoHoldEighths;

  // Fast path (O(N + generated)): with unique non-negative hold ids the per-hold
  // erase step of notes_with_recomputed_hold_eighths is a no-op (strip_hold_eighths
  // already removed every HoldEighth and generated rows of one hold never match
  // another), and generated rows are never visible stars, so they do not influence
  // later holds. The per-hold lookups are therefore built once from the notes that
  // survived the strip. Output order is unchanged: surviving notes first, then
  // generated eighths in hold order.
  if (holds_have_unique_bound_ids(holds)) {
    const int32_t step = hold_eighth_step(ticks_per_quarter);

    struct UnboundOccupied {
      int32_t tick = 0;
      int32_t lane = 0;
      int32_t end_lane = 0;
    };
    std::unordered_map<int32_t, std::vector<int32_t>> occupied_by_parent;
    std::vector<UnboundOccupied> unbound_occupied;
    for (const auto& note : notes) {
      if (is_visible_hold_mid_star(note.note_type)) {
        if (note.parent_hold_id >= 0) {
          occupied_by_parent[note.parent_hold_id].push_back(note.start_tick);
        } else {
          unbound_occupied.push_back({note.start_tick, note.lane, note.end_lane()});
        }
      }
    }
    std::sort(unbound_occupied.begin(), unbound_occupied.end(),
              [](const UnboundOccupied& a, const UnboundOccupied& b) { return a.tick < b.tick; });

    for (const auto& hold : holds) {
      if (budget == 0) break;
      if (hold.end_tick <= hold.start_tick) continue;

      std::unordered_set<int32_t> occupied_ticks;
      const auto in_span = [&hold](int32_t tick) {
        return tick > hold.start_tick && tick < hold.end_tick;
      };
      const auto occupied_it = occupied_by_parent.find(hold.id);
      if (occupied_it != occupied_by_parent.end()) {
        for (const int32_t tick : occupied_it->second) {
          if (in_span(tick)) occupied_ticks.insert(tick);
        }
      }
      auto unbound_it = std::upper_bound(
          unbound_occupied.begin(), unbound_occupied.end(), hold.start_tick,
          [](int32_t tick, const UnboundOccupied& u) { return tick < u.tick; });
      for (; unbound_it != unbound_occupied.end() && unbound_it->tick < hold.end_tick;
           ++unbound_it) {
        if (unbound_it->lane <= hold.end_lane() && hold.lane <= unbound_it->end_lane) {
          occupied_ticks.insert(unbound_it->tick);
        }
      }

      std::size_t added = 0;
      for (int64_t tick64 = static_cast<int64_t>(hold.start_tick) + step; tick64 < hold.end_tick;
           tick64 += step) {
        if (added >= budget) break;
        const int32_t tick = static_cast<int32_t>(tick64);
        if (occupied_ticks.count(tick) != 0) continue;
        ++added;
        NotationNote eighth = hold;
        eighth.id = kAutoNoteId;
        eighth.start_tick = tick;
        eighth.end_tick = tick;
        eighth.note_type = NoteType::HoldEighth;
        eighth.gimmick_type = GimmickType::None;
        eighth.scratch_length = 0;
        eighth.parent_hold_id = hold.id;
        notes.push_back(eighth);
      }
      budget -= added;
    }
    return notes;
  }

  for (const auto& hold : holds) {
    if (budget == 0) break;
    std::size_t generated = 0;
    notes = notes_with_recomputed_hold_eighths(std::move(notes), hold, ticks_per_quarter, budget,
                                               &generated);
    budget -= std::min(budget, generated);
  }
  return notes;
}

bool is_playfield_selectable(const NotationNote& note) noexcept {
  if (note.note_type == NoteType::HoldEighth) return false;
  return !is_split_lane_gimmick(note.gimmick_type);
}

NotationNote convert_note_type(NotationNote note, NoteType target, int32_t ticks_per_quarter) {
  if (!is_playfield_selectable(note)) return note;
  if (target == NoteType::Scratch) return note;
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

  // scratch_length: flick signed span, hold-chain end span, or split color.
  // Hold tail direction is computed from chain geometry unless authored; converting
  // a non-chain note always starts as equal-width bidirectional.
  if (!split) {
    if (is_flick_like(target)) {
      // Official Flick scratchLength is 0 / ±width. Keep authored |sl|>1 spans
      // (JumpScratch / converted hold tails); expand historic ±1 direction.
      note.scratch_length = official_flick_scratch_length(note.scratch_length, note.width);
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
  if (note.note_type == NoteType::HoldEighth || is_split_lane_gimmick(note.gimmick_type)) return;
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
  // Only notes starting at the hold's tick can qualify; first match in notes() order.
  std::vector<std::size_t> starting;
  doc.note_indices_starting_at(hold.start_tick, starting);
  const auto& all = doc.notes();
  for (const std::size_t index : starting) {
    const NotationNote& note = all[index];
    if (note.id == hold.id || !is_legacy_scratch_hold_head(note)) continue;
    if (!same_start_span(note, hold)) continue;
    return note;
  }
  return std::nullopt;
}

}  // namespace

namespace {

// Shared occupancy model of both make_auto_hold_head overloads. `for_each_note`
// receives a visitor and calls it once per note that may affect the hold start
// (all notes for the ChartDocument overload, a candidate subset for the indexed one).
template <typename ForEachNote>
std::optional<NotationNote> make_auto_hold_head_impl(const NotationNote& hold,
                                                     ForEachNote&& for_each_note) {
  if (!is_bindable_hold_body(hold.note_type) || hold.width < 1) return std::nullopt;

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
  const auto visit = [&](const NotationNote& note) {
    if (note.id == hold.id) return;
    if (is_hold_with_tail(note.note_type)) {
      if (!same_tick(note.end_tick, hold.start_tick)) return;
      if (is_hold_chain_body(note.note_type)) {
        const auto [tail_lo, tail_hi] = get_scratch_end_lane_range(note);
        mark_range(occupied, tail_lo, tail_hi);
      } else {
        mark_range(occupied, note.lane, note.end_lane());
      }
      return;
    }
    if (!same_tick(note.start_tick, hold.start_tick)) return;
    if (is_hold_body(note.note_type) || is_hold_mid_star(note.note_type)) return;
    mark_range(occupied, note.lane, note.end_lane());
  };
  for_each_note(visit);

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

}  // namespace

std::optional<NotationNote> make_auto_hold_head(const ChartDocument& doc,
                                                 const NotationNote& hold) {
  if (!is_bindable_hold_body(hold.note_type) || hold.width < 1) return std::nullopt;
  // The occupancy rules only react to notes starting at hold.start_tick (non-tailed)
  // and tailed notes ending there; every other note is a no-op in the visitor.
  std::vector<std::size_t> starting;
  std::vector<std::size_t> tails;
  doc.note_indices_starting_at(hold.start_tick, starting);
  doc.tail_note_indices_ending_at(hold.start_tick, tails);
  std::vector<std::size_t> candidates;
  candidates.reserve(starting.size() + tails.size());
  std::set_union(starting.begin(), starting.end(), tails.begin(), tails.end(),
                 std::back_inserter(candidates));
  const auto& all = doc.notes();
  return make_auto_hold_head_impl(hold, [&](const auto& visit) {
    for (const std::size_t index : candidates) visit(all[index]);
  });
}

std::optional<NotationNote> make_auto_hold_head(const NotationNote& hold,
                                                 const std::vector<NotationNote>& notes,
                                                 const std::vector<std::size_t>& candidates) {
  return make_auto_hold_head_impl(hold, [&](const auto& visit) {
    for (const std::size_t index : candidates) visit(notes[index]);
  });
}

std::optional<NotationNote> make_auto_hold_head(const NotationNote& hold,
                                                 const std::vector<NotationNote>& notes) {
  // Unassigned ids (< 0) must not match notes that also carry an unassigned id.
  NotationNote probe = hold;
  if (probe.id < 0) probe.id = std::numeric_limits<int32_t>::min();
  auto head = make_auto_hold_head_impl(probe, [&](const auto& visit) {
    for (const auto& note : notes) visit(note);
  });
  return head;
}

std::optional<NotationNote> paired_hold_head_for(const ChartDocument& doc,
                                                 const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type) && !is_nontail_hold_body(hold.note_type)) {
    return std::nullopt;
  }
  // A head pairs only at the body's own start tick; first match in notes() order.
  std::vector<std::size_t> starting;
  doc.note_indices_starting_at(hold.start_tick, starting);
  const auto& all = doc.notes();
  for (const std::size_t index : starting) {
    const NotationNote& note = all[index];
    if (note.id == hold.id) continue;
    if (hold_head_pairs_with_body(note, hold)) return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> paired_hold_body_for(const ChartDocument& doc,
                                                 const NotationNote& head) {
  if (!is_hold_head_note(head)) return std::nullopt;
  std::vector<std::size_t> starting;
  doc.note_indices_starting_at(head.start_tick, starting);
  const auto& all = doc.notes();
  for (const std::size_t index : starting) {
    if (hold_head_pairs_with_body(head, all[index])) return all[index];
  }
  return std::nullopt;
}

bool ensure_hold_head_if_needed(ChartDocument& doc, const NotationNote& hold) {
  if (!is_bindable_hold_body(hold.note_type)) return false;
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
  if (is_flick_like(after.note_type)) {
    after.scratch_length = encode_flick_scratch_length(dir, after.width);
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
  if (selected.empty() || target == NoteType::Scratch) return result;
  const int32_t tpq = doc.timing().ticks_per_quarter;
  std::unordered_set<int32_t> handled;
  std::unordered_set<int32_t> remove_ids;

  auto queue_remove = [&](const NotationNote& note) {
    if (remove_ids.insert(note.id).second) result.removals.push_back(note);
  };

  // Sound / ScratchSound conversion is the only path that changes a star:
  // - a selected hold converts its attached visible stars, leaving the hold alone;
  // - a selected star converts its own type and keeps its parent bind.
  // Ordinary notes, heads and eighths are never turned into stars.
  if (is_visible_hold_mid_star(target)) {
    for (const int32_t id : selected) {
      auto note = doc.find_note(id);
      if (!note || !is_playfield_selectable(*note)) continue;
      if (is_bindable_hold_body(note->note_type)) {
        for (const auto& dep : hold_attached_notes_for(doc, *note)) {
          if (!is_visible_hold_mid_star(dep.note_type) || dep.note_type == target) continue;
          NotationNote star_after = convert_note_type(dep, target, tpq);
          star_after.parent_hold_id = note->id;
          result.updates[dep.id] = star_after;
        }
      } else if (is_visible_hold_mid_star(note->note_type) && note->note_type != target) {
        result.updates[id] = convert_note_type(*note, target, tpq);
      }
    }
    return result;
  }

  std::vector<int32_t> selected_bodies;
  selected_bodies.reserve(selected.size());
  for (const int32_t id : selected) {
    auto note = doc.find_note(id);
    if (note && is_playfield_selectable(*note) && is_bindable_hold_body(note->note_type)) {
      selected_bodies.push_back(id);
    }
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
          // HoldEighth is an in-body judgment point and remains valid for both
          // tailed and nontail bodies. Only a non-hold conversion removes it.
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
        // Hold → non-hold removes every attached star, selected or not. Stars
        // themselves are never converted to non-star types by this path.
        queue_remove(dep);
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
    if (!note || !is_playfield_selectable(*note)) continue;
    // Stars keep their type unless the target is a star (handled above).
    if (is_visible_hold_mid_star(note->note_type)) continue;
    NotationNote after = convert_note_type(*note, target, tpq);
    apply_convert_scratch_override(after, scratch_length);
    result.updates[id] = after;
  }

  for (const int32_t id : remove_ids) result.updates.erase(id);
  return result;
}

bool recompute_hold_eighths(ChartDocument& doc, const NotationNote& hold) {
  if (!is_bindable_hold_body(hold.note_type) || hold.end_tick <= hold.start_tick) return false;
  auto notes = notes_with_recomputed_hold_eighths(doc.notes(), hold, doc.timing().ticks_per_quarter,
                                                  kMaxAutoHoldEighths);
  return doc.set_notes(std::move(notes));
}

bool recompute_hold_eighths(ChartDocument& doc) {
  const bool has_derived_input = std::any_of(
      doc.notes().begin(), doc.notes().end(), [](const NotationNote& note) {
        return is_bindable_hold_body(note.note_type) || note.note_type == NoteType::HoldEighth;
      });
  if (!has_derived_input) return false;
  return doc.set_notes(
      with_all_hold_eighths_recomputed(doc.notes(), doc.timing().ticks_per_quarter));
}

std::vector<NotationNote> hold_eighths_for(const ChartDocument& doc, const NotationNote& hold) {
  std::vector<NotationNote> out;
  if (!is_bindable_hold_body(hold.note_type) || hold.end_tick <= hold.start_tick) return out;
  const auto& all = doc.notes();
  // Bound eighths (any tick) plus unbound eighths strictly inside the hold's range;
  // the two sets are disjoint (parent >= 0 vs < 0). Merge by index = notes() order.
  std::vector<std::size_t> picked;
  if (hold.id >= 0) {
    doc.note_indices_with_parent(hold.id, picked);
    picked.erase(std::remove_if(picked.begin(), picked.end(),
                                [&](std::size_t i) {
                                  return all[i].note_type != NoteType::HoldEighth;
                                }),
                 picked.end());
  }
  std::vector<std::size_t> in_range;
  doc.note_indices_starting_in(hold.start_tick + 1, hold.end_tick - 1, in_range);
  for (const std::size_t i : in_range) {
    const NotationNote& note = all[i];
    if (note.note_type != NoteType::HoldEighth || note.parent_hold_id >= 0) continue;
    if (note.start_tick > hold.start_tick && note.start_tick < hold.end_tick &&
        overlaps(note, hold)) {
      picked.push_back(i);
    }
  }
  std::sort(picked.begin(), picked.end());
  for (const std::size_t i : picked) out.push_back(all[i]);
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
  if (hold.id < 0) return out;  // attached_to_hold_by_bind requires hold.id >= 0
  std::vector<std::size_t> children;
  doc.note_indices_with_parent(hold.id, children);
  const auto& all = doc.notes();
  for (const std::size_t index : children) {
    if (attached_to_hold_by_bind(all[index], hold)) out.push_back(all[index]);
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
  if (hold_id < 0) return false;
  // Same predicate as the vector overload, restricted to notes starting at `tick`.
  std::vector<std::size_t> starting;
  doc.note_indices_starting_at(tick, starting);
  const auto& all = doc.notes();
  for (const std::size_t index : starting) {
    const NotationNote& note = all[index];
    if (note.id == except_note_id) continue;
    if (!is_visible_hold_mid_star(note.note_type)) continue;
    if (note.parent_hold_id != hold_id) continue;
    return true;
  }
  return false;
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

namespace {

// Candidate holds for the legacy star bind fallback, bucketed by exact (lane, width)
// and sorted by start_tick. `max_span` (largest end - start in the bucket) bounds how
// far back from a star's tick a hold can start and still cover it.
struct LegacyBindHold {
  int32_t start_tick = 0;
  std::size_t position = 0;  // index into the notes vector (original order)
};

struct LegacyBindBucket {
  std::vector<LegacyBindHold> by_start;
  int64_t max_span = 0;
};

uint64_t legacy_bind_bucket_key(int32_t lane, int32_t width) noexcept {
  return (static_cast<uint64_t>(static_cast<uint32_t>(lane)) << 32) |
         static_cast<uint64_t>(static_cast<uint32_t>(width));
}

}  // namespace

void infer_legacy_star_hold_binds(std::vector<NotationNote>& notes) {
  std::unordered_map<int32_t, const NotationNote*> by_id;
  by_id.reserve(notes.size());
  for (const auto& note : notes) {
    if (note.id >= 0) by_id[note.id] = &note;
  }

  // Built on first use: most loads have no unbound visible star.
  std::unordered_map<uint64_t, LegacyBindBucket> buckets;
  bool buckets_built = false;
  const auto build_buckets = [&]() {
    buckets_built = true;
    for (std::size_t i = 0; i < notes.size(); ++i) {
      const NotationNote& hold = notes[i];
      if (hold.id < 0 || !is_bindable_hold_body(hold.note_type)) continue;
      if (hold.end_tick <= hold.start_tick) continue;
      LegacyBindBucket& bucket = buckets[legacy_bind_bucket_key(hold.lane, hold.width)];
      bucket.by_start.push_back({hold.start_tick, i});
      bucket.max_span = std::max(bucket.max_span, static_cast<int64_t>(hold.end_tick) -
                                                      static_cast<int64_t>(hold.start_tick));
    }
    for (auto& entry : buckets) {
      auto& list = entry.second.by_start;
      std::sort(list.begin(), list.end(), [](const LegacyBindHold& a, const LegacyBindHold& b) {
        return a.start_tick < b.start_tick;
      });
    }
  };

  for (auto& star : notes) {
    if (!is_visible_hold_mid_star(star.note_type)) continue;
    if (star.parent_hold_id >= 0) {
      const auto it = by_id.find(star.parent_hold_id);
      if (it == by_id.end() || !is_bindable_hold_body(it->second->note_type)) {
        star.parent_hold_id = kNoBoundHoldId;
      }
    }
    if (star.parent_hold_id >= 0) continue;

    // Old behaviour: first hold in vector order that matches. A match needs the star's
    // exact lane/width and start < star.start < end, so only that bucket's holds with
    // start_tick in (star.start - max_span, star.start) can qualify; take the lowest
    // original position among them.
    if (!buckets_built) build_buckets();
    const auto bucket_it = buckets.find(legacy_bind_bucket_key(star.lane, star.width));
    if (bucket_it == buckets.end()) continue;
    const LegacyBindBucket& bucket = bucket_it->second;
    const auto first_not_before = std::lower_bound(
        bucket.by_start.begin(), bucket.by_start.end(), star.start_tick,
        [](const LegacyBindHold& h, int32_t tick) { return h.start_tick < tick; });
    bool found = false;
    std::size_t best_position = 0;
    for (auto it = first_not_before; it != bucket.by_start.begin();) {
      --it;
      if (static_cast<int64_t>(it->start_tick) + bucket.max_span <=
          static_cast<int64_t>(star.start_tick)) {
        break;  // this and every earlier hold ends at or before the star
      }
      if (found && it->position > best_position) continue;
      if (!star_matches_hold_for_legacy_bind(star, notes[it->position])) continue;
      found = true;
      best_position = it->position;
    }
    if (found) star.parent_hold_id = notes[best_position].id;
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
  // Candidates start exactly at body.end_tick; first match in notes() order.
  std::vector<std::size_t> starting;
  doc.note_indices_starting_at(body.end_tick, starting);
  const auto& all = doc.notes();
  for (const std::size_t index : starting) {
    const NotationNote& note = all[index];
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
  // Hold-chain bodies are a subset of tailed types, so the end tree holds every
  // candidate; first match in notes() order.
  std::vector<std::size_t> ending;
  doc.tail_note_indices_ending_at(body.start_tick, ending);
  const auto& all = doc.notes();
  for (const std::size_t index : ending) {
    const NotationNote& note = all[index];
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
  // Stars are bound by parent_hold_id and may sit outside the hold's tick range
  // (that is exactly what is pruned), so the time trees cannot narrow this scan.
  // Check on the document first so the common nothing-to-prune case does not copy
  // the whole note list.
  const auto should_prune = [&](const NotationNote& note) {
    if (!is_visible_hold_mid_star(note.note_type)) return false;
    if (note.parent_hold_id != hold.id) return false;
    const bool same_lanes = note.lane == hold.lane && note.width == hold.width;
    if (!same_lanes) return true;
    return note.start_tick <= hold.start_tick || note.start_tick >= hold.end_tick;
  };
  const auto& current = doc.notes();
  if (hold.id >= 0) {
    // Only notes bound to this hold can match: ask the parent map.
    std::vector<std::size_t> children;
    doc.note_indices_with_parent(hold.id, children);
    const bool any = std::any_of(children.begin(), children.end(), [&](std::size_t index) {
      return should_prune(current[index]);
    });
    if (!any) return false;
  } else if (!std::any_of(current.begin(), current.end(), should_prune)) {
    return false;  // negative hold id matches unbound stars: keep the exact old scan
  }
  std::vector<NotationNote> notes = current;
  const auto end = std::remove_if(notes.begin(), notes.end(), should_prune);
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
  // The anchor is the destination of the earliest note, in either direction. snap_tick
  // keeps it non-negative, so every pasted note stays non-negative too (note starts are
  // >= earliest, so start + snapped_anchor - earliest >= snapped_anchor >= 0).
  const int32_t snapped_anchor = snap_tick(static_cast<float>(anchor_tick), grid);
  std::vector<NotationNote> result = clipboard;
  for (auto& note : result) {
    note.start_tick += snapped_anchor - earliest;
    if (note.end_tick > 0) note.end_tick += snapped_anchor - earliest;
  }
  return result;
}

}  // namespace wds::chart_editor

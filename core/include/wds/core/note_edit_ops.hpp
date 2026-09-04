#pragma once

#include <wds/core/edit_grid.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wds::chart_editor {

// Convert one note to any target type. Extra fields are dropped; missing fields
// get defaults (non-hold → hold body duration is one beat).
NotationNote convert_note_type(NotationNote note, NoteType target, int32_t ticks_per_quarter);
// Mirror lanes horizontally. Also negates scratch_length (flick / ScratchHold /
// JumpScratch direction·span) so arrows and end covers flip with the body.
// Split-lane color ids stored in scratch_length are left unchanged.
void mirror_notes(std::vector<NotationNote>& notes, int32_t lane_count);
// Same lane flip as mirror_notes, but the axis is the selection's occupied
// span (body union hold-chain tail cover), not the playfield.
void mirror_notes_about_center(std::vector<NotationNote>& notes);
bool nudge_notes_time(std::vector<NotationNote>& notes, int32_t delta_tick,
                      int32_t min_tick = 0);
bool nudge_notes_lane(std::vector<NotationNote>& notes, int32_t delta_lane, int32_t lane_count);

// Toolbar convert target is used as-is: every authored type can become every
// other type. Extra fields / defaults are applied by convert_note_type.
NoteType resolve_convert_target(const ChartDocument& doc, const NotationNote& note,
                                NoteType target) noexcept;

struct ConvertNotesResult {
  std::unordered_map<int32_t, NotationNote> updates;
  std::vector<NotationNote> removals;
};

// Selection convert: any type → any type. Hold → another hold remaps attached
// visible stars (Sound ↔ ScratchSound) and the paired head family. Hold →
// non-hold deletes unselected stars / eighths / the paired head; selected stars
// convert to the target instead of being deleted.
ConvertNotesResult convert_notes_in_selection(
    const ChartDocument& doc, const std::unordered_set<int32_t>& selected, NoteType target,
    std::optional<int32_t> scratch_length = std::nullopt);

// Replaces HoldEighth notes bound to this hold (unbound open-span fallback).
bool recompute_hold_eighths(ChartDocument& doc, const NotationNote& hold);
bool recompute_hold_eighths(ChartDocument& doc);

// Drop every HoldEighth. File I/O never trusts persisted eighths.
void strip_hold_eighths(std::vector<NotationNote>& notes) noexcept;

// Strip file/stale eighths, then regenerate from every tailed hold (tpq/2 grid;
// ticks occupied by Sound / ScratchSound stay empty).
std::vector<NotationNote> with_all_hold_eighths_recomputed(std::vector<NotationNote> notes,
                                                           int32_t ticks_per_quarter);

// Pure variant for folding eighths into an undoable SetNotesCommand before/after.
std::vector<NotationNote> with_recomputed_hold_eighths(std::vector<NotationNote> notes,
                                                       const NotationNote& hold,
                                                       int32_t ticks_per_quarter);

// Auto head for a hold body start: full body span if free; if other notes
// (taps/heads/flicks/… at start, or hold tails ending here) partially overlap,
// only the single continuous free lane run inside the body. Multiple free
// runs → nullopt (no head). Hold bodies, HoldEighth, and mid-stars never block.
std::optional<NotationNote> make_auto_hold_head(const ChartDocument& doc,
                                                const NotationNote& hold);
bool ensure_hold_head_if_needed(ChartDocument& doc, const NotationNote& hold);

// Official hold heads: HoldStart / CriticalHoldStart / ScratchHoldStart /
// ScratchCriticalHoldStart (instantaneous).
bool is_hold_head_note(const NotationNote& note) noexcept;

// True when `head` is the official start of `body`: same tick, same start
// span (lane + width), and family-compatible types. Blue/CriticalHold bodies
// accept HoldStart / CriticalHoldStart; Scratch* bodies accept
// ScratchHoldStart / ScratchCriticalHoldStart. First ScratchHold segment
// uses this same rule (no overlap / JumpScratch matching).
bool hold_head_pairs_with_body(const NotationNote& head, const NotationNote& body) noexcept;

// Rewrite legacy ScratchHold auto-heads (Normal / Critical / BlueTap) to
// ScratchHoldStart / ScratchCriticalHoldStart. Returns how many notes changed.
int repair_legacy_hold_heads(ChartDocument& doc);

// Official paired head at the hold body's start, if any.
std::optional<NotationNote> paired_hold_head_for(const ChartDocument& doc,
                                                 const NotationNote& hold);
// Official hold body paired with an instantaneous head, if any.
std::optional<NotationNote> paired_hold_body_for(const ChartDocument& doc,
                                                 const NotationNote& head);

// HoldEighth notes bound to `hold` (unbound fallback: open span + lane overlap).
std::vector<NotationNote> hold_eighths_for(const ChartDocument& doc, const NotationNote& hold);

// HoldEighth + visible mid-stars (Sound / ScratchSound) attached to a hold body.
// Does NOT include the paired hold head — heads sync on edit but delete independently.
std::vector<NotationNote> hold_attached_notes_for(const ChartDocument& doc,
                                                  const NotationNote& hold);

// Parent hold body for a mid-star / HoldEighth, if any.
std::optional<NotationNote> parent_hold_for(const ChartDocument& doc, const NotationNote& note);

// Chained hold neighbors (regular or scratch, same family): next starts at this
// end, has no own head, and prev's current tail exactly covers both bodies.
// Looking up prev also requires this body to have no head; the previous segment
// itself may own a head (first segment of a chain).
std::optional<NotationNote> chained_next_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body);
std::optional<NotationNote> chained_prev_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body);

// Drop Sound / ScratchSound mid-stars that left the hold's open time span / lanes.
bool prune_hold_mid_stars(ChartDocument& doc, const NotationNote& hold);

// Visible Sound / ScratchSound only. Same bound hold + same tick, excluding `except_note_id`.
bool hold_has_visible_star_at(const std::vector<NotationNote>& notes, int32_t hold_id,
                              int32_t tick, int32_t except_note_id);
bool hold_has_visible_star_at(const ChartDocument& doc, int32_t hold_id, int32_t tick,
                              int32_t except_note_id);

// True when two bound visible stars share a parent hold id and start_tick.
bool visible_star_tick_conflicts(const std::vector<NotationNote>& notes);

// Clear missing / non-hold parent_hold_id, then bind remaining unbound visible
// stars when type, open span, and exact lanes match. Used on load and before
// save-time id normalization.
void infer_legacy_star_hold_binds(std::vector<NotationNote>& notes);

// Returns copies shifted so the earliest source note starts at snapped anchor_tick.
// Original ids are kept so callers can remap parent_hold_id after assigning new ids.
std::vector<NotationNote> paste_notes_aligned(const std::vector<NotationNote>& clipboard,
                                              int32_t anchor_tick,
                                              const EditGridConfig& grid);

}  // namespace wds::chart_editor

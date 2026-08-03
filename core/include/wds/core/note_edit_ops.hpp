#pragma once

#include <wds/core/edit_grid.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace wds::chart_editor {

NotationNote convert_note_type(NotationNote note, NoteType target, int32_t ticks_per_quarter);
// Mirror lanes horizontally. Also negates scratch_length (flick / ScratchHold /
// JumpScratch direction·span) so arrows and end covers flip with the body.
// Split-lane color ids stored in scratch_length are left unchanged.
void mirror_notes(std::vector<NotationNote>& notes, int32_t lane_count);
void mirror_notes_about_center(std::vector<NotationNote>& notes);
bool nudge_notes_time(std::vector<NotationNote>& notes, int32_t delta_tick);
bool nudge_notes_lane(std::vector<NotationNote>& notes, int32_t delta_lane, int32_t lane_count);

// Map a toolbar convert target to the note-family-correct type.
// Hold heads (when converted alone): only legal head retints
// (HoldStart↔CriticalHoldStart / ScratchHoldStart↔ScratchCriticalHoldStart); illegal
// targets keep the current type. Hold bodies under Tap/Critical/HoldStart/Flick collapse
// to that instantaneous type (UI deletes the paired head). ConvertHold /
// ConvertScratchHold force the matching hold-body type.
NoteType resolve_convert_target(const ChartDocument& doc, const NotationNote& note,
                                NoteType target) noexcept;

// Replaces generated HoldEighth notes belonging to this hold's span.
bool recompute_hold_eighths(ChartDocument& doc, const NotationNote& hold);
bool recompute_hold_eighths(ChartDocument& doc);

// Pure variant for folding eighths into an undoable SetNotesCommand before/after.
std::vector<NotationNote> with_recomputed_hold_eighths(std::vector<NotationNote> notes,
                                                       const NotationNote& hold,
                                                       int32_t ticks_per_quarter);

// Auto head for a hold body start: full body span if free; if other notes
// (non-hold-body at start, or hold tails ending here) partially overlap,
// only the single continuous free lane run inside the body. Multiple free
// runs → nullopt (no head). Other hold bodies starting at the same tick ignored.
std::optional<NotationNote> make_auto_hold_head(const ChartDocument& doc,
                                                const NotationNote& hold);
bool ensure_hold_head_if_needed(ChartDocument& doc, const NotationNote& hold);

// Official hold heads: HoldStart / CriticalHoldStart / ScratchHoldStart /
// ScratchCriticalHoldStart (instantaneous).
bool is_hold_head_note(const NotationNote& note) noexcept;

// Rewrite legacy ScratchHold auto-heads (Normal / Critical / BlueTap) to
// ScratchHoldStart / ScratchCriticalHoldStart. Returns how many notes changed.
int repair_legacy_hold_heads(ChartDocument& doc);

// Auto-paired head at the hold body's start (same tick, overlapping lanes), if any.
std::optional<NotationNote> paired_hold_head_for(const ChartDocument& doc,
                                                 const NotationNote& hold);
// Hold body paired with an instantaneous head at the same start, if any.
std::optional<NotationNote> paired_hold_body_for(const ChartDocument& doc,
                                                 const NotationNote& head);

// HoldEighth notes that sit on the same lanes inside (start, end) of `hold`.
std::vector<NotationNote> hold_eighths_for(const ChartDocument& doc, const NotationNote& hold);

// HoldEighth + visible mid-stars (Sound / SoundPurple) attached to a hold body.
// Does NOT include the paired hold head — heads sync on edit but delete independently.
std::vector<NotationNote> hold_attached_notes_for(const ChartDocument& doc,
                                                  const NotationNote& hold);

// Parent hold body for a mid-star / HoldEighth, if any.
std::optional<NotationNote> parent_hold_for(const ChartDocument& doc, const NotationNote& note);

// Chained ScratchHold neighbors: next starts at this end, has no own head, and
// prev's end (JumpScratch) covers the union of both bodies. When looking
// up prev, the previous segment is allowed to have a head.
std::optional<NotationNote> chained_next_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body);
std::optional<NotationNote> chained_prev_scratch_hold(const ChartDocument& doc,
                                                      const NotationNote& body);

// Drop Sound / SoundPurple mid-stars that left the hold's open time span / lanes.
bool prune_hold_mid_stars(ChartDocument& doc, const NotationNote& hold);

// Returns copies shifted so the earliest source note starts at snapped anchor_tick.
std::vector<NotationNote> paste_notes_aligned(const std::vector<NotationNote>& clipboard,
                                              int32_t anchor_tick,
                                              const EditGridConfig& grid);

}  // namespace wds::chart_editor

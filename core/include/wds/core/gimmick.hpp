#pragma once

#include <wds/core/notation.hpp>
#include <wds/core/types.hpp>

#include <utility>

namespace wds::chart_editor {

// Mirrors GameConst.SplitLaneType
enum class SplitLaneType : int32_t {
  BothEnds = 1,
  Full = 3,
  Light = 5,
  Ignore = 7,
};

// Port of Sirius.GimmickTypeExtensions (libil2cpp.so.c)
bool is_split_lane_gimmick(GimmickType gimmick) noexcept;
int32_t get_split_count(GimmickType gimmick) noexcept;
SplitLaneType get_split_lane_type(GimmickType gimmick) noexcept;

bool is_jump_scratch(GimmickType gimmick) noexcept;
bool is_one_direction(GimmickType gimmick) noexcept;

// Official SplitEffects/{id} fadeIn grows LineHight.localScale.y 0→1 (shared clip).
// Initialize does not reset LineHight rotation. Prefabs whose LineHight is
// rotated 180° about Z grow from the tip (preview percent 0); identity grows
// from the judge line (percent 1). Client 1.96.0 is frozen — these IDs are the
// complete z=180 set. Not gimmickType, not scratch_length % 2.
bool split_fade_grows_from_tip(int32_t scratch_length) noexcept;

// Official Line[i] index for a left-to-right world slot (0 = leftmost).
// LineHight z=180 flips world X, so world_index maps to split_count - world_index.
int32_t split_color_slot(int32_t scratch_length, int32_t split_count,
                         int32_t world_index) noexcept;

// Official FlickNoteObject / CSV rules (game memory, not editor notes):
//   None            → both arrows (GimmickValue ignored)
//   OneDirection    → 0 = left, 1 = right
//   JumpScratch     → signed span; sign selects the side; laneCount = abs(GV)
// Editor-authored Flick stays None + 0/±width. official_chart maps official
// OneDirection 0/1 ↔ that encoding on import/export; old None+nonzero is kept.
bool official_scratch_is_directional(GimmickType gimmick) noexcept;
int32_t official_scratch_arrow_side_sign(GimmickType gimmick, int32_t gimmick_value) noexcept;
int32_t official_scratch_arrow_lane_count(const NotationNote& note) noexcept;

// Editor-internal Flick direction: 0 both, <0 left, >0 right, stored as ±width.
int32_t encode_flick_scratch_length(int32_t direction, int32_t width) noexcept;
int32_t official_flick_scratch_length(int32_t scratch_length, int32_t width) noexcept;
void sync_flick_scratch_length_for_width(NotationNote& note, int32_t previous_width) noexcept;

// Sirius ScratchHoldEnd / JumpScratch end span from scratchLength (signed):
//   sl == 0 → [lane, endLane] (bidirectional arrows)
//   sl > 0  → [lane, lane+sl-1] (right arrows)
//   sl < 0  → [endLane+sl+1, endLane] (left arrows)
std::pair<int32_t, int32_t> get_scratch_end_lane_range(const NotationNote& note) noexcept;

// JumpScratch gimmick: same span formula; otherwise returns the body lane range.
std::pair<int32_t, int32_t> get_jump_scratch_lane_range(const NotationNote& note) noexcept;

// Single entry for edit draw + preview snapshot: hold-chain body uses
// scratch_length end span (regular terminal may still be None/OneDirection);
// other notes use body [lane, endLane]. Returns {lane, width}.
std::pair<int32_t, int32_t> resolve_end_lane_span(const NotationNote& note) noexcept;

// Inclusive occupied [lane, width] for playfield bounds: body union hold-chain
// end-cap cover. Other notes use the body span only.
std::pair<int32_t, int32_t> occupied_lane_span(const NotationNote& note) noexcept;

// Encode ScratchHold end lanes into scratch_length. End is clamped to fully cover
// the body. Equal span → 0 (call apply_scratch_chain_joint_direction for joint
// 0 / ±width arrows); one-sided extension → signed span. Both-sides-wider-than-body
// is NOT supported by Sirius (falls back to the larger side).
void set_scratch_hold_end_lanes(NotationNote& note, int32_t end_left, int32_t end_right) noexcept;

// True when [cover_left, cover_right] (after forcing body cover) extends the body on
// at most one side — i.e. Sirius scratchLength can encode the exact cover.
bool scratch_hold_end_cover_representable(const NotationNote& body, int32_t cover_left,
                                          int32_t cover_right) noexcept;

// Snap a chained next ScratchHold's left lane so the JumpScratch cover of `prev_body`
// stays Sirius-representable. Lanes that would extend both sides of `prev_body` form
// an open illegal interval; that interval is split at its midpoint and desired positions
// adsorb to the nearest legal lane on the left / right. `desired_lane` may be fractional
// (pointer-derived left edge). Result is clamped to [0, lane_count - next_width].
int32_t snap_scratch_chain_next_lane(const NotationNote& prev_body, int32_t next_width,
                                     float desired_lane, int32_t lane_count) noexcept;

// Snap a chained ScratchHold *segment* left-lane while dragging it horizontally.
// `prev` / `next` are the time-abutting neighbors (omit `next` when this is the
// chain terminal — its JumpScratch span stays encoded on `body` and must remain
// in playfield). Illegal lanes are those where a joint JumpScratch would extend
// both sides of its owner body. Among legal lanes, pick nearest to `desired_lane`;
// ties go to the lane closer to `body.lane`, then the lower lane.
int32_t snap_scratch_hold_segment_lane(const NotationNote* prev, const NotationNote& body,
                                       const NotationNote* next, float desired_lane,
                                       int32_t lane_count) noexcept;

// True when `prev`'s current tail (scratch_length span) is exactly the union of
// both bodies. Same-family / time abutment are not checked here.
bool hold_chain_lanes_connected(const NotationNote& prev, const NotationNote& next) noexcept;

// Set `prev`'s JumpScratch to the exact union of `prev` and `next` bodies, then
// re-encode joint direction. Caller must pass a Sirius-representable pair.
void sync_scratch_chain_joint(NotationNote& prev, const NotationNote& next) noexcept;

// Official gimmick write-back after a chain joint (call after sync_scratch_chain_joint).
// Purple: leave gimmick as-is (export promotes None + nonzero sl → JumpScratch).
// Regular: tail ≠ body → JumpScratch + signed span; tail == body → OneDirection, 0.
void apply_hold_chain_gimmick(NotationNote& prev) noexcept;

// Chain-joint JumpScratch direction from adjacent body edges (not the terminal end-cap).
// Each side: next more-left → -1, same → 0, more-right → +1. Sum of left+right scores:
//   <0 left,  ==0 bidirectional,  >0 right.
int32_t scratch_chain_joint_direction_score(const NotationNote& prev_body,
                                            const NotationNote& next_body) noexcept;

// Re-encode `prev` end-cap direction for a non-terminal joint using the score above.
// Preserves a one-sided cover span; when cover equals the body, uses ±width / 0.
void apply_scratch_chain_joint_direction(NotationNote& prev,
                                         const NotationNote& next_body) noexcept;

}  // namespace wds::chart_editor

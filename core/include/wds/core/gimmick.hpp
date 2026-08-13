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

// Sirius ScratchHoldEnd / JumpScratch end span from scratchLength (signed):
//   sl == 0 → [lane, endLane] (bidirectional arrows)
//   sl > 0  → [lane, lane+sl-1] (right arrows)
//   sl < 0  → [endLane+sl+1, endLane] (left arrows)
std::pair<int32_t, int32_t> get_scratch_end_lane_range(const NotationNote& note) noexcept;

// JumpScratch gimmick: same span formula; otherwise returns the body lane range.
std::pair<int32_t, int32_t> get_jump_scratch_lane_range(const NotationNote& note) noexcept;

// Single entry for edit draw + preview snapshot: ScratchHold body end span, else
// JumpScratch gimmick span, else body [lane, endLane]. Returns {lane, width}.
std::pair<int32_t, int32_t> resolve_end_lane_span(const NotationNote& note) noexcept;

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

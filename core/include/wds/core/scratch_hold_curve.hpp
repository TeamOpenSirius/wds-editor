#pragma once

#include <wds/core/easing.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace wds::chart_editor {

struct ScratchHoldCurveRequest {
  int32_t start_tick = 0;
  int32_t end_tick = 0;  // mouse end tick; last segment tail target
  double start_center = 0.0;  // first-hold placement target (origin)
  double end_center = 0.0;    // last-hold placement target (mouse); snapped to hold top
  int32_t width = 1;
  int32_t lane_count = 12;
  int32_t subdivisions_per_beat = 4;
  EasingAlgorithm algorithm = EasingAlgorithm::Linear;
  EasingDirection direction = EasingDirection::In;
  double parameter = 0.0;
};

// Start tick + meter-aware subdivision ticks + mouse end tick. Never emits a
// tick after end_tick. Empty/reversed/zero-length ranges return {}.
std::vector<int32_t> scratch_hold_curve_boundaries(int32_t start_tick, int32_t end_tick,
                                                   const MusicTiming& timing,
                                                   int32_t subdivisions_per_beat);

// Eased sample between note centers, converted to a clamped integer left lane.
int32_t scratch_hold_curve_left_lane(double t, double start_center, double end_center,
                                     int32_t width, int32_t lane_count,
                                     EasingAlgorithm algorithm, EasingDirection direction,
                                     double parameter) noexcept;

// One ScratchHold body per adjacent boundary pair. Each body lane is sampled at
// visual top `end_tick`. Normalization is first-hold top center → last-hold top
// center (not the raw mouse). A single segment uses t = 1. Interior joints use
// sync_scratch_chain_joint; the terminal cap stays equal-width (scratch_length = 0).
std::vector<NotationNote> generate_scratch_hold_curve(const ScratchHoldCurveRequest& request,
                                                      const MusicTiming& timing);

inline double scratch_hold_lane_center(int32_t lane, int32_t width) noexcept {
  const int32_t w = width < 1 ? 1 : width;
  return static_cast<double>(lane) + 0.5 * static_cast<double>(w - 1);
}

// One atomic SetNotesCommand payload: generated bodies, optional auto head,
// previous-terminal end-cap join, and HoldEighth recomputation. Empty/invalid
// generated bodies yield nullopt (caller must not commit).
struct ScratchHoldCurveCommitInput {
  std::vector<NotationNote> notes;
  std::vector<NotationNote> generated_bodies;
  int32_t previous_body_id = -1;
  NotationNote previous_body_original{};
  bool include_head = true;
  int32_t ticks_per_quarter = 480;
  int32_t next_id = 0;
};

struct ScratchHoldCurveCommit {
  std::vector<NotationNote> before;
  std::vector<NotationNote> after;
};

std::optional<ScratchHoldCurveCommit> build_scratch_hold_curve_commit(
    const ScratchHoldCurveCommitInput& input);

}  // namespace wds::chart_editor

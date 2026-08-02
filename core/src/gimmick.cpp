#include <wds/core/gimmick.hpp>

#include <algorithm>
#include <cstdint>

namespace wds::chart_editor {

namespace {

constexpr int32_t kGimmickSplitLaneMin = 11;
constexpr int32_t kGimmickSplitLaneMaxExclusive = 11 + 0x48;

}  // namespace

bool is_split_lane_gimmick(GimmickType gimmick) noexcept {
  // Port of unsigned range check: (uint)(value - min) < span.
  // Must use unsigned wrap so values below min (e.g. None=0) are excluded.
  const int32_t value = static_cast<int32_t>(gimmick);
  const auto u = static_cast<uint32_t>(value - kGimmickSplitLaneMin);
  return u < static_cast<uint32_t>(kGimmickSplitLaneMaxExclusive - kGimmickSplitLaneMin);
}

int32_t get_split_count(GimmickType gimmick) noexcept {
  const int32_t value = static_cast<int32_t>(gimmick);
  return (value + 10) % 20;
}

SplitLaneType get_split_lane_type(GimmickType gimmick) noexcept {
  const int32_t value = static_cast<int32_t>(gimmick);
  const int32_t decade = value / 10;
  return static_cast<SplitLaneType>(decade + (decade & 1) - 1);
}

bool is_jump_scratch(GimmickType gimmick) noexcept {
  return gimmick == GimmickType::JumpScratch;
}

bool is_one_direction(GimmickType gimmick) noexcept {
  return gimmick == GimmickType::OneDirection;
}

std::pair<int32_t, int32_t> get_scratch_end_lane_range(const NotationNote& note) noexcept {
  // Official scratchLength is a signed span (Sirius ScratchHoldEnd / JumpScratch).
  // scratchLane   = sl >= 0 ? lane : endLane + sl + 1
  // scratchEnLane = sl <= 0 ? endLane : lane + sl - 1
  const int32_t sl = note.scratch_length;
  const int32_t end = note.end_lane();
  if (sl == 0) {
    return {note.lane, end};
  }
  if (sl > 0) {
    return {note.lane, note.lane + sl - 1};
  }
  return {end + sl + 1, end};
}

std::pair<int32_t, int32_t> get_jump_scratch_lane_range(const NotationNote& note) noexcept {
  if (!is_jump_scratch(note.gimmick_type)) {
    return {note.lane, note.end_lane()};
  }
  return get_scratch_end_lane_range(note);
}

void set_scratch_hold_end_lanes(NotationNote& note, int32_t end_left, int32_t end_right) noexcept {
  // Tail must fully cover the body.
  // Sirius ScratchHoldEnd scratchLength cannot encode an end that extends BOTH
  // left and right of the body at once (signed span anchors to one body edge).
  end_left = std::min(end_left, note.lane);
  end_right = std::max(end_right, note.end_lane());
  if (end_right < end_left) {
    end_right = end_left;
  }

  const bool ext_left = end_left < note.lane;
  const bool ext_right = end_right > note.end_lane();
  if (!ext_left && !ext_right) {
    // Equal to body → bidirectional (width edits recalculate direction).
    note.scratch_length = 0;
    return;
  }
  if (ext_right && !ext_left) {
    note.scratch_length = end_right - note.lane + 1;
    return;
  }
  if (ext_left && !ext_right) {
    note.scratch_length = end_left - note.end_lane() - 1;
    return;
  }
  // Both sides are not representable; keep the larger one-sided cover.
  const int32_t right_span = end_right - note.lane + 1;
  const int32_t left_span = note.end_lane() - end_left + 1;
  if (right_span >= left_span) {
    note.scratch_length = right_span;
  } else {
    note.scratch_length = end_left - note.end_lane() - 1;
  }
}

bool scratch_hold_end_cover_representable(const NotationNote& body, int32_t cover_left,
                                          int32_t cover_right) noexcept {
  cover_left = std::min(cover_left, body.lane);
  cover_right = std::max(cover_right, body.end_lane());
  if (cover_right < cover_left) return false;
  const bool ext_left = cover_left < body.lane;
  const bool ext_right = cover_right > body.end_lane();
  return !(ext_left && ext_right);
}

int32_t scratch_chain_joint_direction_score(const NotationNote& prev_body,
                                            const NotationNote& next_body) noexcept {
  int32_t score = 0;
  if (next_body.lane < prev_body.lane) {
    score -= 1;
  } else if (next_body.lane > prev_body.lane) {
    score += 1;
  }
  if (next_body.end_lane() < prev_body.end_lane()) {
    score -= 1;
  } else if (next_body.end_lane() > prev_body.end_lane()) {
    score += 1;
  }
  return score;
}

void apply_scratch_chain_joint_direction(NotationNote& prev,
                                         const NotationNote& next_body) noexcept {
  const int32_t score = scratch_chain_joint_direction_score(prev, next_body);
  const auto [cover_l, cover_r] = get_scratch_end_lane_range(prev);
  const bool ext_left = cover_l < prev.lane;
  const bool ext_right = cover_r > prev.end_lane();

  // Terminal-style one-sided spans keep their geometric sign (required for cover).
  if (ext_left && !ext_right) {
    prev.scratch_length = cover_l - prev.end_lane() - 1;
    return;
  }
  if (ext_right && !ext_left) {
    prev.scratch_length = cover_r - prev.lane + 1;
    return;
  }
  if (ext_left && ext_right) {
    // Unrepresentable exact cover: pick a side from the joint score (else larger).
    const int32_t right_span = cover_r - prev.lane + 1;
    const int32_t left_span = prev.end_lane() - cover_l + 1;
    if (score < 0 || (score == 0 && left_span > right_span)) {
      prev.scratch_length = cover_l - prev.end_lane() - 1;
    } else {
      prev.scratch_length = right_span;
    }
    return;
  }

  // Cover equals body: direction is ±width / 0 (span unchanged, arrows change).
  if (score < 0) {
    prev.scratch_length = -prev.width;
  } else if (score > 0) {
    prev.scratch_length = prev.width;
  } else {
    prev.scratch_length = 0;
  }
}

}  // namespace wds::chart_editor

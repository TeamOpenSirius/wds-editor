#include <wds/core/scratch_hold_curve.hpp>

#include <wds/core/edit_grid.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {

std::vector<int32_t> scratch_hold_curve_boundaries(int32_t start_tick, int32_t end_tick,
                                                   const MusicTiming& timing,
                                                   int32_t subdivisions_per_beat) {
  if (end_tick <= start_tick) return {};
  MusicTiming normalized = timing;
  normalize_timing_points(normalized);
  const auto subdivs =
      subdivision_ticks_in_range(start_tick, end_tick, normalized, subdivisions_per_beat);
  std::vector<int32_t> bounds;
  bounds.push_back(start_tick);
  for (const int32_t tick : subdivs) {
    if (tick > start_tick && tick < end_tick) {
      bounds.push_back(tick);
    }
  }
  bounds.push_back(end_tick);
  return bounds;
}

int32_t scratch_hold_curve_left_lane(double t, double start_center, double end_center,
                                     int32_t width, int32_t lane_count,
                                     EasingAlgorithm algorithm, EasingDirection direction,
                                     double parameter) noexcept {
  width = std::max(1, width);
  if (!std::isfinite(start_center)) start_center = 0.0;
  if (!std::isfinite(end_center)) end_center = 0.0;
  const double eased = apply_easing(t, algorithm, direction, parameter);
  const double center = start_center + (end_center - start_center) * eased;
  const double left = center - 0.5 * static_cast<double>(width - 1);
  const int32_t lane =
      std::isfinite(left) ? static_cast<int32_t>(std::lround(left)) : 0;
  return clamp_lane_for_width(lane, width, lane_count);
}

std::vector<NotationNote> generate_scratch_hold_curve(const ScratchHoldCurveRequest& request,
                                                      const MusicTiming& timing) {
  const auto bounds = scratch_hold_curve_boundaries(
      request.start_tick, request.end_tick, timing, request.subdivisions_per_beat);
  if (bounds.size() < 2) return {};

  const int32_t width = std::max(1, request.width);
  const int32_t first_top = bounds[1];
  const int32_t last_top = bounds.back();
  const int32_t first_lane = scratch_hold_curve_left_lane(
      0.0, request.start_center, request.end_center, width, request.lane_count,
      request.algorithm, request.direction, request.parameter);
  const int32_t last_lane = scratch_hold_curve_left_lane(
      1.0, request.start_center, request.end_center, width, request.lane_count,
      request.algorithm, request.direction, request.parameter);
  const double first_top_center = scratch_hold_lane_center(first_lane, width);
  const double last_top_center = scratch_hold_lane_center(last_lane, width);
  std::vector<NotationNote> notes;
  notes.reserve(bounds.size() - 1);
  for (size_t i = 0; i + 1 < bounds.size(); ++i) {
    const int32_t sample_tick = bounds[i + 1];
    double t = 1.0;
    if (bounds.size() > 2 && last_top != first_top) {
      t = static_cast<double>(sample_tick - first_top) /
          static_cast<double>(last_top - first_top);
    }
    NotationNote note;
    note.note_type = request.note_type;
    note.start_tick = bounds[i];
    note.end_tick = bounds[i + 1];
    note.lane = scratch_hold_curve_left_lane(
        t, first_top_center, last_top_center, width, request.lane_count,
        request.algorithm, request.direction, request.parameter);
    note.width = width;
    note.scratch_length = 0;
    notes.push_back(note);
  }
  for (size_t i = 0; i + 1 < notes.size(); ++i) {
    sync_scratch_chain_joint(notes[i], notes[i + 1]);
  }
  return notes;
}

std::optional<ScratchHoldCurveCommit> build_scratch_hold_curve_commit(
    const ScratchHoldCurveCommitInput& input) {
  if (input.generated_bodies.empty()) return std::nullopt;

  ScratchHoldCurveCommit commit;
  commit.before = input.notes;
  if (input.previous_body_id >= 0) {
    for (auto& note : commit.before) {
      if (note.id != input.previous_body_id) continue;
      note.lane = input.previous_body_original.lane;
      note.width = input.previous_body_original.width;
      note.scratch_length = input.previous_body_original.scratch_length;
      note.gimmick_type = input.previous_body_original.gimmick_type;
      break;
    }
  }

  commit.after = commit.before;
  std::vector<NotationNote> holds_for_eighths;
  if (input.previous_body_id >= 0) {
    for (auto& note : commit.after) {
      if (note.id != input.previous_body_id) continue;
      sync_scratch_chain_joint(note, input.generated_bodies.front());
      holds_for_eighths.push_back(note);
      break;
    }
  }

  if (input.include_head) {
    ChartDocument occupancy;
    occupancy.set_notes(commit.after);
    if (auto head = make_auto_hold_head(occupancy, input.generated_bodies.front())) {
      commit.after.push_back(*head);
    }
  }

  for (auto body : input.generated_bodies) {
    body.id = kAutoNoteId;
    commit.after.push_back(body);
    holds_for_eighths.push_back(body);
  }

  for (const auto& hold : holds_for_eighths) {
    commit.after =
        with_recomputed_hold_eighths(std::move(commit.after), hold, input.ticks_per_quarter);
  }

  int32_t next_id = input.next_id;
  for (const auto& note : commit.after) {
    if (note.id >= 0) next_id = std::max(next_id, note.id + 1);
  }
  for (auto& note : commit.after) {
    if (note.id >= 0) continue;
    note.id = next_id++;
  }
  return commit;
}

}  // namespace wds::chart_editor

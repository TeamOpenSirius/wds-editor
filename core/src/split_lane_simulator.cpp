#include <wds/core/split_lane_simulator.hpp>

#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {

namespace {

const NotationNote* find_note_by_id(const std::vector<NotationNote>& notes, int32_t id) {
  for (const auto& note : notes) {
    if (note.id == id) {
      return &note;
    }
  }
  return nullptr;
}

}  // namespace

SplitLaneSimulator::SplitLaneSimulator(PreviewConfig config) : config_(config) {}

void SplitLaneSimulator::set_config(PreviewConfig config) { config_ = config; }

bool SplitLaneSimulator::is_split_active(const NotationNote& note, const MusicTiming& timing,
                                         int64_t preview_time_ms) const {
  if (!is_split_lane_gimmick(note.gimmick_type)) {
    return false;
  }

  const int64_t start_ms = note.start_ms(timing);
  const int64_t end_ms = std::max(start_ms, note.end_ms(timing));
  const int64_t appear_ms = split_fade_sec_to_ms(config_.split_line_animation_start_sec);
  const int64_t expire_ms = official_split_expire_ms(start_ms, end_ms);
  const int64_t hide_tail_ms = official_split_hide_tail_ms();

  // OpenWDS: fadeIn at start−Show; Tick hides at EndTickCount; Destroy 0.5s.
  return preview_time_ms >= start_ms - appear_ms && preview_time_ms < expire_ms + hide_tail_ms;
}

void SplitLaneSimulator::fill_instance(PreviewSplitLaneInstance& out, const NotationNote& note,
                                       const MusicTiming& timing,
                                       int64_t preview_time_ms) const {
  const int32_t split_count = get_split_count(note.gimmick_type);
  const int64_t start_ms = note.start_ms(timing);
  const int64_t end_ms = std::max(start_ms, note.end_ms(timing));
  out.apply_identity(note.id, split_count, get_split_lane_type(note.gimmick_type),
                     note.scratch_length, start_ms, end_ms);
  out.apply_state(/*show=*/true, preview_time_ms < end_ms,
                  config_.lane_count + split_count);

  const int64_t appear_ms = split_fade_sec_to_ms(config_.split_line_animation_start_sec);
  const int64_t fade_out_ms = split_fade_sec_to_ms(config_.split_line_animation_end_sec);
  const int64_t expire_ms = official_split_expire_ms(start_ms, end_ms);

  if (preview_time_ms < start_ms) {
    // Pure function of chart preview_time_ms. OpenWDS does not ease Line X
    // with Time.deltaTime (SplitEffectController.ApplyOriginalSplitPositions
    // writes the SetLineX table once). Only LineHight.localScale.y follows
    // the 1.0s fadeIn clip + 1/60 hop (official_split_fade_scale_at_elapsed).
    // dump-pending-v1: board lossyScale.y == LineHight.
    const float elapsed_sec =
        static_cast<float>(preview_time_ms - (start_ms - appear_ms)) / 1000.0f;
    const float appear_sec = static_cast<float>(appear_ms) / 1000.0f;
    const float t = std::clamp((elapsed_sec - (1.0f / 60.0f)) / appear_sec, 0.0f, 1.0f);
    if (split_fade_grows_from_tip(note.scratch_length)) {
      const float end = official_split_fade_in_from_tip_end(t);
      out.apply_animation(/*line_alpha=*/1.0f, /*percent_start=*/0.0f, /*percent_end=*/end,
                          /*cover_alpha=*/std::clamp(1.0f - end, 0.0f, 1.0f),
                          /*anim_phase=*/0);
    } else {
      // Near end is Initialize origin (percent > 1), not the 16:9 floor.
      const float start = official_split_fade_in_from_judge_start(t);
      out.apply_animation(/*line_alpha=*/1.0f, /*percent_start=*/start,
                          /*percent_end=*/official_split_identity_origin_percent(),
                          /*cover_alpha=*/std::clamp(start, 0.0f, 1.0f), /*anim_phase=*/0);
    }
  } else if (preview_time_ms < expire_ms) {
    if (split_fade_grows_from_tip(note.scratch_length)) {
      out.apply_animation(/*line_alpha=*/1.0f, /*percent_start=*/0.0f,
                          /*percent_end=*/official_split_z180_near_percent(),
                          /*cover_alpha=*/0.0f, /*anim_phase=*/1);
    } else {
      out.apply_animation(/*line_alpha=*/1.0f,
                          /*percent_start=*/official_split_identity_far_percent(),
                          /*percent_end=*/official_split_identity_origin_percent(),
                          /*cover_alpha=*/0.0f, /*anim_phase=*/1);
    }
  } else {
    // Runtime fadeOut clip is 0.3s (1−smoothstep). Destroy at +0.5s.
    const float t = std::clamp(
        static_cast<float>(preview_time_ms - expire_ms) / static_cast<float>(fade_out_ms),
        0.0f, 1.0f);
    const float a = official_split_fade_out_alpha(t);
    if (split_fade_grows_from_tip(note.scratch_length)) {
      out.apply_animation(/*line_alpha=*/a, /*percent_start=*/0.0f,
                          /*percent_end=*/official_split_z180_near_percent(),
                          /*cover_alpha=*/1.0f - a, /*anim_phase=*/2);
    } else {
      out.apply_animation(/*line_alpha=*/a,
                          /*percent_start=*/official_split_identity_far_percent(),
                          /*percent_end=*/official_split_identity_origin_percent(),
                          /*cover_alpha=*/1.0f - a, /*anim_phase=*/2);
    }
  }
}

void SplitLaneSimulator::build_active_splits(
    const std::vector<NotationNote>& notes, const std::vector<int32_t>& split_candidate_ids,
    const MusicTiming& timing, int64_t preview_time_ms,
    std::vector<PreviewSplitLaneInstance>& out) const {
  out.clear();

  std::vector<const NotationNote*> active_notes;
  active_notes.reserve(split_candidate_ids.size());

  for (int32_t id : split_candidate_ids) {
    const NotationNote* note = find_note_by_id(notes, id);
    if (note != nullptr && is_split_active(*note, timing, preview_time_ms)) {
      active_notes.push_back(note);
    }
  }

  std::sort(active_notes.begin(), active_notes.end(),
            [&](const NotationNote* a, const NotationNote* b) {
              return a->start_ms(timing) < b->start_ms(timing);
            });

  for (const NotationNote* note : active_notes) {
    PreviewSplitLaneInstance instance;
    fill_instance(instance, *note, timing, preview_time_ms);
    out.push_back(instance);
  }
}

}  // namespace wds::chart_editor

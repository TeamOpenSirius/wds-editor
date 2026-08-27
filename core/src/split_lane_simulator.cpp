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
  const int64_t disappear_ms = split_fade_sec_to_ms(config_.split_line_animation_end_sec);

  // Official: fadeIn starts at start−Show; fadeOut starts at EndMilliseconds.
  return preview_time_ms >= start_ms - appear_ms && preview_time_ms < end_ms + disappear_ms;
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
  const int64_t disappear_ms = split_fade_sec_to_ms(config_.split_line_animation_end_sec);

  if (preview_time_ms < start_ms) {
    // Official clip: LineHight.localScale.y cubic. Screen percent is that
    // local-Y tip projected through LaneGroup Rx=60° + perspective FOV 50.
    const float t = std::clamp(
        static_cast<float>(preview_time_ms - (start_ms - appear_ms)) /
            static_cast<float>(appear_ms),
        0.0f, 1.0f);
    if (split_fade_grows_from_tip(note.scratch_length)) {
      const float end = official_split_fade_in_from_tip_end(t);
      out.apply_animation(/*line_alpha=*/1.0f, /*percent_start=*/0.0f, /*percent_end=*/end,
                          /*cover_alpha=*/1.0f - end, /*anim_phase=*/0);
    } else {
      const float start = official_split_fade_in_from_judge_start(t);
      out.apply_animation(/*line_alpha=*/1.0f, /*percent_start=*/start, /*percent_end=*/1.0f,
                          /*cover_alpha=*/start, /*anim_phase=*/0);
    }
  } else if (preview_time_ms <= end_ms) {
    out.apply_animation(/*line_alpha=*/1.0f, /*percent_start=*/0.0f, /*percent_end=*/1.0f,
                        /*cover_alpha=*/0.0f, /*anim_phase=*/1);
  } else {
    // Official SplitEffect_fadeOut_anim: full geometry, m_Color.a 1→0 over 0.3s
    // (1 − smoothstep). Hide starts at EndMilliseconds.
    const float t = std::clamp(
        static_cast<float>(preview_time_ms - end_ms) / static_cast<float>(disappear_ms),
        0.0f, 1.0f);
    const float a = official_split_fade_out_alpha(t);
    out.apply_animation(/*line_alpha=*/a, /*percent_start=*/0.0f, /*percent_end=*/1.0f,
                        /*cover_alpha=*/1.0f - a, /*anim_phase=*/2);
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

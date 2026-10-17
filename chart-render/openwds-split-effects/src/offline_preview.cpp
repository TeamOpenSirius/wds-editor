#include <wds/chart_render/openwds_split_effects/offline_preview.hpp>

#include <wds/core/official_playfield.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_render::openwds_split_effects {
namespace {
constexpr float kNoteWidthPerLane = 0.85f;
constexpr float kLaneBorderWidth = 0.075f;
constexpr int kLanes[] = {1, 3, 5, 7, 9, 2, 6, 10};
constexpr int kLaneSpans[] = {1, 2, 1, 1, 2, 1, 2, 1};
float repeat(float value, float length) noexcept {
  return length <= 0.0f ? 0.0f : value - std::floor(value / length) * length;
}
}

void OfflineRhythmPreview::configure(const std::vector<unity::Transform*>& note_transforms) {
  notes_.clear();
  notes_.reserve(note_transforms.size());
  for (std::size_t index = 0; index < note_transforms.size(); ++index) {
    notes_.push_back({note_transforms[index], kLanes[index % 8], kLaneSpans[index % 8],
                      1.5f + static_cast<float>(index) * 0.75f});
  }
  evaluate(0.0f);
}

void OfflineRhythmPreview::evaluate(float elapsed_seconds) {
  const float loop_time = repeat(elapsed_seconds, loop_duration_);
  active_note_count_ = 0;
  for (const auto& note : notes_) {
    if (note.transform == nullptr) continue;
    const float time_until_hit = repeat(note.hit_time - loop_time, loop_duration_);
    const bool visible = time_until_hit <= travel_duration_ ||
                         time_until_hit >= loop_duration_ - despawn_delay_;
    if (note.transform->gameObject != nullptr) note.transform->gameObject->SetActive(visible);
    if (!visible) continue;
    ++active_note_count_;
    const int lane_span = std::max(1, note.lane_span);
    const float note_width =
        wds::chart_editor::official_note_width(lane_span, kNoteWidthPerLane, kLaneBorderWidth);
    const float position_y = time_until_hit <= travel_duration_
                                 ? wds::chart_editor::official_note_local_y(
                                       static_cast<std::int64_t>(std::lround(time_until_hit * 1000.0f)),
                                       0, preview_speed_rate_, 0.0, preview_position_pow3_rate_,
                                       preview_position_pow1_rate_)
                                 : 0.0f;
    note.transform->localPosition = {
        wds::chart_editor::official_note_position_x(note.lane, note_width,
                                                    kNoteWidthPerLane, kLaneBorderWidth),
        position_y, 0.0f};
  }
}

}  // namespace wds::chart_render::openwds_split_effects

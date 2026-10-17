#pragma once

#include <wds/chart_render/unity_api.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wds::chart_render::openwds_split_effects {

struct PreviewNoteState {
  unity::Transform* transform = nullptr;
  int lane = 0;
  int lane_span = 1;
  float hit_time = 0.0f;
};

// Open port of Assets/OpenWDS/Runtime/OfflineRhythmPreview.cs. This facade
// owns only the preview note scheduling/position logic; Unity scene, audio and
// renderer behavior remain outside the contract.
class OfflineRhythmPreview final {
 public:
  int configured_note_count() const noexcept { return static_cast<int>(notes_.size()); }
  int active_note_count() const noexcept { return active_note_count_; }
  float loop_duration() const noexcept { return loop_duration_; }
  void set_loop_duration(float value) noexcept { loop_duration_ = value; }
  void set_travel_duration(float value) noexcept { travel_duration_ = value; }
  void set_preview_position_pow3_rate(double value) noexcept { preview_position_pow3_rate_ = value; }
  void set_preview_position_pow1_rate(double value) noexcept { preview_position_pow1_rate_ = value; }
  void set_preview_speed_rate(float value) noexcept { preview_speed_rate_ = value; }
  void set_despawn_delay(float value) noexcept { despawn_delay_ = value; }
  void configure(const std::vector<unity::Transform*>& note_transforms);
  void evaluate(float elapsed_seconds);
  void on_enable(float now_seconds) noexcept { started_at_ = now_seconds; }
  void update(float now_seconds) { evaluate(now_seconds - started_at_); }
  const std::vector<PreviewNoteState>& notes() const noexcept { return notes_; }

 private:
  float loop_duration_ = 8.0f;
  float travel_duration_ = 2.5f;
  double preview_position_pow3_rate_ = 0.192;
  double preview_position_pow1_rate_ = 1.2;
  float preview_speed_rate_ = 1.0f;
  float despawn_delay_ = 0.35f;
  std::vector<PreviewNoteState> notes_;
  float started_at_ = 0.0f;
  int active_note_count_ = 0;
};

}  // namespace wds::chart_render::openwds_split_effects

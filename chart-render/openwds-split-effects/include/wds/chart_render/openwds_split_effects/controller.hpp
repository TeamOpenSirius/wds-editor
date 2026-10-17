#pragma once

#include "resources.hpp"

#include <wds/chart_render/unity_api.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace wds::chart_render::openwds_split_effects {

struct OpenWdsLineState {
  std::shared_ptr<unity::GameObject> game_object;
  std::shared_ptr<unity::SpriteRenderer> renderer;
  std::shared_ptr<unity::ParticleSystem> particle_system;
  std::vector<std::shared_ptr<unity::GameObject>> element_objects;
  std::vector<std::shared_ptr<unity::ParticleSystem>> particle_systems;
  std::vector<std::string> source_identities;
  unity::Color source_color{};
  unity::Color color{};
  bool enabled = false;
};

// Faithful open port of the OpenWDS SplitEffectController lifecycle. The
// serialized resources remain external to this controller: each instance is
// pinned to one of the 318 bundle closures and all ParticleSystem operations
// go through the stage2 Unity-compatible API contract.
class Controller final {
 public:
  explicit Controller(std::int32_t effect_id, std::string_view resource_root = {});
  ~Controller();

  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  bool valid() const noexcept { return binding_ != nullptr; }
  std::int32_t effect_id() const noexcept { return binding_ ? binding_->id : 0; }
  const OpenWdsEffectBinding* resource() const noexcept { return binding_; }
  const LoadedEffect& loaded_resource() const noexcept { return loaded_; }
  std::vector<OpenWdsLineState>& lines() noexcept { return lines_; }
  const std::vector<OpenWdsLineState>& lines() const noexcept { return lines_; }
  unity::GameObject* game_object() noexcept { return root_.get(); }
  unity::Animator* animator() noexcept { return animator_.get(); }
  const unity::Animator* animator() const noexcept { return animator_.get(); }

  // Equivalent to Initialize in the original controller. The opacity range
  // is kept explicit to preserve the serialized controller call contract.
  bool initialize(bool split_effect_light_setting,
                  int split_effect_line_opacity_settings,
                  int min_opacity_value = 10,
                  int max_opacity_value = 100) noexcept;
  void on_fade_in(int split_count, int split_lane_type) noexcept;
  void on_fade_out() noexcept;
  void clear() noexcept;

 private:
  void set_line_x(std::size_t index, float value) noexcept;
  void apply_original_split_positions(int split_count) noexcept;

  const OpenWdsEffectBinding* binding_ = nullptr;
  LoadedEffect loaded_{};
  std::shared_ptr<unity::GameObject> root_;
  std::shared_ptr<unity::Animator> animator_;
  std::vector<OpenWdsLineState> lines_{};
  bool light_setting_ = false;
  bool initialized_ = false;
  bool active_ = false;
  int opacity_ = 100;
};

std::unique_ptr<Controller> create_controller(std::int32_t effect_id,
                                              std::string_view resource_root = {});

}  // namespace wds::chart_render::openwds_split_effects

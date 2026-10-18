#pragma once

#include "controller.hpp"

#include <wds/chart_render/unity_api.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wds::chart_render::openwds_split_effects {

// Faithful open port of OpenWDS SplitLaneAssetRuntime (pool / OnSplitLane /
// delayed Destroy). Shader remap is name-only; see compatible_shaders.hpp.
class SplitLaneAssetRuntime {
 public:
  explicit SplitLaneAssetRuntime(unity::Transform* parent,
                                 int sprit_effect_setting_type = 1,
                                 int line_opacity = 100,
                                 std::string_view resource_root = {});

  bool on_split_lane(int effect_id, int note_id, int split_count,
                     int split_lane_type, bool should_show);
  void tick(float dt) noexcept;
  void reset();
  int active_count() const noexcept;
  int missing_prefab_request_count() const noexcept;

 private:
  struct PendingDestroy {
    int note_id = 0;
    float remaining = 0.5f;
    std::unique_ptr<Controller> controller;
  };

  unity::Transform* parent_ = nullptr;
  bool light_setting_ = false;
  int line_opacity_ = 100;
  std::string resource_root_;
  std::unordered_map<int, std::unique_ptr<Controller>> active_;
  std::vector<PendingDestroy> pending_destroy_;
  std::unordered_set<int> missing_prefab_requests_;
};

}  // namespace wds::chart_render::openwds_split_effects

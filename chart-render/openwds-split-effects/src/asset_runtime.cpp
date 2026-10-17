#include <wds/chart_render/openwds_split_effects/asset_runtime.hpp>

#include <wds/chart_render/openwds_split_effects/compatible_shaders.hpp>
#include <wds/chart_render/openwds_split_effects/resources.hpp>

#include <string>
#include <utility>

namespace wds::chart_render::openwds_split_effects {
namespace {

// Unity Destroy(go) removes the object from the hierarchy. Drop the unique_ptr
// only after SetParent(nullptr) so the parent's child list is not left dangling.
void detach_and_drop(std::unique_ptr<Controller>& controller) {
  if (!controller) return;
  if (unity::GameObject* go = controller->game_object()) {
    go->SetActive(false);
    go->transform.SetParent(nullptr, false);
  }
  controller.reset();
}

}  // namespace

SplitLaneAssetRuntime::SplitLaneAssetRuntime(unity::Transform* parent,
                                             int sprit_effect_setting_type,
                                             int line_opacity,
                                             std::string_view resource_root)
    : parent_(parent),
      // OpenWDS SpritEffectSettingType.Light == 2 (misspelling kept).
      light_setting_(sprit_effect_setting_type == 2),
      // Stored raw; Controller::initialize clamps with GameSettings 10..100.
      line_opacity_(line_opacity),
      resource_root_(resource_root) {}

bool SplitLaneAssetRuntime::on_split_lane(int effect_id, int note_id,
                                          int split_count, int split_lane_type,
                                          bool should_show) {
  // SplitLaneAssetRuntime.OnSplitLane: effect id 0 is a no-op.
  if (effect_id == 0) return false;

  if (should_show) {
    if (resource_for_effect(effect_id) == nullptr) {
      // Strict lookup: unknown IDs are recorded once, never mapped nearby.
      missing_prefab_requests_.insert(effect_id);
      return false;
    }
    // C# throws in the constructor if parent is null; C++ constructs anyway
    // and fails the show path instead of crashing.
    if (parent_ == nullptr) return false;
    if (active_.find(note_id) != active_.end()) return true;

    // create_controller(effect_id) does not forward resource_root_.
    auto controller = std::make_unique<Controller>(effect_id, resource_root_);
    unity::GameObject* go = controller->game_object();
    if (!controller->valid() || go == nullptr) return false;

    // Instantiate(prefab, parent, worldPositionStays: false).
    go->transform.SetParent(parent_, false);
    go->name = "SplitEffect_" + std::to_string(effect_id) + "_" +
               std::to_string(note_id);
    apply_compatible_shaders(*go);
    // GameSettings.Minimum/MaximumSplitEffectLineOpacity.
    controller->initialize(light_setting_, line_opacity_, 10, 100);
    controller->on_fade_in(split_count, split_lane_type);
    apply_compatible_shaders(*go);
    active_[note_id] = std::move(controller);
    return true;
  }

  const auto it = active_.find(note_id);
  if (it == active_.end()) return false;
  if (it->second) it->second->on_fade_out();
  pending_destroy_.push_back(
      PendingDestroy{note_id, 0.5f, std::move(it->second)});
  active_.erase(it);
  return true;
}

void SplitLaneAssetRuntime::tick(float dt) noexcept {
  // C# UnityEngine.Object.Destroy(go, 0.5f): remaining starts at 0.5s.
  for (auto it = pending_destroy_.begin(); it != pending_destroy_.end();) {
    it->remaining -= dt;
    if (it->remaining <= 0.0f) {
      detach_and_drop(it->controller);
      it = pending_destroy_.erase(it);
    } else {
      ++it;
    }
  }
}

void SplitLaneAssetRuntime::reset() {
  // C# Reset: SetActive(false) + Destroy on each active instance immediately.
  // Pending delayed destroys are also dropped (C++ owns the delay queue).
  for (auto& entry : active_) detach_and_drop(entry.second);
  active_.clear();
  for (auto& pending : pending_destroy_) detach_and_drop(pending.controller);
  pending_destroy_.clear();
}

int SplitLaneAssetRuntime::active_count() const noexcept {
  return static_cast<int>(active_.size());
}

int SplitLaneAssetRuntime::missing_prefab_request_count() const noexcept {
  return static_cast<int>(missing_prefab_requests_.size());
}

}  // namespace wds::chart_render::openwds_split_effects

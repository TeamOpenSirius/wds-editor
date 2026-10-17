#include <wds/chart_render/openwds_split_effects/asset_runtime.hpp>
#include <wds/chart_render/openwds_split_effects/controller.hpp>
#include <wds/chart_render/openwds_split_effects/resources.hpp>

#include <cassert>
#include <cmath>
#include <string>

#ifndef WDS_TEST_REPO_ROOT
#error "WDS_TEST_REPO_ROOT must be injected by CMake (repo root)"
#endif

using namespace wds::chart_render::openwds_split_effects;
namespace unity = wds::chart_render::unity;

namespace {

bool has_initialized_line_child(const unity::Transform& effect) {
  for (int i = 0; i < effect.childCount(); ++i) {
    const unity::Transform* child = effect.GetChild(i);
    if (child && child->gameObject &&
        child->gameObject->name.rfind("_Line", 0) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  const std::string root = std::string(WDS_TEST_REPO_ROOT) + "/resources";

  unity::GameObject parent;
  SplitLaneAssetRuntime runtime(&parent.transform, 1, 100, root);

  // 1. Show 2020: one active controller, parented, initialize + fade-in ran.
  assert(runtime.on_split_lane(2020, 1, 3, 7, true));
  assert(runtime.active_count() == 1);
  const unity::Transform* effect = parent.transform.GetChild(0);
  if (effect != nullptr && effect->gameObject != nullptr) {
    assert(effect->gameObject->name == "SplitEffect_2020_1");
    assert(effect->gameObject->activeSelf);
    assert(has_initialized_line_child(*effect));
    const unity::Transform* line0 = nullptr;
    for (int i = 0; i < effect->childCount(); ++i) {
      const unity::Transform* child = effect->GetChild(i);
      if (child && child->gameObject && child->gameObject->name == "_Line1") {
        line0 = child;
        break;
      }
    }
    if (line0 != nullptr) {
      assert(std::fabs(line0->localPosition.x - (-5.55f)) < 1e-5f);
    }
  }
  // 2020 LineEffects are nonempty: the same initialize + OnFadeIn path Plays.
  {
    Controller probe(2020, root);
    assert(probe.initialize(false, 100, 10, 100));
    probe.on_fade_in(3, 7);
    bool playing = false;
    for (const auto& line : probe.lines()) {
      if (line.particle_system && line.particle_system->isPlaying()) {
        playing = true;
        break;
      }
    }
    assert(playing);
  }

  // Same note_id must not double-spawn (C++ no-op; C# would leak).
  assert(runtime.on_split_lane(2020, 1, 3, 7, true));
  assert(runtime.active_count() == 1);
  assert(parent.transform.childCount() == 1);

  // 2. Hide removes from active immediately; Destroy(go, 0.5f) is queued.
  assert(runtime.on_split_lane(2020, 1, 3, 7, false));
  assert(runtime.active_count() == 0);
  runtime.tick(0.49f);
  assert(runtime.active_count() == 0);
  assert(parent.transform.childCount() >= 1);
  runtime.tick(0.02f);
  assert(runtime.active_count() == 0);
  assert(parent.transform.childCount() == 0);

  // hide + tick(0.5) also clears the pending queue.
  assert(runtime.on_split_lane(2020, 1, 3, 7, true));
  assert(runtime.active_count() == 1);
  assert(runtime.on_split_lane(2020, 1, 3, 7, false));
  assert(runtime.active_count() == 0);
  runtime.tick(0.5f);
  assert(runtime.active_count() == 0);
  assert(parent.transform.childCount() == 0);

  // 3–4. Unknown IDs are missing (once); never mapped to a nearby real ID.
  assert(resource_for_effect(999999) == nullptr);
  assert(resource_for_effect(999998) == nullptr);
  const int active_before_missing = runtime.active_count();
  assert(!runtime.on_split_lane(999999, 2, 1, 7, true));
  assert(runtime.active_count() == active_before_missing);
  assert(runtime.missing_prefab_request_count() == 1);
  assert(!runtime.on_split_lane(999999, 2, 1, 7, true));
  assert(runtime.missing_prefab_request_count() == 1);
  assert(parent.transform.childCount() == 0);

  // effect_id 0 is a no-op in C# OnSplitLane (including hide of a live note).
  assert(!runtime.on_split_lane(0, 1, 3, 7, true));
  assert(runtime.on_split_lane(2020, 7, 3, 7, true));
  assert(!runtime.on_split_lane(0, 7, 3, 7, false));
  assert(runtime.active_count() == 1);
  runtime.reset();
  assert(runtime.active_count() == 0);

  // Hide of a note that was never shown.
  assert(!runtime.on_split_lane(2020, 99, 3, 7, false));

  // C++ parent==nullptr: construct, show fails.
  SplitLaneAssetRuntime orphan(nullptr, 1, 100, root);
  assert(!orphan.on_split_lane(2020, 3, 3, 7, true));
  assert(orphan.active_count() == 0);

  // reset() drops active immediately (and any pending delay).
  assert(runtime.on_split_lane(2020, 5, 3, 7, true));
  assert(runtime.active_count() == 1);
  runtime.reset();
  assert(runtime.active_count() == 0);
  assert(parent.transform.childCount() == 0);
  // C# Reset does not clear the missing-prefab HashSet.
  assert(runtime.missing_prefab_request_count() == 1);

  return 0;
}

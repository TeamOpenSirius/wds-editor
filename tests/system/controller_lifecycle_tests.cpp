#include <wds/chart_render/openwds_split_effects/controller.hpp>

#include <cassert>
#include <string>

#ifndef WDS_TEST_REPO_ROOT
#define WDS_TEST_REPO_ROOT "."
#endif

using namespace wds::chart_render::openwds_split_effects;

int main() {
  const std::string resources = std::string(WDS_TEST_REPO_ROOT) + "/resources";
  auto controller = create_controller(10050, resources);
  if (!controller) controller = create_controller(2020, resources);
  assert(controller && controller->valid());
  assert(controller->initialize(false, 100, 10, 100));

  assert(controller->animator() != nullptr);
  assert(controller->game_object() != nullptr);
  assert(controller->animator()->transform.parent == &controller->game_object()->transform);
  assert(controller->animator()->transform.localScale.y == 27.0f);
  assert(controller->animator()->transform.localPosition.y == -5.0f);
  assert(controller->game_object()->transform.localScale.y != 27.0f);

  assert(controller->lines().size() == controller->loaded_resource().controller_lines.size());
  const auto& lines = controller->lines();
  if (!lines.empty() && !lines.data()[0].element_objects.empty()) {
    const auto& first_line = lines.data()[0];
    const auto& element = first_line.element_objects.data()[0];
    if (element && first_line.game_object) {
      assert(element->transform.parent == &first_line.game_object->transform);
      assert(element->transform.localPosition.y == 2.5f);
    }
  }

  bool saw_manual_seed = false;
  for (const auto& line : controller->lines()) {
    for (const auto& system : line.particle_systems) {
      if (!system || system->useAutoRandomSeed()) continue;
      saw_manual_seed = true;
      assert(system->randomSeed() <= 999u);
    }
  }
  (void)saw_manual_seed;

  controller->on_fade_in(3, 7);
  controller->clear();
  for (const auto& line : controller->lines()) {
    for (const auto& system : line.particle_systems) {
      if (system) assert(!system->isPlaying());
    }
  }
  return 0;
}

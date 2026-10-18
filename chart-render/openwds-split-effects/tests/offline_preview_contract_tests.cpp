#include <wds/chart_render/openwds_split_effects/offline_preview.hpp>
#include <wds/core/official_playfield.hpp>

#include <cassert>
#include <cmath>
#include <memory>

using namespace wds::chart_render::openwds_split_effects;

int main() {
  std::vector<std::shared_ptr<wds::chart_render::unity::GameObject>> objects;
  std::vector<wds::chart_render::unity::Transform*> transforms;
  for (int i = 0; i < 8; ++i) {
    objects.push_back(std::make_shared<wds::chart_render::unity::GameObject>("note"));
    transforms.push_back(&objects.back()->transform);
  }
  OfflineRhythmPreview preview;
  preview.configure(transforms);
  assert(preview.configured_note_count() == 8);
  assert(preview.active_note_count() == 2);
  assert(objects[0]->activeSelf && objects[1]->activeSelf);
  assert(!objects[2]->activeSelf);
  preview.evaluate(2.0f);
  assert(preview.active_note_count() >= 1);
  assert(std::fabs(objects[0]->transform.localPosition.x -
                   wds::chart_editor::official_note_position_x(1,
                     wds::chart_editor::official_note_width(1, 0.85f, 0.075f), 0.85f, 0.075f)) < 1e-5f);
  preview.on_enable(10.0f);
  preview.update(10.5f);
  assert(preview.active_note_count() >= 0);
  return 0;
}

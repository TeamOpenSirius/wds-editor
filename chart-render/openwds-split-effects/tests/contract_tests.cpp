#include <wds/chart_render/openwds_split_effects/controller.hpp>

#include <cassert>
#include <cstddef>
#include <set>
#include <string>

using namespace wds::chart_render::openwds_split_effects;
namespace unity = wds::chart_render::unity;

namespace {

const SerializedObjectRecord* record_for_identity(const LoadedEffect& graph,
                                                  const std::string& identity) {
  const auto split = identity.rfind('#');
  if (split == std::string::npos) return nullptr;
  try {
    return find_serialized_object(graph, identity.substr(0, split),
                                  std::stoll(identity.substr(split + 1)));
  } catch (...) {
    return nullptr;
  }
}

}  // namespace

int main() {
  assert(resource_has_exact_id_set());
  const auto ids = resource_effect_ids();
  assert(ids.size() == kOpenWdsEffectCount);
  assert(resource_for_effect(0) == nullptr);
  assert(resource_for_effect(999999) == nullptr);
  std::set<int> unique(ids.begin(), ids.end());
  assert(unique.size() == kOpenWdsEffectCount);
  for (const int id : ids) {
    const auto* binding = resource_for_effect(id);
    assert(binding != nullptr && binding->id == id);
    const auto paths = resource_bundle_paths(*binding);
    assert(paths.size() == binding->bundle_count);
    auto controller = create_controller(id);
    assert(controller && controller->valid());
    assert(controller->loaded_resource().complete);
    assert(controller->loaded_resource().unresolved_references.empty());
    assert(!controller->loaded_resource().objects.empty());
    assert(controller->loaded_resource().particle_system_count > 0);
    assert(controller->loaded_resource().controller_line_reference_count > 0);
    assert(controller->loaded_resource().controller_element_reference_count > 0);
    assert(!controller->loaded_resource().element_particle_references.empty());
    assert(!controller->loaded_resource().objects.front().serialized_json.empty());
    assert(controller->initialize(false, 100));
    const auto& loaded = controller->loaded_resource();
    for (const auto& line : controller->lines()) {
      if (line.particle_systems.empty()) {
        assert(line.particle_system == nullptr);
        continue;
      }
      assert(line.particle_systems.size() == line.source_identities.size());
      for (std::size_t i = 0; i < line.particle_systems.size(); ++i) {
        const auto& system = line.particle_systems[i];
        assert(system);
        const auto* source = record_for_identity(loaded, line.source_identities[i]);
        assert(source != nullptr && source->serialized_type == "ParticleSystem");
        unity::ParticleSystem expected;
        assert(hydrate_particle_system(expected, *source, loaded));
        assert(system->main().maxParticles == expected.main().maxParticles);
        assert(system->textureSheetAnimation().mode ==
               unity::ParticleSystemAnimationMode::Sprites);
      }
    }
    controller->on_fade_in(3, 7);
    // Unity permits an element with an empty/null LineEffects array; preserve
    // that serialized condition instead of inventing a placeholder system.
    // RelWithDebInfo defines NDEBUG, so do not use assert() as the empty guard.
    const auto& lines = controller->lines();
    if (!lines.empty()) {
      const auto& ps = lines.data()[0].particle_system;
      if (ps) {
        assert(ps->isPlaying());
        assert(ps->resource_provenance().effectId == id);
      }
    }
    controller->on_fade_out();
    controller->clear();
    const auto& lines_after = controller->lines();
    if (!lines_after.empty()) {
      const auto& ps = lines_after.data()[0].particle_system;
      if (ps) {
        assert(!ps->isPlaying());
      }
    }
  }
  return 0;
}

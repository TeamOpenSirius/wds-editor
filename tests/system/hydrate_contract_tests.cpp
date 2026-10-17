#include <wds/chart_render/openwds_split_effects/resources.hpp>

#include <cassert>
#include <string>

using namespace wds::chart_render::openwds_split_effects;
namespace unity = wds::chart_render::unity;

namespace {

std::string resource_root() {
#ifdef WDS_TEST_REPO_ROOT
  return std::string(WDS_TEST_REPO_ROOT) + "/resources";
#else
  return std::string("./resources");
#endif
}

void assert_10050_modules(unity::ParticleSystem& ps) {
  assert(ps.main().maxParticles == 1000);
  assert(ps.textureSheetAnimation().mode == unity::ParticleSystemAnimationMode::Sprites);
  assert(ps.main().startColor.mode == unity::ParticleSystemGradientMode::RandomColor);
  assert(ps.noise().enabled);
  assert(ps.noise().positionAmount.constant == 1.0f ||
         ps.noise().positionAmount.Evaluate(0.0f) == 1.0f);
}

}  // namespace

int main() {
  const std::string root = resource_root();

  {
    unity::ParticleSystem ignored;
    SerializedObjectRecord empty;
    empty.serialized_type = "ParticleSystem";
    LoadedEffect graph;
    assert(!hydrate_particle_system(ignored, empty, graph));
    empty.serialized_json = "{}";
    empty.serialized_type = "Transform";
    assert(!hydrate_particle_system(ignored, empty, graph));
    SerializedObjectRecord empty_renderer;
    empty_renderer.serialized_type = "ParticleSystemRenderer";
    unity::ParticleSystemRenderer renderer;
    assert(!hydrate_particle_system_renderer(renderer, empty_renderer, graph));
  }

  const LoadedEffect effect_10050 = load_effect(10050, root);
  assert(effect_10050.particle_system_count >= 2);
  int hydrated_ps = 0;
  int hydrated_psr = 0;
  int sprite_total = 0;
  for (const auto& object : effect_10050.objects) {
    if (object.serialized_type == "ParticleSystem") {
      unity::ParticleSystem ps;
      assert(hydrate_particle_system(ps, object, effect_10050));
      assert_10050_modules(ps);
      sprite_total += ps.textureSheetAnimation().spriteCount();
      ++hydrated_ps;
    } else if (object.serialized_type == "ParticleSystemRenderer") {
      unity::ParticleSystemRenderer psr;
      assert(hydrate_particle_system_renderer(psr, object, effect_10050));
      ++hydrated_psr;
    }
  }
  assert(hydrated_ps == 2);
  assert(sprite_total == 6);
  assert(hydrated_psr >= 1);

  const auto ids = resource_effect_ids();
  assert(ids.size() == kOpenWdsEffectCount);
  assert(ids.size() == 318);
  for (const std::int32_t id : ids) {
    const LoadedEffect effect = load_effect(id, root);
    bool hydrated = false;
    for (const auto& object : effect.objects) {
      if (object.serialized_type != "ParticleSystem") continue;
      unity::ParticleSystem ps;
      assert(hydrate_particle_system(ps, object, effect));
      assert(ps.main().maxParticles > 0);
      assert(ps.textureSheetAnimation().mode ==
             unity::ParticleSystemAnimationMode::Sprites);
      hydrated = true;
      break;
    }
    assert(hydrated);
  }
  return 0;
}

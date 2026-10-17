#include <wds/chart_render/openwds_split_effects/compatible_shaders.hpp>

#include <cassert>
#include <cstring>
#include <memory>
#include <string>

using wds::chart_render::openwds_split_effects::apply_compatible_shaders;
using wds::chart_render::openwds_split_effects::compatible_shader_name;

namespace unity = wds::chart_render::unity;

static void expect_mapping(const char* authored, const char* expected) {
  const char* mapped = compatible_shader_name(authored);
  assert(mapped != nullptr);
  assert(std::strcmp(mapped, expected) == 0);
}

int main() {
  expect_mapping("SplitEffect/ParticleAdditive",
                 "OpenWDS/SplitEffect/ParticleAdditive");
  expect_mapping("SplitEffect/ParticleAlpha", "OpenWDS/SplitEffect/ParticleAlpha");
  expect_mapping("SplitEffect/SplitEffectSyuriken",
                 "OpenWDS/SplitEffect/SplitEffectSyuriken");
  expect_mapping("SplitEffect/ParticleTrailAdditive",
                 "OpenWDS/SplitEffect/ParticleTrailAdditive");
  expect_mapping("SenceFX/ButterflyEffect", "OpenWDS/SplitEffect/ButterflyEffect");

  const char* unknown = "Custom/UnknownShader";
  assert(compatible_shader_name(unknown) == unknown);

  assert(compatible_shader_name(nullptr) == nullptr);

  unity::Material material;
  material.shader = std::make_shared<unity::Shader>();
  material.shader->name = "SplitEffect/ParticleAdditive";
  assert(apply_compatible_shaders(material) == 1);
  assert(material.shader != nullptr);
  assert(material.shader->name == "OpenWDS/SplitEffect/ParticleAdditive");
  assert(apply_compatible_shaders(material) == 0);

  unity::Material mesh_material;
  mesh_material.shader = std::make_shared<unity::Shader>();
  mesh_material.shader->name = "SplitEffect/ParticleAlpha";
  unity::MeshRenderer mesh_renderer;
  mesh_renderer.sharedMaterial = &mesh_material;
  unity::GameObject root;
  root.meshRenderer = &mesh_renderer;
  assert(apply_compatible_shaders(root) == 1);
  assert(mesh_material.shader != nullptr);
  assert(mesh_material.shader->name == "OpenWDS/SplitEffect/ParticleAlpha");

  return 0;
}

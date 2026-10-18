#include <wds/chart_render/openwds_split_effects/compatible_shaders.hpp>

#include <cstring>

namespace wds::chart_render::openwds_split_effects {
namespace {

// OpenWDS SplitLaneAssetRuntime.CompatibleShaderNames. Authored misspelling
// SenceFX is kept; values are name remaps only (no .shader compile/load).
struct CompatibleShaderMapping {
  const char* authored;
  const char* compatible;
};

constexpr CompatibleShaderMapping kCompatibleShaderNames[] = {
    {"SplitEffect/ParticleAdditive", "OpenWDS/SplitEffect/ParticleAdditive"},
    {"SplitEffect/ParticleAlpha", "OpenWDS/SplitEffect/ParticleAlpha"},
    {"SplitEffect/SplitEffectSyuriken", "OpenWDS/SplitEffect/SplitEffectSyuriken"},
    {"SplitEffect/ParticleTrailAdditive", "OpenWDS/SplitEffect/ParticleTrailAdditive"},
    {"SenceFX/ButterflyEffect", "OpenWDS/SplitEffect/ButterflyEffect"},
};

int apply_material_pointer(unity::Material* material) {
  if (material == nullptr) return 0;
  return apply_compatible_shaders(*material);
}

}  // namespace

const char* compatible_shader_name(const char* authored) {
  if (authored == nullptr) return nullptr;
  for (const auto& mapping : kCompatibleShaderNames) {
    if (std::strcmp(authored, mapping.authored) == 0) return mapping.compatible;
  }
  return authored;
}

int apply_compatible_shaders(unity::Material& material) {
  if (material.shader == nullptr) return 0;
  const char* authored = material.shader->name.c_str();
  const char* mapped = compatible_shader_name(authored);
  if (mapped == authored || std::strcmp(mapped, authored) == 0) return 0;
  material.shader = unity::Shader::Find(mapped);
  return 1;
}

int apply_compatible_shaders(unity::GameObject& root) {
  int replacements = 0;
  if (root.meshRenderer != nullptr) {
    replacements += apply_material_pointer(root.meshRenderer->sharedMaterial);
    for (unity::Material* material : root.meshRenderer->materials) {
      replacements += apply_material_pointer(material);
    }
  }
  if (root.particleSystemRenderer != nullptr) {
    replacements += apply_material_pointer(root.particleSystemRenderer->material);
    for (unity::Material* material : root.particleSystemRenderer->materials) {
      replacements += apply_material_pointer(material);
    }
  }
  const int child_count = root.transform.childCount();
  for (int index = 0; index < child_count; ++index) {
    unity::Transform* child = root.transform.GetChild(index);
    if (child == nullptr || child->gameObject == nullptr) continue;
    replacements += apply_compatible_shaders(*child->gameObject);
  }
  return replacements;
}

}  // namespace wds::chart_render::openwds_split_effects

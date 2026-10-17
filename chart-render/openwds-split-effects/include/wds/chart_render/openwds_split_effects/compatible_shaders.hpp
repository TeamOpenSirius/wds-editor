#pragma once

#include <wds/chart_render/unity_api.hpp>

namespace wds::chart_render::openwds_split_effects {

// OpenWDS SplitLaneAssetRuntime.CompatibleShaderNames. Returns the remapped
// shader name, or authored when no mapping exists. Never null when authored
// is non-null; returns nullptr only when authored is nullptr.
const char* compatible_shader_name(const char* authored);

// Remap Material::shader names on renderers reachable from root. Does not
// compile or load Unity .shader files.
int apply_compatible_shaders(unity::GameObject& root);
int apply_compatible_shaders(unity::Material& material);

}  // namespace wds::chart_render::openwds_split_effects

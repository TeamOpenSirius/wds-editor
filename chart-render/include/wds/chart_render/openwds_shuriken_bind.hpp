#pragma once

#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/unity_api.hpp>

namespace wds::chart_render {

// Hydrated unity::ParticleSystem is source of truth. catalog.inc is oracle
// only: curve / UV / burst table indices, wing, blend, and mesh flags that
// still live in those tables. Mapping copies scripts/generate_split_effect_catalog.py
// + scripts/extract_split_effect_catalog.py. No new measurements or hops.
void bind_from_unity(const unity::ParticleSystem& src, SplitEffectParticle& dst) noexcept;
void bind_from_unity(const unity::ParticleSystem& src, SplitEffectParticle& dst,
                     const SplitEffectParticle* catalog_oracle) noexcept;

}  // namespace wds::chart_render

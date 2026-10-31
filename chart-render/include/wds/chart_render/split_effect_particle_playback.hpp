#pragma once

#include <cstdint>

namespace wds::chart_render {

// Effects whose recovered SplitEffectElement._lineEffects PPtrs are broken or
// duplicated, so the Unity reference never starts their ParticleSystems.
//
// Live Play dump 2026-11-01 (chart 0.5 s, SpritEffectSettingType=Normal=1):
//   11151: all ParticleSystems particleCount=0 / playing=false;
//          _Line1 LineEffects = [11151_1, 11151_1] (duplicate, 11151_2 absent).
// The recovered prefab references do not survive instantiation, so OnFadeIn's
// foreach (particle in element.LineEffects) plays nothing. Native used to spawn
// the catalog's systems anyway, which added a halo the reference does not have.
//
// Add an id only after the same live-Play check (particleCount / isPlaying).
inline constexpr int32_t kSplitEffectParticlesOffIds[] = {11151};

inline constexpr bool official_split_effect_particles_off(int32_t effect_id) noexcept {
  for (const int32_t id : kSplitEffectParticlesOffIds) {
    if (id == effect_id) {
      return true;
    }
  }
  return false;
}

}  // namespace wds::chart_render

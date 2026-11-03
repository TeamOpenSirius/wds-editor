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
// 11152-11157 are slot-rotated reuses of the same prefab: each one instantiates
// SplitEffectElements11151 (catalog.json `element.goName`; see
// scripts/generate_split_effect_catalog.py restore_reused_element_prefab_particles),
// so they carry the same broken/duplicated _lineEffects PPtrs and the reference
// plays no particles for them either. 11150 is a different prefab
// (SplitEffectElements11150 with four distinct, resolved children) and keeps its
// particles.
//
// This list is consumed by the shared particle collector
// (prepare_split_tasks), not by a renderer: the realtime preview, the MP4
// export, the capture CLI, the bench and any future renderer all inherit it.
// Add an id only after the same live-Play check (particleCount / isPlaying).
inline constexpr int32_t kSplitEffectParticlesOffIds[] = {11151, 11152, 11153, 11154,
                                                          11155, 11156, 11157};

inline constexpr bool official_split_effect_particles_off(int32_t effect_id) noexcept {
  for (const int32_t id : kSplitEffectParticlesOffIds) {
    if (id == effect_id) {
      return true;
    }
  }
  return false;
}

}  // namespace wds::chart_render

#pragma once

#include <wds/chart_render/shuriken/curve.hpp>
#include <wds/chart_render/xorshift128_r32_table.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace wds::chart_render {

// Official 1.96.0 SplitEffects/{id} particle catalog, compiled offline from
// catalog.json. Startup must not parse the 12MB JSON or load Addressable
// bundles. Thumb atlas is skins/split_effect_thumbs.png (lazy-load later).
// Color UV atlas is skins/split_effect_uvs.png (not in SkinCatalog::load;
// preview preloads on idle / first use).

enum class SplitEffectBlend : uint8_t {
  Additive = 0,  // SplitEffect/ParticleAdditive pass: SrcAlpha + One
  Alpha = 1,     // SplitEffect/ParticleAlpha pass: SrcAlpha + OneMinusSrcAlpha
  Other = 2,     // SplitEffectSyurikenTrail → ParticleTrailAdditive (SrcAlpha+One)
};

// Official shaders.bundle compiled pass (not leftover material _DstBlend):
// ParticleAdditive destBlend=1 (One); ParticleAlpha destBlend=10 (OneMinusSrcAlpha);
// ParticleTrailAdditive destBlend=1 (One). Catalog Other is Trail, not Alpha.
inline bool official_split_effect_src_alpha_one(uint8_t blend) noexcept {
  return blend == static_cast<uint8_t>(SplitEffectBlend::Additive) ||
         blend == static_cast<uint8_t>(SplitEffectBlend::Other);
}

// Official SortingGroup=20: Line SpriteRenderer is sortingOrder 0 / Add, then
// PSR ParticleAdd (sort≥1), Alpha, Trails (Other). The plate is never the
// late HDR bucket. catalog.json effects[*].sortingGroup is 20 / layer 0 on
// all 1.96.0 prefabs. Note.prefab SpriteRenderer m_SortingOrder is 10, same
// layer. Unity 2022.3 Renderer.sortingOrder / Sorting Group Order in Layer:
// lower queues first, so the whole group dest-blends after notes on
// OfflineRhythmPreview (m_HDR=1). ParticleAdd shader multiplies
// Custom1.x*0.5+1 unclamped; UNORM glow clamped 10160/10220 to white.
// ParticleTrailAdditive trailMaterial stays Other / glow. ParticleAdd
// trailMaterial (10220 1_1 class) uses the same late-HDR Additive bucket.
// Same layer 0, later draw wins:
//   Note 10 → SplitEffect SortingGroup 20 → Bomb SortingGroup 30 →
//   TimingEffect SpriteRenderer 99. Combo lives on GameHudCanvas
//   ScreenSpaceOverlay (after all cameras), not this world stack.
// Bomb: Assets/Resources/Prefabs/Features/Game/Bomb/{Default,Notes,Sakura}/*
//   SortingGroup m_SortingOrder 30.
// TimingEffect / TimingAssistEffect.prefab SpriteRenderer m_SortingOrder 99.
// GameHudRuntime Create: canvas.renderMode = RenderMode.ScreenSpaceOverlay.
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Renderer-sortingOrder.html
// https://docs.unity3d.com/2022.3/Documentation/Manual/class-SortingGroup.html
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/RenderMode.ScreenSpaceOverlay.html
inline constexpr int8_t kOfficialSyurikenLineSortingOrder = 0;
inline constexpr int16_t kOfficialSplitEffectSortingGroupOrder = 20;
inline constexpr int16_t kOfficialNoteSpriteSortingOrder = 10;
inline constexpr int16_t kOfficialBombSortingGroupOrder = 30;
inline constexpr int16_t kOfficialTimingEffectSortingOrder = 99;

inline bool official_split_effect_late_hdr(uint8_t blend, uint8_t sorting_order) noexcept {
  return blend == static_cast<uint8_t>(SplitEffectBlend::Additive) && sorting_order >= 1;
}

inline bool official_syuriken_line_uses_late_hdr() noexcept {
  return official_split_effect_late_hdr(static_cast<uint8_t>(SplitEffectBlend::Additive),
                                        kOfficialSyurikenLineSortingOrder);
}

// Unity 2022.3 ParticleSystemRenderer.trailMaterial. OpenWDS extract stores
// it as renderer.materials[1] (or materials[0] when the renderer has one).
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystemRenderer-trailMaterial.html
// https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysTrailsModule.html
enum class SplitEffectTrailMaterial : uint8_t {
  ParticleTrail = 0,  // SplitEffectSyurikenTrail / ParticleTrailAdditive
  ParticleAdd = 1,    // SplitEffectSyurikenParticleAdd
  ParticleAlpha = 2,  // SplitEffectSyurikenParticleAlpha
};

enum class SplitEffectEmission : uint8_t {
  None = 0,
  Burst = 1,
  RateOverTime = 2,
  RateOverDistance = 3,
};

enum class SplitEffectWing : uint8_t {
  None = 0,
  RedWing = 1,    // 10470 red_wing
  WhiteWing = 2,  // 10470 white_wing
  Wing10630 = 3,  // 10630 / 10631 *_wing
};

enum class SplitEffectSimulationSpace : uint8_t {
  // Unity 2022.3 ParticleSystemSimulationSpace enum:
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystemSimulationSpace.html
  Local = 0,  // moveWithTransform / ParticleSystemSimulationSpace
  World = 1,
  Custom = 2,  // customSimulationSpace Transform; runtime evaluator EVAL-GAP
};

// Unity 2022.3 ParticleSystemScalingMode. Catalog is Local on every system.
enum class SplitEffectScalingMode : uint8_t {
  Hierarchy = 0,
  Local = 1,  // "only the Particle System transform scale, ignoring any parents"
  Shape = 2,
};

constexpr uint8_t kSplitEffectFlagLooping = 1u << 0;
constexpr uint8_t kSplitEffectFlagColorOverLifetime = 1u << 1;
constexpr uint8_t kSplitEffectFlagVelocity = 1u << 2;
constexpr uint8_t kSplitEffectFlagNoise = 1u << 3;
constexpr uint8_t kSplitEffectFlagTrails = 1u << 4;
constexpr uint8_t kSplitEffectFlagSizeOverLifetime = 1u << 5;
// OpenWDS Butterfly mesh + ButterflyEffect.shader. Sprite follows the prefab
// pathId (moth SenceFx_301, dedicated bat-wing Split_10620_1, Split_11091).
// The flag is the shader path, not the picture. Do not bind by m_Name only:
// the sheet also has a heart named Split_10620_1.
constexpr uint8_t kSplitEffectFlagButterfly = 1u << 6;
// OpenWDS models.bundle Mesh "Butterfly" pathId 3717266357240379075:
// two quads, AABB ±0.2 x ±0.1, z=0. Each wing u: spine→tip, v: y=-0.1→+0.1.
// Right wing inset 1/256. Instanced Mesh samples Sprite.texture (not
// textureRect); Billboard still maps 0–1 onto textureRect.
constexpr float kSplitEffectButterflyMeshHalfX = 0.2f;
constexpr float kSplitEffectButterflyMeshHalfY = 0.1f;
// 8 official mesh UVs (same pathId). Index 0–3 right, 4–7 left.
constexpr float kSplitEffectButterflyMeshUvU[8] = {
    0.99609375f, 0.99609375f, 0.00390631f, 0.00390625f,
    0.0f,        0.0f,        1.0f,        1.0f};
constexpr float kSplitEffectButterflyMeshUvV[8] = {
    0.00390625f, 0.99609375f, 0.99609357f, 0.00390627f,
    0.0f,        1.0f,        1.0f,        0.0f};
constexpr uint8_t kSplitEffectFlagCubeMesh = 1u << 7;
// OpenWDS models.bundle Mesh "Ring". PSR Mesh + startSize scales that mesh.
// flags is uint16 so this bit is not truncated.
constexpr uint16_t kSplitEffectFlagRingMesh = 1u << 8;
// Unity 2022.3 Primitive Cube: "six sides, which are 1 x 1 units."
// PSR Mesh startSize scales that mesh. UV "repeats on each side".
// OpenWDS ParticleAlpha/Add _MainTex is unbound; shader default is "white".
constexpr float kSplitEffectCubeMeshHalf = 0.5f;

#if __has_include("split_effect_ring_mesh.inc")
#include "split_effect_ring_mesh.inc"
#else
#include "../../../src/split_effect_ring_mesh.inc"
#endif

constexpr uint8_t kSplitEffectRenderBillboard = 0;
constexpr uint8_t kSplitEffectRenderStretch = 1;
constexpr uint8_t kSplitEffectRenderMesh = 4;

// Unity 2022.3 ParticleSystemRenderer.lengthScale: "the length of the
// particle compared to its width". "A value of 1 is neutral". Width is
// Main Start Size X (3D: x = width, Manual/PartSysMainModule.html).
// Y is height on that same page — the particle's height axis, not
// lengthScale and not Y×LS. Do not pick max(Y, width×LS).
// View cam-up is not playfield +Y (LaneGroup Rx=60°): putting height on
// cam-up stands the quad off the split line. Stretch aligns that height
// axis to 3D velocity while the quad still faces the camera.
inline float official_split_effect_stretch_along(float size_x, float size_y,
                                                 uint8_t size_3d) noexcept {
  (void)size_y;
  (void)size_3d;
  return size_x;
}

inline float official_split_effect_stretch_height(float size_x, float size_y,
                                                  uint8_t size_3d) noexcept {
  (void)size_x;
  (void)size_3d;
  // kettlewell LS term is current height (GetCurrentSize3D.y), not width.
  // Unity 2022.3 SOL Separate Axes: Y is height even when startSize3D is
  // off (Manual/PartSysSizeOverLifeModule.html). live dump 1070_5/6
  // (official-1070-fog-v30): start_3d = (s,s,s), current.y follows SOL Y
  // (0.35 → ~17.5), Bake along tracks current.y. Substituting size_x when
  // !size_3d turned those layers into dots. Non-3D without separateAxes:
  // eval_sol_axes sets sy=sx, so size_y == size_x. Do not restore the
  // size_3d width gate. Hop length still LS×this, not 2022.3 topology.
  return size_y;
}

inline float official_split_effect_stretch_length(float size_x, float size_y, uint8_t size_3d,
                                                  float length_scale, float velocity_scale,
                                                  float speed) noexcept {
  // Approved unofficial (kettlewell 2025-02, not Unity 2022.3):
  // |v| * velocityScale + lengthScale * size.y
  // size.y is GetCurrentSize3D.y: Main height when startSize3D, else the
  // 1D startSize after SOL Y (2022.3 GetCurrentSize3D applies curves to
  // startSize3D; dump 1070_5/6). Renderer Length Scale is "current size"
  // along velocity (Manual/PartSysRendererModule.html) — not start width.
  const float ls_size = official_split_effect_stretch_height(size_x, size_y, size_3d);
  return std::fabs(speed) * velocity_scale + length_scale * ls_size;
}

// live dump v22 (visual-gap-v22). Not 2022.3.
// Hamming-1 isolate Sweep stored/total ∈ {0, 0.02, 0.04, 0.049, 0.051, …, 1}
// with authored LS=2: Unity Bake along = LS × Main Y on every sample,
// including stored/total≈0. The v21 |stored| > 0.05|total| cut is withdrawn.
// Shared draw still uses totalVelocity for kettlewell direction.
// Official 11320_2 Bake along stays ~Y. v24 Hamming-1: flipping maxP
// 0.5→10 or flattening startSize curves to Constant 0.8/1.1 does not
// make official Bake LS×Y; isolate Bake stays LS×Y at maxP=0.5.
// v25 Hamming-1: official LS=1 firstAlong=X; LS=2 still ~Y; LS=4
// firstAlong≈2.07×Y (not 4×Y). pin / VOL off stay ~Y. Isolate +Y /
// official-like / +Z stay LS×Y. 11770_1 stays LS×Y. Compound gap is
// not stored/total, not maxP, not playfield pin, not VOL, not vel dir.
// v25 regress: no candidate hits 11320 LS=1/2/4 + 11770 + isolate + 11530.
// max(X, LS×Y/2) fits 11320-only and fights the LS×Y witnesses.
// v26 denser LS + power/exp/log/trig: official 11320 non-X edge ≈
// 0.517×LS×Y across LS∈[0.25,8] (fit p≈1, not LS^0.5 / log / trig /
// negative exp). 11770 and isolate stay LS×Y on the same grid.
// (LS/2)×Y and max(X, LS×Y/2) remain 11320-only. v27: catalog size twins
// 10560_2 / 10570_5 are LS×Y on the same const flatten; not one family.
// v28 Hamming-1 remaining diffs (duration / startLifetime / Shape.enabled /
// radius / startSpeed / RoD) and the full swap leave 11320 HALF and
// 10560 LS×Y. Coefficient is not those fields.
// v29 Hamming-1 (live dump, not 2022.3): VOL MinMaxState Constant↔
// TwoConstants, Custom Data, startColor, autoRandomSeed, mute siblings,
// rotateWithStretch, in-host iso CopySerialized leave 11320 HALF
// (~0.5168×LS×Y) and 10560 LS×Y. bake_nocam / bake_default match camera
// Bake (not view-construction flatten). Shader uv.z*0.5 is brightness.
// OpenWDS Runtime / native draw_stretch / recovered 2.31.2
// SplitEffectController ARM64 have no 11320 length branch.
// 2026-10-08 (hop, not 2022.3): applied LS = authored × Sprite.textureRect
// height/width (same OpenWDS field Billboard already uses). Unity GetCurrentSize3D
// stays the authored 3D size; aspect is mesh-time. v25 LS=1 firstEdges
// [0.568, 0.800] — the 0.568 edge is LS×Y×aspect; max-edge=X was a
// measure artifact, not max(X, …). v26 LS grid non-width edge / (LS×Y)
// matches textureRect h/w; square sprites (10560 / 11770 / isolate) stay
// ×1. Compare scripts/compare_stretch_sprite_aspect_logs.py (26/26).
// 2022.3 lengthScale page does not mention sprite aspect. Withdraw the
// 2026-10-07 11320 ID branch. Do not write (LS/2)×Y. Do not restore the
// 0.05 cut. Do not write a Stretch maxP clamp. Do not max/blend Y with LS×X.
// 2026-10-08 live dump official-1070-fog-v30 (hop size.y, not 2022.3 mesh):
// !startSize3D + SOL separateAxes still uses GetCurrentSize3D.y as LS size.
// Withdraw the !size_3d → width substitution. 1070_5/6 catalog class n=2.
inline float official_split_effect_uv_aspect_hw(uint16_t index) noexcept;
inline bool official_split_effect_temp_11320_chain_stretch(int32_t effect_id, uint8_t render_mode,
                                                           uint16_t uv_name_index) noexcept;

inline float official_split_effect_stretch_applied_length_scale(
    float stored_x, float stored_y, float stored_z, float total_x, float total_y, float total_z,
    float authored, int32_t effect_id = 0, uint8_t render_mode = 0,
    uint16_t uv_name_index = 0xFFFFu) noexcept {
  (void)stored_x;
  (void)stored_y;
  (void)stored_z;
  (void)total_x;
  (void)total_y;
  (void)total_z;
  (void)effect_id;
  (void)render_mode;
  return authored * official_split_effect_uv_aspect_hw(uv_name_index);
}

// Unity 2022.3 Stretched Billboard faces the Camera and aligns to velocity
// (Manual/PartSysRendererModule). View = camera plane, not a 3D strip.
// Trail Cross(forward, tangent) is Trails View, not this mesh.
// This roll is the non-3D Stretch align: oriented_quad local +X is the
// width×LS length; r = atan2(view_y, view_x) sends that axis to view-vel.
// 3D Start Size already occupies View X/Y (width/height). Rolling that
// rectangle remaps height onto the vel-perp (11770-class rails 横向).
inline float official_split_effect_stretch_view_roll(float vel_x, float vel_y, float vel_z,
                                                     float lane_tilt_rad) noexcept {
  const float view_x = vel_x;
  const float view_y = vel_y * std::cos(lane_tilt_rad) + vel_z * (-std::sin(lane_tilt_rad));
  if (view_x * view_x + view_y * view_y < 1e-16f) {
    return 0.0f;
  }
  return std::atan2(view_y, view_x);
}

// live dump v1 (synth-mesh-measure-v14 Wave 1 / Wave 4). Not 2022.3.
// Same-frame young |p − emitter.world| ∈ [R·(1−thickness), R]. Shape page
// names radiusThickness 0=surface / 1=volume; it does not say how to
// measure the shell. Shared by Sphere / Hemisphere / Circle (radiusThickness).
inline void split_effect_shape_radius_shell(float radius, float thickness, float& inner,
                                            float& outer) noexcept {
  const float t = std::clamp(thickness, 0.0f, 1.0f);
  outer = radius > 0.0f ? radius : 0.0f;
  inner = outer * (1.0f - t);
}

// live dump v1 (synth-mesh-measure-v14 Wave 1 / Wave 4). Not 2022.3.
// Cumulative seed count ≈ RoD × Σ|Δps.worldPosition|. Emission API names
// Transform-since-last-update; it does not say Euclidean world path or
// unique seeds as N. Forbidden: lossyY / chord / max(wda, 2·Σdt).
inline float split_effect_rod_seeds_from_ps_world_path(float rod,
                                                       float sum_abs_delta_ps_world) noexcept {
  const float r = rod > 0.0f ? rod : 0.0f;
  const float s = sum_abs_delta_ps_world > 0.0f ? sum_abs_delta_ps_world : 0.0f;
  return r * s;
}

// live dump v1 (synth-infer-v15 Wave 1 / Wave 5). Not 2022.3.
// Inherit Initial (mode 0): birth stored/total ≈ inherit_k × emitter_v, anim ≈ 0.
// Inherit Current (mode != 0): young stored unchanged by inherit, anim/total ≈
// inherit_k × emitter_v. Shared path; no ID special case. Official line travel
// still supplies emitter_v when the caller has no Transform/Rigidbody vector.
inline void split_effect_inherit_velocity_terms(uint8_t inherit_enabled, uint8_t inherit_mode,
                                                float inherit_k, float evx, float evy, float evz,
                                                float& stored_ax, float& stored_ay, float& stored_az,
                                                float& anim_x, float& anim_y,
                                                float& anim_z) noexcept {
  stored_ax = stored_ay = stored_az = 0.0f;
  anim_x = anim_y = anim_z = 0.0f;
  if (inherit_enabled == 0) {
    return;
  }
  const float ix = inherit_k * evx;
  const float iy = inherit_k * evy;
  const float iz = inherit_k * evz;
  if (inherit_mode == 0) {
    stored_ax = ix;
    stored_ay = iy;
    stored_az = iz;
  } else {
    anim_x = ix;
    anim_y = iy;
    anim_z = iz;
  }
}

// live dump v1 (synth-infer-v15 Wave 1 / Wave 5). Not 2022.3.
// Δp ≈ total · dt · volSpeedMod. Do not rewrite stored as v0 × sm.
inline void split_effect_speedmod_delta_position(float total_x, float total_y, float total_z,
                                                 float dt, float speed_mod, float& dpx, float& dpy,
                                                 float& dpz) noexcept {
  dpx = total_x * dt * speed_mod;
  dpy = total_y * dt * speed_mod;
  dpz = total_z * dt * speed_mod;
}

// live dump v23 WITHDRAW. Not 2022.3. Official Force / Limit pages have no
// birth impulse. Hamming-1 Unity young stored.z matches no-impulse
// (birth vz=4 then Force+iso_drag) on both sides of the former AND-gate
// (dampen 0.49/0.50, drag 0.49/0.50, Fz 1.74/1.75). Always off. Do not
// restore τ=0.5 or the compound window. Tau kept only to name the withdrawn hop.
inline constexpr float kSplitEffectForceBirthImpulseTau = 0.5f;

inline bool official_split_effect_force_birth_impulse_applies(uint8_t force_enabled,
                                                              uint8_t clamp_enabled,
                                                              float clamp_dampen,
                                                              float clamp_drag, float force_x,
                                                              float force_y,
                                                              float force_z) noexcept {
  (void)force_enabled;
  (void)clamp_enabled;
  (void)clamp_dampen;
  (void)clamp_drag;
  (void)force_x;
  (void)force_y;
  (void)force_z;
  return false;
}

// Unity 2022.3 Position Amount is a multiplier; 0 turns curl off (v19
// iso_noise_size/rot). live dump v23: Unity |anim| scales through 1e-6 /
// 1.1e-6 / 1e-5 (max/amt≈0.923); no binary jump at 1e-6. Zero test is
// amount!=0. Do not change 2020 Noise ε / curl. Not 2022.3.
inline bool official_split_effect_noise_position_curl_on(float amount) noexcept {
  return std::fabs(amount) > 0.0f;
}

inline void split_effect_iso_drag_scale(float k, float dt, float& vx, float& vy,
                                        float& vz) noexcept {
  if (k <= 1e-8f || dt <= 1e-8f) {
    return;
  }
  const float s = std::sqrt(vx * vx + vy * vy + vz * vz);
  if (s <= 1e-12f) {
    vx = 0.0f;
    vy = 0.0f;
    vz = 0.0f;
    return;
  }
  const float new_s = 1.0f / (1.0f / s + k * dt);
  const float m = new_s / s;
  vx *= m;
  vy *= m;
  vz *= m;
}

// live dump force-drag-2020-class-v1. Not 2022.3.
// Drag the composed total, then stored = total − anim (anim is re-evaluated
// next frame at full VOL / Noise). k = drag × (mulSize ? size : 1).
// mulVel=true (2020/2030/10100): iso 1/|v| += k dt, do not also multiply k
// by |v| (e0000 |stored| 2.75 vs Unity 2.78; *|v| overshoots).
// mulVel=false (11423 / e0004): |v| −= k dt (e0004 |stored| 2.39 vs 2.33;
// 11423 age 0.7 |stored| 1.40 vs 1.42). Do not run iso on this branch —
// iso k=2 cancels 11423 VOL Y=−12. Do not restore 1 − k·dt as the shared
// discrete (that is not the mulVel=false Hamming).
inline void split_effect_limit_drag_on_total(float k, float dt, uint8_t mul_vel, float& vx,
                                             float& vy, float& vz) noexcept {
  if (mul_vel != 0) {
    split_effect_iso_drag_scale(k, dt, vx, vy, vz);
    return;
  }
  if (k <= 1e-8f || dt <= 1e-8f) {
    return;
  }
  const float s = std::sqrt(vx * vx + vy * vy + vz * vz);
  if (s <= 1e-12f) {
    vx = 0.0f;
    vy = 0.0f;
    vz = 0.0f;
    return;
  }
  const float dec = k * dt;
  if (s <= dec) {
    vx = 0.0f;
    vy = 0.0f;
    vz = 0.0f;
    return;
  }
  const float m = (s - dec) / s;
  vx *= m;
  vy *= m;
  vz *= m;
}

// live dump v1 (synth-infer-v15 Wave 1 / Wave 5) + cone-dir-v1. Not 2022.3.
// Cone axis decompose: outer = radius + h·tan(angle). t==4 base uses h=0 so
// outer = R. inner = outer·(1−thickness) is the Sphere-style hole width;
// Cone birth no longer samples that linear hole — ρ² ~ U[1−thickness, 1],
// so rad ∈ [outer·√(1−t), outer] (see sample_shape). At catalog t ∈ {0, 1}
// those coincide (√1=1, √0=0). Shared by Cone / ConeVolume / ConeVolumeShell.
// Does not replace the Sphere |p−emitter.world| shell hop. t7 fixture
// collapse is unproven; no t7 special case. Only caller is sample_shape
// (outer); carry / dead-skip re-run sample_shape and do not read inner.
inline void split_effect_cone_radial_shell(float height_along_axis, float radius, float angle_deg,
                                           float thickness, float& inner, float& outer) noexcept {
  const float h = height_along_axis > 0.0f ? height_along_axis : 0.0f;
  const float ang = std::max(angle_deg, 0.0f) * (3.14159265358979323846f / 180.0f);
  outer = radius + h * std::tan(ang);
  if (outer < 0.0f) {
    outer = 0.0f;
  }
  const float t = std::clamp(thickness, 0.0f, 1.0f);
  inner = outer * (1.0f - t);
}

// live dump n7 (align2-n7-unity-v1 11520_1 Hamming-1 maxP 0.5 vs 10). Not 2022.3.
// Billboard: size' = min(GetCurrentSize, maxP * 2 * viewZ * tan(fov/2) * aspect)
// when viewZ > 0. Measure GetCurrentSize vs viewport *width* at camera-forward
// depth (not camDist, not viewport height, not Bake AABB / long-axis, not
// Start Size projection). Bake span_right ratio = size'/size (roll cancels).
// viewZ<=0: do not clamp (caller keeps size). Ban AABB / long-axis. Do not
// write a Stretch width clamp. Mesh: Manual min/max Billboard-only.
// synth-infer-v15 Wave 3/5 calibration is not on disk.
inline float split_effect_billboard_maxp_span(float current_size,
                                              float maxp_world_cap) noexcept {
  const float a = current_size > 0.0f ? current_size : 0.0f;
  const float b = maxp_world_cap > 0.0f ? maxp_world_cap : 0.0f;
  return a < b ? a : b;
}

inline float split_effect_billboard_maxp_world_cap(float maxp, float view_z,
                                                   float tan_half_fov,
                                                   float aspect) noexcept {
  if (view_z <= 0.0f || tan_half_fov <= 0.0f) {
    return 0.0f;  // caller: treat as no-op (keep size)
  }
  const float a = aspect > 0.0f ? aspect : 1.0f;
  const float cap = maxp * (2.0f * view_z * tan_half_fov * a);
  return cap > 0.0f ? cap : 0.0f;
}

// hop: live dump align2-u1-unity-v1 U1-6. Clamp GetCurrentSize (width axis)
// then scale Billboard draw width by textureRect_w(i)/textureRect_w(0).
// `draw_size` is evaluate's Billboard size (already × that ratio); Stretch /
// Mesh pass draw_size == floored GetCurrentSize so the ratio is 1.
inline float split_effect_billboard_draw_width(float current_size, float draw_size, float maxp,
                                               float view_z, float tan_half_fov, float aspect,
                                               uint8_t render_mode) noexcept {
  const float unscaled = current_size > 1e-4f ? current_size : 1e-4f;
  float sprite_scale = 1.0f;
  if (render_mode == kSplitEffectRenderBillboard && unscaled > 1e-8f) {
    sprite_scale = draw_size / unscaled;
  }
  if (maxp <= 1e-8f) {
    return 0.0f;
  }
  float size = unscaled;
  if (view_z > 0.0f) {
    const float cap = split_effect_billboard_maxp_world_cap(maxp, view_z, tan_half_fov, aspect);
    if (cap > 0.0f) {
      size = split_effect_billboard_maxp_span(size, cap);
    }
  }
  return size * sprite_scale;
}

constexpr int32_t kSplitEffectThumbTilePx = 32;
constexpr int32_t kSplitEffectThumbAtlasTilesX = 25;
constexpr int32_t kSplitEffectThumbAtlasTilesY = 25;
constexpr int32_t kSplitEffectThumbTileCount = 625;
// Typical official textureRect side (many sprites). Atlas tiles are
// native w×h plus gutter — use SplitEffectUvTile, not this as a cell size.
constexpr int32_t kSplitEffectUvTilePx = 128;
// Must match scripts/generate_split_effect_catalog.py UV_GUTTER_PX.
// Playback UVs map 0–1 onto the inner content, not this transparent ring.
constexpr int32_t kSplitEffectUvGutterPx = 1;
constexpr uint16_t kSplitEffectNoUvName = 0xFFFFu;

struct SplitEffectGradColorKey {
  float t;
  float r;
  float g;
  float b;
};

struct SplitEffectGradAlphaKey {
  float t;
  float a;
};

struct SplitEffectHermiteKey {
  float t;
  float v;
  float in_s;
  float out_s;
};

using SplitEffectSolKey = SplitEffectHermiteKey;

struct SplitEffectBurst {
  float time;
  float count;  // countCurve.scalar; Constant minScalar is unused
  uint8_t cycles;
  float interval;
  float probability;
};

struct SplitEffectUvTile {
  uint16_t x;
  uint16_t y;
  uint16_t w;
  uint16_t h;
  uint8_t present;  // 0 → missing official sprite; skip (do not invent soft_disk)
};

// OpenWDS Sprite.m_RD.textureRect / texture m_Width,m_Height, normalized.
// Unity v is bottom-up. Instanced Mesh samples this span of Sprite.texture;
// the native atlas still packs only the tight rect.
struct SplitEffectUvTexRect {
  float u0;
  float u1;
  float v0;
  float v1;
};

struct SplitEffectParticle {
  float lifetime;
  float lifetime_min;
  float size;
  float size_min;
  float color_r;
  float color_g;
  float color_b;
  float color_a;
  float emission;
  uint8_t emission_kind;  // SplitEffectEmission
  uint8_t shape_type;     // Unity ParticleSystemShapeType
  uint8_t blend;          // SplitEffectBlend
  uint16_t flags;
  uint16_t uv_name_index;  // kSplitEffectNoUvName if unnamed
  uint8_t element_index;
  uint8_t wing_kind;  // SplitEffectWing; unused (not Butterfly)
  uint8_t life_random;   // 1 if Unity Two Constants / Two Curves
  uint8_t size_random;
  uint8_t speed_random;
  float start_speed;
  float start_speed_min;
  float vel_x;
  float vel_x_min;
  float vel_y;
  float vel_y_min;
  float vel_z;
  float vel_z_min;
  float orbital_x;
  float orbital_y;
  float orbital_z;
  float radial;
  // live dump v1 hop: seeds ≈ this × Σ|Δps.worldPosition|. Not 2022.3.
  float rate_over_distance;
  float length_in_sec;
  float shape_radius;
  float shape_scale_x;
  float shape_scale_y;
  float shape_scale_z;
  float shape_pos_x;
  float shape_pos_y;
  float shape_pos_z;
  float noise_strength;
  float noise_frequency;
  float trail_ratio;
  // Unity 2022.3 Trails Lifetime: multiplier of the particle startLifetime,
  // not seconds. Official majority is Constant 1.0 (1× particle life).
  float trail_lifetime;
  float trail_width;
  float trail_r;
  float trail_g;
  float trail_b;
  float trail_a;
  uint16_t col_color_begin;
  uint8_t col_color_count;
  uint16_t col_alpha_begin;
  uint8_t col_alpha_count;
  float max_particle_size;  // PSR m_MaxParticleSize, viewport fraction
  uint16_t sol_begin;
  uint8_t sol_count;
  uint8_t simulation_space;  // SplitEffectSimulationSpace; Custom preserved, runtime EVAL-GAP
  uint16_t uv_frame_begin;   // indices into kSplitEffectUvFrames
  uint8_t uv_frame_count;
  uint8_t start_frame_random;
  float start_frame;
  float start_frame_min;
  float fot;  // constant frameOverTime if fot_count==0
  float fot_min;
  uint8_t fot_random;
  uint16_t fot_begin;
  uint8_t fot_count;
  uint8_t render_mode;  // 0 Billboard, 1 Stretch, 4 Mesh
  float length_scale;
  float velocity_scale;
  float pivot_y;
  uint16_t custom1_x_begin;
  uint8_t custom1_x_count;
  uint16_t custom1_y_begin;
  uint8_t custom1_y_count;
  float custom1_x;
  float custom1_y;
  float butterfly_wing_x;
  float sol_y;  // separateAxes Y multiplier (1 if unused)
  uint16_t max_particles;
  float rot_z;
  float rot_z_min;
  uint8_t rot_z_random;
  float rot_x;
  float rot_x_min;
  float rot_y;
  float rot_y_min;
  uint8_t rotation_3d;
  float color_r_min;
  float color_g_min;
  float color_b_min;
  uint8_t color_random;
  uint16_t size_curve_begin;
  uint8_t size_curve_count;
  uint16_t size_min_curve_begin;
  uint8_t size_min_curve_count;
  uint16_t speed_curve_begin;
  uint8_t speed_curve_count;
  uint8_t render_alignment;  // ParticleSystemRenderSpace: 0 View, 1 World, 2 Local
  float rate_over_time;
  float rate_over_distance_min;
  uint8_t rod_random;
  float shape_angle;
  float shape_arc;
  // Unity 2022.3 ParticleSystemShapeMultiModeValue on the shape that owns it:
  // Circle/Cone/Donut use Arc Mode; Edge uses Radius Mode. 0 Random, 1 Loop,
  // 2 PingPong, 3 BurstSpread. arcSpeed is complete 360° rotations / second.
  uint8_t shape_arc_mode;
  float shape_arc_spread;
  float shape_arc_speed;
  // live dump v1 hop: young |p−emitter.world| ∈ [R·(1−thickness), R]. Not 2022.3.
  float shape_radius_thickness;
  float shape_rot_x;
  float shape_rot_y;
  float shape_rot_z;
  float random_direction_amount;
  float spherical_direction_amount;
  uint8_t align_to_direction;
  float orbital_offset_x;
  float orbital_offset_y;
  float orbital_offset_z;
  float pivot_x;
  float pivot_z;
  float start_size_y;
  float start_size_y_min;
  uint8_t size_3d;
  uint8_t scaling_mode;
  uint16_t burst_begin;
  uint8_t burst_count;
  uint16_t sol_y_begin;
  uint8_t sol_y_count;
  uint16_t sol_z_begin;
  uint8_t sol_z_count;
  uint16_t col_min_color_begin;
  uint8_t col_min_color_count;
  uint16_t col_min_alpha_begin;
  uint8_t col_min_alpha_count;
  uint8_t col_minmax_state;
  uint16_t rot_ol_begin;
  uint8_t rot_ol_count;
  float rot_ol;
  float rot_ol_min;
  uint8_t rot_ol_enabled;
  uint8_t rot_ol_random;
  uint8_t noise_octaves;
  float noise_octave_scale;
  float noise_octave_mult;
  float noise_scroll;
  float noise_position_amount;
  uint8_t noise_separate_axes;
  float noise_strength_y;
  float noise_strength_z;
  float trail_min_vertex;
  float simulation_speed;
  uint8_t auto_random_seed;
  uint32_t random_seed;
  uint8_t vol_in_world;
  // startColor MinMaxGradient. State 4 = RandomColor: Evaluate(random) on
  // maxGradient (Unity ParticleSystemGradientMode). State 1 Gradient / 3
  // TwoGradients: Evaluate(time=emit_u) at emission (live dump v19 hop).
  // Mode 0 Blend / 1 Fixed.
  uint16_t start_col_color_begin;
  uint8_t start_col_color_count;
  uint16_t start_col_alpha_begin;
  uint8_t start_col_alpha_count;
  uint8_t start_color_state;
  uint8_t start_col_mode;
  // PSR m_SortingOrder. Official 2020: fog/thorns=1, petals=2 (higher draws later).
  uint8_t sorting_order;
  // InitialModule.gravityModifier. All official entries are Constant (state 0).
  float gravity_modifier;
  // Force over Lifetime (Unity ForceModule). Official 2020 fog/petals: local z=+2.
  uint8_t force_enabled;
  uint8_t force_in_world;
  uint8_t force_randomize;
  uint8_t force_x_random;
  float force_x;
  float force_x_min;
  uint16_t force_x_begin;
  uint8_t force_x_count;
  uint16_t force_x_min_begin;
  uint8_t force_x_min_count;
  uint8_t force_y_random;
  float force_y;
  float force_y_min;
  uint16_t force_y_begin;
  uint8_t force_y_count;
  uint16_t force_y_min_begin;
  uint8_t force_y_min_count;
  uint8_t force_z_random;
  float force_z;
  float force_z_min;
  uint16_t force_z_begin;
  uint8_t force_z_count;
  uint16_t force_z_min_begin;
  uint8_t force_z_min_count;
  // Limit Velocity over Lifetime (ClampVelocityModule).
  uint8_t clamp_enabled;
  uint8_t clamp_separate;
  uint8_t clamp_in_world;
  uint8_t clamp_mul_size;
  uint8_t clamp_mul_vel;
  float clamp_dampen;
  float clamp_speed;
  float clamp_speed_min;
  uint8_t clamp_speed_random;
  uint16_t clamp_speed_begin;
  uint8_t clamp_speed_count;
  float clamp_x;
  float clamp_y;
  float clamp_z;
  float clamp_drag;
  float clamp_drag_min;
  uint8_t clamp_drag_random;
  uint16_t clamp_drag_begin;
  uint8_t clamp_drag_count;
  // Rotation by Speed. Official 2021 petals: separateAxes TwoConstants.
  uint8_t rbs_enabled;
  uint8_t rbs_separate;
  float rbs_x;
  float rbs_x_min;
  uint8_t rbs_x_random;
  float rbs_y;
  float rbs_y_min;
  uint8_t rbs_y_random;
  float rbs_z;
  float rbs_z_min;
  uint8_t rbs_z_random;
  float rbs_range_x;
  float rbs_range_y;
  // Inherit Velocity. Official 2020 is off; 53 systems use mode 0 (Initial).
  uint8_t inherit_enabled;
  uint8_t inherit_mode;
  float inherit_mult;
  float inherit_mult_min;
  uint8_t inherit_random;
  // NoiseModule.damping. Unity: strength ∝ frequency when set.
  uint8_t noise_damping;
  // Custom1.x TwoCurves min envelope. OpenWDS uv.z = Custom1.x.
  float custom1_x_min;
  uint8_t custom1_x_random;
  uint16_t custom1_x_min_begin;
  uint8_t custom1_x_min_count;
  // Velocity over Lifetime curves. Do not bake the MinMax scalar as a constant
  // birth velocity — official Two Curves start at 0 (butterflies fly on Force Z).
  uint8_t vel_x_random;
  uint16_t vel_x_begin;
  uint8_t vel_x_count;
  uint16_t vel_x_min_begin;
  uint8_t vel_x_min_count;
  uint8_t vel_y_random;
  uint16_t vel_y_begin;
  uint8_t vel_y_count;
  uint16_t vel_y_min_begin;
  uint8_t vel_y_min_count;
  uint8_t vel_z_random;
  uint16_t vel_z_begin;
  uint8_t vel_z_count;
  uint16_t vel_z_min_begin;
  uint8_t vel_z_min_count;
  // Trails module Mode. Unity 2022.3: 0 Particle (path behind one particle),
  // 1 Ribbon (connect living particles by age). Official majority is Ribbon.
  uint8_t trail_mode;
  // Trails.dieWithParticles. 0 = PerParticle trails keep vertices until each
  // expires at t_rec + Lifetime×startLifetime (keep = life + L×life).
  // Ribbon has no independent trail points; dieWith=false still drops the
  // particle at death. 1 = drop the trail when the particle dies.
  uint8_t trail_die_with;
  // Trails.sizeAffectsWidth. Unity multiplies width by current particle size
  // (startSize × SOL), not birth startSize alone.
  uint8_t trail_size_affects_width;
  // Trails.widthOverTrail MinMaxCurve. State uses Unity's ParticleSystemCurveMode
  // values (Constant=0, Curve=1, TwoCurves=2, TwoConstants=3). The max curve
  // keys are stored in trail_width_*; TwoCurves stores the matching min curve.
  uint8_t trail_width_state;
  float trail_width_min;
  uint16_t trail_width_begin;
  uint8_t trail_width_count;
  uint16_t trail_width_min_begin;
  uint8_t trail_width_min_count;
  // Trails Color-over-Trail alpha. Ribbon t is 3D arc/total, t=0 = Bake
  // chain start = oldest living particle (hop, ribbon-jitter-v1 / align2-n6,
  // not 2022.3). Particle mode t=0 is the head (newest). Official currents
  // are 1 → 0.
  float trail_over_a0;
  float trail_over_a1;
  // Size-over-Lifetime Two Curves min envelope. Unity 2022.3
  // ParticleSystemCurveMode.TwoCurves: random between minCurve and maxCurve
  // at the same t (one curveMultiplier). Official extract had max only.
  uint16_t sol_min_begin;
  uint8_t sol_min_count;
  uint16_t sol_y_min_begin;
  uint8_t sol_y_min_count;
  uint16_t sol_z_min_begin;
  uint8_t sol_z_min_count;
  // Rotation over Lifetime. Unity 2022.3 separateAxes: "When disabled, only
  // the z-axis rotation curve is used."
  uint8_t rot_ol_separate;
  uint16_t rot_ol_min_begin;
  uint8_t rot_ol_min_count;
  float rot_ol_x;
  float rot_ol_x_min;
  uint8_t rot_ol_x_random;
  uint16_t rot_ol_x_begin;
  uint8_t rot_ol_x_count;
  uint16_t rot_ol_x_min_begin;
  uint8_t rot_ol_x_min_count;
  float rot_ol_y;
  float rot_ol_y_min;
  uint8_t rot_ol_y_random;
  uint16_t rot_ol_y_begin;
  uint8_t rot_ol_y_count;
  uint16_t rot_ol_y_min_begin;
  uint8_t rot_ol_y_min_count;
  // Shape.randomPositionAmount. Unity 2022.3: "Move particles by a random
  // amount, up to the specified value." Cone also multiplies by Scale.
  float random_position_amount;
  // Main startSizeY MinMaxCurve (3D Start Size). Evaluated at emission
  // (system age / Duration), same as startSize X.
  uint16_t size_y_curve_begin;
  uint8_t size_y_curve_count;
  uint16_t size_y_min_curve_begin;
  uint8_t size_y_min_curve_count;
  // Emission.rateOverDistance MinMaxCurve. Unity 2022.3: rate "can vary
  // over the lifetime of the system according to a curve."
  uint16_t rod_curve_begin;
  uint8_t rod_curve_count;
  uint16_t rod_min_curve_begin;
  uint8_t rod_min_curve_count;
  // Main startSizeZ. Unity 2022.3 Manual/PartSysMainModule.html: z (depth)
  // only applies to 3D Mesh particles. Billboard/Stretch store it, draw does
  // not invent a third screen axis. catalog.json startSizeZ.
  float start_size_z;
  float start_size_z_min;
  uint16_t size_z_curve_begin;
  uint8_t size_z_curve_count;
  uint16_t size_z_min_curve_begin;
  uint8_t size_z_min_curve_count;
  // Trails colorOverTrail gradient keys. Unity samples this MinMaxGradient
  // along the trail. Ribbon t=0 oldest (3D arc/total); Particle t=0 head
  // (newest). state 0/1/2/3 are Color, Gradient, TwoColors, TwoGradients.
  uint16_t trail_over_alpha_begin;
  uint8_t trail_over_alpha_count;
  uint8_t trail_over_state;
  uint16_t trail_over_color_begin;
  uint8_t trail_over_color_count;
  uint16_t trail_over_min_alpha_begin;
  uint8_t trail_over_min_alpha_count;
  uint16_t trail_over_min_color_begin;
  uint8_t trail_over_min_color_count;
  uint8_t trail_over_mode;
  uint8_t trail_over_min_mode;
  float trail_over_r;
  float trail_over_g;
  float trail_over_b;
  float trail_over_color_a;
  float trail_over_r_min;
  float trail_over_g_min;
  float trail_over_b_min;
  float trail_over_a_min;
  // Trails.inheritParticleColor. 2022.3: when enabled, trail color overrides
  // sit on top of the particle color. catalog.json trails.inheritParticleColor.
  uint8_t trail_inherit_particle_color;
  // GameObject transform. catalog.json transform.localPosition / localScale /
  // localRotation. Quat is the usable rotation; angleDeg+axis is the extract's
  // axis-angle form of that same quat.
  float local_pos_x;
  float local_pos_y;
  float local_pos_z;
  float local_scale_x;
  float local_scale_y;
  float local_scale_z;
  float local_quat_x;
  float local_quat_y;
  float local_quat_z;
  float local_quat_w;
  float local_angle_deg;
  float local_axis_x;
  float local_axis_y;
  float local_axis_z;
  // catalog.json shape.enabled. Unity 2022.3 ShapeModule enabled.
  uint8_t shape_enabled;
  // Main startLifetime MinMaxCurve keys when state=1 (Curve). 38 official
  // systems. Evaluate at system age / Duration — emit still uses lifetime.
  uint16_t life_curve_begin;
  uint8_t life_curve_count;
  // Emission.rateOverTime MinMaxCurve keys when state=1 (Curve). 39 official
  // systems. emit.cpp still uses rate_over_time scalar.
  uint16_t rot_curve_begin;
  uint8_t rot_curve_count;
  // startColor TwoGradients (state 3) minGradient. catalog.json start_color()
  // only writes minColor/maxColor for state 3 — counts stay 0 until extract
  // stores the key tables. State 1 Gradient uses start_col_* (maxGradient).
  uint16_t start_col_min_color_begin;
  uint8_t start_col_min_color_count;
  uint16_t start_col_min_alpha_begin;
  uint8_t start_col_min_alpha_count;
  // Trails Color-over-Lifetime. catalog.json trails.colorOverLifetime, not
  // just maxColor. minMaxState 0 Color / 1 Gradient / 2 TwoColors / 3 TwoGradients.
  // 121 official Trail systems are non-constant. trail_r/g/b/a stay maxColor.
  uint8_t trail_col_state;
  uint16_t trail_col_color_begin;
  uint8_t trail_col_color_count;
  uint16_t trail_col_alpha_begin;
  uint8_t trail_col_alpha_count;
  uint16_t trail_col_min_color_begin;
  uint8_t trail_col_min_color_count;
  uint16_t trail_col_min_alpha_begin;
  uint8_t trail_col_min_alpha_count;
  float trail_col_r_min;
  float trail_col_g_min;
  float trail_col_b_min;
  float trail_col_a_min;
  // PSR freeformStretching / rotateWithStretchDirection. Unity 2022.3
  // ParticleSystemRenderer. catalog.json extract_psr omits both — stored 0
  // unless the JSON grows the keys.
  uint8_t freeform_stretching;
  uint8_t rotate_with_stretch;
  // PSR m_CameraVelocityScale. catalog.json renderer.cameraVelocityScale.
  // ScriptReference/ParticleSystemRenderer-cameraVelocityScale.html
  float camera_velocity_scale;
  // NoiseModule.rotationAmount / sizeAmount. catalog.json noise.* Amount.
  // Official enabled systems are Constant (state 0).
  float noise_rotation_amount;
  float noise_size_amount;
  // Main startColor minColor.a. TwoColors Evaluate lerps all four channels;
  // color_r/g/b_min already exist. live dump v2 (re-color-v5).
  float color_a_min = 0.0f;
  // Unity 2022.3 ShapeModule.length. Official default 5. Authored 0 stays 0.
  float shape_length = 5.0f;
  // live dump v1 hop input: VOL speedModifier scalar. Default 1 is Unity's
  // multiplier identity when the module is off. Enabled 0 stays 0. Not 2022.3.
  float vol_speed_mod = 1.0f;
  // Unity 2022.3 MainModule.startDelay: seconds before playback starts.
  // Catalog rows also have split_effect_start_delay.inc; bind copies use this.
  float start_delay = 0.0f;
  // PSR m_Flip. Unity 2022.3 ParticleSystemRenderer.flip. OpenWDS
  // renderer.flip (jsonl overlay). live dump psr-flip-v1 hop (not 2022.3):
  // Bernoulli UV swap on Billboard+Stretch. z unused. Authored 0 stays 0.
  float flip_x = 0.0f;
  float flip_y = 0.0f;
  // OpenWDS ParticleSystem GameObject.m_IsActive. Unity 2022.3
  // GameObject.activeSelf / SetActive: deactivating the GO disables its
  // components, including ParticleSystemRenderer.
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/GameObject-activeSelf.html
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/GameObject.SetActive.html
  // SplitEffectController Instantiates the element prefab and Play()s
  // LineEffects; it never SetActive(true) on an authored-inactive child.
  // 0 → do not Play / evaluate (1–9 / 10393 and leftover layers).
  uint8_t go_active = 1;
  // Unity 2022.3 MinMaxCurve TwoCurves min envelope. startSpeed Evaluate(time,
  // lerpFactor) between minCurve and maxCurve; one curveMultiplier (scalar)
  // is already baked into both key tables.
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.MinMaxCurve.Evaluate.html
  uint16_t speed_min_curve_begin = 0;
  uint8_t speed_min_curve_count = 0;
  // Texture Sheet frameOverTime TwoCurves min envelope. Sampled at particle
  // normalized age t01, lerpFactor fixed at birth.
  uint16_t fot_min_begin = 0;
  uint8_t fot_min_count = 0;
  // NoiseModule.strength MinMaxCurve. Manual: curve over the particle lifetime.
  // https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysNoiseModule.html
  float noise_strength_min = 0.0f;
  uint8_t noise_strength_random = 0;
  uint16_t noise_strength_begin = 0;
  uint8_t noise_strength_count = 0;
  uint16_t noise_strength_min_begin = 0;
  uint8_t noise_strength_min_count = 0;
  float noise_strength_y_min = 0.0f;
  uint8_t noise_strength_y_random = 0;
  uint16_t noise_strength_y_begin = 0;
  uint8_t noise_strength_y_count = 0;
  uint16_t noise_strength_y_min_begin = 0;
  uint8_t noise_strength_y_min_count = 0;
  float noise_strength_z_min = 0.0f;
  uint8_t noise_strength_z_random = 0;
  uint16_t noise_strength_z_begin = 0;
  uint8_t noise_strength_z_count = 0;
  uint16_t noise_strength_z_min_begin = 0;
  uint8_t noise_strength_z_min_count = 0;
  // VOL orbitalX/Y/Z TwoConstants min. Constant / TwoConstants only in catalog.
  float orbital_x_min = 0.0f;
  uint8_t orbital_x_random = 0;
  float orbital_y_min = 0.0f;
  uint8_t orbital_y_random = 0;
  float orbital_z_min = 0.0f;
  uint8_t orbital_z_random = 0;
  // Trails.lifetime TwoConstants min. Curve keys are packed for state 1
  // (live dump dump-pending-v2 hop: L = scalar × curve(age/startLifetime)).
  float trail_lifetime_min = 0.0f;
  uint8_t trail_lifetime_random = 0;
  // Shape arcSpeed Curve (state 1) keys. Loop/PingPong integral hop; scalar
  // stays in shape_arc_speed. TwoConstants/TwoCurves leave count 0.
  uint16_t shape_arc_speed_begin = 0;
  uint8_t shape_arc_speed_count = 0;
  uint16_t trail_lifetime_begin = 0;
  uint8_t trail_lifetime_count = 0;
  uint16_t trail_lifetime_min_begin = 0;
  uint8_t trail_lifetime_min_count = 0;
};

struct SplitEffectCatalogEntry {
  int32_t id;
  uint8_t element_count;
  uint8_t has_thumb;  // 0 → empty atlas tile, do not invent a shape
  uint16_t particle_count;
  uint16_t particle_begin;
  uint16_t thumb_tile;
};

// Unity 2022.3 ParticleSystem.SubModule (Manual/PartSysSubEmitModule.html).
// type: Birth=0 Collision=1 Death=2 Trigger=3 Manual=4
// properties: InheritColor=1 Size=2 Rotation=4 Lifetime=8 Duration=16
struct SplitEffectSubEmitter {
  uint16_t parent_index;
  uint16_t child_index;
  uint8_t type;
  uint8_t properties;
  float emit_probability;
};

constexpr uint8_t kSplitEffectSubEmitBirth = 0;
constexpr uint8_t kSplitEffectSubEmitCollision = 1;
constexpr uint8_t kSplitEffectSubEmitDeath = 2;
constexpr uint8_t kSplitEffectSubEmitTrigger = 3;
constexpr uint8_t kSplitEffectSubEmitManual = 4;

constexpr uint8_t kSplitEffectSubInheritColor = 1;
constexpr uint8_t kSplitEffectSubInheritSize = 2;
constexpr uint8_t kSplitEffectSubInheritRotation = 4;
constexpr uint8_t kSplitEffectSubInheritLifetime = 8;
constexpr uint8_t kSplitEffectSubInheritDuration = 16;

// live dump count-v1 hop, not 2022.3. Child Main curves (startSize / startSpeed /
// startLifetime / startColor Gradient) evaluate at
// u = parent.age / parent.startLifetime at the child's emission.
// Death → 1 (parent age == life), same as the v23 Evaluate(1) LOCK isolates
// (iso_sub_inherit_dur0/1, iso_sub_death_size_child were all Death).
// Do not restore parent duration_u / InheritDuration bit=16.
inline float official_split_effect_sub_emit_u(float parent_age, float parent_life) noexcept {
  if (parent_life <= 1e-8f) {
    return 1.0f;
  }
  return std::clamp(parent_age / parent_life, 0.0f, 1.0f);
}

namespace split_effect_catalog_detail {
#if __has_include("split_effect_catalog.inc")
#include "split_effect_catalog.inc"
#else
#include "../../../src/split_effect_catalog.inc"
#endif
#if __has_include("split_effect_subemitters.inc")
#include "split_effect_subemitters.inc"
#else
#include "../../../src/split_effect_subemitters.inc"
#endif
#if __has_include("split_effect_trail_material.inc")
#include "split_effect_trail_material.inc"
#else
#include "../../../src/split_effect_trail_material.inc"
#endif
#if __has_include("split_effect_start_delay.inc")
#include "split_effect_start_delay.inc"
#else
#include "../../../src/split_effect_start_delay.inc"
#endif
}  // namespace split_effect_catalog_detail

inline const SplitEffectCatalogEntry* official_split_effect(int32_t effect_id) noexcept {
  using split_effect_catalog_detail::kOfficialSplitEffects;
  const auto begin = std::begin(kOfficialSplitEffects);
  const auto end = std::end(kOfficialSplitEffects);
  const auto it = std::lower_bound(
      begin, end, effect_id,
      [](const SplitEffectCatalogEntry& e, int32_t id) { return e.id < id; });
  if (it == end || it->id != effect_id) {
    return nullptr;
  }
  return &(*it);
}

inline const SplitEffectParticle* official_split_effect_particles(
    const SplitEffectCatalogEntry& entry, uint16_t* count = nullptr) noexcept {
  using split_effect_catalog_detail::kSplitEffectParticles;
  if (count != nullptr) {
    *count = entry.particle_count;
  }
  if (entry.particle_count == 0) {
    return nullptr;
  }
  return kSplitEffectParticles + entry.particle_begin;
}

inline std::vector<int32_t> official_split_effect_ids() {
  using split_effect_catalog_detail::kOfficialSplitEffects;
  std::vector<int32_t> out;
  out.reserve(std::size(kOfficialSplitEffects));
  for (const auto& entry : kOfficialSplitEffects) {
    out.push_back(entry.id);
  }
  return out;
}

inline const char* official_split_effect_uv_name(uint16_t index) noexcept {
  using split_effect_catalog_detail::kSplitEffectUvNameCount;
  using split_effect_catalog_detail::kSplitEffectUvNames;
  if (index >= kSplitEffectUvNameCount) {
    return nullptr;
  }
  return kSplitEffectUvNames[index];
}

// Catalog identity of the 11320_2 chain sprite. Length hop no longer
// branches on this; applied LS uses textureRect h/w for every Stretch.
inline bool official_split_effect_temp_11320_chain_stretch(int32_t effect_id, uint8_t render_mode,
                                                           uint16_t uv_name_index) noexcept {
  if (effect_id != 11320 || render_mode != kSplitEffectRenderStretch) {
    return false;
  }
  const char* name = official_split_effect_uv_name(uv_name_index);
  return name != nullptr && std::strcmp(name, "Split_11320") == 0;
}

inline bool official_split_effect_thumb_px(uint16_t tile, int32_t& x, int32_t& y) noexcept {
  if (tile >= static_cast<uint16_t>(kSplitEffectThumbTileCount)) {
    return false;
  }
  x = static_cast<int32_t>(tile % static_cast<uint16_t>(kSplitEffectThumbAtlasTilesX)) *
      kSplitEffectThumbTilePx;
  y = static_cast<int32_t>(tile / static_cast<uint16_t>(kSplitEffectThumbAtlasTilesX)) *
      kSplitEffectThumbTilePx;
  return true;
}

inline const char* official_split_effect_thumb_atlas_relpath() noexcept {
  return "split_effect_thumbs.png";
}

inline const char* official_split_effect_uv_atlas_relpath() noexcept {
  return "split_effect_uvs.png";
}

// SplitEffectSyurikenTrail (common materials.bundle path 9199983673264716074):
// shader SplitEffect/ParticleTrailAdditive, _MainTex = BombEffectTrail 128²,
// _AddtiveIntencity = 2. Official systems whose trailMaterial is that
// shared Trail mat (blend Other, 128 particle slots) use this texture.
// ParticleAdd / ParticleAlpha trailMaterial use the shader default "white".
inline const char* official_split_effect_trail_tex_relpath() noexcept {
  return "bomb_effect_trail.png";
}

inline constexpr float kOfficialSplitEffectTrailAdditiveIntensity = 2.0f;

// Unity 2022.3 MainModule.startDelay. Catalog pointers read the sidecar
// (bind copies are not addresses in kSplitEffectParticles, so they use
// the struct field filled by bind_from_unity).
inline float official_split_effect_start_delay(const SplitEffectParticle& p) noexcept {
  using split_effect_catalog_detail::kSplitEffectParticles;
  using split_effect_catalog_detail::kSplitEffectStartDelay;
  using split_effect_catalog_detail::kSplitEffectStartDelayCount;
  const ptrdiff_t n = static_cast<ptrdiff_t>(std::size(kSplitEffectParticles));
  const ptrdiff_t i = &p - kSplitEffectParticles;
  if (i >= 0 && i < n && static_cast<uint32_t>(i) < kSplitEffectStartDelayCount) {
    return kSplitEffectStartDelay[static_cast<uint32_t>(i)];
  }
  return p.start_delay;
}

// live dump psr-flip-v1 hop (not 2022.3). Same mix3 warmup as shuriken birth
// RNG. Distinct extras so X/Y are independent (neighbor mix3 extras share a
// xorshift family). Skip the draw when p<=0; always swap when p>=1.
inline uint32_t split_effect_psr_flip_mix3(uint32_t a, uint32_t b, uint32_t c) noexcept {
  return xorshift128_r32_mix3(a, b, c);
}

inline float split_effect_psr_flip_u01(uint32_t h) noexcept {
  return static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

inline constexpr uint32_t kSplitEffectPsrFlipUExtra = 0x46585058u;
inline constexpr uint32_t kSplitEffectPsrFlipVExtra = 0x46595059u;

inline uint8_t official_split_effect_psr_flip_axis(uint32_t seed, uint32_t extra,
                                                   float p01) noexcept {
  const float p = std::clamp(p01, 0.0f, 1.0f);
  if (!(p > 0.0f)) {
    return 0;
  }
  if (p >= 1.0f) {
    return 1;
  }
  return split_effect_psr_flip_u01(split_effect_psr_flip_mix3(seed, extra, 1u)) < p ? 1 : 0;
}

inline void official_split_effect_psr_flip_uv(uint32_t seed, float flip_x, float flip_y,
                                              uint8_t& flip_u, uint8_t& flip_v) noexcept {
  flip_u = official_split_effect_psr_flip_axis(seed, kSplitEffectPsrFlipUExtra, flip_x);
  flip_v = official_split_effect_psr_flip_axis(seed, kSplitEffectPsrFlipVExtra, flip_y);
}

inline void official_split_effect_apply_psr_flip_uv(uint8_t flip_u, uint8_t flip_v, float& u0,
                                                    float& v0, float& u1, float& v1) noexcept {
  if (flip_u != 0) {
    const float t = u0;
    u0 = u1;
    u1 = t;
  }
  if (flip_v != 0) {
    const float t = v0;
    v0 = v1;
    v1 = t;
  }
}

inline SplitEffectTrailMaterial official_split_effect_trail_material(
    uint32_t catalog_index) noexcept {
  using split_effect_catalog_detail::kSplitEffectTrailMaterial;
  using split_effect_catalog_detail::kSplitEffectTrailMaterialCount;
  if (catalog_index >= kSplitEffectTrailMaterialCount) {
    return SplitEffectTrailMaterial::ParticleTrail;
  }
  const uint8_t kind = kSplitEffectTrailMaterial[catalog_index];
  if (kind == static_cast<uint8_t>(SplitEffectTrailMaterial::ParticleAdd)) {
    return SplitEffectTrailMaterial::ParticleAdd;
  }
  if (kind == static_cast<uint8_t>(SplitEffectTrailMaterial::ParticleAlpha)) {
    return SplitEffectTrailMaterial::ParticleAlpha;
  }
  return SplitEffectTrailMaterial::ParticleTrail;
}

inline bool official_split_effect_trail_uses_bomb_tex(SplitEffectTrailMaterial material) noexcept {
  return material == SplitEffectTrailMaterial::ParticleTrail;
}

inline float official_split_effect_trail_intensity(SplitEffectTrailMaterial material) noexcept {
  return official_split_effect_trail_uses_bomb_tex(material)
             ? kOfficialSplitEffectTrailAdditiveIntensity
             : 1.0f;
}

inline uint8_t official_split_effect_trail_blend(SplitEffectTrailMaterial material) noexcept {
  if (material == SplitEffectTrailMaterial::ParticleAdd) {
    return static_cast<uint8_t>(SplitEffectBlend::Additive);
  }
  if (material == SplitEffectTrailMaterial::ParticleAlpha) {
    return static_cast<uint8_t>(SplitEffectBlend::Alpha);
  }
  return static_cast<uint8_t>(SplitEffectBlend::Other);
}

inline const SplitEffectUvTile* official_split_effect_uv_tile(uint16_t index) noexcept {
  using split_effect_catalog_detail::kSplitEffectUvNameCount;
  using split_effect_catalog_detail::kSplitEffectUvTiles;
  if (index >= kSplitEffectUvNameCount) {
    return nullptr;
  }
  return &kSplitEffectUvTiles[index];
}

// Per-sprite TSA aspect (textureRect h/w). Billboard height uses the current
// frame. Stretch applied LS uses the same table at mesh time.
inline float official_split_effect_uv_aspect_hw(uint16_t index) noexcept {
  using split_effect_catalog_detail::kSplitEffectUvAspect;
  using split_effect_catalog_detail::kSplitEffectUvNameCount;
  if (index >= kSplitEffectUvNameCount) {
    return 1.0f;
  }
  const float aspect = kSplitEffectUvAspect[index];
  return aspect > 1e-4f ? aspect : 1.0f;
}

// OpenWDS Sprite.textureRect width in pixels. Atlas tiles pack those pixels
// plus kSplitEffectUvGutterPx on each side (generate_split_effect_catalog.py).
inline float official_split_effect_uv_rect_w(uint16_t index) noexcept {
  const SplitEffectUvTile* tile = official_split_effect_uv_tile(index);
  if (tile == nullptr || tile->w == 0) {
    return 1.0f;
  }
  const int inner = static_cast<int>(tile->w) - 2 * kSplitEffectUvGutterPx;
  return inner > 0 ? static_cast<float>(inner) : static_cast<float>(tile->w);
}

// hop: live dump align2-u1-unity-v1 U1-6 10420 Bake 6218 particles RMSE 0.
// Billboard TSA Sprites: quad_w = size_x × tr_w(i)/tr_w(0),
// quad_h = size_y × tr_h(i)/tr_w(0). i = current frame, 0 = first TSA sprite.
// Single sprite ≡ native prior (w=sx, h=sy×h/w). Do not label 2022.3.
// Do not implement U1-6 threshold-E.
inline float official_split_effect_billboard_sprite_width_norm(uint16_t current_uv,
                                                              uint16_t first_uv) noexcept {
  if (current_uv == kSplitEffectNoUvName || first_uv == kSplitEffectNoUvName) {
    return 1.0f;
  }
  if (current_uv == first_uv) {
    return 1.0f;
  }
  const float w0 = official_split_effect_uv_rect_w(first_uv);
  if (w0 <= 1e-6f) {
    return 1.0f;
  }
  const float wi = official_split_effect_uv_rect_w(current_uv);
  if (wi <= 0.0f) {
    return 1.0f;
  }
  return wi / w0;
}

// OpenWDS Sprite.m_Pivot (normalized). Missing / unnamed → center (0.5, 0.5).
inline void official_split_effect_uv_sprite_pivot(uint16_t index, float& px, float& py) noexcept {
  using split_effect_catalog_detail::kSplitEffectUvNameCount;
  using split_effect_catalog_detail::kSplitEffectUvPivotX;
  using split_effect_catalog_detail::kSplitEffectUvPivotY;
  if (index >= kSplitEffectUvNameCount) {
    px = 0.5f;
    py = 0.5f;
    return;
  }
  px = kSplitEffectUvPivotX[index];
  py = kSplitEffectUvPivotY[index];
}

// OpenWDS Sprite.m_RD.textureRect / texture size. Missing / unnamed → 0..1.
inline void official_split_effect_uv_tex_rect(uint16_t index, float& u0, float& u1, float& v0,
                                              float& v1) noexcept {
  using split_effect_catalog_detail::kSplitEffectUvNameCount;
  using split_effect_catalog_detail::kSplitEffectUvTexRect;
  if (index >= kSplitEffectUvNameCount) {
    u0 = 0.0f;
    u1 = 1.0f;
    v0 = 0.0f;
    v1 = 1.0f;
    return;
  }
  const SplitEffectUvTexRect& r = kSplitEffectUvTexRect[index];
  u0 = r.u0;
  u1 = r.u1;
  v0 = r.v0;
  v1 = r.v1;
}

inline bool official_split_effect_uv_px(uint16_t index, int32_t& x, int32_t& y,
                                        bool& present) noexcept {
  const SplitEffectUvTile* tile = official_split_effect_uv_tile(index);
  if (tile == nullptr) {
    return false;
  }
  x = static_cast<int32_t>(tile->x);
  y = static_cast<int32_t>(tile->y);
  present = tile->present != 0;
  return true;
}

// Catalog / QImage y is PNG top-down. load_png_rgba8 uploads bottom-up (V=0 is
// the last PNG row). Sampling the catalog y on the GPU reads the vertically
// mirrored tile — e.g. Split_202_5 (fog) becomes Split_11770_1 (the red hook).
inline bool official_split_effect_uv_gpu_px(uint16_t index, int32_t& x, int32_t& y,
                                            bool& present) noexcept {
  const SplitEffectUvTile* tile = official_split_effect_uv_tile(index);
  if (tile == nullptr) {
    return false;
  }
  x = static_cast<int32_t>(tile->x);
  y = static_cast<int32_t>(tile->y);
  present = tile->present != 0;
  using split_effect_catalog_detail::kSplitEffectUvAtlasHeight;
  y = kSplitEffectUvAtlasHeight - y - static_cast<int32_t>(tile->h);
  return true;
}

inline float official_split_effect_eval_hermite(const SplitEffectHermiteKey* keys, uint8_t count,
                                                float t01, float fallback) noexcept {
  static_assert(sizeof(SplitEffectHermiteKey) == sizeof(shuriken::HermiteKey),
                "Hermite key layout must match shuriken::HermiteKey");
  return shuriken::eval_hermite(reinterpret_cast<const shuriken::HermiteKey*>(keys), count, t01,
                                fallback);
}

inline float official_split_effect_eval_curve_keys(uint16_t begin, uint8_t count, float t01,
                                                   float fallback) noexcept {
  using split_effect_catalog_detail::kSplitEffectCurveKeys;
  if (count == 0) {
    return fallback;
  }
  return official_split_effect_eval_hermite(kSplitEffectCurveKeys + begin, count, t01, fallback);
}

inline float official_split_effect_integrate_curve_keys(uint16_t begin, uint8_t count, float u01,
                                                        float fallback) noexcept {
  using split_effect_catalog_detail::kSplitEffectCurveKeys;
  if (count == 0) {
    return fallback * std::clamp(u01, 0.0f, 1.0f);
  }
  return shuriken::integrate_hermite(
      reinterpret_cast<const shuriken::HermiteKey*>(kSplitEffectCurveKeys + begin), count, u01,
      fallback);
}

inline uint16_t official_split_effect_uv_frame(const SplitEffectParticle& p, float start_u,
                                               float t01, float fot_const) noexcept {
  using split_effect_catalog_detail::kSplitEffectUvFrames;
  if (p.uv_frame_count == 0) {
    return kSplitEffectNoUvName;
  }
  float fot = fot_const;
  if (p.fot_count > 0) {
    const float vmax =
        official_split_effect_eval_curve_keys(p.fot_begin, p.fot_count, t01, p.fot);
    if (p.fot_min_count > 0) {
      const float vmin =
          official_split_effect_eval_curve_keys(p.fot_min_begin, p.fot_min_count, t01, p.fot);
      fot = vmin + (vmax - vmin) * std::clamp(fot_const, 0.0f, 1.0f);
    } else {
      fot = vmax;
    }
  }
  const float u = start_u + fot;
  int idx = static_cast<int>(std::floor(u * static_cast<float>(p.uv_frame_count)));
  if (idx < 0) {
    idx = 0;
  }
  if (idx >= static_cast<int>(p.uv_frame_count)) {
    idx = static_cast<int>(p.uv_frame_count) - 1;
  }
  return kSplitEffectUvFrames[p.uv_frame_begin + static_cast<uint16_t>(idx)];
}

inline uint16_t official_split_effect_uv_frame(const SplitEffectParticle& p, float start_u,
                                               float t01) noexcept {
  return official_split_effect_uv_frame(p, start_u, t01, p.fot);
}

inline uint16_t official_split_effect_uv_first_frame(const SplitEffectParticle& p) noexcept {
  using split_effect_catalog_detail::kSplitEffectUvFrames;
  if (p.uv_frame_count == 0) {
    return p.uv_name_index;
  }
  return kSplitEffectUvFrames[p.uv_frame_begin];
}

inline float official_split_effect_eval_sol_axis(uint16_t begin, uint8_t count, float t01,
                                                 float fallback) noexcept {
  using split_effect_catalog_detail::kSplitEffectSolKeys;
  if (count == 0) {
    return fallback;
  }
  return official_split_effect_eval_hermite(kSplitEffectSolKeys + begin, count, t01, fallback);
}

// Max envelope only. Unity 2022.3 TwoCurves is Evaluate(t, lerpFactor)
// between minCurve and maxCurve — live particles use eval_sol_axes.
inline float official_split_effect_eval_sol(const SplitEffectParticle& p, float t01) noexcept {
  if ((p.flags & kSplitEffectFlagSizeOverLifetime) == 0) {
    return 1.0f;
  }
  return official_split_effect_eval_sol_axis(p.sol_begin, p.sol_count, t01, 1.0f);
}

inline float official_split_effect_eval_sol_y(const SplitEffectParticle& p, float t01) noexcept {
  if ((p.flags & kSplitEffectFlagSizeOverLifetime) == 0) {
    return 1.0f;
  }
  if (p.sol_y_count > 0) {
    return official_split_effect_eval_sol_axis(p.sol_y_begin, p.sol_y_count, t01, p.sol_y);
  }
  if (p.sol_y > 1e-4f && std::fabs(p.sol_y - 1.0f) > 1e-4f) {
    return p.sol_y;
  }
  return official_split_effect_eval_sol(p, t01);
}

inline void official_split_effect_eval_gradient_mode(uint16_t color_begin, uint8_t color_count,
                                                     uint16_t alpha_begin, uint8_t alpha_count,
                                                     float t01, uint8_t mode, float& r, float& g,
                                                     float& b, float& a) noexcept {
  using split_effect_catalog_detail::kSplitEffectColAlphas;
  using split_effect_catalog_detail::kSplitEffectColColors;
  const float t = std::clamp(t01, 0.0f, 1.0f);
  r = 1.0f;
  g = 1.0f;
  b = 1.0f;
  a = 1.0f;
  const bool fixed = mode == 1;
  if (color_count > 0) {
    const SplitEffectGradColorKey* keys = kSplitEffectColColors + color_begin;
    if (fixed) {
      // Unity 2022.3 GradientMode.Fixed: the key whose time is greater than
      // the requested evaluation time (first t_key > t; else the last key).
      // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/GradientMode.Fixed.html
      r = keys[color_count - 1].r;
      g = keys[color_count - 1].g;
      b = keys[color_count - 1].b;
      for (uint8_t i = 0; i < color_count; ++i) {
        if (keys[i].t > t) {
          r = keys[i].r;
          g = keys[i].g;
          b = keys[i].b;
          break;
        }
      }
    } else if (t <= keys[0].t) {
      r = keys[0].r;
      g = keys[0].g;
      b = keys[0].b;
    } else if (t >= keys[color_count - 1].t) {
      const auto& k = keys[color_count - 1];
      r = k.r;
      g = k.g;
      b = k.b;
    } else {
      for (uint8_t i = 1; i < color_count; ++i) {
        if (t <= keys[i].t) {
          const float span = std::max(keys[i].t - keys[i - 1].t, 1e-6f);
          const float u = (t - keys[i - 1].t) / span;
          r = keys[i - 1].r + (keys[i].r - keys[i - 1].r) * u;
          g = keys[i - 1].g + (keys[i].g - keys[i - 1].g) * u;
          b = keys[i - 1].b + (keys[i].b - keys[i - 1].b) * u;
          break;
        }
      }
    }
  }
  if (alpha_count > 0) {
    const SplitEffectGradAlphaKey* keys = kSplitEffectColAlphas + alpha_begin;
    if (fixed) {
      // Same GradientMode.Fixed rule as color keys (first t_key > t; else last).
      a = keys[alpha_count - 1].a;
      for (uint8_t i = 0; i < alpha_count; ++i) {
        if (keys[i].t > t) {
          a = keys[i].a;
          break;
        }
      }
    } else if (t <= keys[0].t) {
      a = keys[0].a;
    } else if (t >= keys[alpha_count - 1].t) {
      a = keys[alpha_count - 1].a;
    } else {
      for (uint8_t i = 1; i < alpha_count; ++i) {
        if (t <= keys[i].t) {
          const float span = std::max(keys[i].t - keys[i - 1].t, 1e-6f);
          const float u = (t - keys[i - 1].t) / span;
          a = keys[i - 1].a + (keys[i].a - keys[i - 1].a) * u;
          break;
        }
      }
    }
  }
}

inline void official_split_effect_eval_gradient(uint16_t color_begin, uint8_t color_count,
                                                uint16_t alpha_begin, uint8_t alpha_count,
                                                float t01, float& r, float& g, float& b,
                                                float& a) noexcept {
  official_split_effect_eval_gradient_mode(color_begin, color_count, alpha_begin, alpha_count, t01,
                                           0, r, g, b, a);
}

inline void official_split_effect_eval_start_color(const SplitEffectParticle& p, float u01,
                                                   float& r, float& g, float& b,
                                                   float& a) noexcept {
  r = p.color_r;
  g = p.color_g;
  b = p.color_b;
  a = p.color_a;
  // Unity ParticleSystemGradientMode.RandomColor (4): a random sample on
  // maxGradient, not a lerp of the first and last keys.
  if (p.start_color_state == 4 && p.start_col_color_count > 0) {
    float ga = 1.0f;
    official_split_effect_eval_gradient_mode(p.start_col_color_begin, p.start_col_color_count,
                                             p.start_col_alpha_begin, p.start_col_alpha_count, u01,
                                             p.start_col_mode, r, g, b, ga);
    if (p.start_col_alpha_count > 0) {
      a = ga;
    }
    return;
  }
  if (p.color_random != 0) {
    const float u = std::clamp(u01, 0.0f, 1.0f);
    r = p.color_r_min + (p.color_r - p.color_r_min) * u;
    g = p.color_g_min + (p.color_g - p.color_g_min) * u;
    b = p.color_b_min + (p.color_b - p.color_b_min) * u;
    // Unity 2022.3 MinMaxGradient.Evaluate TwoColors: lerp minColor→maxColor
    // including alpha. RGB-only left start.a stuck at maxColor.
    // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.MinMaxGradient.Evaluate.html
    a = p.color_a_min + (p.color_a - p.color_a_min) * u;
  }
}

inline void official_split_effect_eval_col(const SplitEffectParticle& p, float t01, float col_u,
                                           float& r, float& g, float& b, float& a) noexcept {
  official_split_effect_eval_gradient(p.col_color_begin, p.col_color_count, p.col_alpha_begin,
                                      p.col_alpha_count, t01, r, g, b, a);
  // Unity MinMaxGradient: 0 Color, 1 Gradient, 2 TwoColors, 3 TwoGradients.
  // Typetree always serializes a dummy minGradient (often a=1). Only state 3
  // randomly lerps the two real gradients; blending the dummy washed 1742
  // Gradient systems into opaque orbs.
  if (p.col_minmax_state != 3) {
    return;
  }
  if (p.col_min_color_count == 0 && p.col_min_alpha_count == 0) {
    return;
  }
  float r2 = 1.0f, g2 = 1.0f, b2 = 1.0f, a2 = 1.0f;
  official_split_effect_eval_gradient(p.col_min_color_begin, p.col_min_color_count,
                                      p.col_min_alpha_begin, p.col_min_alpha_count, t01, r2, g2,
                                      b2, a2);
  const float u = std::clamp(col_u, 0.0f, 1.0f);
  r = r2 + (r - r2) * u;
  g = g2 + (g - g2) * u;
  b = b2 + (b - b2) * u;
  a = a2 + (a - a2) * u;
}

inline void official_split_effect_eval_col(const SplitEffectParticle& p, float t01, float& r,
                                           float& g, float& b, float& a) noexcept {
  official_split_effect_eval_col(p, t01, 0.0f, r, g, b, a);
}

// Unity 2022.3 Trails Color over Trail: MinMaxGradient sampled over trail
// length. Pages do not name which end is t=0 or whether t is index vs arc.
// Ribbon stitch passes 3D arc/total, t=0 at the oldest living particle
// (hop, ribbon-jitter-v1 / align2-n6 Bake station 0). Particle mode
// passes t=0 at the head (newest). col_u is the
// same per-particle random factor used by the other TwoColors/TwoGradients
// modules; state 0/1/2/3 are Color, Gradient, TwoColors, TwoGradients.
inline void official_split_effect_eval_trail_over(const SplitEffectParticle& p, float t_head,
                                                  float col_u, float& r, float& g, float& b,
                                                  float& a) noexcept {
  r = p.trail_over_r;
  g = p.trail_over_g;
  b = p.trail_over_b;
  a = p.trail_over_color_a;
  const float t = std::clamp(t_head, 0.0f, 1.0f);
  const float u = std::clamp(col_u, 0.0f, 1.0f);
  if (p.trail_over_state == 1 &&
      (p.trail_over_color_count > 0 || p.trail_over_alpha_count > 0)) {
    official_split_effect_eval_gradient_mode(
        p.trail_over_color_begin, p.trail_over_color_count, p.trail_over_alpha_begin,
        p.trail_over_alpha_count, t, p.trail_over_mode, r, g, b, a);
    return;
  }
  if (p.trail_over_state == 2) {
    r = p.trail_over_r_min + (p.trail_over_r - p.trail_over_r_min) * u;
    g = p.trail_over_g_min + (p.trail_over_g - p.trail_over_g_min) * u;
    b = p.trail_over_b_min + (p.trail_over_b - p.trail_over_b_min) * u;
    a = p.trail_over_a_min + (p.trail_over_color_a - p.trail_over_a_min) * u;
    return;
  }
  if (p.trail_over_state == 3 &&
      (p.trail_over_color_count > 0 || p.trail_over_alpha_count > 0 ||
       p.trail_over_min_color_count > 0 || p.trail_over_min_alpha_count > 0)) {
    float r_max = 1.0f, g_max = 1.0f, b_max = 1.0f, a_max = 1.0f;
    float r_min = 1.0f, g_min = 1.0f, b_min = 1.0f, a_min = 1.0f;
    official_split_effect_eval_gradient_mode(
        p.trail_over_color_begin, p.trail_over_color_count, p.trail_over_alpha_begin,
        p.trail_over_alpha_count, t, p.trail_over_mode, r_max, g_max, b_max, a_max);
    official_split_effect_eval_gradient_mode(
        p.trail_over_min_color_begin, p.trail_over_min_color_count,
        p.trail_over_min_alpha_begin, p.trail_over_min_alpha_count, t,
        p.trail_over_min_mode, r_min, g_min, b_min, a_min);
    r = r_min + (r_max - r_min) * u;
    g = g_min + (g_max - g_min) * u;
    b = b_min + (b_max - b_min) * u;
    a = a_min + (a_max - a_min) * u;
  }
}

inline float official_split_effect_eval_trail_over_alpha(const SplitEffectParticle& p,
                                                         float t_head) noexcept {
  float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
  official_split_effect_eval_trail_over(p, t_head, 0.0f, r, g, b, a);
  return a;
}

inline const SplitEffectBurst* official_split_effect_bursts(const SplitEffectParticle& p,
                                                            uint8_t* count = nullptr) noexcept {
  using split_effect_catalog_detail::kSplitEffectBursts;
  if (count != nullptr) {
    *count = p.burst_count;
  }
  if (p.burst_count == 0) {
    return nullptr;
  }
  return kSplitEffectBursts + p.burst_begin;
}

inline uint16_t official_split_effect_subemitter_count() noexcept {
  using split_effect_catalog_detail::kSplitEffectSubEmitterCount;
  return kSplitEffectSubEmitterCount;
}

inline const SplitEffectSubEmitter* official_split_effect_subemitter_at(uint16_t i) noexcept {
  using split_effect_catalog_detail::kSplitEffectSubEmitterCount;
  using split_effect_catalog_detail::kSplitEffectSubEmitters;
  if (i >= kSplitEffectSubEmitterCount) {
    return nullptr;
  }
  return kSplitEffectSubEmitters + i;
}

inline const SplitEffectSubEmitter* official_split_effect_subemitters_for_parent(
    uint16_t parent_index, uint16_t* count = nullptr) noexcept {
  using split_effect_catalog_detail::kSplitEffectSubEmitterCount;
  using split_effect_catalog_detail::kSplitEffectSubEmitters;
  const auto* begin = kSplitEffectSubEmitters;
  const auto* end = begin + kSplitEffectSubEmitterCount;
  const auto* it = std::lower_bound(
      begin, end, parent_index,
      [](const SplitEffectSubEmitter& s, uint16_t p) { return s.parent_index < p; });
  if (it == end || it->parent_index != parent_index) {
    if (count != nullptr) {
      *count = 0;
    }
    return nullptr;
  }
  const auto* first = it;
  while (it != end && it->parent_index == parent_index) {
    ++it;
  }
  if (count != nullptr) {
    *count = static_cast<uint16_t>(it - first);
  }
  return first;
}

inline const char* official_split_effect_shape_name(uint8_t type) noexcept {
  switch (type) {
    case 0:
      return "Sphere";
    case 1:
      return "SphereShell";
    case 2:
      return "Hemisphere";
    case 3:
      return "HemisphereShell";
    case 4:
      return "Cone";
    case 5:
      return "Box";
    case 6:
      return "Mesh";
    case 7:
      return "ConeVolume";
    case 8:
      return "ConeVolumeShell";
    case 10:
      return "Circle";
    case 12:
      return "SingleSidedEdge";
    case 15:
      return "BoxShell";
    case 16:
      return "BoxEdge";
    case 17:
      return "Donut";
    case 18:
      return "Rectangle";
    default:
      return nullptr;
  }
}

// startLifetime / rateOverTime Curve (state 1) tables. Existing emit/evaluate
// keep using lifetime and rate_over_time scalars until those hops consume this.
inline float official_split_effect_eval_start_lifetime(const SplitEffectParticle& p,
                                                       float t01) noexcept {
  return official_split_effect_eval_curve_keys(p.life_curve_begin, p.life_curve_count, t01,
                                               p.lifetime);
}

inline float official_split_effect_eval_rate_over_time(const SplitEffectParticle& p,
                                                       float t01) noexcept {
  return official_split_effect_eval_curve_keys(p.rot_curve_begin, p.rot_curve_count, t01,
                                               p.rate_over_time);
}

// Full startColor key tables (Gradient state 1, RandomColor 4, TwoGradients 3).
// Unity 2022.3 MinMaxGradient.Evaluate(time, lerpFactor): time samples each
// gradient; lerpFactor blends min→max. Do not reuse one u for both.
// fill_live: Gradient/TwoGradients time = emit_u; RandomColor time = start_color_u.
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.MinMaxGradient.Evaluate.html
// Does not replace official_split_effect_eval_start_color — that hop still
// uses first-key / TwoColors for state 1/3.
inline void official_split_effect_eval_start_color_keys(const SplitEffectParticle& p, float time,
                                                        float lerpFactor, float& r, float& g,
                                                        float& b, float& a) noexcept {
  r = p.color_r;
  g = p.color_g;
  b = p.color_b;
  a = p.color_a;
  // TwoColors (2): Evaluate(time, lerpFactor) uses lerpFactor on min/max
  // Color, not the dummy maxGradient keys Unity always serializes.
  // live dump v2 (re-color-v5).
  if (p.start_color_state == 2) {
    official_split_effect_eval_start_color(p, lerpFactor, r, g, b, a);
    return;
  }
  if ((p.start_color_state == 1 || p.start_color_state == 4) && p.start_col_color_count > 0) {
    float ga = 1.0f;
    official_split_effect_eval_gradient_mode(p.start_col_color_begin, p.start_col_color_count,
                                             p.start_col_alpha_begin, p.start_col_alpha_count, time,
                                             p.start_col_mode, r, g, b, ga);
    if (p.start_col_alpha_count > 0) {
      a = ga;
    }
    return;
  }
  if (p.start_color_state == 3 && p.start_col_color_count > 0) {
    float r1 = 1.0f, g1 = 1.0f, b1 = 1.0f, a1 = 1.0f;
    official_split_effect_eval_gradient(p.start_col_color_begin, p.start_col_color_count,
                                        p.start_col_alpha_begin, p.start_col_alpha_count, time, r1,
                                        g1, b1, a1);
    if (p.start_col_min_color_count == 0 && p.start_col_min_alpha_count == 0) {
      r = r1;
      g = g1;
      b = b1;
      a = a1;
      return;
    }
    float r2 = 1.0f, g2 = 1.0f, b2 = 1.0f, a2 = 1.0f;
    official_split_effect_eval_gradient(p.start_col_min_color_begin, p.start_col_min_color_count,
                                        p.start_col_min_alpha_begin, p.start_col_min_alpha_count,
                                        time, r2, g2, b2, a2);
    const float u = std::clamp(lerpFactor, 0.0f, 1.0f);
    r = r2 + (r1 - r2) * u;
    g = g2 + (g1 - g2) * u;
    b = b2 + (b1 - b2) * u;
    a = a2 + (a1 - a2) * u;
    return;
  }
  official_split_effect_eval_start_color(p, time, r, g, b, a);
}

inline void official_split_effect_eval_start_color_keys(const SplitEffectParticle& p, float time,
                                                        float& r, float& g, float& b,
                                                        float& a) noexcept {
  official_split_effect_eval_start_color_keys(p, time, 0.0f, r, g, b, a);
}

// Trails Color-over-Lifetime full tables. t01 is particle age / startLifetime.
inline void official_split_effect_eval_trail_col(const SplitEffectParticle& p, float t01,
                                                 float col_u, float& r, float& g, float& b,
                                                 float& a) noexcept {
  r = p.trail_r;
  g = p.trail_g;
  b = p.trail_b;
  a = p.trail_a;
  if (p.trail_col_color_count > 0 || p.trail_col_alpha_count > 0) {
    official_split_effect_eval_gradient(p.trail_col_color_begin, p.trail_col_color_count,
                                        p.trail_col_alpha_begin, p.trail_col_alpha_count, t01, r, g,
                                        b, a);
    if (p.trail_col_state == 3 &&
        (p.trail_col_min_color_count > 0 || p.trail_col_min_alpha_count > 0)) {
      float r2 = 1.0f, g2 = 1.0f, b2 = 1.0f, a2 = 1.0f;
      official_split_effect_eval_gradient(p.trail_col_min_color_begin, p.trail_col_min_color_count,
                                          p.trail_col_min_alpha_begin, p.trail_col_min_alpha_count,
                                          t01, r2, g2, b2, a2);
      const float u = std::clamp(col_u, 0.0f, 1.0f);
      r = r2 + (r - r2) * u;
      g = g2 + (g - g2) * u;
      b = b2 + (b - b2) * u;
      a = a2 + (a - a2) * u;
    }
    return;
  }
  if (p.trail_col_state == 2) {
    const float u = std::clamp(col_u, 0.0f, 1.0f);
    r = p.trail_col_r_min + (p.trail_r - p.trail_col_r_min) * u;
    g = p.trail_col_g_min + (p.trail_g - p.trail_col_g_min) * u;
    b = p.trail_col_b_min + (p.trail_b - p.trail_col_b_min) * u;
    a = p.trail_col_a_min + (p.trail_a - p.trail_col_a_min) * u;
  }
}

inline void official_split_effect_eval_trail_col(const SplitEffectParticle& p, float t01, float& r,
                                                 float& g, float& b, float& a) noexcept {
  official_split_effect_eval_trail_col(p, t01, 0.0f, r, g, b, a);
}

}  // namespace wds::chart_render

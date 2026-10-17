#include <wds/chart_render/openwds_shuriken_bind.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace wds::chart_render {
namespace {

using unity::Color;
using unity::Gradient;
using unity::MinMaxCurve;
using unity::MinMaxGradient;
using unity::ParticleSystemCurveMode;
using unity::ParticleSystemGradientMode;
using unity::ParticleSystemRenderMode;
using unity::ParticleSystemRenderSpace;
using unity::ParticleSystemRenderer;
using unity::ParticleSystemShapeType;
using unity::ParticleSystemSimulationSpace;
using unity::RendererModule;

bool is_random_minmax(ParticleSystemCurveMode mode) noexcept {
  return mode == ParticleSystemCurveMode::TwoCurves ||
         mode == ParticleSystemCurveMode::TwoConstants;
}

// generate scalar_pair: catalog.json scalar / minScalar.
// hydrate: Constant → constant=scalar; TwoConstants → constant / constantMin;
// Curve / TwoCurves → curveMultiplier=scalar (constant stays 0). Do not
// evaluate Hermite into a new table.
void scalar_pair(const MinMaxCurve& c, float& mx, float& mn) noexcept {
  switch (c.mode) {
    case ParticleSystemCurveMode::Constant:
      mx = c.constant;
      mn = c.constant;
      break;
    case ParticleSystemCurveMode::TwoConstants:
      mx = c.constant;
      mn = c.constantMin;
      break;
    case ParticleSystemCurveMode::Curve:
    case ParticleSystemCurveMode::TwoCurves:
      mx = c.curveMultiplier;
      mn = c.constantMin;
      break;
    default:
      mx = c.constant;
      mn = c.constant;
      break;
  }
}

float scalar_max(const MinMaxCurve& c) noexcept {
  float mx = 0.0f;
  float mn = 0.0f;
  scalar_pair(c, mx, mn);
  return mx;
}

void pack_enabled(const MinMaxCurve& c, bool enabled, float& mx, float& mn,
                  uint8_t& random) noexcept {
  if (!enabled) {
    mx = 0.0f;
    mn = 0.0f;
    random = 0;
    return;
  }
  scalar_pair(c, mx, mn);
  random = is_random_minmax(c.mode) ? 1 : 0;
}

void zero_enabled(float& mx, float& mn, uint8_t& random) noexcept {
  mx = 0.0f;
  mn = 0.0f;
  random = 0;
}

bool material_has_butterfly(const ParticleSystemRenderer& psr,
                            const RendererModule& rend) noexcept {
  const auto has = [](const unity::Material* material) noexcept {
    return material != nullptr && material->name.find("Butterfly") != std::string::npos;
  };
  if (has(psr.material) || has(rend.material)) {
    return true;
  }
  for (unity::Material* material : psr.materials) {
    if (has(material)) {
      return true;
    }
  }
  for (unity::Material* material : rend.materials) {
    if (has(material)) {
      return true;
    }
  }
  return false;
}

template <typename T>
T pick_renderer(T psr, T rend, T def) noexcept {
  if (psr != def) {
    return psr;
  }
  if (rend != def) {
    return rend;
  }
  return psr;
}

void start_color_pack(const MinMaxGradient& sc, float& r, float& g, float& b, float& a,
                      float& rm, float& gm, float& bm, float& am, uint8_t& color_random,
                      uint8_t& state) noexcept {
  // generate start_color_pack. extract start_color always writes maxColor.
  state = static_cast<uint8_t>(sc.mode);
  const Color max_c = sc.color;
  const Color min_c = sc.colorMin;
  const bool has_max_keys = !sc.gradient.colorKeys.empty();
  const Color max_first = has_max_keys ? sc.gradient.colorKeys.front().color : max_c;
  const Color max_last = has_max_keys ? sc.gradient.colorKeys.back().color : max_c;

  if (sc.mode == ParticleSystemGradientMode::TwoGradients) {
    r = max_c.r;
    g = max_c.g;
    b = max_c.b;
    a = max_c.a;
    rm = min_c.r;
    gm = min_c.g;
    bm = min_c.b;
    am = min_c.a;
    color_random = 1;
    return;
  }
  if (sc.mode == ParticleSystemGradientMode::RandomColor) {
    const Color hi = has_max_keys ? max_first : max_c;
    const Color lo = has_max_keys ? max_last : hi;
    r = hi.r;
    g = hi.g;
    b = hi.b;
    rm = lo.r;
    gm = lo.g;
    bm = lo.b;
    // Unity 2022.3 Gradient.Evaluate: RGB from colorKeys, A from alphaKeys.
    // ColorKey.a is often 0 in the typetree; generate uses alphaKeys too.
    // ScriptReference/Gradient-alphaKeys.html
    const auto& aks = sc.gradient.alphaKeys;
    if (!aks.empty()) {
      a = aks.front().alpha;
      am = aks.back().alpha;
    } else {
      a = hi.a;
      am = lo.a;
    }
    color_random = 1;
    return;
  }
  if (sc.mode == ParticleSystemGradientMode::TwoColors) {
    // Unity 2022.3 Evaluate TwoColors: lerp(minColor, maxColor, lerpFactor).
    // Typetree always serializes a dummy white maxGradient; those keys are
    // not the TwoColors pair. live dump v2 (re-color-v5).
    r = max_c.r;
    g = max_c.g;
    b = max_c.b;
    a = max_c.a;
    rm = min_c.r;
    gm = min_c.g;
    bm = min_c.b;
    am = min_c.a;
    color_random = 1;
    return;
  }
  r = max_c.r;
  g = max_c.g;
  b = max_c.b;
  a = max_c.a;
  rm = max_c.r;
  gm = max_c.g;
  bm = max_c.b;
  am = max_c.a;
  color_random = 0;
}

void trail_over_alpha_pair(const MinMaxGradient& cot, float& a0, float& a1) noexcept {
  const int state = static_cast<int>(cot.mode);
  if (state == 1 || state == 2) {
    const auto& aks = cot.gradient.alphaKeys;
    if (!aks.empty()) {
      a0 = aks.front().alpha;
      a1 = aks.back().alpha;
      return;
    }
    const auto& cks = cot.gradient.colorKeys;
    if (!cks.empty()) {
      a0 = cks.front().color.a;
      a1 = cks.back().color.a;
      return;
    }
  }
  a0 = cot.color.a;
  a1 = cot.color.a;
}

void quat_axis_angle(const unity::Quaternion& q, float& angle_deg, float& ax, float& ay,
                     float& az) noexcept {
  // extract_split_effect_catalog.py quat_axis_angle
  float w = q.w;
  if (w < -1.0f) {
    w = -1.0f;
  }
  if (w > 1.0f) {
    w = 1.0f;
  }
  angle_deg = std::acos(std::fabs(w)) * (360.0f / 3.14159265358979323846f);
  const float s = std::sqrt(std::max(0.0f, 1.0f - w * w));
  if (s < 1e-6f) {
    ax = 0.0f;
    ay = 0.0f;
    az = 1.0f;
    return;
  }
  ax = q.x / s;
  ay = q.y / s;
  az = q.z / s;
}

void bind_shape_arc(const unity::ShapeModule& sh, SplitEffectParticle& dst) noexcept {
  const int st = static_cast<int>(sh.shapeType);
  // generate shape_multi_mode: Cone/ConeVolume/Circle/Donut use Arc Mode;
  // SingleSidedEdge uses Radius Mode on radius (PartSysShapeModule).
  if (st == 4 || st == 7 || st == 8 || st == 10 || st == 17) {
    dst.shape_arc_mode = static_cast<uint8_t>(sh.arcMode);
    dst.shape_arc_spread = sh.arcSpread;
    dst.shape_arc_speed = sh.arcSpeed;
    return;
  }
  if (st == 12) {
    // Edge Radius Mode lives on extract shape.radius.mode. ShapeModule has
    // no radius MultiMode members; hydrate_shape does not map them. Leave
    // oracle / zeros.
    return;
  }
  dst.shape_arc_mode = 0;
  dst.shape_arc_spread = 0.0f;
  dst.shape_arc_speed = 0.0f;
}

}  // namespace

void bind_from_unity(const unity::ParticleSystem& src, SplitEffectParticle& dst) noexcept {
  bind_from_unity(src, dst, nullptr);
}

void bind_from_unity(const unity::ParticleSystem& src, SplitEffectParticle& dst,
                     const SplitEffectParticle* catalog_oracle) noexcept {
  // live dump v1 (reports/re-subemit-work-report.md):
  // Sub-emitter *particle* fields use this hydrate+oracle bind (same as main).
  // Slot topology (parent/child/type/inherit) stays
  // official_split_effect_subemitters_for_parent.
  // Sidecar mapping (live dump v2 writer): modules.subEmitters =
  // src.subEmitters().enabled, not catalog nslots>0; childSystemPath is the
  // hydrate child GO or "" when the PPtr is null (10830_4 empty slots).
  // playOnAwake is serialized true on children; collect skips independent
  // evaluate() for slot targets. Custom simulation space is EVAL-GAP.
  // Burst table identity stays catalog burst_begin/count — hydrate cannot
  // rewrite kSplitEffectBursts. Do not mark this mapping as Unity 2022.3.
  if (catalog_oracle != nullptr) {
    dst = *catalog_oracle;
  } else {
    dst = SplitEffectParticle{};
  }
  // OpenWDS GameObject.m_IsActive. Unity 2022.3 activeSelf: inactive GO
  // disables ParticleSystem / ParticleSystemRenderer. Missing owner keeps
  // the oracle / default (1).
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/GameObject-activeSelf.html
  if (const unity::GameObject* go = src.gameObject()) {
    dst.go_active = go->activeSelf ? 1 : 0;
  }

  const auto& main = src.main();
  const auto& emission = src.emission();
  const auto& sh = src.shape();
  const auto& vol = src.velocityOverLifetime();
  const auto& force = src.forceOverLifetime();
  const auto& clamp = src.limitVelocityOverLifetime();
  const auto& col = src.colorOverLifetime();
  const auto& sol = src.sizeOverLifetime();
  const auto& rol = src.rotationOverLifetime();
  const auto& rbs = src.rotationBySpeed();
  const auto& noise = src.noise();
  const auto& inherit = src.inheritVelocity();
  const auto& tsa = src.textureSheetAnimation();
  const auto& trails = src.trails();
  const auto& custom = src.customData();
  const auto& psr = src.particleSystemRenderer();
  const auto& rend = src.renderer();

  float life_max = 0.0f;
  float life_min = 0.0f;
  scalar_pair(main.startLifetime, life_max, life_min);
  dst.lifetime = life_max;
  dst.lifetime_min = life_min;
  dst.life_random = is_random_minmax(main.startLifetime.mode) ? 1 : 0;

  float size_max = 0.0f;
  float size_min = 0.0f;
  scalar_pair(main.startSize, size_max, size_min);
  dst.size = size_max;
  dst.size_min = size_min;
  dst.size_random = is_random_minmax(main.startSize.mode) ? 1 : 0;
  dst.size_3d = main.startSize3D ? 1 : 0;
  if (main.startSize3D) {
    scalar_pair(main.startSizeY, dst.start_size_y, dst.start_size_y_min);
    scalar_pair(main.startSizeZ, dst.start_size_z, dst.start_size_z_min);
  }

  float speed_max = 0.0f;
  float speed_min = 0.0f;
  scalar_pair(main.startSpeed, speed_max, speed_min);
  dst.start_speed = speed_max;
  dst.start_speed_min = speed_min;
  dst.speed_random = is_random_minmax(main.startSpeed.mode) ? 1 : 0;

  start_color_pack(main.startColor, dst.color_r, dst.color_g, dst.color_b, dst.color_a,
                   dst.color_r_min, dst.color_g_min, dst.color_b_min, dst.color_a_min,
                   dst.color_random, dst.start_color_state);

  dst.gravity_modifier = scalar_max(main.gravityModifier);
  int max_p = main.maxParticles;
  if (max_p < 1) {
    max_p = 1;
  }
  if (max_p > 10000) {
    max_p = 10000;
  }
  dst.max_particles = static_cast<uint16_t>(max_p);
  dst.length_in_sec = main.duration;
  dst.start_delay = scalar_max(main.startDelay);
  dst.simulation_space = static_cast<uint8_t>(main.simulationSpace);
  dst.scaling_mode = static_cast<uint8_t>(main.scalingMode);
  float sim_speed = main.simulationSpeed;
  if (sim_speed <= 1e-6f) {
    sim_speed = 1.0f;
  }
  dst.simulation_speed = sim_speed;
  dst.auto_random_seed = src.get_useAutoRandomSeed() ? 1 : 0;
  dst.random_seed = src.get_randomSeed();

  float rz_max = 0.0f;
  float rz_min = 0.0f;
  float rx_max = 0.0f;
  float rx_min = 0.0f;
  float ry_max = 0.0f;
  float ry_min = 0.0f;
  scalar_pair(main.startRotation, rz_max, rz_min);
  scalar_pair(main.startRotationX, rx_max, rx_min);
  scalar_pair(main.startRotationY, ry_max, ry_min);
  if (!is_random_minmax(main.startRotation.mode)) {
    rz_min = rz_max;
  }
  if (!is_random_minmax(main.startRotationX.mode)) {
    rx_min = rx_max;
  }
  if (!is_random_minmax(main.startRotationY.mode)) {
    ry_min = ry_max;
  }
  dst.rot_z = rz_max;
  dst.rot_z_min = rz_min;
  dst.rot_z_random = is_random_minmax(main.startRotation.mode) ? 1 : 0;
  dst.rot_x = rx_max;
  dst.rot_x_min = rx_min;
  dst.rot_y = ry_max;
  dst.rot_y_min = ry_min;
  dst.rotation_3d = main.startRotation3D ? 1 : 0;

  // Unity 2022.3 EmissionModule.enabled: module off emits neither Rate
  // over Time nor Rate over Distance. Burst table identity still follows
  // the oracle slice (hydrate cannot rewrite kSplitEffectBursts).
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.EmissionModule-enabled.html
  const float rot_rate = emission.enabled ? scalar_max(emission.rateOverTime) : 0.0f;
  float rod_max = 0.0f;
  float rod_min = 0.0f;
  if (emission.enabled) {
    scalar_pair(emission.rateOverDistance, rod_max, rod_min);
  }
  dst.rate_over_time = rot_rate;
  dst.rate_over_distance = rod_max;
  dst.rate_over_distance_min = rod_min;
  dst.rod_random =
      emission.enabled && is_random_minmax(emission.rateOverDistance.mode) ? 1 : 0;
  const int burst_n = emission.burstCount();
  const uint16_t oracle_burst_begin =
      catalog_oracle != nullptr ? catalog_oracle->burst_begin : dst.burst_begin;
  const uint8_t oracle_burst_count =
      catalog_oracle != nullptr ? catalog_oracle->burst_count : dst.burst_count;
  const float oracle_emission = catalog_oracle != nullptr ? catalog_oracle->emission : dst.emission;
  // live dump v2: official_split_effect_bursts reads kSplitEffectBursts +
  // burst_begin. Hydrate m_Bursts cannot enter that table. Keep the catalog
  // slice when the oracle has bursts (10311 / 11020_2 Birth+burst=1). A
  // hydrate-only count with begin 0 would read the wrong rows.
  if (oracle_burst_count > 0) {
    dst.burst_begin = oracle_burst_begin;
    dst.burst_count = oracle_burst_count;
  } else {
    dst.burst_begin = 0;
    dst.burst_count = 0;
  }
  if (burst_n > 0 || oracle_burst_count > 0) {
    if (rot_rate > 0.0f) {
      dst.emission_kind = static_cast<uint8_t>(SplitEffectEmission::RateOverTime);
      dst.emission = rot_rate;
    } else if (rod_max > 0.0f) {
      dst.emission_kind = static_cast<uint8_t>(SplitEffectEmission::RateOverDistance);
      dst.emission = rod_max;
    } else {
      dst.emission_kind = static_cast<uint8_t>(SplitEffectEmission::Burst);
      dst.emission = burst_n > 0 ? scalar_max(emission.bursts.front().count) : oracle_emission;
    }
  } else if (rot_rate > 0.0f) {
    dst.emission_kind = static_cast<uint8_t>(SplitEffectEmission::RateOverTime);
    dst.emission = rot_rate;
  } else if (rod_max > 0.0f) {
    dst.emission_kind = static_cast<uint8_t>(SplitEffectEmission::RateOverDistance);
    dst.emission = rod_max;
  } else {
    dst.emission_kind = static_cast<uint8_t>(SplitEffectEmission::None);
    dst.emission = 0.0f;
  }

  dst.shape_type = static_cast<uint8_t>(sh.shapeType);
  dst.shape_radius = sh.radius;
  dst.shape_scale_x = sh.scale.x;
  dst.shape_scale_y = sh.scale.y;
  dst.shape_scale_z = sh.scale.z;
  dst.shape_pos_x = sh.position.x;
  dst.shape_pos_y = sh.position.y;
  dst.shape_pos_z = sh.position.z;
  dst.shape_angle = sh.angle;
  dst.shape_arc = sh.arc;
  dst.shape_radius_thickness = sh.radiusThickness;
  dst.shape_length = sh.length;
  dst.shape_rot_x = sh.rotation.x;
  dst.shape_rot_y = sh.rotation.y;
  dst.shape_rot_z = sh.rotation.z;
  dst.random_direction_amount = sh.randomDirectionAmount;
  dst.spherical_direction_amount = sh.sphericalDirectionAmount;
  dst.align_to_direction = sh.alignToDirection ? 1 : 0;
  dst.random_position_amount = sh.randomPositionAmount;
  dst.shape_enabled = sh.enabled ? 1 : 0;
  bind_shape_arc(sh, dst);

  if (vol.enabled) {
    scalar_pair(vol.x, dst.vel_x, dst.vel_x_min);
    scalar_pair(vol.y, dst.vel_y, dst.vel_y_min);
    scalar_pair(vol.z, dst.vel_z, dst.vel_z_min);
    dst.vel_x_random = is_random_minmax(vol.x.mode) ? 1 : 0;
    dst.vel_y_random = is_random_minmax(vol.y.mode) ? 1 : 0;
    dst.vel_z_random = is_random_minmax(vol.z.mode) ? 1 : 0;
    dst.orbital_x = scalar_max(vol.orbitalX);
    dst.orbital_y = scalar_max(vol.orbitalY);
    dst.orbital_z = scalar_max(vol.orbitalZ);
    dst.orbital_offset_x = scalar_max(vol.orbitalOffsetX);
    dst.orbital_offset_y = scalar_max(vol.orbitalOffsetY);
    dst.orbital_offset_z = scalar_max(vol.orbitalOffsetZ);
    dst.radial = scalar_max(vol.radial);
    dst.vol_in_world = vol.space == ParticleSystemSimulationSpace::World ? 1 : 0;
    // live dump v1 hop: enabled speedModifier including 0. Identity 1 only
    // when the module is off. Do not rewrite authored 0 → 1.
    dst.vol_speed_mod = scalar_max(vol.speedModifier);
  } else {
    dst.vel_x = dst.vel_x_min = 0.0f;
    dst.vel_y = dst.vel_y_min = 0.0f;
    dst.vel_z = dst.vel_z_min = 0.0f;
    dst.vel_x_random = dst.vel_y_random = dst.vel_z_random = 0;
    dst.orbital_x = dst.orbital_y = dst.orbital_z = 0.0f;
    dst.orbital_offset_x = dst.orbital_offset_y = dst.orbital_offset_z = 0.0f;
    dst.radial = 0.0f;
    dst.vol_in_world = 0;
    dst.vol_speed_mod = 1.0f;
  }

  dst.force_enabled = force.enabled ? 1 : 0;
  dst.force_in_world =
      (force.enabled && force.space == ParticleSystemSimulationSpace::World) ? 1 : 0;
  dst.force_randomize = (force.enabled && force.randomized) ? 1 : 0;
  pack_enabled(force.x, force.enabled, dst.force_x, dst.force_x_min, dst.force_x_random);
  pack_enabled(force.y, force.enabled, dst.force_y, dst.force_y_min, dst.force_y_random);
  pack_enabled(force.z, force.enabled, dst.force_z, dst.force_z_min, dst.force_z_random);

  dst.clamp_enabled = clamp.enabled ? 1 : 0;
  dst.clamp_separate = (clamp.enabled && clamp.separateAxes) ? 1 : 0;
  dst.clamp_in_world =
      (clamp.enabled && clamp.space == ParticleSystemSimulationSpace::World) ? 1 : 0;
  dst.clamp_mul_size = (clamp.enabled && clamp.multiplyBySize) ? 1 : 0;
  dst.clamp_mul_vel = (clamp.enabled && clamp.multiplyByVelocity) ? 1 : 0;
  if (clamp.enabled) {
    dst.clamp_dampen = scalar_max(clamp.dampen);
    pack_enabled(clamp.limit, true, dst.clamp_speed, dst.clamp_speed_min,
                 dst.clamp_speed_random);
    dst.clamp_x = scalar_max(clamp.limitX);
    dst.clamp_y = scalar_max(clamp.limitY);
    dst.clamp_z = scalar_max(clamp.limitZ);
    pack_enabled(clamp.drag, true, dst.clamp_drag, dst.clamp_drag_min, dst.clamp_drag_random);
  } else {
    dst.clamp_dampen = 0.0f;
    zero_enabled(dst.clamp_speed, dst.clamp_speed_min, dst.clamp_speed_random);
    dst.clamp_x = dst.clamp_y = dst.clamp_z = 0.0f;
    zero_enabled(dst.clamp_drag, dst.clamp_drag_min, dst.clamp_drag_random);
  }

  dst.col_minmax_state = static_cast<uint8_t>(col.color.mode);

  if (sol.enabled && sol.separateAxes) {
    dst.sol_y = scalar_max(sol.y);
  } else {
    dst.sol_y = 1.0f;
  }

  dst.rot_ol_enabled = rol.enabled ? 1 : 0;
  dst.rot_ol_separate = (rol.enabled && rol.separateAxes) ? 1 : 0;
  pack_enabled(rol.z, rol.enabled, dst.rot_ol, dst.rot_ol_min, dst.rot_ol_random);
  pack_enabled(rol.x, dst.rot_ol_separate != 0, dst.rot_ol_x, dst.rot_ol_x_min,
               dst.rot_ol_x_random);
  pack_enabled(rol.y, dst.rot_ol_separate != 0, dst.rot_ol_y, dst.rot_ol_y_min,
               dst.rot_ol_y_random);

  dst.rbs_enabled = rbs.enabled ? 1 : 0;
  dst.rbs_separate = (rbs.enabled && rbs.separateAxes) ? 1 : 0;
  pack_enabled(rbs.x, rbs.enabled, dst.rbs_x, dst.rbs_x_min, dst.rbs_x_random);
  pack_enabled(rbs.y, rbs.enabled, dst.rbs_y, dst.rbs_y_min, dst.rbs_y_random);
  pack_enabled(rbs.z, rbs.enabled, dst.rbs_z, dst.rbs_z_min, dst.rbs_z_random);
  if (rbs.enabled) {
    dst.rbs_range_x = rbs.range.constantMin;
    dst.rbs_range_y = rbs.range.constant;
  } else {
    dst.rbs_range_x = 0.0f;
    dst.rbs_range_y = 1.0f;
  }

  dst.inherit_enabled = inherit.enabled ? 1 : 0;
  if (inherit.enabled) {
    dst.inherit_mode = static_cast<uint8_t>(inherit.mode);
    pack_enabled(inherit.curve, true, dst.inherit_mult, dst.inherit_mult_min,
                 dst.inherit_random);
  } else {
    dst.inherit_mode = 0;
    zero_enabled(dst.inherit_mult, dst.inherit_mult_min, dst.inherit_random);
  }

  if (noise.enabled) {
    dst.noise_strength = scalar_max(noise.strength);
    dst.noise_frequency = scalar_max(noise.frequency);
    int oct = noise.octaveCount;
    if (oct < 1) {
      oct = 1;
    }
    if (oct > 8) {
      oct = 8;
    }
    dst.noise_octaves = static_cast<uint8_t>(oct);
    dst.noise_octave_scale = noise.octaveScale;
    dst.noise_octave_mult = noise.octaveMultiplier;
    dst.noise_scroll = scalar_max(noise.scrollSpeed);
    // Unity 2022.3 Position Amount is a multiplier; 0 disables position
    // curl (same Hamming-1 iso_noise_size/rot as eval_noise_velocity).
    // Do not rewrite enabled 0 → 1.
    dst.noise_position_amount = scalar_max(noise.positionAmount);
    dst.noise_rotation_amount = scalar_max(noise.rotationAmount);
    dst.noise_size_amount = scalar_max(noise.sizeAmount);
    dst.noise_separate_axes = noise.separateAxes ? 1 : 0;
    dst.noise_strength_y = noise.separateAxes ? scalar_max(noise.strengthY) : 0.0f;
    dst.noise_strength_z = noise.separateAxes ? scalar_max(noise.strengthZ) : 0.0f;
    dst.noise_damping = noise.damping ? 1 : 0;
  } else {
    dst.noise_strength = 0.0f;
    dst.noise_frequency = 0.0f;
    dst.noise_octaves = 1;
    dst.noise_octave_scale = 2.0f;
    dst.noise_octave_mult = 0.5f;
    dst.noise_scroll = 0.0f;
    dst.noise_position_amount = 0.0f;
    dst.noise_rotation_amount = 0.0f;
    dst.noise_size_amount = 0.0f;
    dst.noise_separate_axes = 0;
    dst.noise_strength_y = 0.0f;
    dst.noise_strength_z = 0.0f;
    dst.noise_damping = 0;
  }

  if (trails.enabled) {
    dst.trail_ratio = trails.ratio;
    dst.trail_lifetime = scalar_max(trails.lifetime);
    dst.trail_width = scalar_max(trails.widthOverTrail);
    dst.trail_r = trails.colorOverLifetime.color.r;
    dst.trail_g = trails.colorOverLifetime.color.g;
    dst.trail_b = trails.colorOverLifetime.color.b;
    dst.trail_a = trails.colorOverLifetime.color.a;
    dst.trail_mode = static_cast<uint8_t>(trails.mode);
    dst.trail_die_with = trails.dieWithParticles ? 1 : 0;
    dst.trail_size_affects_width = trails.sizeAffectsWidth ? 1 : 0;
    dst.trail_min_vertex = trails.minVertexDistance;
    dst.trail_inherit_particle_color = trails.inheritParticleColor ? 1 : 0;
    dst.trail_width_state = static_cast<uint8_t>(trails.widthOverTrail.mode);
    scalar_pair(trails.widthOverTrail, dst.trail_width, dst.trail_width_min);
    dst.trail_over_state = static_cast<uint8_t>(trails.colorOverTrail.mode);
    dst.trail_over_r = trails.colorOverTrail.color.r;
    dst.trail_over_g = trails.colorOverTrail.color.g;
    dst.trail_over_b = trails.colorOverTrail.color.b;
    dst.trail_over_color_a = trails.colorOverTrail.color.a;
    dst.trail_over_r_min = trails.colorOverTrail.colorMin.r;
    dst.trail_over_g_min = trails.colorOverTrail.colorMin.g;
    dst.trail_over_b_min = trails.colorOverTrail.colorMin.b;
    dst.trail_over_a_min = trails.colorOverTrail.colorMin.a;
    trail_over_alpha_pair(trails.colorOverTrail, dst.trail_over_a0, dst.trail_over_a1);
    dst.trail_col_state = static_cast<uint8_t>(trails.colorOverLifetime.mode);
    dst.trail_col_r_min = trails.colorOverLifetime.colorMin.r;
    dst.trail_col_g_min = trails.colorOverLifetime.colorMin.g;
    dst.trail_col_b_min = trails.colorOverLifetime.colorMin.b;
    dst.trail_col_a_min = trails.colorOverLifetime.colorMin.a;
  } else {
    dst.trail_ratio = 0.0f;
    dst.trail_lifetime = 0.0f;
    dst.trail_width = 0.0f;
    dst.trail_r = 1.0f;
    dst.trail_g = 1.0f;
    dst.trail_b = 1.0f;
    dst.trail_a = 1.0f;
    dst.trail_mode = 0;
    dst.trail_die_with = 1;
    dst.trail_size_affects_width = 1;
    dst.trail_min_vertex = 0.0f;
    dst.trail_inherit_particle_color = 0;
    dst.trail_width_state = 0;
    dst.trail_width_min = 0.0f;
    dst.trail_over_a0 = 1.0f;
    dst.trail_over_a1 = 1.0f;
    dst.trail_over_state = 0;
    dst.trail_over_r = 1.0f;
    dst.trail_over_g = 1.0f;
    dst.trail_over_b = 1.0f;
    dst.trail_over_color_a = 1.0f;
    dst.trail_over_r_min = 1.0f;
    dst.trail_over_g_min = 1.0f;
    dst.trail_over_b_min = 1.0f;
    dst.trail_over_a_min = 1.0f;
    dst.trail_col_state = 0;
    dst.trail_col_r_min = 1.0f;
    dst.trail_col_g_min = 1.0f;
    dst.trail_col_b_min = 1.0f;
    dst.trail_col_a_min = 1.0f;
  }

  scalar_pair(tsa.startFrame, dst.start_frame, dst.start_frame_min);
  dst.start_frame_random = is_random_minmax(tsa.startFrame.mode) ? 1 : 0;
  scalar_pair(tsa.frameOverTime, dst.fot, dst.fot_min);
  dst.fot_random = is_random_minmax(tsa.frameOverTime.mode) ? 1 : 0;

  if (custom.enabled) {
    const MinMaxCurve c1x = custom.GetVector(unity::ParticleSystemCustomData::Custom1, 0);
    const MinMaxCurve c1y = custom.GetVector(unity::ParticleSystemCustomData::Custom1, 1);
    scalar_pair(c1x, dst.custom1_x, dst.custom1_x_min);
    dst.custom1_x_random = is_random_minmax(c1x.mode) ? 1 : 0;
    dst.custom1_y = scalar_max(c1y);
  } else {
    dst.custom1_x = 0.0f;
    dst.custom1_y = 0.0f;
    dst.custom1_x_min = 0.0f;
    dst.custom1_x_random = 0;
  }

  dst.render_mode = static_cast<uint8_t>(
      pick_renderer(psr.renderMode, rend.renderMode, ParticleSystemRenderMode::Billboard));
  dst.render_alignment = static_cast<uint8_t>(
      pick_renderer(psr.alignment, rend.alignment, ParticleSystemRenderSpace::View));
  dst.length_scale = pick_renderer(psr.lengthScale, rend.lengthScale, 2.0f);
  dst.velocity_scale = pick_renderer(psr.velocityScale, rend.velocityScale, 0.0f);
  float cam_vs = pick_renderer(psr.cameraVelocityScale, rend.cameraVelocityScale, 0.0f);
  if (!std::isfinite(cam_vs)) {
    cam_vs = 0.0f;
  }
  dst.camera_velocity_scale = cam_vs;
  // PSR m_MaxParticleSize authored viewport fraction. Do not compute AABB.
  dst.max_particle_size = pick_renderer(psr.maxParticleSize, rend.maxParticleSize, 0.5f);
  dst.pivot_x = psr.pivot.x;
  dst.pivot_y = psr.pivot.y;
  dst.pivot_z = psr.pivot.z;
  dst.flip_x = psr.flip.x;
  dst.flip_y = psr.flip.y;
  int order = pick_renderer(psr.sortingOrder, rend.sortingOrder, 0);
  if (order < 0) {
    order = 0;
  }
  if (order > 255) {
    order = 255;
  }
  dst.sorting_order = static_cast<uint8_t>(order);
  dst.freeform_stretching =
      pick_renderer(psr.freeformStretching, rend.freeformStretching, false) ? 1 : 0;
  dst.rotate_with_stretch =
      pick_renderer(psr.rotateWithStretchDirection, rend.rotateWithStretchDirection, false) ? 1
                                                                                            : 0;

  if (const unity::Transform* xf = src.transform()) {
    dst.local_pos_x = xf->localPosition.x;
    dst.local_pos_y = xf->localPosition.y;
    dst.local_pos_z = xf->localPosition.z;
    dst.local_scale_x = xf->localScale.x;
    dst.local_scale_y = xf->localScale.y;
    dst.local_scale_z = xf->localScale.z;
    dst.local_quat_x = xf->localRotation.x;
    dst.local_quat_y = xf->localRotation.y;
    dst.local_quat_z = xf->localRotation.z;
    dst.local_quat_w = xf->localRotation.w;
    quat_axis_angle(xf->localRotation, dst.local_angle_deg, dst.local_axis_x, dst.local_axis_y,
                    dst.local_axis_z);
  }

  uint16_t flags = 0;
  if (main.loop) {
    flags |= kSplitEffectFlagLooping;
  }
  if (col.enabled) {
    flags |= kSplitEffectFlagColorOverLifetime;
  }
  if (vol.enabled) {
    flags |= kSplitEffectFlagVelocity;
  }
  if (noise.enabled) {
    flags |= kSplitEffectFlagNoise;
  }
  if (trails.enabled) {
    flags |= kSplitEffectFlagTrails;
  }
  if (sol.enabled) {
    flags |= kSplitEffectFlagSizeOverLifetime;
  }
  if (material_has_butterfly(psr, rend)) {
    flags |= kSplitEffectFlagButterfly;
  }
  if (catalog_oracle != nullptr) {
    flags |= static_cast<uint16_t>(catalog_oracle->flags &
                                   (kSplitEffectFlagButterfly | kSplitEffectFlagCubeMesh |
                                    kSplitEffectFlagRingMesh));
  }
  dst.flags = flags;
}

}  // namespace wds::chart_render

#include <wds/chart_render/shuriken/evaluate.hpp>
#include <wds/chart_render/shuriken/minmax.hpp>

#include "internal.hpp"
#include "split_collect_pool.hpp"
#include "split_eval_cache.hpp"
#include "split_eval_carry.hpp"
#include "split_eval_persist.hpp"
#include "split_wall_profile.hpp"

#include <wds/common/page_touch.hpp>
#include <wds/common/perf_stages.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/official_split_x.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace wds::chart_render::shuriken {

namespace {

RuntimeCapabilityObserver g_runtime_capability_observer = nullptr;

// live dump v2: OpenWDS LiveParticleDump.C32 is Color32(c) then byte/255.
// Unity implicit Color→Color32 is Clamp01 and Round. Not a 2022.3 equation.
float color32_u8(float c) noexcept {
  const float x = std::clamp(c, 0.0f, 1.0f);
  return std::round(x * 255.0f) / 255.0f;
}

// hop count-fix-v1, not 2022.3. Birth InheritColor children spawned at the
// parent's birth instant inherit parent GetCurrentColor at the parent's age
// at the end of the 1/60 Play frame that contains that birth (same grid as
// RoD 帧末求值 / split_effect_next_frame_play_t). Catalog prediction vs
// Unity child start.a: 11020_2 / 1030/10311 / 10130/10311 mean ratio
// 0.928 / 0.946 / 0.985. Falsified: parent startColor (energy 3.1×).
// Death + InheritColor: parent pose stays last-alive (age ≈ life), even
// when the child birth is snapped to death_emit_t. Later-frame attached
// Birth children still use pose_age = event_t − parent_birth.
float inherit_color_age_at_birth_frame_end(float parent_birth_t) noexcept {
  const float fe = split_effect_next_frame_play_t(parent_birth_t);
  const float age = fe - parent_birth_t;
  return age > 0.0f ? age : 0.0f;
}

void eval_particle_sol_at_age(const SplitEffectParticle& p, const ParticleState& st, float age,
                              float& sol_x, float& sol_y, float& sol_z) noexcept {
  sol_x = sol_y = sol_z = 1.0f;
  if ((p.flags & kSplitEffectFlagSizeOverLifetime) == 0) {
    return;
  }
  const float t01 = st.life > 1e-6f ? age / st.life : 1.0f;
  // Non-separateAxes keeps one u (salt 21). separateAxes TwoCurves: live dump
  // dump-pending-v2 11350 hop (not 2022.3) — independent stream like VOL.
  // Catalog has 0 SOL TwoConstants; ROL stays shared (DUMP-v1 C4-01).
  const float sol_u = u01(mix3(st.seed, 21, 1u));
  float sol_ux = sol_u;
  float sol_uy = sol_u;
  float sol_uz = sol_u;
  const bool sol_separate = p.sol_y_count > 0 || p.sol_z_count > 0 ||
                            (p.sol_y > 1e-4f && std::fabs(p.sol_y - 1.0f) > 1e-4f);
  const bool sol_two_curves =
      p.sol_min_count > 0 || p.sol_y_min_count > 0 || p.sol_z_min_count > 0;
  if (sol_separate && sol_two_curves) {
    rng_fill_u01_3(st.seed, 1u, kSaltSolAxes, sol_ux, sol_uy, sol_uz);
  }
  eval_sol_axes(p, t01, sol_ux, sol_uy, sol_uz, sol_x, sol_y, sol_z);
}

// Same frame-end parent age as InheritColor (hop, not 2022.3): a parent SOL
// keyed to 0 at age 0 would otherwise hand InheritSize children size 0
// (100101 / 11770_10 / 11770_14; align2-v1 Unity size q10/q50/q90 vs native
// mean log ratio −0.22). Falsified: parent startSize without SOL (+2.06, 8×).
void eval_particle_size_at_age(const SplitEffectParticle& p, const ParticleState& st, float age,
                               float& sx, float& sy, float& sz) noexcept {
  float sol_x = 1.0f, sol_y = 1.0f, sol_z = 1.0f;
  eval_particle_sol_at_age(p, st, age, sol_x, sol_y, sol_z);
  sx = st.size * sol_x;
  sy = st.size_y * sol_y;
  sz = st.size_z * sol_z;
}

// Last-alive parent age for Death pose. Tiny Constant lives (10220 GO1
// 0.0001) must not collapse to 0 via life−1e-4. live dump dump-pending-v1
// hop, not 2022.3.
float death_parent_pose_age(float life) noexcept {
  if (life <= 1e-8f) {
    return 0.0f;
  }
  return life * (1.0f - 1e-4f);
}

// hop: Custom Data curves are sampled at the start of the current
// simulation step, not at the dump age. Unity 2022.3
// Time.maximumParticleDeltaTime default 0.03 slices hitch frames; OpenWDS
// GameRuntime.ApplyFrameRate is 60 fps (same 1/60 as the LineHight
// animator-lag hop). live dump align2-u1-unity-v1 3011 1558 particles:
// best δ=0.0249–0.0256 (stuttered run, ~100 ms hitch → 4×0.025);
// align2-u3-unity-v1 δ≈1/60. t01 = max(age − kSplitEffectFrameSec, 0) /
// startLifetime. Constants / TwoConstants ignore t01. Do not apply to
// COL / SOL / ROL / VOL. Do not label 2022.3.
float custom_data_t01(float age, float life) noexcept {
  if (life <= 1e-6f) {
    return 1.0f;
  }
  const float a = age > kSplitEffectFrameSec ? age - kSplitEffectFrameSec : 0.0f;
  return a / life;
}

void eval_particle_color_at_age(const SplitEffectParticle& p, const ParticleState& st, float age,
                                float& r, float& g, float& b, float& a) noexcept {
  const float t01 = st.life > 1e-6f ? std::clamp(age / st.life, 0.0f, 1.0f) : 1.0f;
  float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
  eval_col(p, t01, st.col_u, cr, cg, cb, ca);
  float sr = p.color_r;
  float sg = p.color_g;
  float sb = p.color_b;
  float sa = p.color_a;
  float start_color_time = st.start_color_u;
  if (p.start_color_state == 1 || p.start_color_state == 3) {
    start_color_time = st.emit_u;
  }
  official_split_effect_eval_start_color_keys(p, start_color_time, u01(mix3(st.seed, 16, 1u)), sr,
                                              sg, sb, sa);
  if ((st.inherit_props & kSplitEffectSubInheritColor) != 0) {
    sr *= st.inherit_r;
    sg *= st.inherit_g;
    sb *= st.inherit_b;
    sa *= st.inherit_a;
  }
  r = color32_u8(color32_u8(sr) * cr);
  g = color32_u8(color32_u8(sg) * cg);
  b = color32_u8(color32_u8(sb) * cb);
  a = color32_u8(color32_u8(sa) * ca);
}

// Pose-independent draw alpha (same ops as fill_live). Used to drop before
// integrate. Trails / endpoint-birth still force-keep in the caller.
float live_draw_alpha(const ParticleState& st, float age) {
  const SplitEffectParticle& p = *st.p;
  const float t01 = st.life > 1e-6f ? age / st.life : 1.0f;
  const float cd_t01 = custom_data_t01(age, st.life);
  float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
  eval_col(p, t01, st.col_u, cr, cg, cb, ca);
  float sr = p.color_r;
  float sg = p.color_g;
  float sb = p.color_b;
  float sa = p.color_a;
  float start_color_time = st.start_color_u;
  if (p.start_color_state == 1 || p.start_color_state == 3) {
    start_color_time = st.emit_u;
  }
  official_split_effect_eval_start_color_keys(p, start_color_time, u01(mix3(st.seed, 16, 1u)), sr,
                                              sg, sb, sa);
  if ((st.inherit_props & kSplitEffectSubInheritColor) != 0) {
    sa *= st.inherit_a;
  }
  sa = color32_u8(sa);
  float a = color32_u8(sa * ca);
  float custom1_x = p.custom1_x;
  if (p.custom1_x_count > 0) {
    custom1_x = official_split_effect_eval_curve_keys(p.custom1_x_begin, p.custom1_x_count, cd_t01,
                                                     p.custom1_x);
  }
  if (p.custom1_x_random != 0) {
    float lo = p.custom1_x_min;
    if (p.custom1_x_min_count > 0) {
      lo = official_split_effect_eval_curve_keys(p.custom1_x_min_begin, p.custom1_x_min_count,
                                                 cd_t01, p.custom1_x_min);
    }
    // live dump dump-pending-v1 (11220_5): Custom1 independent of startColor salt 16.
    custom1_x = lo + (custom1_x - lo) * u01(mix3(st.seed, kSaltCustom1, 1u));
  }
  if ((p.flags & kSplitEffectFlagButterfly) == 0) {
    a *= custom1_x * 0.5f + 1.0f;
  }
  return a;
}

bool primary_alpha_keep(const SplitEffectParticle& p, const EmitContext& ctx, float raw_age,
                        float draw_a) noexcept {
  (void)p;
  (void)ctx;
  (void)raw_age;
  (void)draw_a;
  // hop count-fix-v1, not 2022.3. Unity ParticleSystem.particleCount /
  // GetParticles includes Color32.a=0 rows (11400 start.a=0 always;
  // 10771 COL alpha holes). Native used to drop draw_a<0.001 before
  // integrate (trails / endpoint-birth were the only force-keeps), so
  // COUNT n collapsed and leftover-visible α mean went up by ~1/n.
  // Draw still uses current_a (invisible). Not a 2022.3 equation.
  // do_fill stays 1: golden full hashes pose/size/color/velocity of a=0
  // rows the renderer never reads. Skipping appearance would cut 11400
  // but change those groups — see PERF-v1 11400-ALPHA0 proposal.
  return true;
}

bool child_may_rod(const SplitEffectParticle& p) noexcept {
  return p.rate_over_distance > 1e-6f || p.rod_curve_count > 0 || p.rod_min_curve_count > 0 ||
         (p.rod_random != 0 && p.rate_over_distance_min > 1e-6f);
}

}  // namespace

RuntimeCapabilityStatus runtime_capability_status(const SplitEffectParticle& p) noexcept {
  return p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Custom)
             ? RuntimeCapabilityStatus::CustomSimulationSpaceUnavailable
             : RuntimeCapabilityStatus::Supported;
}

void set_runtime_capability_observer(RuntimeCapabilityObserver observer) noexcept {
  g_runtime_capability_observer = observer;
}

bool observers_installed() noexcept {
  return g_runtime_capability_observer != nullptr || integration_observers_installed();
}

float fade_scale_at_play_t(double t) noexcept {
  const double anim_t = t - static_cast<double>(kSplitEffectAnimatorLagSec);
  if (anim_t <= 0.0) {
    return 0.0f;
  }
  if (anim_t >= static_cast<double>(wds::chart_editor::kOfficialSplitFadeInSec)) {
    return 1.0f;
  }
  return wds::chart_editor::official_split_fade_in_scale(static_cast<float>(anim_t));
}

float applied_birth_fade(const EmitContext& ctx, float birth_t) noexcept {
  if (!ctx.parent_motion.simulation_play_clock_candidate) {
    return fade_scale_at_play_t(birth_t);
  }
  const double visual_birth_t = static_cast<double>(birth_t) +
                                ctx.parent_motion.simulation_play_origin_seconds -
                                ctx.parent_motion.visual_fade_origin_seconds;
  return fade_scale_at_play_t(visual_birth_t);
}

float invert_fade_in_scale(float scale) noexcept {
  const float s = std::clamp(scale, 0.0f, 1.0f);
  if (s <= 0.0f) {
    return 0.0f;
  }
  if (s >= 1.0f) {
    return 1.0f;
  }
  uint32_t bits = 0;
  std::memcpy(&bits, &s, sizeof(bits));
  // Same 20-iter bisection, memoized by exact scale bits. Constant-RoD remake
  // hits the same k→scale every frame after fade-in (10380 play ~6% of sample).
  thread_local uint32_t memo_bits[4096]{};
  thread_local float memo_t[4096]{};
  thread_local uint8_t memo_used[4096]{};
  const unsigned slot = static_cast<unsigned>(bits * 2654435761u) & 4095u;
  if (memo_used[slot] != 0 && memo_bits[slot] == bits) {
    return memo_t[slot];
  }
  float lo = 0.0f;
  float hi = 1.0f;
  for (int i = 0; i < 20; ++i) {
    const float mid = 0.5f * (lo + hi);
    if (wds::chart_editor::official_split_fade_in_scale(mid) < s) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const float t = 0.5f * (lo + hi);
  memo_used[slot] = 1;
  memo_bits[slot] = bits;
  memo_t[slot] = t;
  return t;
}

float emit_local_y(int32_t color_id) noexcept {
  // OpenWDS SplitEffectController.Initialize writes
  // element.localPosition.y = EffectOffsetPosition (2.5) on the Line child of
  // LineHight. LineHight.localPosition.y is the unscaled origin
  // (official_split_z180_line_hight_y / catalog.json lineHight.localPosition),
  // not this fade lever. localRotation z=180 is split_fade_grows_from_tip.
  (void)color_id;
  return wds::chart_editor::kOfficialZ180LineHightY;
}

// Unity Transform: SplitEffect (y=-5, scale.y=27) × LineHight (catalog
// localPosition / localRotation z=180) × element y=2.5. Input is PS/element
// local Y, not Unity World. Output is LaneGroup Main Y. Velocity is
// signed_lever * fade' (same helper; live dump refix-d hop, not 2022.3).
void emitter_main_pose(int32_t color_id, float local_y, float& origin,
                       float& signed_lever) noexcept {
  const float lever = local_y * wds::chart_editor::kOfficialLineHeightScale;
  if (wds::chart_editor::split_fade_grows_from_tip(color_id)) {
    origin = wds::chart_editor::official_split_z180_origin_y(color_id);
    signed_lever = -lever;
    return;
  }
  origin = wds::chart_editor::official_split_identity_origin_y();
  signed_lever = lever;
}

float local_y_to_main_y(int32_t color_id, float local_y, float fade_scale) noexcept {
  float origin = 0.0f;
  float signed_lever = 0.0f;
  emitter_main_pose(color_id, local_y, origin, signed_lever);
  return origin + signed_lever * std::max(fade_scale, 1e-6f);
}

float emitter_main_y(int32_t color_id, float fade_scale) noexcept {
  return local_y_to_main_y(color_id, emit_local_y(color_id), fade_scale);
}

float emitter_travel(int32_t color_id, float fade_scale) noexcept {
  return std::fabs(emit_local_y(color_id)) * wds::chart_editor::kOfficialLineHeightScale *
         std::max(fade_scale, 0.0f);
}

// OpenWDS Initialize: Random.Range(0, 1000) once per line. Mix start_ms so
// the same color / line index at another chart time is a different draw.
uint32_t stable_emitter_identity(const EmitContext& ctx) noexcept {
  const uint32_t start_tok = static_cast<uint32_t>(ctx.line_start_ms) + 1u;
  return mix3(static_cast<uint32_t>(ctx.color_id), static_cast<uint32_t>(ctx.line_index + 1),
              start_tok) %
         999u;
}

namespace {

uint32_t hash_auto_particle_seed(const EmitContext& ctx, const SplitEffectParticle& p,
                                 int birth_index, uint32_t extra, uint32_t gen) noexcept {
  const uint32_t ps_i = catalog_index(p);
  return mix3(static_cast<uint32_t>(ctx.color_id),
              static_cast<uint32_t>(ctx.line_index * 131 + birth_index * 17 + p.element_index),
              mix3(extra, ps_i, gen));
}

uint32_t hash_line_particle_seed(const EmitContext& ctx, const SplitEffectParticle& p,
                                 int birth_index, uint32_t extra) noexcept {
  const uint32_t ps_i = catalog_index(p);
  return mix3(stable_emitter_identity(ctx), static_cast<uint32_t>(birth_index + 1),
              mix3(extra, ps_i, 1u));
}

ParticleState make_state(const EmitContext& ctx, const SplitEffectParticle& p, int birth_index,
                         uint32_t extra, float emit_u, float birth_t, uint32_t seed) {
  ParticleState st;
  st.color_id = ctx.color_id;
  st.line_index = ctx.line_index;
  st.line_x = ctx.line_x;
  st.fade_scale = ctx.fade_scale;
  st.birth_fade = ctx.fade_scale;
  st.line_alpha = ctx.line_alpha;
  st.p = &p;
  st.birth_t = birth_t;
  st.birth_parent_z = 0.0f;
  st.birth_origin_y_delta = ctx.parent_motion.birth_pose_candidate
                                ? ctx.parent_motion.birth_origin_y_delta
                                : 0.0f;
  st.seed = seed;
  st.birth_index = birth_index;
  const float eu = std::clamp(emit_u, 0.0f, 1.0f);
  st.emit_u = eu;
  // Unity 2022.3 Main startLifetime is set on the particle at birth
  // (ScriptReference/ParticleSystem.MainModule-startLifetime.html). Curve
  // mode evaluates at system age / Duration (emit_u), same as startSize.
  if (p.life_curve_count > 0) {
    st.life = std::max(official_split_effect_eval_start_lifetime(p, eu), 1e-4f);
  } else {
    st.life = std::max(pick(p.lifetime, p.lifetime_min, p.life_random != 0, st.seed), 1e-4f);
  }
  if (p.size_curve_count > 0) {
    const float vmax =
        official_split_effect_eval_curve_keys(p.size_curve_begin, p.size_curve_count, eu, p.size);
    if (p.size_min_curve_count > 0) {
      const float vmin = official_split_effect_eval_curve_keys(
          p.size_min_curve_begin, p.size_min_curve_count, eu, p.size_min);
      st.size = pick(vmax, vmin, true, mix3(st.seed, 2, extra));
    } else {
      st.size = vmax;
    }
  } else {
    st.size = pick(p.size, p.size_min, p.size_random != 0, mix3(st.seed, 2, extra));
  }
  if (p.size_3d != 0) {
    if (p.size_y_curve_count > 0) {
      const float vmax = official_split_effect_eval_curve_keys(
          p.size_y_curve_begin, p.size_y_curve_count, eu, p.start_size_y);
      if (p.size_y_min_curve_count > 0) {
        const float vmin = official_split_effect_eval_curve_keys(
            p.size_y_min_curve_begin, p.size_y_min_curve_count, eu, p.start_size_y_min);
        st.size_y = pick(vmax, vmin, true, mix3(st.seed, 22, extra));
      } else {
        st.size_y = vmax;
      }
    } else {
      st.size_y = pick(p.start_size_y, p.start_size_y_min, p.size_random != 0,
                       mix3(st.seed, 22, extra));
    }
  } else {
    st.size_y = st.size;
  }
  // Unity 2022.3 Main startSizeZ: depth. Only Mesh draw uses it
  // (Manual/PartSysMainModule.html + startSizeZ API: billboards have no Z).
  if (p.size_3d != 0) {
    if (p.size_z_curve_count > 0) {
      const float vmax = official_split_effect_eval_curve_keys(
          p.size_z_curve_begin, p.size_z_curve_count, eu, p.start_size_z);
      if (p.size_z_min_curve_count > 0) {
        const float vmin = official_split_effect_eval_curve_keys(
            p.size_z_min_curve_begin, p.size_z_min_curve_count, eu, p.start_size_z_min);
        st.size_z = pick(vmax, vmin, true, mix3(st.seed, 23, extra));
      } else {
        st.size_z = vmax;
      }
    } else {
      st.size_z = pick(p.start_size_z, p.start_size_z_min, p.size_random != 0,
                       mix3(st.seed, 23, extra));
    }
  } else {
    st.size_z = st.size;
  }
  if (p.speed_curve_count > 0) {
    const float vmax = official_split_effect_eval_curve_keys(p.speed_curve_begin, p.speed_curve_count,
                                                            eu, p.start_speed);
    if (p.speed_min_curve_count > 0) {
      const float vmin = official_split_effect_eval_curve_keys(
          p.speed_min_curve_begin, p.speed_min_curve_count, eu, p.start_speed);
      st.speed = pick(vmax, vmin, true, mix3(st.seed, kSaltStartSpeedTwoCurves, extra));
    } else {
      st.speed = vmax;
    }
  } else {
    st.speed = pick(p.start_speed, p.start_speed_min, p.speed_random != 0, mix3(st.seed, 3, extra));
  }
  // startSpeed * shape dir is the only baked velocity. VOL is evaluated each
  // step (Two Curves start at 0 — baking the scalar made butterflies fly +X).
  st.vx = 0.0f;
  st.vy = 0.0f;
  st.vz = 0.0f;
  // Same nearby-salt collapse as Shape cube-reject (salts 4/5/6 and 40/41/42).
  rng_fill_u01_3(st.seed, extra, 4u, st.vol_ux, st.vol_uy, st.vol_uz);
  rng_fill_u01_3(st.seed, extra, 40u, st.force_ux, st.force_uy, st.force_uz);
  rng_fill_u01_3(st.seed, extra, kSaltNoiseStrength, st.noise_ux, st.noise_uy, st.noise_uz);
  st.inherit_k = 0.0f;
  if (p.inherit_enabled != 0) {
    st.inherit_k = pick(p.inherit_mult, p.inherit_mult_min, p.inherit_random != 0,
                        mix3(st.seed, 43, extra));
  }
  if (ctx.parent_motion.emitter_vel_candidate) {
    st.emitter_vel_set = 1;
    st.emitter_vx = ctx.parent_motion.emitter_vx;
    st.emitter_vy = ctx.parent_motion.emitter_vy;
    st.emitter_vz = ctx.parent_motion.emitter_vz;
  } else {
    st.emitter_vel_set = 0;
    st.emitter_vx = 0.0f;
    st.emitter_vy = 0.0f;
    st.emitter_vz = 0.0f;
  }
  float orb_ux = 0.0f, orb_uy = 0.0f, orb_uz = 0.0f;
  rng_fill_u01_3(st.seed, extra, kSaltOrbital, orb_ux, orb_uy, orb_uz);
  st.orb_x = eval_minmax_axis(p.orbital_x, p.orbital_x_min, p.orbital_x_random, 0, 0, 0, 0, 0.0f,
                             orb_ux);
  st.orb_y = eval_minmax_axis(p.orbital_y, p.orbital_y_min, p.orbital_y_random, 0, 0, 0, 0, 0.0f,
                             orb_uy);
  st.orb_z = eval_minmax_axis(p.orbital_z, p.orbital_z_min, p.orbital_z_random, 0, 0, 0, 0, 0.0f,
                             orb_uz);
  st.orb_ox = p.orbital_offset_x;
  st.orb_oy = p.orbital_offset_y;
  st.orb_oz = p.orbital_offset_z;
  st.radial = p.radial;
  sample_shape(p, st.seed, extra, birth_t, birth_index, st.spawn_x, st.spawn_y, st.spawn_z, st.dir_x,
               st.dir_y, st.dir_z);
  st.start_u = pick(p.start_frame, p.start_frame_min, p.start_frame_random != 0, mix3(st.seed, 7, extra));
  if (p.fot_min_count > 0) {
    st.fot_u = u01(mix3(st.seed, kSaltFotTwoCurves, extra));
  } else {
    st.fot_u = pick(p.fot, p.fot_min, p.fot_random != 0, mix3(st.seed, 8, extra));
  }
  st.trail_life_mul =
      pick(p.trail_lifetime, p.trail_lifetime_min, p.trail_lifetime_random != 0,
           mix3(st.seed, kSaltTrailLifetime, extra));
  st.col_u = u01(mix3(st.seed, 13, extra));
  st.start_color_u = u01(mix3(st.seed, 15, extra));
  st.rot_z = pick(p.rot_z, p.rot_z_min, p.rot_z_random != 0, mix3(st.seed, 9, extra));
  if (p.rotation_3d != 0) {
    st.rot_x = pick(p.rot_x, p.rot_x_min, true, mix3(st.seed, 10, extra));
    st.rot_y = pick(p.rot_y, p.rot_y_min, true, mix3(st.seed, 11, extra));
  }
  return st;
}

float system_emit_u(const SplitEffectParticle& p, float birth_t) noexcept {
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const float sim = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = start_delay_wall_clock(p);
  const float sim_birth = std::max(0.0f, birth_t - delay) * sim;
  if (length <= 1e-6f) {
    return 0.0f;
  }
  if ((p.flags & kSplitEffectFlagLooping) != 0) {
    float wrapped = std::fmod(sim_birth, length);
    if (wrapped < 0.0f) {
      wrapped += length;
    }
    return wrapped / length;
  }
  return std::clamp(sim_birth / length, 0.0f, 1.0f);
}

uint32_t particle_seed(const EmitContext& ctx, const SplitEffectParticle& p,
                       const Birth& b) noexcept {
  if (p.auto_random_seed == 0) {
    return hash_line_particle_seed(ctx, p, b.index, b.extra);
  }
  const uint32_t gen = ctx.line_play_gen != 0 ? ctx.line_play_gen : 1u;
  return hash_auto_particle_seed(ctx, p, b.index, b.extra, gen);
}

// hop, not 2022.3. Unity Particle.randomSeed is written at emit and stays
// with that particle; compacting the live array does not remint it.
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.Particle-randomSeed.html
// Native dest merge used to overwrite Birth.index with dest.size() so later
// parents' children rehashed every time an earlier parent grew. Seed here
// from the per-parent emit identity, mixed with the parent so two parents'
// k=0 do not share a stream. Unity RNG is closed; this is identity, not a
// numerical match.
uint32_t child_emit_seed(const EmitContext& ctx, const SplitEffectParticle& child, const Birth& cb,
                         const Birth& parent_b, uint16_t slot_i) noexcept {
  const uint32_t parent_tok =
      mix3(static_cast<uint32_t>(parent_b.index + 1), parent_b.extra + 1u,
           static_cast<uint32_t>(slot_i) + 1u);
  return mix3(particle_seed(ctx, child, cb), parent_tok, 0x53425545u);
}

// Same startLifetime as make_state (curve @ emit_u, else pick). Cap walk
// needs death times before evaluate; lifetimes are deterministic per seed.
float birth_start_life(const SplitEffectParticle& p, const EmitContext&, const Birth&, float emit_u,
                       uint32_t seed) noexcept {
  const float eu = std::clamp(emit_u, 0.0f, 1.0f);
  if (p.life_curve_count > 0) {
    return std::max(official_split_effect_eval_start_lifetime(p, eu), 1e-4f);
  }
  return std::max(pick(p.lifetime, p.lifetime_min, p.life_random != 0, seed), 1e-4f);
}

float birth_start_life(const SplitEffectParticle& p, const EmitContext& ctx, const Birth& b,
                       float emit_u) noexcept {
  return birth_start_life(p, ctx, b, emit_u, particle_seed(ctx, p, b));
}

// Unity 2022.3 Main.maxParticles: maximum number of particles in the system
// at once. Births that would exceed the cap are dropped, not deferred.
// https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysMainModule.html
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.MainModule-maxParticles.html
struct AliveCapTls {
  std::vector<size_t> ord;
  std::vector<float> heap;
  std::vector<float> death;
  std::vector<uint8_t> keep;
  std::vector<size_t> mine;
  std::vector<Birth> cand;
  std::vector<uint8_t> ref_keep;
};
thread_local AliveCapTls g_alive_cap_tls;

template <typename BirthVec, typename KeepVec, typename DeathAt, typename IndexAt>
void mark_alive_cap_greedy(int cap, const BirthVec& births, DeathAt death_at, KeepVec& keep,
                           IndexAt index_at, size_t n) {
  auto& heap = g_alive_cap_tls.heap;
  heap.clear();
  for (size_t i = 0; i < n; ++i) {
    const size_t oi = index_at(i);
    const float t = births[oi].time;
    while (!heap.empty() && heap.front() <= t) {
      std::pop_heap(heap.begin(), heap.end(), std::greater<float>{});
      heap.pop_back();
    }
    if (static_cast<int>(heap.size()) >= cap) {
      continue;
    }
    heap.push_back(death_at(oi));
    std::push_heap(heap.begin(), heap.end(), std::greater<float>{});
    keep[oi] = 1;
  }
}

// Unity 2022.3 maxParticles greedy-by-birth-time. Comparator (time, then
// index) is a strict total order when no time is NaN, so sort ≡ stable_sort.
// Births are usually already time-ordered: skip building ord in that case.
template <typename BirthVec, typename DeathVec, typename KeepVec>
void mark_alive_cap(int cap, const BirthVec& births, const DeathVec& death_wall, KeepVec& keep) {
  keep.assign(births.size(), 0);
  if (births.empty()) {
    return;
  }
  if (cap <= 0 || static_cast<int>(births.size()) <= cap) {
    keep.assign(births.size(), 1);
    return;
  }
  const size_t n = births.size();
  bool any_nan = false;
  for (size_t i = 0; i < n; ++i) {
    if (std::isnan(births[i].time)) {
      any_nan = true;
      break;
    }
  }
  const bool already_sorted =
      !any_nan && std::is_sorted(births.begin(), births.end(),
                                 [](const auto& a, const auto& b) { return a.time < b.time; });
  if (already_sorted) {
    mark_alive_cap_greedy(cap, births, [&](size_t i) { return death_wall[i]; }, keep,
                          [](size_t i) { return i; }, n);
    return;
  }
  auto& ord = g_alive_cap_tls.ord;
  ord.resize(n);
  std::iota(ord.begin(), ord.end(), 0);
  auto cmp = [&](size_t a, size_t b) {
    if (births[a].time != births[b].time) {
      return births[a].time < births[b].time;
    }
    return a < b;
  };
  if (any_nan) {
    std::stable_sort(ord.begin(), ord.end(), cmp);
  } else {
    std::sort(ord.begin(), ord.end(), cmp);
  }
  mark_alive_cap_greedy(cap, births, [&](size_t i) { return death_wall[i]; }, keep,
                        [&](size_t i) { return ord[i]; }, n);
}

// Same greedy as mark_alive_cap, but death_wall is a callable evaluated lazily
// only at push time. When the death value is expensive to precompute for every
// birth (e.g. lifetime math the greedy then discards), this avoids the work on
// the births the cap drops.
template <typename BirthVec, typename KeepVec, typename DeathAt>
void mark_alive_cap_lazy(int cap, const BirthVec& births, DeathAt death_at, KeepVec& keep) {
  keep.assign(births.size(), 0);
  if (births.empty()) {
    return;
  }
  if (cap <= 0 || static_cast<int>(births.size()) <= cap) {
    keep.assign(births.size(), 1);
    return;
  }
  const size_t n = births.size();
  bool any_nan = false;
  for (size_t i = 0; i < n; ++i) {
    if (std::isnan(births[i].time)) {
      any_nan = true;
      break;
    }
  }
  const bool already_sorted =
      !any_nan && std::is_sorted(births.begin(), births.end(),
                                 [](const auto& a, const auto& b) { return a.time < b.time; });
  if (already_sorted) {
    mark_alive_cap_greedy(cap, births, death_at, keep, [](size_t i) { return i; }, n);
    return;
  }
  auto& ord = g_alive_cap_tls.ord;
  ord.resize(n);
  std::iota(ord.begin(), ord.end(), 0);
  auto cmp = [&](size_t a, size_t b) {
    if (births[a].time != births[b].time) {
      return births[a].time < births[b].time;
    }
    return a < b;
  };
  if (any_nan) {
    std::stable_sort(ord.begin(), ord.end(), cmp);
  } else {
    std::sort(ord.begin(), ord.end(), cmp);
  }
  mark_alive_cap_greedy(cap, births, death_at, keep, [&](size_t i) { return ord[i]; }, n);
}

// Diagnostic: once per wall-clock second while perf is on, summarize
// apply_max_particles_alive_cap traffic so the subemit-plan lifetime cost is
// visible. No cost when perf is disabled.
#if WDS_DIAG_ENABLED
static std::atomic<int> g_submitplan_calls{0};
static std::atomic<int> g_submitplan_max_items{0};
static std::atomic<int64_t> g_submitplan_last_sec{0};
#endif  // WDS_DIAG_ENABLED

void apply_max_particles_alive_cap(const SplitEffectParticle& p, const EmitContext& ctx,
                                   std::vector<Birth>& births,
                                   const float* death_wall = nullptr) {
  const int cap =
      p.max_particles > 0 ? static_cast<int>(p.max_particles) : kSplitEffectParticlePool;
  if (cap <= 0 || static_cast<int>(births.size()) <= cap) {
    return;
  }
#if WDS_DIAG_ENABLED
  if (wds::common::perf::enabled()) {
    g_submitplan_calls.fetch_add(1, std::memory_order_relaxed);
    const int n = static_cast<int>(births.size());
    int prev = g_submitplan_max_items.load(std::memory_order_relaxed);
    while (n > prev && !g_submitplan_max_items.compare_exchange_weak(
                           prev, n, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    const int64_t now_sec =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    int64_t last = g_submitplan_last_sec.load(std::memory_order_relaxed);
    if (now_sec != last &&
        g_submitplan_last_sec.compare_exchange_strong(last, now_sec, std::memory_order_relaxed)) {
      const int n_calls = g_submitplan_calls.exchange(0, std::memory_order_relaxed);
      const int max_items = g_submitplan_max_items.exchange(0, std::memory_order_relaxed);
      std::fprintf(stderr, "SUBMITPLAN_SEC n=%d max_items=%d\n", n_calls, max_items);
    }
  }
#endif  // WDS_DIAG_ENABLED
  const float sim = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  auto& keep = g_alive_cap_tls.keep;
  if (death_wall != nullptr) {
    auto& death = g_alive_cap_tls.death;
    death.resize(births.size());
    for (size_t i = 0; i < births.size(); ++i) {
      death[i] = death_wall[i];
    }
    mark_alive_cap(cap, births, death, keep);
  } else {
    // Lazy: defer the lifetime math to push time so only births the cap keeps
    // pay for it. The greedy compares death by heap order, so the birth order
    // and cap semantics are unchanged versus precomputing death[].
    mark_alive_cap_lazy(cap, births, [&](size_t i) {
      const float life =
          birth_start_life(p, ctx, births[i], system_emit_u(p, birth_curve_t(births[i])));
      return births[i].time + life / sim;
    }, keep);
  }
  size_t w = 0;
  for (size_t i = 0; i < births.size(); ++i) {
    if (keep[i] != 0) {
      births[w++] = births[i];
    }
  }
  births.resize(w);
}

bool should_parallel_eval(size_t n) noexcept {
  // Nested parallel_for on the collect pool inflated default task_sum ~3.5x
  // vs THREADS=1 (cache locks + steal). Top-level collect tasks already
  // cover systems; inner plan/realize/subemit stay serial on the worker.
  return n >= 2 && !observers_installed() && split_collect_thread_count() > 1 &&
         split_collect_claim_depth() == 0;
}

void stamp_evaluated_live(SplitEffectLiveParticle& live, const EmitContext& ctx,
                          const SplitEffectParticle& p, const ParticleState& st, float age,
                          uint32_t cat_idx) {
  SplitWallScope wall_stamp(SplitWallSec::Stamp);
  live.effect_id = ctx.color_id;
  live.line_index = ctx.line_index;
  live.catalog_index = cat_idx;
  live.element_index = p.element_index;
  live.seed = st.seed;
  live.age = age;
  live.sampled_lifetime = st.life;
  live.remaining_lifetime = std::max(st.life - age, 0.0f);
  live.birth_fade = st.birth_fade;
  live.birth_parent_z = st.birth_parent_z;
  live.birth_origin_y_delta = st.birth_origin_y_delta;
  live.effective_distance =
      p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)
          ? emitter_travel(ctx.color_id, st.birth_fade)
          : -1.0f;
}

uint64_t mix64(uint64_t h, uint64_t v) noexcept {
  h ^= v;
  h *= 1099511628211ull;
  return h;
}

uint64_t hash_f32(uint64_t h, float f) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &f, sizeof(u));
  return mix64(h, u);
}

uint64_t hash_particle_state_fields(uint64_t h, const ParticleState& st) noexcept {
  h = hash_f32(h, st.life);
  h = hash_f32(h, st.size);
  h = hash_f32(h, st.size_y);
  h = hash_f32(h, st.size_z);
  h = hash_f32(h, st.speed);
  h = hash_f32(h, st.vx);
  h = hash_f32(h, st.vy);
  h = hash_f32(h, st.vz);
  h = hash_f32(h, st.spawn_x);
  h = hash_f32(h, st.spawn_y);
  h = hash_f32(h, st.spawn_z);
  h = hash_f32(h, st.dir_x);
  h = hash_f32(h, st.dir_y);
  h = hash_f32(h, st.dir_z);
  h = hash_f32(h, st.force_ux);
  h = hash_f32(h, st.force_uy);
  h = hash_f32(h, st.force_uz);
  h = hash_f32(h, st.vol_ux);
  h = hash_f32(h, st.vol_uy);
  h = hash_f32(h, st.vol_uz);
  h = hash_f32(h, st.inherit_k);
  h = hash_f32(h, st.emitter_vx);
  h = hash_f32(h, st.emitter_vy);
  h = hash_f32(h, st.emitter_vz);
  h = mix64(h, st.emitter_vel_set);
  h = hash_f32(h, st.orb_x);
  h = hash_f32(h, st.orb_y);
  h = hash_f32(h, st.orb_z);
  h = hash_f32(h, st.noise_ux);
  h = hash_f32(h, st.noise_uy);
  h = hash_f32(h, st.noise_uz);
  h = hash_f32(h, st.trail_life_mul);
  h = hash_f32(h, st.orb_ox);
  h = hash_f32(h, st.orb_oy);
  h = hash_f32(h, st.orb_oz);
  h = hash_f32(h, st.radial);
  h = hash_f32(h, st.birth_t);
  h = hash_f32(h, st.birth_fade);
  h = hash_f32(h, st.birth_parent_z);
  h = hash_f32(h, st.birth_origin_y_delta);
  h = hash_f32(h, st.event_wx);
  h = hash_f32(h, st.event_wy);
  h = hash_f32(h, st.event_wz);
  h = mix64(h, st.event_emit);
  h = mix64(h, st.inherit_props);
  h = hash_f32(h, st.emit_u);
  return h;
}

uint64_t hash_system_sim_inputs(const SplitEffectParticle* p, int32_t color_id) noexcept {
  uint64_t h = 1469598103934665603ull;
  if (p != nullptr) {
    const SplitEffectParticle& ps = *p;
    h = mix64(h, ps.simulation_space);
    h = mix64(h, ps.scaling_mode);
    h = mix64(h, ps.flags);
    h = hash_f32(h, ps.gravity_modifier);
    h = mix64(h, ps.force_enabled);
    h = mix64(h, ps.force_in_world);
    h = mix64(h, ps.force_randomize);
    h = hash_f32(h, ps.force_x);
    h = hash_f32(h, ps.force_y);
    h = hash_f32(h, ps.force_z);
    h = mix64(h, ps.clamp_enabled);
    h = mix64(h, ps.clamp_separate);
    h = mix64(h, ps.clamp_in_world);
    h = mix64(h, ps.clamp_mul_size);
    h = mix64(h, ps.clamp_mul_vel);
    h = hash_f32(h, ps.clamp_dampen);
    h = hash_f32(h, ps.clamp_speed);
    h = hash_f32(h, ps.clamp_x);
    h = hash_f32(h, ps.clamp_y);
    h = hash_f32(h, ps.clamp_z);
    h = hash_f32(h, ps.clamp_drag);
    h = hash_f32(h, ps.vel_x);
    h = hash_f32(h, ps.vel_y);
    h = hash_f32(h, ps.vel_z);
    h = hash_f32(h, ps.vol_speed_mod);
    h = mix64(h, ps.vol_in_world);
    h = mix64(h, ps.inherit_enabled);
    h = mix64(h, ps.inherit_mode);
    h = mix64(h, ps.noise_octaves);
    h = hash_f32(h, ps.noise_strength);
    h = hash_f32(h, ps.noise_strength_min);
    h = mix64(h, ps.noise_strength_random);
    h = mix64(h, ps.noise_strength_count);
    h = mix64(h, ps.noise_strength_min_count);
    h = hash_f32(h, ps.noise_strength_y);
    h = hash_f32(h, ps.noise_strength_y_min);
    h = mix64(h, ps.noise_strength_y_random);
    h = mix64(h, ps.noise_strength_y_count);
    h = hash_f32(h, ps.noise_strength_z);
    h = hash_f32(h, ps.noise_strength_z_min);
    h = mix64(h, ps.noise_strength_z_random);
    h = mix64(h, ps.noise_strength_z_count);
    h = hash_f32(h, ps.orbital_x);
    h = hash_f32(h, ps.orbital_x_min);
    h = mix64(h, ps.orbital_x_random);
    h = hash_f32(h, ps.orbital_y);
    h = hash_f32(h, ps.orbital_y_min);
    h = mix64(h, ps.orbital_y_random);
    h = hash_f32(h, ps.orbital_z);
    h = hash_f32(h, ps.orbital_z_min);
    h = mix64(h, ps.orbital_z_random);
    h = hash_f32(h, ps.noise_frequency);
    h = hash_f32(h, ps.noise_scroll);
    h = hash_f32(h, ps.noise_position_amount);
    h = mix64(h, static_cast<uint32_t>(ps.element_index) + 1u);
    h = mix64(h, ps.random_seed);
    h = mix64(h, ps.auto_random_seed);
  }
  const uint32_t noise_mode = split_noise_exact_enabled() ? 1u : (split_noise_vol_enabled() ? 2u : 3u);
  h = mix64(h, noise_mode);
  h = mix64(h, static_cast<uint32_t>(p != nullptr ? split_noise_hold_for(*p, color_id)
                                                   : split_noise_hold()));
  h = mix64(h, split_noise_hold_spread() ? 1u : 0u);
  h = mix64(h, static_cast<uint32_t>(split_walk_hz()));
  h = mix64(h, (p != nullptr && split_particle_trail_path_straight(*p)) ? 1u : 0u);
  h = mix64(h, split_closed_form() ? 1u : 0u);
  return h;
}

struct SystemEvalHash {
  uint64_t prefix = 1469598103934665603ull;
  const SplitEffectParticle* p = nullptr;
  int32_t color_id = 0;
};

SystemEvalHash make_system_eval_hash(const SplitEffectParticle& p, int32_t color_id) noexcept {
  SystemEvalHash s;
  s.p = &p;
  s.color_id = color_id;
  s.prefix = hash_system_sim_inputs(&p, color_id);
  return s;
}

thread_local SystemEvalHash g_tls_sys_hash{};

// Per-collect-frame system hash cache. The old single slot thrashed when a
// frame alternated primary/parent/child systems; the hash itself only depends
// on the bound row + color id. The frame token invalidates all entries at the
// next collect, so pointers into reused task/bound storage cannot go stale
// across frames. Direct evaluate()/tests (token == 0) keep the old single-slot
// path; callers that mutate global noise/integrator config bump the generation.
constexpr int kSysHashCacheSlots = 128;
struct SystemEvalHashEntry {
  const SplitEffectParticle* p = nullptr;
  int32_t color_id = 0;
  uint32_t frame_token = 0;
  uint32_t config_gen = 0;
  uint64_t prefix = 1469598103934665603ull;
};
thread_local SystemEvalHashEntry g_tls_sys_hash_cache[kSysHashCacheSlots];
std::atomic<uint32_t> g_sys_hash_frame_token{0};
std::atomic<uint32_t> g_sys_hash_config_gen{0};

inline std::size_t sys_hash_slot(const SplitEffectParticle* p, int32_t color_id) noexcept {
  const std::uintptr_t u = reinterpret_cast<std::uintptr_t>(p);
  std::uint64_t h = (u >> 4) * 0x9e3779b97f4a7c15ull + static_cast<uint64_t>(color_id);
  return static_cast<std::size_t>(h >> 32) & (kSysHashCacheSlots - 1);
}

void note_eval_system_hash(const SplitEffectParticle& p, int32_t color_id) noexcept {
  SplitWallScope wall_hash(SplitWallSec::CfHash);
  const uint32_t token = g_sys_hash_frame_token.load(std::memory_order_relaxed);
  if (token == 0) {
    if (g_tls_sys_hash.p == &p && g_tls_sys_hash.color_id == color_id) {
      WDS_PERF_COUNT("shuriken.sys_hash.hit", 1);
      return;
    }
    WDS_PERF_COUNT("shuriken.sys_hash.recompute", 1);
    WDS_PERF_SCOPE("shuriken.sys_hash");
    g_tls_sys_hash = make_system_eval_hash(p, color_id);
    return;
  }
  const uint32_t gen = g_sys_hash_config_gen.load(std::memory_order_relaxed);
  SystemEvalHashEntry& e = g_tls_sys_hash_cache[sys_hash_slot(&p, color_id)];
  if (e.p == &p && e.color_id == color_id && e.frame_token == token && e.config_gen == gen) {
    WDS_PERF_COUNT("shuriken.sys_hash.hit", 1);
    g_tls_sys_hash.p = &p;
    g_tls_sys_hash.color_id = color_id;
    g_tls_sys_hash.prefix = e.prefix;
    return;
  }
  WDS_PERF_COUNT("shuriken.sys_hash.recompute", 1);
  WDS_PERF_SCOPE("shuriken.sys_hash");
  g_tls_sys_hash = make_system_eval_hash(p, color_id);
  e.p = &p;
  e.color_id = color_id;
  e.frame_token = token;
  e.config_gen = gen;
  e.prefix = g_tls_sys_hash.prefix;
}

uint64_t hash_integration_inputs(const ParticleState& st, const SystemEvalHash* sys) noexcept {
  const SystemEvalHash* use = sys != nullptr ? sys : &g_tls_sys_hash;
  uint64_t h = (use->p == st.p && use->color_id == st.color_id)
                   ? use->prefix
                   : hash_system_sim_inputs(st.p, st.color_id);
  return hash_particle_state_fields(h, st);
}

uint32_t system_id_of(const EmitContext& ctx, const SplitEffectParticle& p) noexcept {
  if (ctx.has_catalog_index_override != 0) {
    return ctx.catalog_index_override;
  }
  return catalog_index(p);
}

ParticleCacheKey make_cache_key(const EmitContext& ctx, const SplitEffectParticle& p, const Birth& b,
                                uint32_t seed, const ParticleState& st, uint32_t catalog_i,
                                const SystemEvalHash* sys = nullptr) {
  SplitWallScope wall_ck(SplitWallSec::CacheKey);
  (void)p;
  ParticleCacheKey key;
  if (!split_eval_cache_enabled()) {
    return key;
  }
  key.epoch = split_eval_cache_epoch();
  key.color_id = ctx.color_id;
  key.line_index = ctx.line_index;
  key.line_start_ms = ctx.line_start_ms;
  key.catalog_index = catalog_i;
  key.birth_index = b.index;
  key.birth_extra = b.extra;
  key.line_play_gen = ctx.line_play_gen != 0 ? ctx.line_play_gen : 1u;
  key.seed = seed;
  if (st.p != nullptr) {
    note_eval_system_hash(*st.p, st.color_id);
  }
  key.input_hash = hash_integration_inputs(st, sys != nullptr ? sys : &g_tls_sys_hash);
  key.valid = 1;
  return key;
}

uint32_t bound_catalog_index(const SplitEffectParticle* particles, uint16_t count,
                             const EmitContext& ctx, const SplitEffectParticle& p) {
  using split_effect_catalog_detail::kSplitEffectParticles;
  const auto* begin = kSplitEffectParticles;
  const ptrdiff_t n = static_cast<ptrdiff_t>(std::size(kSplitEffectParticles));
  const ptrdiff_t i = &p - begin;
  if (i >= 0 && i < n) {
    return static_cast<uint32_t>(i);
  }
  const auto* entry = official_split_effect(ctx.color_id);
  const ptrdiff_t local = &p - particles;
  if (entry != nullptr && local >= 0 && local < static_cast<ptrdiff_t>(count)) {
    return static_cast<uint32_t>(entry->particle_begin) + static_cast<uint32_t>(local);
  }
  return 0;
}

void precommit_alive_cap_tls() {
  auto& t = g_alive_cap_tls;
  constexpr std::size_t k = 4096;
  t.ord.reserve(k);
  t.heap.reserve(k);
  t.death.reserve(k);
  t.keep.reserve(k);
  t.mine.reserve(k);
  t.cand.reserve(k);
  t.ref_keep.reserve(k);
  t.ord.resize(k);
  t.heap.resize(k);
  t.death.resize(k);
  t.keep.resize(k);
  t.mine.resize(k);
  t.cand.resize(k);
  t.ref_keep.resize(k);
  if (!t.ord.empty()) {
    wds::common::touch_pages(t.ord.data(), t.ord.size() * sizeof(size_t));
  }
  if (!t.heap.empty()) {
    wds::common::touch_pages(t.heap.data(), t.heap.size() * sizeof(float));
  }
  t.ord.clear();
  t.heap.clear();
  t.death.clear();
  t.keep.clear();
  t.mine.clear();
  t.cand.clear();
  t.ref_keep.clear();
}

}  // namespace

void split_eval_precommit_tls() noexcept {
  precommit_alive_cap_tls();
}

void fill_live(const ParticleState& st, float age, bool want_trail, SplitEffectLiveParticle& out) {
  PoseIntegrator integ(st);
  fill_live(integ, st, age, want_trail, out, true, -1.0f);
}

void fill_live(PoseIntegrator& integ, const ParticleState& st, float age, bool want_trail,
               SplitEffectLiveParticle& out, bool want_fd, float trail_clock) {
  set_rol_cache_key(nullptr);
  if (want_trail && st.p != nullptr && (st.p->flags & kSplitEffectFlagTrails) != 0 &&
      st.p->trail_mode != 1 && !split_particle_trail_path_straight(st)) {
    integ.begin_grid_log();
  }
  {
    SplitWallScope wall_int(SplitWallSec::Integrate);
    WDS_PERF_SCOPE("shuriken.integrate");
    SplitDiagScope diag_int(SplitDiagPhase::Integrate);
    integ.sample(age, out.world_x, out.main_y, out.z, nullptr);
  }
  fill_live_after_pose(integ, st, age, want_trail, out, want_fd, trail_clock);
}

void fill_live_after_pose(PoseIntegrator& integ, const ParticleState& st, float age,
                          bool want_trail, SplitEffectLiveParticle& out, bool want_fd,
                          float trail_clock) {
  const SplitEffectParticle& p = *st.p;
  float billboard_sprite_w_scale = 1.0f;
  set_rol_cache_key(nullptr);
  {
  SplitWallScope wall_app(SplitWallSec::Appearance);
  WDS_PERF_SCOPE("shuriken.appearance");
  out.emit_u = st.emit_u;
  out.inherit_props = st.inherit_props;
  // live dump v1 (write-intents/physics.md): copy the last integrate_pose
  // observer terms onto live. Dump reads these; do not pair by pointer
  // catalog_index. Not a 2022.3 equation. Does not change the pose.
  const IntegrationVelocityTrace& vel = last_integration_velocity();
  out.stored_vx = vel.stored_x;
  out.stored_vy = vel.stored_y;
  out.stored_vz = vel.stored_z;
  out.vol_vx = vel.vol_x;
  out.vol_vy = vel.vol_y;
  out.vol_vz = vel.vol_z;
  out.noise_vx = vel.noise_x;
  out.noise_vy = vel.noise_y;
  out.noise_vz = vel.noise_z;
  out.total_vx = vel.total_x;
  out.total_vy = vel.total_y;
  out.total_vz = vel.total_z;
  out.birth_t = st.birth_t;
  {
  const float t01 = st.life > 1e-6f ? age / st.life : 1.0f;
  const float cd_t01 = custom_data_t01(age, st.life);
  float sol_x = 1.0f, sol_y = 1.0f, sol_z = 1.0f;
  eval_particle_sol_at_age(p, st, age, sol_x, sol_y, sol_z);
  // live dump v1 / v19: GetCurrentSize = start × SOL
  // (Manual/PartSysSizeOverLifetimeModule.html). Allow 0; do not 1e-4-floor
  // this observation. Noise sizeAmount is a multiplier on size
  // (Manual/PartSysNoiseModule.html, ScriptReference sizeAmount) but the
  // pages do not say 1+amount*n. v19 10160 GO5 Unity size.current and
  // viewport span stay constant over life; 1+amount*n with scrolling Perlin
  // would pulse several times. Draw uses start×SOL. Not a 2022.3 sizeAmount
  // measure — that apply formula stays 未证实.
  out.current_size = st.size * sol_x;
  out.current_size_y = st.size_y * sol_y;
  out.current_size_z = st.size_z * sol_z;
  // live dump v1: Particle.startSize / startSize3D are birth values (after
  // InheritSize). Do not write draw size or GetCurrentSize here.
  out.start_size = st.size;
  out.start_size_y = st.size_y;
  out.start_size_z = st.size_z;
  // Unity separateAxes: width = startSizeX * solX, height = startSizeY * solY.
  // Do not multiply solY onto an already solX-scaled width.
  float width = std::max(st.size * sol_x, 1e-4f);
  float height = std::max(st.size_y * sol_y, 1e-4f);
  // Mesh depth = startSizeZ × SOL Z (Main + Size over Lifetime Z pages).
  // Non-3D startSize is one value for all axes.
  out.size_z = std::max(st.size_z * sol_z, 1e-4f);
  out.size = width;
  out.size_y = height;
  out.size_3d = p.size_3d;
  out.sol_y = height / width;
  // Official PSR m_MaxParticleSize. 0 means viewport size 0 (do not remap
  // to 0.5). 10 is authored on 127 systems and must stay 10.
  // live dump n7 Billboard maxP: clamp is draw-only in preview;
  // GetCurrentSize / out.size stay unclamped. Stretch / Mesh: do not write
  // a width clamp. Ban AABB / long-axis.
  out.max_particle_size = p.max_particle_size;
  float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
  eval_col(p, t01, st.col_u, cr, cg, cb, ca);
  float sr = p.color_r;
  float sg = p.color_g;
  float sb = p.color_b;
  float sa = p.color_a;
  // Unity 2022.3 MinMaxGradient.Evaluate(time, lerpFactor): time is valid for
  // Gradient / TwoGradients. Main Start Color is initial on emission.
  // live dump v19 10160 GO1: time = emit_u (system age / Duration at birth),
  // same as startSize — not mix3 seed 15. RandomColor keeps start_color_u.
  // TwoColors uses lerpFactor on minColor/maxColor, not the dummy white
  // maxGradient. hop (which time ParticleSystem passes is dump-fit).
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.MinMaxGradient.Evaluate.html
  // https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysMainModule.html
  float start_color_time = st.start_color_u;
  if (p.start_color_state == 1 || p.start_color_state == 3) {
    start_color_time = st.emit_u;
  }
  official_split_effect_eval_start_color_keys(p, start_color_time, u01(mix3(st.seed, 16, 1u)), sr,
                                              sg, sb, sa);
  if ((st.inherit_props & kSplitEffectSubInheritColor) != 0) {
    sr *= st.inherit_r;
    sg *= st.inherit_g;
    sb *= st.inherit_b;
    sa *= st.inherit_a;
  }
  // live dump v2: Particle.startColor is Color32 (after InheritColor).
  // Quantize start before COL so HDR start × COL does not stay ~1.5× the
  // Unity byte color. GetCurrentColor dump is Color32(start × COL) in
  // current_*; Custom1 hop stays on draw `r`. Not a 2022.3 equation.
  sr = color32_u8(sr);
  sg = color32_u8(sg);
  sb = color32_u8(sb);
  sa = color32_u8(sa);
  out.start_r = sr;
  out.start_g = sg;
  out.start_b = sb;
  out.start_a = sa;
  // live dump v2: Unity Particle.GetCurrentColor applies color modules to
  // startColor and returns Color32. Custom1 is a vertex stream, not a color
  // module (2022.3 GetCurrentColor / customData pages). Dump the Color32
  // current here; Custom1 hop stays on draw `r` below.
  out.current_r = color32_u8(sr * cr);
  out.current_g = color32_u8(sg * cg);
  out.current_b = color32_u8(sb * cb);
  out.current_a = color32_u8(sa * ca);
  out.r = out.current_r;
  out.g = out.current_g;
  out.b = out.current_b;
  // Unity Linear writes linear vertex color then the sRGB framebuffer encodes
  // it. Our swapchain is UNORM / display-referred — the official screen of
  // Linear+sRGB is the inspector Color (0.42–1.0), not the 0.15 linear value.
  // Writing linear here hid dark Alpha petals and crushed Add fog.
  // fadeOut clip writes SpriteRenderer.a only. Particles keep startColor × COL
  // until OpenWDS Destroy(0.5s) drops the whole collect window.
  out.a = out.current_a;
  out.uv_name_index = official_split_effect_uv_frame(p, st.start_u, t01, st.fot_u);
  // Current-frame textureRect h/w. Billboard height uses sol_y. Stretch
  // length must not: GetCurrentSize3D.y stays authored; applied LS multiplies
  // the same aspect at mesh time.
  out.sol_y *= official_split_effect_uv_aspect_hw(out.uv_name_index);
  // hop: live dump align2-u1-unity-v1 U1-6 10420 6218 particles RMSE 0.
  // Billboard TSA Sprites: after maxP clamp of GetCurrentSize (width),
  // quad_w = size' × tr_w(i)/tr_w(0), quad_h = size_y' × tr_h(i)/tr_w(0).
  // Bake the width ratio into out.size after trails (trails use GetCurrentSize).
  // Single sprite ≡ prior native. Do not label 2022.3. Do not write U1-6 E.
  if (p.render_mode == kSplitEffectRenderBillboard) {
    billboard_sprite_w_scale = official_split_effect_billboard_sprite_width_norm(
        out.uv_name_index, official_split_effect_uv_first_frame(p));
  }
  out.blend = p.blend;
  out.sorting_order = p.sorting_order;
  out.flags = p.flags;
  out.render_mode = p.render_mode;
  out.length_scale = p.length_scale;
  out.velocity_scale = p.velocity_scale;
  out.camera_velocity_scale = p.camera_velocity_scale;
  out.freeform_stretching = p.freeform_stretching;
  out.rotate_with_stretch = p.rotate_with_stretch;
  out.noise_rotation_amount = p.noise_rotation_amount;
  out.noise_size_amount = p.noise_size_amount;
  out.pivot_x = p.pivot_x;
  // Billboard / Mesh: GPU atlas V is bottom-up vs Unity texture +Y. Negating
  // PSR pivot.y puts World/View spikes on the opposite edge (outward).
  // Stretch kettlewell uses authored PSR pivot.y (OpenWDS renderer.pivot).
  // live dump wrap-v33 1070_5/6: Bake first-quad centroid ≈ particle pose
  // at pivot Y=0.5. Billboard-negate (live=-0.5) put the whole strip on
  // +velocity / past the tip, so wrap never reached the camera. Catalog
  // Stretch |pivot.y|≥0.5 share this fill path (1070_5/6 Local, 19 View,
  // 4032 y=-5). hop / live dump; not 2022.3.
  if (p.render_mode == kSplitEffectRenderStretch) {
    out.pivot_y = p.pivot_y;
  } else {
    out.pivot_y = -p.pivot_y;
  }
  // live dump dump-pending-v2 11580_1 hop (not 2022.3). Billboard only:
  // |centroid−particle| = |(0.5 − Sprite.m_Pivot)| × firstEdges. Combine
  // with PSR in Unity space, then the Billboard Y negate above stays on PSR.
  // dump-pending-v2 has magnitude only (pred 0.1489 vs median 0.143), no
  // signed-X. After Billboard oxp = +pivot.x × width (align2-u1-unity-v1),
  // keep sprite X net = −(sprite−0.5)×width (pre-oxp-flip). Unproven.
  // live_y += (sprite_y − 0.5). Mesh / Stretch keep PSR only.
  if (p.render_mode == kSplitEffectRenderBillboard) {
    float spx = 0.5f;
    float spy = 0.5f;
    official_split_effect_uv_sprite_pivot(out.uv_name_index, spx, spy);
    out.pivot_x -= (spx - 0.5f);
    out.pivot_y += spy - 0.5f;
  }
  out.pivot_z = p.pivot_z;
  // live dump psr-flip-v1 hop (not 2022.3). Freeze U/V swap from birth seed
  // so frames stay stable. Billboard+Stretch draw swaps atlas corners.
  // Mesh / Trails unproven — draw does not apply these bits.
  official_split_effect_psr_flip_uv(st.seed, p.flip_x, p.flip_y, out.flip_u, out.flip_v);
  out.custom1_x = p.custom1_x;
  out.custom1_y = p.custom1_y;
  out.butterfly_wing_x = p.butterfly_wing_x;
  const float rot_u = u01(mix3(st.seed, 14, 1u));
  out.roll = st.rot_z + eval_rotation_over_life(p, age, st.life, rot_u, 2);
  if (p.rotation_3d != 0) {
    out.tilt_x = st.rot_x + eval_rotation_over_life(p, age, st.life, rot_u, 0);
    out.tilt_y = st.rot_y + eval_rotation_over_life(p, age, st.life, rot_u, 1);
  } else {
    out.tilt_x = 0.0f;
    out.tilt_y = 0.0f;
  }
  if (p.rbs_enabled != 0) {
    // Unity 2022.3 Rotation by Speed (Manual/PartSysRotBySpeedModule.html):
    // Angular Velocity is degrees/s (catalog stores radians). Speed Range
    // "is only applied when the velocity is in one of the curve modes."
    // Official 383 systems are TwoConstants (state 3) on every axis — range
    // {0,1} is unused. Do not remap by Main-space speed.
    const uint32_t rbs_extra = 1u;
    uint32_t rbs_rng = rng_start(st.seed, rbs_extra, 31u);
    const float ox = pick(p.rbs_x, p.rbs_x_min, p.rbs_x_random != 0, rng_next(rbs_rng, st.seed, rbs_extra));
    const float oy = pick(p.rbs_y, p.rbs_y_min, p.rbs_y_random != 0, rng_next(rbs_rng, st.seed, rbs_extra));
    const float oz = pick(p.rbs_z, p.rbs_z_min, p.rbs_z_random != 0, rng_next(rbs_rng, st.seed, rbs_extra));
    if (p.rbs_separate != 0) {
      out.tilt_x += ox * age;
      out.tilt_y += oy * age;
      out.roll += oz * age;
    } else {
      out.roll += oz * age;
    }
  }
  if ((p.flags & kSplitEffectFlagNoise) != 0 && std::fabs(p.noise_rotation_amount) > 1e-8f) {
    // live dump v1 (synth-stretch-extra-v16): rotation3D.z += amount * age
    // (degrees). hop_n / curl n failed this pose; n=1 passed the gate.
    // Noise-shaped n remains 未证实. Not a 2022.3 page.
    out.roll += p.noise_rotation_amount * kDegToRad * age;
  }
  // live dump v1: Particle.angularVelocity3D is instantaneous ω (rad/s here;
  // writer emits degrees/s). ROL ω(t01) + RBS constants. rotationAmount does
  // not write ω. Does not change the pose.
  {
    auto omega_axis = [&](uint8_t axis) noexcept {
      if (p.rot_ol_enabled == 0) {
        return 0.0f;
      }
      if (p.rot_ol_separate == 0 && axis != 2) {
        return 0.0f;
      }
      if (axis == 0 && p.rot_ol_separate != 0) {
        return eval_minmax_axis(p.rot_ol_x, p.rot_ol_x_min, p.rot_ol_x_random, p.rot_ol_x_begin,
                                p.rot_ol_x_count, p.rot_ol_x_min_begin, p.rot_ol_x_min_count, t01,
                                rot_u);
      }
      if (axis == 1 && p.rot_ol_separate != 0) {
        return eval_minmax_axis(p.rot_ol_y, p.rot_ol_y_min, p.rot_ol_y_random, p.rot_ol_y_begin,
                                p.rot_ol_y_count, p.rot_ol_y_min_begin, p.rot_ol_y_min_count, t01,
                                rot_u);
      }
      return eval_minmax_axis(p.rot_ol, p.rot_ol_min, p.rot_ol_random, p.rot_ol_begin,
                              p.rot_ol_count, p.rot_ol_min_begin, p.rot_ol_min_count, t01, rot_u);
    };
    out.ang_vx = omega_axis(0);
    out.ang_vy = omega_axis(1);
    out.ang_vz = omega_axis(2);
    if (p.rbs_enabled != 0) {
      const uint32_t rbs_extra = 1u;
      uint32_t rbs_rng = rng_start(st.seed, rbs_extra, 31u);
      const float ox =
          pick(p.rbs_x, p.rbs_x_min, p.rbs_x_random != 0, rng_next(rbs_rng, st.seed, rbs_extra));
      const float oy =
          pick(p.rbs_y, p.rbs_y_min, p.rbs_y_random != 0, rng_next(rbs_rng, st.seed, rbs_extra));
      const float oz =
          pick(p.rbs_z, p.rbs_z_min, p.rbs_z_random != 0, rng_next(rbs_rng, st.seed, rbs_extra));
      if (p.rbs_separate != 0) {
        out.ang_vx += ox;
        out.ang_vy += oy;
        out.ang_vz += oz;
      } else {
        out.ang_vz += oz;
      }
    }
    // live dump v1: angularVelocity3D stays 0 when only rotationAmount is
    // nonzero. Do not write ω from this field.
  }
  out.render_alignment = p.render_alignment;
  if (p.custom1_x_count > 0) {
    out.custom1_x = official_split_effect_eval_curve_keys(p.custom1_x_begin, p.custom1_x_count,
                                                         cd_t01, p.custom1_x);
  }
  if (p.custom1_x_random != 0) {
    float lo = p.custom1_x_min;
    if (p.custom1_x_min_count > 0) {
      lo = official_split_effect_eval_curve_keys(p.custom1_x_min_begin, p.custom1_x_min_count,
                                                 cd_t01, p.custom1_x_min);
    }
    out.custom1_x = lo + (out.custom1_x - lo) * u01(mix3(st.seed, kSaltCustom1, 1u));
  }
  if (p.custom1_y_count > 0) {
    out.custom1_y = official_split_effect_eval_curve_keys(p.custom1_y_begin, p.custom1_y_count,
                                                         cd_t01, p.custom1_y);
  }
  if ((p.flags & kSplitEffectFlagButterfly) != 0) {
    // Official ButterflyEffect: rgb = min(rgb, 1) * (Custom1.x + 1). Not the
    // ParticleAdd/Alpha (x * 0.5 + 1) term. Alpha is not multiplied.
    const float bright = out.custom1_x + 1.0f;
    out.r = std::min(out.r, 1.0f) * bright;
    out.g = std::min(out.g, 1.0f) * bright;
    out.b = std::min(out.b, 1.0f) * bright;
    if (out.butterfly_wing_x <= 1e-6f) {
      out.butterfly_wing_x = kSplitEffectButterflyMeshHalfX;
    }
    out.blend = static_cast<uint8_t>(SplitEffectBlend::Additive);
  } else {
    // OpenWDS ParticleAlpha/Add: color *= (Custom1.x * 0.5 + 1.0). Module off
    // writes uv.z=0 so the multiplier is 1. Do not invent Custom1 for 2020.
    const float bright = out.custom1_x * 0.5f + 1.0f;
    out.r *= bright;
    out.g *= bright;
    out.b *= bright;
    out.a *= bright;
  }
  }
  }
  if (want_fd) {
    constexpr float kDt = 1e-3f;
    // Same-committed reconstruction: (tail(r+dt) - tail(r)) / dt. Avoids
    // comparing two different grid partitions when age and age+dt straddle
    // a step boundary (that FD inflated minVertexDistance on the old
    // equal-split path). Stretch still uses stored/total, not this.
    SplitWallScope wall_int(SplitWallSec::Integrate);
    // Diagnostics only: the finite-difference tail re-runs the integrator, so
    // it belongs in integrate_ms (it is not a separate visible stage, but it
    // is charged to no task scope today).
    SplitDiagScope diag_fd(SplitDiagPhase::Integrate);
    integ.finite_difference(age, kDt, out.world_x, out.main_y, out.z, out.vel_x, out.vel_y,
                            out.vel_z);
  }
  if (want_trail) {
    IntegratorGridState live_lo{};
    integ.export_grid(live_lo);
    SplitWallScope wall_tr(SplitWallSec::Trails);
    WDS_PERF_SCOPE("shuriken.trails");
    // Traverse = per-particle realization of the trail / ribbon vertices
    // (sampling n ages per particle). Charged separately from Integrate so a
    // Stretch/Trail-heavy catalog shows up in its own column.
    SplitDiagScope diag_tr(SplitDiagPhase::Traverse);
    const float clock = trail_clock >= 0.0f ? trail_clock : age;
    fill_trails(integ, st, clock, out);
    integ.import_grid(live_lo);
  } else {
    split_effect_clear_live_trails(out);
  }
  out.size *= billboard_sprite_w_scale;
  set_rol_cache_key(nullptr);
}

namespace {

bool env_dead_skip() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_DEAD_SKIP");
  if (raw == nullptr || raw[0] == '\0') {
    return true;
  }
  return !(raw[0] == '0' || raw[0] == 'n' || raw[0] == 'N' || raw[0] == 'f' || raw[0] == 'F');
}

std::atomic<int> g_dead_skip{-1};

bool dead_skip_on() noexcept {
  int v = g_dead_skip.load(std::memory_order_acquire);
  if (v < 0) {
    const int want = env_dead_skip() ? 1 : 0;
    int expected = -1;
    if (g_dead_skip.compare_exchange_strong(expected, want, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
      return want != 0;
    }
    return expected != 0;
  }
  return v != 0;
}

constexpr float kDeadSkipInf = 1.0e30f;

float max_keep_sim_conservative_uncached(const SplitEffectParticle& p) noexcept {
  float life = std::max(p.lifetime, p.lifetime_min);
  if (p.life_curve_count > 0) {
    using split_effect_catalog_detail::kSplitEffectCurveKeys;
    for (uint8_t i = 0; i < p.life_curve_count; ++i) {
      life = std::max(life, kSplitEffectCurveKeys[p.life_curve_begin + i].v);
    }
    for (int s = 0; s <= 32; ++s) {
      life = std::max(life, official_split_effect_eval_start_lifetime(
                                p, static_cast<float>(s) / 32.0f));
    }
    life *= 2.0f;
  }
  life = std::max(life, 1e-4f);
  return particle_keep_sim_conservative(p, life);
}

// Catalog-only (lifetime / trail flags / curve keys). Same value every
// call; remember by table index so parent-event tree walks do not
// re-sample 33 lifetime points. Not a simulation cache.
float max_keep_sim_conservative(const SplitEffectParticle& p) noexcept {
  using split_effect_catalog_detail::kSplitEffectParticles;
  const ptrdiff_t n = static_cast<ptrdiff_t>(std::size(kSplitEffectParticles));
  const ptrdiff_t i = &p - kSplitEffectParticles;
  if (i < 0 || i >= n) {
    return max_keep_sim_conservative_uncached(p);
  }
  static std::vector<std::atomic<uint32_t>> memo = [] {
    std::vector<std::atomic<uint32_t>> v(static_cast<size_t>(n));
    for (auto& a : v) {
      a.store(0, std::memory_order_relaxed);
    }
    return v;
  }();
  const uint32_t bits = memo[static_cast<size_t>(i)].load(std::memory_order_relaxed);
  if (bits != 0) {
    float cached = 0.0f;
    std::memcpy(&cached, &bits, sizeof(cached));
    return cached;
  }
  const float v = max_keep_sim_conservative_uncached(p);
  uint32_t store = 0;
  std::memcpy(&store, &v, sizeof(store));
  if (store == 0) {
    store = 1;
  }
  memo[static_cast<size_t>(i)].store(store, std::memory_order_relaxed);
  return v;
}

float parent_last_death_wall(const SplitEffectParticle& p) noexcept {
  if ((p.flags & kSplitEffectFlagLooping) != 0) {
    return kDeadSkipInf;
  }
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = start_delay_wall_clock(p);
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  return delay + (length + max_keep_sim_conservative(p)) / speed;
}

const SplitEffectSubEmitter* subemitters_of(uint16_t parent_index, uint16_t* nslots) noexcept {
  struct Idx {
    std::vector<uint16_t> first;
    std::vector<uint16_t> n;
    Idx() {
      using split_effect_catalog_detail::kSplitEffectSubEmitterCount;
      using split_effect_catalog_detail::kSplitEffectSubEmitters;
      uint16_t max_p = 0;
      for (uint16_t i = 0; i < kSplitEffectSubEmitterCount; ++i) {
        max_p = std::max(max_p, kSplitEffectSubEmitters[i].parent_index);
      }
      first.assign(static_cast<size_t>(max_p) + 1u, 0);
      n.assign(static_cast<size_t>(max_p) + 1u, 0);
      for (uint16_t i = 0; i < kSplitEffectSubEmitterCount; ++i) {
        const uint16_t p = kSplitEffectSubEmitters[i].parent_index;
        if (n[p] == 0) {
          first[p] = i;
        }
        ++n[p];
      }
    }
  };
  static const Idx idx;
  if (parent_index >= idx.n.size() || idx.n[parent_index] == 0) {
    if (nslots != nullptr) {
      *nslots = 0;
    }
    return nullptr;
  }
  if (nslots != nullptr) {
    *nslots = idx.n[parent_index];
  }
  return split_effect_catalog_detail::kSplitEffectSubEmitters + idx.first[parent_index];
}

float tree_last_alive_wall(const SplitEffectParticle* ps, uint16_t count, uint32_t base,
                           const SplitEffectParticle&, uint32_t node_idx, int depth,
                           float node_last_death) noexcept {
  float last = node_last_death;
  if (depth >= 6 || node_last_death >= kDeadSkipInf * 0.5f) {
    return last;
  }
  uint16_t nslots = 0;
  const SplitEffectSubEmitter* slots = subemitters_of(static_cast<uint16_t>(node_idx), &nslots);
  if (slots == nullptr || nslots == 0) {
    return last;
  }
  for (uint16_t s = 0; s < nslots; ++s) {
    if (slots[s].type != kSplitEffectSubEmitBirth && slots[s].type != kSplitEffectSubEmitDeath) {
      continue;
    }
    if (slots[s].child_index < base) {
      return kDeadSkipInf;
    }
    const uint32_t local = static_cast<uint32_t>(slots[s].child_index) - base;
    if (local >= count) {
      return kDeadSkipInf;
    }
    const SplitEffectParticle& child = ps[static_cast<uint16_t>(local)];
    if ((child.flags & kSplitEffectFlagLooping) != 0) {
      return kDeadSkipInf;
    }
    const float csim = child.simulation_speed > 1e-6f ? child.simulation_speed : 1.0f;
    const float ckeep = max_keep_sim_conservative(child);
    const float clen = child.length_in_sec > 1e-6f ? child.length_in_sec : 1.0f;
    const float death_pad =
        slots[s].type == kSplitEffectSubEmitDeath ? kSplitEffectFrameSec : 0.0f;
    const float child_last = node_last_death + death_pad + std::max(ckeep, clen) / csim;
    last = std::max(last, child_last);
    last = std::max(last, tree_last_alive_wall(ps, count, base, child, slots[s].child_index,
                                               depth + 1, child_last));
    if (last >= kDeadSkipInf * 0.5f) {
      return kDeadSkipInf;
    }
  }
  return last;
}

}  // namespace

void set_dead_system_skip(bool enabled) noexcept {
  g_dead_skip.store(enabled ? 1 : 0, std::memory_order_release);
}

bool dead_system_skip_enabled() noexcept {
  return dead_skip_on();
}

bool primary_eval_can_emit(const SplitEffectParticle& p, float now_t) noexcept {
  if (p.go_active == 0) {
    return false;
  }
  if (!dead_skip_on() || observers_installed()) {
    return true;
  }
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = start_delay_wall_clock(p);
  const float now_sim = (now_t - delay) * speed;
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  const float max_life = std::max(p.lifetime, p.lifetime_min);
  const float keep = particle_keep_sim_conservative(p, max_life);
  if (now_sim < 0.0f || (!looping && now_sim > length + keep)) {
    return false;
  }
  return true;
}

bool subemit_eval_can_emit(const SplitEffectParticle* particles, uint16_t count, int element_slot,
                           const EmitContext& ctx) noexcept {
  if (particles == nullptr || count == 0) {
    return false;
  }
  if (!dead_skip_on() || observers_installed()) {
    return true;
  }
  const uint32_t base = bound_catalog_index(particles, count, ctx, particles[0]);
  float last = -kDeadSkipInf;
  bool any_slot = false;
  for (uint16_t pi = 0; pi < count; ++pi) {
    const SplitEffectParticle& parent = particles[pi];
    if (static_cast<int>(parent.element_index) != element_slot || parent.go_active == 0) {
      continue;
    }
    const uint32_t parent_idx = bound_catalog_index(particles, count, ctx, parent);
    uint16_t nslots = 0;
    const SplitEffectSubEmitter* slots =
        subemitters_of(static_cast<uint16_t>(parent_idx), &nslots);
    if (slots == nullptr || nslots == 0) {
      continue;
    }
    bool fire = false;
    for (uint16_t s = 0; s < nslots; ++s) {
      if (slots[s].type == kSplitEffectSubEmitBirth || slots[s].type == kSplitEffectSubEmitDeath) {
        fire = true;
        break;
      }
    }
    if (!fire) {
      continue;
    }
    any_slot = true;
    if ((parent.flags & kSplitEffectFlagLooping) != 0) {
      return true;
    }
    const float delay = start_delay_wall_clock(parent);
    if (ctx.now_t + 1e-4f < delay) {
      continue;
    }
    const float p_last = parent_last_death_wall(parent);
    if (p_last >= kDeadSkipInf * 0.5f) {
      return true;
    }
    last = std::max(last, tree_last_alive_wall(particles, count, base, parent, parent_idx, 0, p_last));
    if (last >= kDeadSkipInf * 0.5f) {
      return true;
    }
  }
  if (!any_slot) {
    return false;
  }
  if (last < -kDeadSkipInf * 0.5f) {
    return false;
  }
  return ctx.now_t <= last + 1e-3f;
}

namespace {

void refresh_carried_state(ParticleState& st, const EmitContext& ctx,
                           const SplitEffectParticle& p) noexcept {
  st.p = &p;
  st.color_id = ctx.color_id;
  st.line_index = ctx.line_index;
  st.line_x = ctx.line_x;
  st.fade_scale = ctx.fade_scale;
  st.line_alpha = ctx.line_alpha;
}

uint64_t hash_system_inputs(const SplitEffectParticle& p, const EmitContext& ctx) {
  // Same prefix as note_eval_system_hash. Walk/persist keys reuse it when
  // the TLS system already matches (fill_parent_sys_hoist warms it).
  uint64_t h = (g_tls_sys_hash.p == &p && g_tls_sys_hash.color_id == ctx.color_id)
                   ? g_tls_sys_hash.prefix
                   : hash_system_sim_inputs(&p, ctx.color_id);
  h = mix64(h, ctx.parent_motion.rod_motion_candidate ? 1u : 0u);
  h = mix64(h, ctx.parent_motion.emitter_vel_candidate ? 1u : 0u);
  h = hash_f32(h, ctx.parent_motion.emitter_vx);
  h = hash_f32(h, ctx.parent_motion.emitter_vy);
  h = hash_f32(h, ctx.parent_motion.emitter_vz);
  h = mix64(h, ctx.parent_motion.birth_pose_candidate ? 1u : 0u);
  h = hash_f32(h, ctx.parent_motion.birth_origin_y_delta);
  return h;
}

SystemCarryKey make_system_key(const EmitContext& ctx, const SplitEffectParticle& p, uint8_t kind) {
  SystemCarryKey key;
  key.epoch = split_eval_cache_epoch();
  key.color_id = ctx.color_id;
  key.line_index = ctx.line_index;
  key.line_start_ms = ctx.line_start_ms;
  key.catalog_index = system_id_of(ctx, p);
  key.line_play_gen = ctx.line_play_gen != 0 ? ctx.line_play_gen : 1u;
  key.input_hash = hash_system_inputs(p, ctx);
  key.kind = kind;
  return key;
}

void fill_job_clock(PrimaryEvalJob& job, const SplitEffectParticle& p, const EmitContext& ctx) {
  const float sim = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  job.raw_age = ctx.now_t * sim - job.b.time * sim;
  job.age = std::max(job.raw_age, 0.0f);
  job.emit_u = system_emit_u(p, birth_curve_t(job.b));
  job.seed = particle_seed(ctx, p, job.b);
}

float plan_exact_trail_mul(const SplitEffectParticle& p, uint32_t seed, uint32_t extra) noexcept {
  if (p.trail_lifetime_count > 0) {
    return trail_lifetime_mul_hi(p);
  }
  return pick(p.trail_lifetime, p.trail_lifetime_min, p.trail_lifetime_random != 0,
              mix3(seed, kSaltTrailLifetime, extra));
}

void finish_plan_job(PrimaryEvalJob& job, const SplitEffectParticle& p, const EmitContext& ctx) {
  const float trail_mul = p.trail_lifetime_count > 0
                              ? trail_lifetime_mul_hi(p)
                              : (job.st.trail_life_mul >= 0.0f ? job.st.trail_life_mul
                                                               : p.trail_lifetime);
  const float max_age = particle_keep_sim(p, job.st.life, trail_mul);
  if (job.age >= max_age) {
    return;
  }
  job.alive = 1;
  job.pose_age = job.age < job.st.life ? job.age : std::max(job.st.life - 1e-4f, 0.0f);
  float a = 1.0f;
  {
    SplitWallScope wall_alpha(SplitWallSec::AlphaPlan);
    WDS_PERF_SCOPE("shuriken.alpha");
    a = live_draw_alpha(job.st, job.age);
    job.alpha_keep = primary_alpha_keep(p, ctx, job.raw_age, a) ? 1 : 0;
  }
  job.do_fill = job.alpha_keep;
  // Local A/B: WDS_SPLIT_ALPHA0_SKIP=1 skips appearance for a<0.001 and no
  // trails. Default off. Adopt only if isolated gain is >= ~3 ms.
  if (job.do_fill != 0 && a < 0.001f && (p.flags & kSplitEffectFlagTrails) == 0) {
    static const bool skip_alpha0 = [] {
      const char* env = std::getenv("WDS_SPLIT_ALPHA0_SKIP");
      return env != nullptr && env[0] == '1' && env[1] == '\0';
    }();
    if (skip_alpha0) {
      job.do_fill = 0;
    }
  }
}

void make_state_job(PrimaryEvalJob& job, const EmitContext& ctx, const SplitEffectParticle& p) {
  SplitWallScope wall_ms(SplitWallSec::MakeState);
  WDS_PERF_SCOPE("shuriken.make_state");
  job.st = make_state(ctx, p, job.b.index, job.b.extra, job.emit_u, job.b.time, job.seed);
  if (job.b.parent_pose != 0) {
    job.st.birth_parent_z = job.b.parent_z;
  }
  job.st.birth_fade = applied_birth_fade(ctx, job.b.time);
  job.has_grid = 0;
  job.has_state = 1;
}

void adopt_stored_state(PrimaryEvalJob& job, const ParticleState& st, const IntegratorGridState& grid,
                        uint8_t has_grid, const EmitContext& ctx, const SplitEffectParticle& p) {
  SplitWallScope wall_adopt(SplitWallSec::AdoptCarry);
  job.st = st;
  refresh_carried_state(job.st, ctx, p);
  // Child persist keeps baked event_wx / birth_parent_z. Child Birth.parent_pose
  // is not the bake (parent_pose on that Birth is unused for children).
  if (job.is_child == 0 && job.b.parent_pose != 0) {
    job.st.birth_parent_z = job.b.parent_z;
  }
  job.st.birth_fade = applied_birth_fade(ctx, job.b.time);
  job.grid = grid;
  job.has_grid = has_grid;
  job.has_state = 1;
}

void adopt_carry_job(PrimaryEvalJob& job, const CarryParticle& cp, const EmitContext& ctx,
                     const SplitEffectParticle& p) {
  adopt_stored_state(job, cp.st, cp.grid, cp.has_grid, ctx, p);
}

void commit_primary_carry(const SystemCarryKey& key, const EmitContext& ctx,
                          const std::vector<PrimaryEvalJob>& jobs) {
  if (!split_eval_carry_enabled()) {
    return;
  }
  thread_local std::vector<CarryParticle> parts;
  parts.clear();
  parts.reserve(jobs.size());
  for (const PrimaryEvalJob& job : jobs) {
    if (job.alive == 0 || job.has_state == 0) {
      continue;
    }
    CarryParticle cp;
    cp.b = job.b;
    cp.st = job.st;
    cp.st.p = nullptr;
    cp.grid = job.grid;
    cp.seed = job.seed;
    cp.emit_u = job.emit_u;
    cp.has_grid = job.has_grid;
    parts.push_back(cp);
  }
  std::sort(parts.begin(), parts.end(), [](const CarryParticle& a, const CarryParticle& b) {
    if (a.b.index != b.b.index) {
      return a.b.index < b.b.index;
    }
    return a.b.extra < b.b.extra;
  });
  int next_birth = 0;
  for (const PrimaryEvalJob& job : jobs) {
    if (job.b.index + 1 > next_birth) {
      next_birth = job.b.index + 1;
    }
  }
  split_eval_carry_store(key, ctx.now_t, std::move(parts), next_birth);
}

bool try_primary_carry(const EmitContext& ctx, const SplitEffectParticle& p,
                       SystemCarrySnapshot& snap) {
  if (!split_eval_carry_enabled() || observers_installed()) {
    return false;
  }
  const SystemCarryKey key = make_system_key(ctx, p, 0);
  if (split_eval_carry_lookup_for_time(key, ctx.now_t, snap)) {
    return true;
  }
  split_eval_carry_note_fallback();
  return false;
}

}  // namespace

void begin_eval_system_hash_frame() noexcept {
  g_sys_hash_frame_token.fetch_add(1, std::memory_order_relaxed);
}

void bump_eval_system_hash_config_generation() noexcept {
  g_sys_hash_config_gen.fetch_add(1, std::memory_order_relaxed);
}

void evaluate_plan(const SplitEffectParticle& p, const EmitContext& ctx,
                   std::vector<PrimaryEvalJob>& jobs) {
  jobs.clear();
  if (p.go_active == 0) {
    return;
  }
  SplitWallScope wall_plan(SplitWallSec::PlanGate);
  {
    SplitWallScope wall_gate(SplitWallSec::PlanGate);
    note_eval_system_hash(p, ctx.color_id);
    if (g_runtime_capability_observer != nullptr) {
      g_runtime_capability_observer(p, ctx, runtime_capability_status(p));
    }
  }
  thread_local std::vector<Birth> births;
  births.clear();
  if (births.capacity() < 64) {
    births.reserve(64);
  }
  // Always remake the birth list. Carry now stores only cap-kept particles
  // (has_state after realize), so extend-from-snap.parts() would drop
  // discarded-but-alive seeds. collect_births is the remake path and is
  // byte-identical. hop schedule, not 2022.3.
  {
    WDS_PERF_SCOPE("shuriken.births");
    SplitWallScope wall_births(SplitWallSec::Births);
    collect_births(p, ctx.now_t, ctx.color_id, births, &ctx);
  }
  {
    SplitWallScope wall_cap(SplitWallSec::CapSort);
    apply_max_particles_alive_cap(p, ctx, births);
  }
  WDS_PERF_COUNT("shuriken.births", static_cast<int64_t>(births.size()));
  constexpr float kBirthAgeEpsilon = 1e-5f;
  {
    WDS_PERF_SCOPE("shuriken.evaluate.plan");
    SplitWallScope wall_filter(SplitWallSec::BirthFilter);
    jobs.clear();
    if (jobs.capacity() < births.size()) {
      SplitWallScope wall_grow(SplitWallSec::VecGrow);
      jobs.reserve(births.size());
    }
    for (const Birth& b : births) {
      PrimaryEvalJob job{};
      job.b = b;
      fill_job_clock(job, p, ctx);
      if (job.raw_age < -kBirthAgeEpsilon) {
        continue;
      }
      WDS_PERF_COUNT("deadskip.primary_jobs", 1);
      const float life = birth_start_life(p, ctx, job.b, job.emit_u);
      const float trail_mul = plan_exact_trail_mul(p, job.seed, job.b.extra);
      if (job.age >= particle_keep_sim(p, life, trail_mul)) {
        WDS_PERF_COUNT("deadskip.primary_skip", 1);
        continue;
      }
      // Closed-form keep ≡ make_state life + trail pick. primary_alpha_keep
      // is always true (count-fix-v1). Defer make_state until realize so the
      // preview-pool cap can drop candidates first. hop, not 2022.3.
      job.st.life = life;
      job.alive = 1;
      job.alpha_keep = 1;
      job.do_fill = 1;
      job.has_state = 0;
      job.pose_age = job.age < life ? job.age : std::max(life - 1e-4f, 0.0f);
      jobs.push_back(job);
    }
  }
}

bool job_realize_kept(const PrimaryEvalJob& job, bool write_unfilled_stamps) noexcept {
  if (job.alive == 0 || job.alpha_keep == 0) {
    return false;
  }
  return write_unfilled_stamps || job.do_fill != 0;
}

int finish_realize_outputs(const SplitEffectParticle& p, const EmitContext& ctx,
                           std::vector<PrimaryEvalJob>& jobs, SplitEffectLiveParticle* out,
                           int written, bool commit_carry) {
  const uint32_t cat = system_id_of(ctx, p);
  if (ctx.want_trail && p.trail_mode == 1) {
    SplitWallScope wall_stitch(SplitWallSec::Stitch);
    SplitDiagScope diag_stitch(SplitDiagPhase::Stitch);
    stitch_ribbons(p, out, written, ctx.line_index, cat);
  }
  if (commit_carry && split_eval_carry_enabled() && !observers_installed()) {
    commit_primary_carry(make_system_key(ctx, p, 0), ctx, jobs);
  }
  WDS_PERF_COUNT("shuriken.alive", written);
#if WDS_DIAG_ENABLED
  split_wall_note_realized(written);
#endif  // WDS_DIAG_ENABLED
  return written;
}

int evaluate_realize_scalar(const SplitEffectParticle& p, const EmitContext& ctx,
                            std::vector<PrimaryEvalJob>& jobs, SplitEffectLiveParticle* out,
                            int max_out, bool write_unfilled_stamps) {
  SplitWallScope wall_real(SplitWallSec::RealizeMatch);
  note_eval_system_hash(p, ctx.color_id);
  const bool persist_ok =
      persist_primary_eligible(p) && !observers_installed();
  PersistClaim persist_claim;
  const SystemCarryKey persist_key = make_system_key(ctx, p, 0);
  if (persist_ok) {
    {
      SplitWallScope wall_claim(SplitWallSec::PersistClaim);
      split_eval_persist_claim(persist_key, ctx.now_t, persist_claim);
    }
    if (persist_claim.kind == PersistClaimKind::Forward) {
      persist_cull_dead(persist_claim, p, ctx.now_t);
    }
  }
  const bool resume_persist = persist_ok && persist_claim.kind == PersistClaimKind::Forward &&
                              persist_slot_count(persist_claim) > 0;
  SystemCarrySnapshot snap;
  const bool resume_carry = !resume_persist && try_primary_carry(ctx, p, snap);
  int written = 0;
  int persist_cursor = 0;
  {
    WDS_PERF_SCOPE("shuriken.evaluate.realize");
    for (PrimaryEvalJob& job : jobs) {
      job.write_slot = 0xffffffffu;
      if (!job_realize_kept(job, write_unfilled_stamps) || written >= max_out) {
        continue;
      }
      if (job.has_state == 0 && job.do_fill != 0 && resume_persist) {
        SplitWallScope wall_match(SplitWallSec::RealizeMatch);
        int idx = -1;
        const int nparts = persist_slot_count(persist_claim);
        while (job.persist_i < 0 && persist_cursor < nparts) {
          const PersistSlot* slot = persist_slot_at(persist_claim, persist_cursor);
          if (slot == nullptr) {
            break;
          }
          if (carry_birth_eq(slot->b, job.b) && slot->seed == job.seed) {
            idx = persist_cursor;
            ++persist_cursor;
            break;
          }
          if (slot->b.index < job.b.index ||
              (slot->b.index == job.b.index && slot->b.extra < job.b.extra)) {
            ++persist_cursor;
            continue;
          }
          break;
        }
        if (idx < 0) {
          idx = persist_find_slot(persist_claim, job.b, job.seed);
        }
        if (idx >= 0) {
          const PersistSlot* slot = persist_slot_at(persist_claim, idx);
          if (slot != nullptr) {
            adopt_stored_state(job, slot->st, slot->grid, slot->has_grid, ctx, p);
            job.persist_i = idx;
          }
        }
      }
      if (job.has_state == 0 && resume_carry) {
        SplitWallScope wall_match(SplitWallSec::RealizeMatch);
        const int idx = carry_find_particle(snap, job.b, job.seed);
        if (idx >= 0) {
          adopt_carry_job(job, snap.parts()[static_cast<size_t>(idx)], ctx, p);
        }
      }
      if (job.do_fill != 0 && job.has_state == 0) {
        make_state_job(job, ctx, p);
      }
      evaluate_realize_job(p, ctx, job, out[written], nullptr);
      job.write_slot = static_cast<uint32_t>(written);
      ++written;
    }
  }
  if (persist_ok) {
    persist_finish(persist_claim, persist_key, ctx.now_t, jobs);
  } else {
    persist_release(persist_claim);
  }
  return finish_realize_outputs(p, ctx, jobs, out, written, !persist_ok);
}

void evaluate_realize_job(const SplitEffectParticle& p, const EmitContext& ctx, PrimaryEvalJob& job,
                          SplitEffectLiveParticle& dest, IntegratorHot* hot) {
  const uint32_t stamp_cat = catalog_index(p);
  if (job.do_fill == 0) {
    dest = SplitEffectLiveParticle{};
    stamp_evaluated_live(dest, ctx, p, job.st, job.age, stamp_cat);
    return;
  }
  WDS_PERF_COUNT("shuriken.integrate.realize", 1);
  const uint32_t cat = system_id_of(ctx, p);
  ParticleCacheKey key{};
  if (job.has_grid == 0) {
    key = make_cache_key(ctx, p, job.b, job.seed, job.st, cat, &g_tls_sys_hash);
  }
  if (hot == nullptr && job.persist_i >= 0) {
    hot = persist_claimed_hot(job.persist_i);
  }
  PoseIntegrator integ(job.st);
  if (job.has_grid != 0) {
    integ.import_carry_grid(job.grid);
  }
  if (hot != nullptr) {
    integ.bind_hot(hot);
  }
  integ.set_cache_key(key.valid != 0 ? &key : nullptr);
  fill_live(integ, job.st, job.pose_age, ctx.want_trail, dest, true, job.age);
  integ.export_grid(job.grid);
  job.has_grid = 1;
  stamp_evaluated_live(dest, ctx, p, job.st, job.age, stamp_cat);
}

int evaluate_realize(const SplitEffectParticle& p, const EmitContext& ctx,
                     std::vector<PrimaryEvalJob>& jobs, SplitEffectLiveParticle* out, int max_out,
                     bool write_unfilled_stamps) {
  if (out == nullptr || max_out <= 0) {
    return 0;
  }
  return evaluate_realize_scalar(p, ctx, jobs, out, max_out, write_unfilled_stamps);
}

void evaluate_drop_primary_carry(const EmitContext& ctx, const SplitEffectParticle& p) noexcept {
  split_eval_carry_drop(make_system_key(ctx, p, 0));
}

int evaluate(const SplitEffectParticle& p, const EmitContext& ctx, SplitEffectLiveParticle* out,
             int max_out) {
  if (out == nullptr || max_out <= 0) {
    return 0;
  }
  if (p.go_active == 0) {
    return 0;
  }
  thread_local std::vector<PrimaryEvalJob> jobs;
  jobs.clear();
  evaluate_plan(p, ctx, jobs);
  return evaluate_realize(p, ctx, jobs, out, max_out);
}

namespace {

struct SubSlotOut {
  uint16_t slot_i = 0;
  uint32_t local_child = 0;
  uint8_t type = 0;
  uint8_t properties = 0;
  std::shared_ptr<std::vector<Birth>> births_hold;
  std::vector<Birth> births;
  std::vector<SubParentPose> poses;
  std::vector<uint8_t> pose_ready;
  std::vector<uint8_t> pose_hydrated;
  std::vector<SplitEffectLiveParticle> lives;
  std::vector<uint8_t> keep;
  std::vector<CarryParticle> carry;
  std::vector<SubemitFillItem> items;
  size_t burst_count = 0;
  int n_full_births = 0;
  uint8_t incr_resume = 0;
};

size_t slot_birth_n(const SubSlotOut& s) noexcept {
  if (!s.births.empty()) {
    return s.births.size();
  }
  return s.births_hold ? s.births_hold->size() : 0;
}

const Birth& slot_birth_at(const SubSlotOut& s, size_t i) noexcept {
  if (!s.births.empty()) {
    return s.births[i];
  }
  return (*s.births_hold)[i];
}

void slot_acquire_vecs(SubSlotOut& dest) {
  dest.births.clear();
  dest.poses.clear();
  dest.pose_ready.clear();
  dest.pose_hydrated.clear();
  dest.keep.clear();
  dest.carry.clear();
  dest.items.clear();
  dest.lives.clear();
}

std::shared_ptr<std::vector<Birth>> take_birth_shared(std::size_t reserve_n = 0) {
  auto p = std::make_shared<std::vector<Birth>>();
  if (reserve_n > 0) {
    p->reserve(reserve_n);
  }
  return p;
}

std::shared_ptr<std::vector<SubemitWalkPose>> take_walk_poses(std::size_t n) {
  return std::make_shared<std::vector<SubemitWalkPose>>(n);
}

void slot_release_vecs(SubSlotOut& dest) {
  dest.births_hold.reset();
  dest.births.clear();
  dest.poses.clear();
  dest.pose_ready.clear();
  dest.pose_hydrated.clear();
  dest.lives.clear();
  dest.items.clear();
  dest.keep.clear();
  dest.carry.clear();
}

// WDS_SPLIT_DEFER_ROT=0 restores pre-change PLAN: make_state + pose_at
// in collect_subemit_parent_event (no deferred hydrate). Default on:
// interleaved A/B 10990 play (same load) showed defer not worse.
bool split_defer_rot_parent_pose() noexcept {
  static const bool on = [] {
    const char* raw = std::getenv("WDS_SPLIT_DEFER_ROT");
    if (raw == nullptr || raw[0] == '\0') {
      return true;
    }
    const std::string_view v(raw);
    return v != "0" && v != "off" && v != "false" && v != "OFF";
  }();
  return on;
}

struct SubEventWork {
  uint16_t local = 0;
  uint32_t parent_idx = 0;
  Birth b{};
  std::vector<SubSlotOut> slots;
  CarryParticle parent_cp{};
};

std::vector<std::vector<SubEventWork>>& tls_event_vecs() {
  thread_local std::vector<std::vector<SubEventWork>> pool;
  return pool;
}

std::vector<SubEventWork> take_event_vec(std::size_t n) {
  auto& pool = tls_event_vecs();
  std::vector<SubEventWork> v;
  if (!pool.empty()) {
    v = std::move(pool.back());
    pool.pop_back();
  }
  if (v.size() < n) {
    v.resize(n);
  }
  return v;
}

void give_event_vec(std::vector<SubEventWork>&& v, std::size_t used) {
  for (std::size_t i = 0; i < used && i < v.size(); ++i) {
    for (auto& s : v[i].slots) {
      slot_release_vecs(s);
    }
  }
  if (tls_event_vecs().size() >= 32) {
    return;
  }
  tls_event_vecs().push_back(std::move(v));
}

void pose_to_stored(const SubParentPose& a, SubemitWalkPose& b) noexcept {
  b.event_t = a.event_t;
  b.type = a.type;
  b.wx = a.wx;
  b.wy = a.wy;
  b.wz = a.wz;
  b.size = a.size;
  b.size_y = a.size_y;
  b.size_z = a.size_z;
  b.r = a.r;
  b.g = a.g;
  b.b = a.b;
  b.a = a.a;
  b.roll = a.roll;
  b.tilt_x = a.tilt_x;
  b.tilt_y = a.tilt_y;
  b.life = a.life;
  b.age = a.age;
  b.duration_u = a.duration_u;
  b.seed = a.seed;
  b.birth_origin_y_delta = a.birth_origin_y_delta;
  b.written = 1;
}

void stored_to_pose(const SubemitWalkPose& a, SubParentPose& b) noexcept {
  b.event_t = a.event_t;
  b.type = a.type;
  b.wx = a.wx;
  b.wy = a.wy;
  b.wz = a.wz;
  b.size = a.size;
  b.size_y = a.size_y;
  b.size_z = a.size_z;
  b.r = a.r;
  b.g = a.g;
  b.b = a.b;
  b.a = a.a;
  b.roll = a.roll;
  b.tilt_x = a.tilt_x;
  b.tilt_y = a.tilt_y;
  b.life = a.life;
  b.age = a.age;
  b.duration_u = a.duration_u;
  b.seed = a.seed;
  b.birth_origin_y_delta = a.birth_origin_y_delta;
}

SubemitWalkKey make_walk_key(const EmitContext& ctx, const SplitEffectParticle& parent, const Birth& b,
                             uint16_t slot_i, const SystemCarryKey* hoisted_parent = nullptr) {
  SubemitWalkKey key;
  key.parent = hoisted_parent != nullptr ? *hoisted_parent : make_system_key(ctx, parent, 0);
  key.birth_index = b.index;
  key.birth_extra = b.extra;
  std::memcpy(&key.birth_time_bits, &b.time, sizeof(key.birth_time_bits));
  key.slot_i = slot_i;
  return key;
}

void march_attached_rate(const SplitEffectParticle& child, const ParticleState& pst, const Birth& b,
                         float emit_end, int cap, bool child_loop, float child_len, float child_sim,
                         uint32_t child_i, bool need_rod_pose, PoseIntegrator& parent_integ,
                         float& last_pose_age, SubemitWalkCursor& cur, std::vector<Birth>& births,
                         int prefix_n,
                         void (*on_emit)(void*, float t1, float wx, float wy, float wz, bool have_xyz,
                                         int lo, int hi),
                         void* on_emit_ctx) {
  // hop (not 2022.3): default 60Hz snap-to-step-end. WDS_SPLIT_WALK_HZ=30|120
  // selects the coarser / finer walk.
  // Event-time interpolation was rejected: pose_at would miss the step-end
  // cache and re-integrate backward, cancelling the cheaper march.
  const float kDt = 1.0f / static_cast<float>(split_walk_hz());
  float t = cur.t;
  float frac_t = cur.frac_t;
  float frac_d = cur.frac_d;
  float prev_x = cur.prev_x;
  float prev_y = cur.prev_y;
  float prev_z = cur.prev_z;
  bool have_prev = cur.have_prev != 0;
  int k = cur.k;
  auto total = [&]() { return prefix_n + static_cast<int>(births.size()); };
  for (; t < emit_end - 1e-8f && total() < cap && k < cap;) {
    const int before_emit = static_cast<int>(births.size());
    const float step = t + kDt;
    const float t1 = std::min(step, emit_end);
    if (!(t1 > t)) {
      break;
    }
    const float play = (t1 - b.time) * child_sim;
    if (!child_loop && play > child_len + 1e-6f) {
      break;
    }
    float u = 0.0f;
    if (child_len > 1e-6f) {
      if (child_loop) {
        float wrapped = std::fmod(play, child_len);
        if (wrapped < 0.0f) {
          wrapped += child_len;
        }
        u = wrapped / child_len;
      } else {
        u = std::clamp(play / child_len, 0.0f, 1.0f);
      }
    }
    const float rot = official_split_effect_eval_rate_over_time(child, u);
    float rod = official_split_effect_eval_curve_keys(child.rod_curve_begin, child.rod_curve_count, u,
                                                     child.rate_over_distance);
    if (child.rod_min_curve_count > 0) {
      const float lo = official_split_effect_eval_curve_keys(
          child.rod_min_curve_begin, child.rod_min_curve_count, u, child.rate_over_distance_min);
      rod = pick(rod, lo, true, mix3(child_i, static_cast<uint32_t>(k + 1), 3u));
    } else if (child.rod_random != 0 && child.rod_curve_count == 0) {
      rod = pick(child.rate_over_distance, child.rate_over_distance_min, true,
                 mix3(child_i, static_cast<uint32_t>(k + 1), 3u));
    }
    SubParentPose step_pose;
    if (need_rod_pose) {
      const float age = std::max(0.0f, t1 - b.time);
      const float pose_age = age < pst.life ? age : std::max(pst.life - 1e-4f, 0.0f);
      WDS_PERF_COUNT("shuriken.integrate.walk", 1);
      if (pose_age + 1e-7f < last_pose_age) {
        PoseIntegrator tmp(pst);
        tmp.set_cache_key(parent_integ.cache_key());
        tmp.sample(pose_age, step_pose.wx, step_pose.wy, step_pose.wz, nullptr);
      } else {
        parent_integ.sample(pose_age, step_pose.wx, step_pose.wy, step_pose.wz, nullptr);
        last_pose_age = pose_age;
      }
    }
    if (rot > 1e-6f) {
      frac_t += rot * (t1 - t);
      while (frac_t >= 1.0f && total() < cap && k < cap) {
        frac_t -= 1.0f;
        Birth cb;
        cb.time = t1;
        cb.index = total();
        cb.extra = 800u + static_cast<uint32_t>(k);
        births.push_back(cb);
        ++k;
      }
    }
    if (need_rod_pose && rod > 1e-6f && have_prev) {
      const float dx = step_pose.wx - prev_x;
      const float dy = step_pose.wy - prev_y;
      const float dz = step_pose.wz - prev_z;
      const float dS = std::sqrt(dx * dx + dy * dy + dz * dz);
      frac_d += split_effect_rod_seeds_from_ps_world_path(rod, dS);
      while (frac_d >= 1.0f && total() < cap && k < cap) {
        frac_d -= 1.0f;
        Birth cb;
        cb.time = t1;
        cb.index = total();
        cb.extra = 900u + static_cast<uint32_t>(k);
        births.push_back(cb);
        ++k;
      }
    }
    if (need_rod_pose) {
      prev_x = step_pose.wx;
      prev_y = step_pose.wy;
      prev_z = step_pose.wz;
      have_prev = true;
    }
    if (on_emit != nullptr && static_cast<int>(births.size()) > before_emit) {
      on_emit(on_emit_ctx, t1, step_pose.wx, step_pose.wy, step_pose.wz, need_rod_pose, before_emit,
              static_cast<int>(births.size()));
    }
    const bool full_step = (t1 == step);
    if (full_step) {
      cur.t = t1;
      cur.frac_t = frac_t;
      cur.frac_d = frac_d;
      cur.prev_x = prev_x;
      cur.prev_y = prev_y;
      cur.prev_z = prev_z;
      cur.have_prev = have_prev ? 1 : 0;
      cur.k = k;
      cur.n_full_births = total();
      cur.last_pose_age = last_pose_age;
      if (need_rod_pose) {
        parent_integ.export_grid(cur.parent_grid);
        cur.has_parent_grid = 1;
      }
    }
    t = t1;
  }
}

// Time-only leftover-frac RoT (constant or curve). Same extras / step-end
// times as march_attached_rate when need_rod_pose=false. Parent pose unused.
void collect_attached_rot_births(const SplitEffectParticle& child, const Birth& b, float emit_end,
                                 int cap, bool child_loop, float child_len, float child_sim,
                                 SubemitWalkCursor& cur, std::vector<Birth>& births, int prefix_n) {
  const float kDt = 1.0f / static_cast<float>(split_walk_hz());
  float t = cur.t;
  float frac_t = cur.frac_t;
  int k = cur.k;
  auto total = [&]() { return prefix_n + static_cast<int>(births.size()); };
  for (; t < emit_end - 1e-8f && total() < cap && k < cap;) {
    const float step = t + kDt;
    const float t1 = std::min(step, emit_end);
    if (!(t1 > t)) {
      break;
    }
    const float play = (t1 - b.time) * child_sim;
    if (!child_loop && play > child_len + 1e-6f) {
      break;
    }
    float u = 0.0f;
    if (child_len > 1e-6f) {
      if (child_loop) {
        float wrapped = std::fmod(play, child_len);
        if (wrapped < 0.0f) {
          wrapped += child_len;
        }
        u = wrapped / child_len;
      } else {
        u = std::clamp(play / child_len, 0.0f, 1.0f);
      }
    }
    const float rot = official_split_effect_eval_rate_over_time(child, u);
    if (rot > 1e-6f) {
      frac_t += rot * (t1 - t);
      while (frac_t >= 1.0f && total() < cap && k < cap) {
        frac_t -= 1.0f;
        Birth cb;
        cb.time = t1;
        cb.index = total();
        cb.extra = 800u + static_cast<uint32_t>(k);
        births.push_back(cb);
        ++k;
      }
    }
    if (t1 == step) {
      cur.t = t1;
      cur.frac_t = frac_t;
      cur.k = k;
      cur.n_full_births = total();
    }
    t = t1;
  }
}

void fill_identity_parent_pose(SubParentPose& pose, float event_t, uint8_t type, float parent_life,
                               float parent_birth, uint32_t seed) noexcept {
  pose = SubParentPose{};
  pose.event_t = event_t;
  pose.type = type;
  pose.seed = seed;
  pose.life = std::max(parent_life, 1e-4f);
  const float age = std::max(0.0f, event_t - parent_birth);
  // Death emit_u = official_split_effect_sub_emit_u(age, life) = 1, same as
  // pose_at (pose.age = pst.life). fill_subemit_child uses that u.
  pose.age = type == kSplitEffectSubEmitDeath
                 ? pose.life
                 : (age < pose.life ? age : std::max(pose.life - 1e-4f, 0.0f));
}

float child_tree_span_from_birth(const SplitEffectParticle* ps, uint16_t count, uint32_t base,
                                 const SplitEffectParticle& child, uint32_t child_idx) noexcept {
  if ((child.flags & kSplitEffectFlagLooping) != 0) {
    return kDeadSkipInf;
  }
  const float sim = child.simulation_speed > 1e-6f ? child.simulation_speed : 1.0f;
  const float keep = max_keep_sim_conservative(child);
  const float child_last0 = keep / sim;
  return tree_last_alive_wall(ps, count, base, child, child_idx, 0, child_last0);
}

bool child_may_live_span(float child_birth_t, float now_t, float tree_span) noexcept {
  return now_t <= child_birth_t + tree_span + 1e-6f;
}

[[maybe_unused]] bool child_may_live_at_now(const SplitEffectParticle* ps, uint16_t count,
                                              uint32_t base, const SplitEffectParticle& child,
                                              uint32_t child_idx, float child_birth_t,
                                              float now_t) noexcept {
  return child_may_live_span(child_birth_t, now_t,
                             child_tree_span_from_birth(ps, count, base, child, child_idx));
}

struct ParentSlotHoist {
  const SplitEffectParticle* child = nullptr;
  uint32_t local_child = 0;
  uint16_t child_index = 0;
  uint8_t type = 0;
  uint8_t properties = 0;
  uint8_t element_ok = 0;
  uint8_t may_rod = 0;
  uint8_t has_rate = 0;
  float emit_probability = 1.0f;
  float tree_span = 0.0f;
};

struct ParentSysHoist {
  const SplitEffectSubEmitter* slots = nullptr;
  const ParentSlotHoist* slot_h = nullptr;
  SystemCarryKey parent_key{};
  uint16_t nslots = 0;
  uint8_t persist_want = 0;
  uint8_t any_rod = 0;
  uint8_t have_parent_key = 0;
  float const_life = -1.0f;
};

void fill_parent_sys_hoist(ParentSysHoist& ph, ParentSlotHoist* slot_h,
                           const SplitEffectParticle& parent, const SplitEffectParticle* particles,
                           uint16_t count, uint32_t base, uint32_t parent_idx, int element_slot,
                           float const_life, const EmitContext* ctx) {
  uint16_t nslots = 0;
  const SplitEffectSubEmitter* slots = subemitters_of(static_cast<uint16_t>(parent_idx), &nslots);
  ph.slots = slots;
  ph.nslots = nslots;
  ph.slot_h = slot_h;
  ph.const_life = const_life;
  ph.persist_want = persist_primary_eligible(parent) ? 1 : 0;
  ph.any_rod = 0;
  if (ctx != nullptr) {
    note_eval_system_hash(parent, ctx->color_id);
    ph.parent_key = make_system_key(*ctx, parent, 0);
    ph.have_parent_key = 1;
  } else {
    ph.have_parent_key = 0;
  }
  if (slots == nullptr || slot_h == nullptr) {
    ph.nslots = 0;
    return;
  }
  for (uint16_t s = 0; s < nslots; ++s) {
    const SplitEffectSubEmitter& slot = slots[s];
    ParentSlotHoist& sh = slot_h[s];
    sh = ParentSlotHoist{};
    sh.type = slot.type;
    sh.properties = slot.properties;
    sh.emit_probability = slot.emit_probability;
    sh.child_index = slot.child_index;
    if (slot.child_index < base) {
      continue;
    }
    const uint32_t local_child = static_cast<uint32_t>(slot.child_index) - base;
    if (local_child >= count) {
      continue;
    }
    const SplitEffectParticle& child = particles[static_cast<uint16_t>(local_child)];
    sh.local_child = local_child;
    sh.child = &child;
    if (static_cast<int>(child.element_index) != element_slot) {
      continue;
    }
    sh.element_ok = 1;
    sh.may_rod = child_may_rod(child) ? 1 : 0;
    if (sh.may_rod != 0) {
      ph.any_rod = 1;
    }
    sh.has_rate = (slot.type == kSplitEffectSubEmitBirth &&
                   (child.rate_over_time > 1e-6f || child.rate_over_distance > 1e-6f ||
                    child.rot_curve_count > 0 || child.rod_curve_count > 0))
                      ? 1
                      : 0;
    sh.tree_span = child_tree_span_from_birth(particles, count, base, child, slot.child_index);
  }
}

SubSlotOut& slot_reuse_dest(SubEventWork& ev, size_t& used, uint16_t nslots) {
  if (ev.slots.capacity() < static_cast<size_t>(nslots)) {
    ev.slots.reserve(static_cast<size_t>(nslots));
  }
  if (used >= ev.slots.size()) {
    ev.slots.emplace_back();
  }
  SubSlotOut& dest = ev.slots[used++];
  dest.births_hold.reset();
  dest.incr_resume = 0;
  dest.burst_count = 0;
  dest.n_full_births = 0;
  dest.slot_i = 0;
  dest.local_child = 0;
  dest.type = 0;
  dest.properties = 0;
  slot_acquire_vecs(dest);
  return dest;
}

void slot_trim_event(SubEventWork& ev, size_t used) {
  if (ev.slots.size() <= used) {
    return;
  }
  for (size_t i = used; i < ev.slots.size(); ++i) {
    slot_release_vecs(ev.slots[i]);
  }
  ev.slots.resize(used);
}

void collect_subemit_parent_event(SubEventWork& ev, const SplitEffectParticle* particles,
                                  uint16_t count, int element_slot, const EmitContext& ctx,
                                  uint32_t base, const SystemCarrySnapshot* snap, bool fill_now,
                                  bool persist_parent_store = true,
                                  const uint8_t* child_incr = nullptr,
                                  const int* child_cand_n = nullptr,
                                  const ParentSysHoist* sys = nullptr) {
  SplitWallScope wall_parent(SplitWallSec::ParentEvents);
  const SplitEffectParticle& parent = particles[ev.local];
  const ParentSysHoist* ph = sys;
  uint16_t nslots = 0;
  const SplitEffectSubEmitter* slots = nullptr;
  if (ph != nullptr) {
    nslots = ph->nslots;
    slots = ph->slots;
  } else {
    slots = subemitters_of(static_cast<uint16_t>(ev.parent_idx), &nslots);
  }
  if (slots == nullptr || nslots == 0) {
    return;
  }
  const Birth& b = ev.b;
  const uint32_t seed = particle_seed(ctx, parent, b);
  float emit_u = 0.0f;
  bool have_emit_u = false;
  auto ensure_emit_u = [&]() noexcept {
    if (!have_emit_u) {
      emit_u = system_emit_u(parent, birth_curve_t(b));
      have_emit_u = true;
    }
    return emit_u;
  };
  const float parent_life_closed =
      (ph != nullptr && ph->const_life >= 0.0f)
          ? ph->const_life
          : birth_start_life(parent, ctx, b, ensure_emit_u());
  PersistParticleView persist_parent;
  bool persist_parent_hit = false;
  const bool persist_want = persist_parent_store && (ph == nullptr || ph->persist_want != 0) &&
                            persist_primary_eligible(parent);
  if (persist_want) {
    EmitContext persist_ctx = ctx;
    persist_ctx.catalog_index_override = ev.parent_idx;
    persist_ctx.has_catalog_index_override = 1;
    SplitWallScope wall_claim(SplitWallSec::PersistClaim);
    persist_parent_hit = split_eval_persist_lookup_particle(
        make_system_key(persist_ctx, parent, 0), ctx.now_t, b, seed, persist_parent);
  }
  int carried_i = -1;
  if (!persist_parent_hit && snap != nullptr && !snap->parts().empty()) {
    SplitWallScope wall_carry(SplitWallSec::CarryLookup);
    carried_i = carry_find_particle(*snap, b, seed);
  }
  float parent_life = parent_life_closed;
  if (persist_parent_hit) {
    parent_life = persist_parent.st.life;
  } else if (carried_i >= 0) {
    parent_life = snap->parts()[static_cast<size_t>(carried_i)].st.life;
  }
  const float death_t = b.time + parent_life;
  // live dump dump-pending-v1 hop, not 2022.3. Death children are born at the
  // end of the parent's death frame (same 1/60 Play grid as RoD / Birth IC).
  const float death_emit_t = split_effect_next_frame_play_t(death_t);
  const bool do_birth = ctx.now_t + 1e-6f >= b.time;
  const bool do_death = ctx.now_t + 1e-6f >= death_emit_t;
  bool any_rod = false;
  bool any_open_slot = false;
  {
    SplitWallScope wall_gate(SplitWallSec::PlanGate);
    if (ph != nullptr && ph->slot_h != nullptr) {
      for (uint16_t s = 0; s < nslots; ++s) {
        const ParentSlotHoist& sh = ph->slot_h[s];
        if (sh.element_ok == 0) {
          continue;
        }
        if (sh.type != kSplitEffectSubEmitBirth && sh.type != kSplitEffectSubEmitDeath) {
          continue;
        }
        if ((sh.type == kSplitEffectSubEmitBirth && !do_birth) ||
            (sh.type == kSplitEffectSubEmitDeath && !do_death)) {
          continue;
        }
        if (sh.emit_probability < 1.0f &&
            u01(mix3(seed, static_cast<uint32_t>(sh.type + 1),
                     static_cast<uint32_t>(sh.child_index + 1))) > sh.emit_probability) {
          continue;
        }
        any_open_slot = true;
        if (sh.may_rod != 0) {
          any_rod = true;
        }
      }
    } else {
      for (uint16_t s = 0; s < nslots; ++s) {
        const SplitEffectSubEmitter& slot = slots[s];
        if (slot.type != kSplitEffectSubEmitBirth && slot.type != kSplitEffectSubEmitDeath) {
          continue;
        }
        if ((slot.type == kSplitEffectSubEmitBirth && !do_birth) ||
            (slot.type == kSplitEffectSubEmitDeath && !do_death)) {
          continue;
        }
        if (slot.emit_probability < 1.0f &&
            u01(mix3(seed, static_cast<uint32_t>(slot.type + 1),
                     static_cast<uint32_t>(slot.child_index + 1))) > slot.emit_probability) {
          continue;
        }
        if (slot.child_index < base) {
          continue;
        }
        const uint32_t local_child = static_cast<uint32_t>(slot.child_index) - base;
        if (local_child >= count) {
          continue;
        }
        const SplitEffectParticle& child = particles[static_cast<uint16_t>(local_child)];
        if (static_cast<int>(child.element_index) != element_slot) {
          continue;
        }
        any_open_slot = true;
        if (child_may_rod(child)) {
          any_rod = true;
        }
      }
    }
  }
  if (!any_open_slot) {
    return;
  }
  WDS_PERF_COUNT("deadskip.parent_events", 1);
  // RoD children need a parent walk to know birth times. RoT/burst births
  // are closed-form; cap only reads identity, so defer pose_at until after
  // the preview-pool cap. hop, not 2022.3.
  // Two-phase collect defers RoT/burst pose_at until after the preview-pool
  // cap. The one-shot evaluate_subemitters(out) path fills in this function,
  // so it still needs the live parent pose here. hop, not 2022.3.
  const bool defer_rot = split_defer_rot_parent_pose();
  const bool need_parent_state = any_rod || fill_now || !defer_rot;
  if (need_parent_state) {
    note_eval_system_hash(parent, ctx.color_id);
  }
  ParticleState pst;
  if (need_parent_state) {
    if (persist_parent_hit) {
      pst = persist_parent.st;
      refresh_carried_state(pst, ctx, parent);
    } else if (carried_i >= 0) {
      pst = snap->parts()[static_cast<size_t>(carried_i)].st;
      refresh_carried_state(pst, ctx, parent);
    } else {
      SplitWallScope wall_ms(SplitWallSec::MakeState);
      WDS_PERF_SCOPE("shuriken.make_state");
      pst = make_state(ctx, parent, b.index, b.extra, ensure_emit_u(), b.time, seed);
    }
    if (b.parent_pose != 0) {
      pst.birth_parent_z = b.parent_z;
    }
    pst.birth_fade = applied_birth_fade(ctx, b.time);
    parent_life = pst.life;
  } else {
    pst.life = parent_life;
    WDS_PERF_COUNT("deadskip.parent_make_state_skip", 1);
  }
  const ParticleCacheKey parent_key =
      need_parent_state ? make_cache_key(ctx, parent, b, seed, pst, ev.parent_idx, &g_tls_sys_hash)
                        : ParticleCacheKey{};
  std::optional<PoseIntegrator> parent_integ;
  if (need_parent_state) {
    parent_integ.emplace(pst);
    if (persist_parent_hit && persist_parent.has_grid != 0) {
      parent_integ->import_carry_grid(persist_parent.grid);
    } else if (carried_i >= 0 && snap->parts()[static_cast<size_t>(carried_i)].has_grid != 0) {
      parent_integ->import_carry_grid(snap->parts()[static_cast<size_t>(carried_i)].grid);
    }
    parent_integ->set_cache_key(parent_key.valid != 0 ? &parent_key : nullptr);
  }
  struct PoseCacheEntry {
    float event_t = 0.0f;
    uint8_t type = 0;
    SubParentPose pose;
  };
  thread_local std::vector<PoseCacheEntry> pose_cache;
  pose_cache.clear();
  float last_pose_age = -1.0f;
  auto finish_pose = [&](float event_t, uint8_t type, float pose_age,
                         const SplitEffectLiveParticle& live, SubParentPose& pose) {
    pose.event_t = event_t;
    pose.type = type;
    pose.wx = live.world_x;
    pose.wy = live.main_y;
    pose.wz = live.z;
    pose.size = live.current_size;
    pose.size_y = live.current_size_y;
    pose.size_z = live.current_size_z;
    pose.r = live.current_r;
    pose.g = live.current_g;
    pose.b = live.current_b;
    pose.a = live.current_a;
    if (type == kSplitEffectSubEmitBirth && std::fabs(event_t - b.time) <= 1e-5f) {
      const float ic_age = inherit_color_age_at_birth_frame_end(b.time);
      if (ic_age > pose_age + 1e-8f) {
        eval_particle_color_at_age(parent, pst, ic_age, pose.r, pose.g, pose.b, pose.a);
        eval_particle_size_at_age(parent, pst, ic_age, pose.size, pose.size_y, pose.size_z);
      }
    }
    pose.roll = live.roll;
    pose.tilt_x = live.tilt_x;
    pose.tilt_y = live.tilt_y;
    pose.life = pst.life;
    pose.age = pose_age;
    pose.duration_u = system_emit_u(parent, event_t);
    pose.seed = seed;
    pose.birth_origin_y_delta = pst.birth_origin_y_delta;
    PoseCacheEntry entry;
    entry.event_t = event_t;
    entry.type = type;
    entry.pose = pose;
    pose_cache.push_back(entry);
  };
  auto pose_from_sampled = [&](float event_t, uint8_t type, float wx, float wy, float wz,
                               SubParentPose& pose) {
    if (!parent_integ) {
      fill_identity_parent_pose(pose, event_t, type, parent_life, b.time, seed);
      return;
    }
    const float age = std::max(0.0f, event_t - b.time);
    const float pose_age =
        type == kSplitEffectSubEmitDeath
            ? pst.life
            : (age < pst.life ? age : std::max(pst.life - 1e-4f, 0.0f));
    const float integ_age =
        type == kSplitEffectSubEmitDeath ? death_parent_pose_age(pst.life) : pose_age;
    SplitEffectLiveParticle live{};
    live.world_x = wx;
    live.main_y = wy;
    live.z = wz;
    fill_live_after_pose(*parent_integ, pst, integ_age, false, live, false);
    live.effect_id = ctx.color_id;
    live.line_index = ctx.line_index;
    live.catalog_index = ev.parent_idx;
    live.element_index = parent.element_index;
    live.seed = pst.seed;
    live.age = age;
    live.effective_distance =
        parent.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)
            ? emitter_travel(ctx.color_id, pst.birth_fade)
            : -1.0f;
    finish_pose(event_t, type, pose_age, live, pose);
  };
  auto pose_at = [&](float event_t, uint8_t type, SubParentPose& pose) {
    if (!parent_integ) {
      fill_identity_parent_pose(pose, event_t, type, parent_life, b.time, seed);
      return;
    }
    for (const PoseCacheEntry& e : pose_cache) {
      if (e.type == type && std::fabs(e.event_t - event_t) <= 1e-8f) {
        pose = e.pose;
        return;
      }
    }
    const float age = std::max(0.0f, event_t - b.time);
    const float pose_age =
        type == kSplitEffectSubEmitDeath
            ? pst.life
            : (age < pst.life ? age : std::max(pst.life - 1e-4f, 0.0f));
    const float integ_age =
        type == kSplitEffectSubEmitDeath ? death_parent_pose_age(pst.life) : pose_age;
    SplitEffectLiveParticle live{};
    WDS_PERF_COUNT("shuriken.integrate.pose_at", 1);
    if (integ_age + 1e-7f < last_pose_age) {
      PoseIntegrator tmp(pst);
      tmp.set_cache_key(parent_integ->cache_key());
      fill_live(tmp, pst, integ_age, false, live, false);
    } else {
      fill_live(*parent_integ, pst, integ_age, false, live, false);
      last_pose_age = integ_age;
    }
    live.effect_id = ctx.color_id;
    live.line_index = ctx.line_index;
    live.catalog_index = ev.parent_idx;
    live.element_index = parent.element_index;
    live.seed = pst.seed;
    live.age = age;
    live.effective_distance =
        parent.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)
            ? emitter_travel(ctx.color_id, pst.birth_fade)
            : -1.0f;
    finish_pose(event_t, type, pose_age, live, pose);
  };
  SubParentPose birth_pose;
  SubParentPose death_pose;
  birth_pose.event_t = b.time;
  birth_pose.type = kSplitEffectSubEmitBirth;
  birth_pose.seed = seed;
  death_pose.event_t = death_emit_t;
  death_pose.type = kSplitEffectSubEmitDeath;
  death_pose.seed = seed;
  size_t slot_used = 0;
  for (uint16_t s = 0; s < nslots; ++s) {
    const SplitEffectSubEmitter& slot = slots[s];
    const ParentSlotHoist* sh = (ph != nullptr && ph->slot_h != nullptr) ? &ph->slot_h[s] : nullptr;
    uint32_t local_child = 0;
    const SplitEffectParticle* child_p = nullptr;
    uint8_t slot_type = slot.type;
    uint8_t slot_properties = slot.properties;
    float emit_probability = slot.emit_probability;
    uint16_t child_index = slot.child_index;
    if (sh != nullptr) {
      if (sh->element_ok == 0) {
        continue;
      }
      slot_type = sh->type;
      slot_properties = sh->properties;
      emit_probability = sh->emit_probability;
      child_index = sh->child_index;
      local_child = sh->local_child;
      child_p = sh->child;
    } else {
      if (slot.type != kSplitEffectSubEmitBirth && slot.type != kSplitEffectSubEmitDeath) {
        continue;
      }
      if (slot.child_index < base) {
        continue;
      }
      local_child = static_cast<uint32_t>(slot.child_index) - base;
      if (local_child >= count) {
        continue;
      }
      child_p = &particles[static_cast<uint16_t>(local_child)];
      if (static_cast<int>(child_p->element_index) != element_slot) {
        continue;
      }
    }
    if (slot_type != kSplitEffectSubEmitBirth && slot_type != kSplitEffectSubEmitDeath) {
      continue;
    }
    if ((slot_type == kSplitEffectSubEmitBirth && !do_birth) ||
        (slot_type == kSplitEffectSubEmitDeath && !do_death)) {
      continue;
    }
    const SubParentPose& pose = slot_type == kSplitEffectSubEmitBirth ? birth_pose : death_pose;
    if (emit_probability < 1.0f &&
        u01(mix3(pose.seed, static_cast<uint32_t>(slot_type + 1),
                 static_cast<uint32_t>(child_index + 1))) > emit_probability) {
      continue;
    }
    if (child_p == nullptr) {
      continue;
    }
    const SplitEffectParticle& child = *child_p;
    SubSlotOut& dest = slot_reuse_dest(ev, slot_used, nslots);
    dest.slot_i = s;
    dest.local_child = local_child;
    dest.type = slot_type;
    dest.properties = slot_properties;
    collect_event_bursts(child, pose.event_t, ctx.color_id, 0, dest.births);
    dest.burst_count = dest.births.size();
    dest.n_full_births = static_cast<int>(dest.births.size());
    SubemitWalkCursor walk{};
    walk.t = b.time;
    walk.n_full_births = static_cast<int>(dest.births.size());
    walk.burst_count = dest.burst_count;
    bool walk_resume = false;
    int pose_from = 0;
    const bool has_rate =
        sh != nullptr
            ? sh->has_rate != 0
            : (slot_type == kSplitEffectSubEmitBirth &&
               (child.rate_over_time > 1e-6f || child.rate_over_distance > 1e-6f ||
                child.rot_curve_count > 0 || child.rod_curve_count > 0));
    if (has_rate) {
      const float child_len = child.length_in_sec > 1e-6f ? child.length_in_sec : 1.0f;
      const float child_sim = child.simulation_speed > 1e-6f ? child.simulation_speed : 1.0f;
      const bool child_loop = (child.flags & kSplitEffectFlagLooping) != 0;
      float emit_end = std::min(ctx.now_t, b.time + pst.life);
      if (!child_loop) {
        emit_end = std::min(emit_end, b.time + child_len / child_sim);
      }
      const int cap = kSplitEffectParticlePool;
      if (emit_end > b.time + 1e-8f) {
        const SubemitWalkKey wkey = make_walk_key(
            ctx, parent, b, s, (ph != nullptr && ph->have_parent_key != 0) ? &ph->parent_key : nullptr);
        const bool need_rod_pose = sh != nullptr ? sh->may_rod != 0 : child_may_rod(child);
        thread_local std::vector<Birth> extra_births;
        extra_births.clear();
        int prefix_n = static_cast<int>(dest.births.size());
        if (split_eval_carry_enabled() && !observers_installed()) {
          SplitWallScope wall_wlu(SplitWallSec::CarryLookup);
          SubemitWalkCursor found;
          if (split_eval_walk_lookup_for_now(wkey, ctx.now_t, found, need_rod_pose && parent_integ) &&
              found.t <= emit_end + 1e-8f && found.births) {
            dest.births_hold = found.births;
            dest.births.clear();
            dest.burst_count = found.burst_count;
            walk = found;
            walk_resume = true;
            pose_from = walk.n_full_births;
            prefix_n = walk.n_full_births;
            if (prefix_n > static_cast<int>(found.births->size())) {
              prefix_n = static_cast<int>(found.births->size());
            }
            if (walk.has_parent_grid != 0 && parent_integ) {
              parent_integ->import_carry_grid(walk.parent_grid);
              last_pose_age = walk.last_pose_age;
            }
          }
        }
        {
          SplitWallScope wall_walk(SplitWallSec::SubemitWalk);
          const uint32_t child_i = bound_catalog_index(particles, count, ctx, child);
          const uint8_t emit_type = slot.type;
          if (need_rod_pose && parent_integ) {
            auto share = [&](float t1, float wx, float wy, float wz, bool have_xyz, int lo, int hi) {
              if (!have_xyz) {
                return;
              }
              bool any_live = false;
              const float live_span = sh != nullptr ? sh->tree_span : child_tree_span_from_birth(
                  particles, count, base, child, slot.child_index);
              for (int i = lo; i < hi && i < static_cast<int>(extra_births.size()); ++i) {
                if (child_may_live_span(extra_births[static_cast<size_t>(i)].time, ctx.now_t,
                                        live_span)) {
                  any_live = true;
                  break;
                }
              }
              if (!any_live) {
                return;
              }
              for (const PoseCacheEntry& e : pose_cache) {
                if (e.type == emit_type && std::fabs(e.event_t - t1) <= 1e-8f) {
                  return;
                }
              }
              SubParentPose sampled;
              pose_from_sampled(t1, emit_type, wx, wy, wz, sampled);
            };
            auto on_emit = [](void* raw, float t1, float wx, float wy, float wz, bool have_xyz, int lo,
                              int hi) {
              (*static_cast<decltype(share)*>(raw))(t1, wx, wy, wz, have_xyz, lo, hi);
            };
            march_attached_rate(child, pst, b, emit_end, cap, child_loop, child_len, child_sim,
                                child_i, true, *parent_integ, last_pose_age, walk, extra_births,
                                prefix_n, on_emit, &share);
          } else {
            collect_attached_rot_births(child, b, emit_end, cap, child_loop, child_len, child_sim,
                                        walk, extra_births, prefix_n);
          }
        }
        const bool births_grew = !extra_births.empty();
        {
          SplitWallScope wall_grow(SplitWallSec::VecGrow);
        if (births_grew) {
          if (dest.births_hold) {
            auto grown = take_birth_shared(static_cast<size_t>(prefix_n) + extra_births.size());
            grown->insert(grown->end(), dest.births_hold->begin(),
                          dest.births_hold->begin() + prefix_n);
            grown->insert(grown->end(), extra_births.begin(), extra_births.end());
            dest.births_hold = std::move(grown);
            dest.births.clear();
          } else {
            dest.births.insert(dest.births.end(), extra_births.begin(), extra_births.end());
          }
        }
        }
        const size_t bn = slot_birth_n(dest);
        {
          SplitWallScope wall_grow2(SplitWallSec::VecGrow);
          dest.poses.reserve(bn);
          dest.pose_ready.reserve(bn);
          dest.pose_hydrated.reserve(bn);
          dest.poses.resize(bn);
          dest.pose_ready.assign(bn, 1);
          dest.pose_hydrated.assign(bn, 0);
        }
        // Replay poses written after realize hydrate (or a live RoD
        // integrator). Identity stubs are never stored. hop, not 2022.3.
        const float walk_gap = ctx.now_t - walk.now_t;
        const bool rot_pose_resume =
            walk_resume && !parent_integ && walk.poses && !walk.poses->empty() &&
            walk_gap >= -1e-6f && walk_gap <= (1.0f / 60.0f) + 1e-5f;
        {
          SplitWallScope wall_hyd(SplitWallSec::PoseHydrate);
        if (rot_pose_resume) {
          const int npose = std::min(static_cast<int>(walk.poses->size()),
                                     static_cast<int>(dest.poses.size()));
          for (int i = 0; i < npose; ++i) {
            if ((*walk.poses)[static_cast<size_t>(i)].written == 0) {
              continue;
            }
            stored_to_pose((*walk.poses)[static_cast<size_t>(i)], dest.poses[static_cast<size_t>(i)]);
            dest.pose_hydrated[static_cast<size_t>(i)] = 1;
          }
        } else if (walk_resume && parent_integ && walk.poses &&
                   static_cast<int>(walk.poses->size()) >= pose_from) {
          for (int i = 0; i < pose_from && i < static_cast<int>(dest.poses.size()); ++i) {
            stored_to_pose((*walk.poses)[static_cast<size_t>(i)], dest.poses[static_cast<size_t>(i)]);
            dest.pose_hydrated[static_cast<size_t>(i)] = 1;
          }
        } else {
          pose_from = 0;
        }
        }
        const uint32_t child_idx = bound_catalog_index(particles, count, ctx, child);
        const uint8_t live_hydrated = parent_integ ? 1 : 0;
        const float live_span =
            sh != nullptr ? sh->tree_span
                          : child_tree_span_from_birth(particles, count, base, child, child_idx);
        for (size_t bi = static_cast<size_t>(pose_from); bi < bn; ++bi) {
          WDS_PERF_COUNT("deadskip.child_poses", 1);
          const Birth& cb = slot_birth_at(dest, bi);
          bool live_now = false;
          {
            SplitWallScope wall_gate(SplitWallSec::PlanGate);
            live_now = child_may_live_span(cb.time, ctx.now_t, live_span);
          }
          if (!live_now) {
            WDS_PERF_COUNT("deadskip.child_pose_skip", 1);
            fill_identity_parent_pose(dest.poses[bi], cb.time, slot.type, parent_life, b.time, seed);
            continue;
          }
          pose_at(cb.time, slot.type, dest.poses[bi]);
          dest.pose_hydrated[bi] = live_hydrated;
        }
        if (split_eval_carry_enabled() && !observers_installed() &&
            !split_eval_carry_store_inhibited()) {
          SplitWallScope wall_wst(SplitWallSec::CarryStore);
          const int nfull = std::max(0, std::min(walk.n_full_births, static_cast<int>(bn)));
          const bool poses_dirty = parent_integ.has_value();
          dest.n_full_births = nfull;
          walk.n_full_births = nfull;
          walk.burst_count = dest.burst_count;
          if (walk_resume && !births_grew && !poses_dirty) {
            split_eval_walk_touch(wkey, ctx.now_t, walk);
          } else {
            if (dest.births_hold && static_cast<int>(dest.births_hold->size()) <= nfull) {
              walk.births = dest.births_hold;
            } else if (dest.births_hold) {
              walk.births = take_birth_shared(static_cast<size_t>(nfull));
              walk.births->insert(walk.births->end(), dest.births_hold->begin(),
                                  dest.births_hold->begin() + nfull);
            } else {
              walk.births = take_birth_shared(static_cast<size_t>(nfull));
              walk.births->insert(walk.births->end(), dest.births.begin(),
                                  dest.births.begin() + nfull);
            }
            if (parent_integ) {
              auto stored_p = take_walk_poses(static_cast<size_t>(nfull));
              for (int i = 0; i < nfull; ++i) {
                pose_to_stored(dest.poses[static_cast<size_t>(i)],
                               (*stored_p)[static_cast<size_t>(i)]);
              }
              walk.poses = std::move(stored_p);
            } else {
              walk.has_parent_grid = 0;
            }
            walk.n_full_births = nfull;
            walk.burst_count = dest.burst_count;
            split_eval_walk_store(wkey, ctx.now_t, std::move(walk));
          }
        } else {
          dest.n_full_births = walk.n_full_births;
        }
        if (child_incr != nullptr && dest.local_child < count &&
            child_incr[dest.local_child] != 0 && walk_resume) {
          SplitWallScope wall_grow(SplitWallSec::VecGrow);
          const int capn = child.max_particles > 0 ? static_cast<int>(child.max_particles)
                                                   : kSplitEffectParticlePool;
          const int old_n = child_cand_n != nullptr ? child_cand_n[dest.local_child] : 0;
          const int extra_n = births_grew ? static_cast<int>(extra_births.size()) : 0;
          if (old_n + extra_n <= capn) {
            dest.births.clear();
            dest.births_hold.reset();
            if (extra_n > 0) {
              dest.births = extra_births;
            }
            dest.burst_count = 0;
            dest.n_full_births = extra_n;
            dest.incr_resume = 1;
            const size_t bn_now = slot_birth_n(dest);
            if (dest.poses.size() > bn_now) {
              dest.poses.erase(dest.poses.begin(), dest.poses.end() - static_cast<std::ptrdiff_t>(bn_now));
              dest.pose_ready.erase(dest.pose_ready.begin(),
                                    dest.pose_ready.end() - static_cast<std::ptrdiff_t>(bn_now));
              dest.pose_hydrated.erase(dest.pose_hydrated.begin(),
                                       dest.pose_hydrated.end() - static_cast<std::ptrdiff_t>(bn_now));
            } else {
              dest.poses.resize(bn_now);
              dest.pose_ready.resize(bn_now, 1);
              dest.pose_hydrated.resize(bn_now, 0);
            }
          }
        }
        continue;
      }
    }
    {
      SplitWallScope wall_grow(SplitWallSec::VecGrow);
      dest.poses.reserve(dest.births.size());
      dest.pose_ready.reserve(dest.births.size());
      dest.pose_hydrated.reserve(dest.births.size());
      dest.poses.resize(dest.births.size());
      dest.pose_ready.assign(dest.births.size(), 1);
      dest.pose_hydrated.assign(dest.births.size(), 0);
    }
    const uint32_t burst_child_idx = bound_catalog_index(particles, count, ctx, child);
    const uint8_t burst_hydrated = parent_integ ? 1 : 0;
    const float burst_span =
        sh != nullptr ? sh->tree_span
                      : child_tree_span_from_birth(particles, count, base, child, burst_child_idx);
    for (size_t bi = 0; bi < dest.births.size(); ++bi) {
      WDS_PERF_COUNT("deadskip.child_poses", 1);
      bool live_now = false;
      {
        SplitWallScope wall_gate(SplitWallSec::PlanGate);
        live_now = child_may_live_span(dest.births[bi].time, ctx.now_t, burst_span);
      }
      if (!live_now) {
        WDS_PERF_COUNT("deadskip.child_pose_skip", 1);
        fill_identity_parent_pose(dest.poses[bi], dest.births[bi].time, slot.type, parent_life,
                                  b.time, seed);
        continue;
      }
      pose_at(dest.births[bi].time, slot.type, dest.poses[bi]);
      dest.pose_hydrated[bi] = burst_hydrated;
    }
  }
  slot_trim_event(ev, slot_used);
}

struct ChildSysHoist {
  float sim = 1.0f;
  float trail_hi = 0.0f;
  float const_life = -1.0f;
  uint8_t keep_uses_trail = 0;
};

void fill_child_sys_hoist(const SplitEffectParticle& p, ChildSysHoist& h) noexcept {
  h.sim = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  h.keep_uses_trail = ((p.flags & kSplitEffectFlagTrails) != 0 && p.trail_die_with == 0 &&
                       p.trail_mode == 0)
                          ? 1
                          : 0;
  h.trail_hi = h.keep_uses_trail != 0 ? trail_lifetime_mul_hi(p) : 0.0f;
  if (p.life_curve_count == 0 && p.life_random == 0) {
    h.const_life = std::max(p.lifetime, 1e-4f);
  } else {
    h.const_life = -1.0f;
  }
}

void fill_subemit_child(const SplitEffectParticle& child, const Birth& cb, const SubParentPose& born,
                        uint8_t properties, const EmitContext& ctx, uint32_t child_idx,
                        SplitEffectLiveParticle& live, uint8_t& keep,
                        const SystemCarrySnapshot* snap, CarryParticle* carried,
                        const Birth& parent_b, uint16_t slot_i, bool do_fill_live,
                        SubemitFillItem* dest_item, const SplitEffectParticle* parent = nullptr,
                        uint32_t parent_idx = 0, uint8_t pose_hydrated = 0,
                        const ChildSysHoist* sys = nullptr) {
  keep = 0;
  // PLAN (do_fill_live=false) only needs identity + keep. Cache key / sys
  // hash is for realize make_cache_key. hop, not 2022.3.
  if (do_fill_live) {
    note_eval_system_hash(child, ctx.color_id);
  }
  const float sim = sys != nullptr ? sys->sim
                                   : (child.simulation_speed > 1e-6f ? child.simulation_speed : 1.0f);
  const float age = ctx.now_t * sim - cb.time * sim;
  const float eu = official_split_effect_sub_emit_u(born.age, born.life);
  const uint32_t cseed = child_emit_seed(ctx, child, cb, parent_b, slot_i);
  auto write_dest_identity = [&](SubemitFillItem* dest, uint8_t filled_now, float pose_age_now) {
    if (dest == nullptr) {
      return;
    }
    dest->child = &child;
    dest->parent = parent;
    dest->child_idx = child_idx;
    dest->parent_idx = parent_idx;
    dest->b = cb;
    dest->parent_b = parent_b;
    dest->born = born;
    dest->seed = cseed;
    dest->age = age;
    dest->pose_age = pose_age_now;
    dest->emit_u = eu;
    dest->slot_i = slot_i;
    dest->properties = properties;
    dest->ready = 1;
    dest->filled = filled_now;
    dest->pose_hydrated = pose_hydrated;
  };
  ParticleState st;
  IntegratorGridState grid{};
  uint8_t has_grid = 0;
  int carried_i = snap != nullptr ? carry_find_child(*snap, cb, cseed, parent_b, slot_i) : -1;
  if (carried_i >= 0 && (snap->parts()[static_cast<size_t>(carried_i)].has_grid == 0 ||
                         snap->parts()[static_cast<size_t>(carried_i)].on_grid == 0)) {
    carried_i = -1;
  }
  if (carried_i < 0) {
    float life = 0.0f;
    float cons_keep = 0.0f;
    {
      life = (sys != nullptr && sys->const_life >= 0.0f)
                 ? sys->const_life
                 : birth_start_life(child, ctx, cb, eu, cseed);
      if ((properties & kSplitEffectSubInheritLifetime) != 0) {
        const float rem =
            born.life > 1e-8f ? std::clamp(1.0f - born.age / born.life, 0.0f, 1.0f) : 0.0f;
        life *= rem;
      }
      const float cons_trail =
          sys != nullptr ? sys->trail_hi : trail_lifetime_mul_hi(child);
      cons_keep = particle_keep_sim(child, life, cons_trail);
      if (age < 0.0f || age >= cons_keep || life <= 1e-8f) {
        return;
      }
    }
    if (!do_fill_live) {
      float max_age = 0.0f;
      {
        const bool uses_trail =
            sys != nullptr ? sys->keep_uses_trail != 0
                           : ((child.flags & kSplitEffectFlagTrails) != 0 &&
                              child.trail_die_with == 0 && child.trail_mode == 0);
        if (!uses_trail) {
          max_age = life;
        } else {
          const float trail_mul =
              child.trail_lifetime_count > 0
                  ? (sys != nullptr ? sys->trail_hi : trail_lifetime_mul_hi(child))
                  : pick(child.trail_lifetime, child.trail_lifetime_min,
                         child.trail_lifetime_random != 0,
                         mix3(cseed, kSaltTrailLifetime, cb.extra));
          max_age = particle_keep_sim(child, life, trail_mul);
        }
        if (age >= max_age) {
          return;
        }
      }
      keep = 1;
      const float pose_age = age < life ? age : std::max(life - 1e-4f, 0.0f);
      write_dest_identity(dest_item, 0, pose_age);
      return;
    }
  }
  if (carried_i >= 0) {
    st = snap->parts()[static_cast<size_t>(carried_i)].st;
    refresh_carried_state(st, ctx, child);
    st.birth_fade = applied_birth_fade(ctx, cb.time);
    has_grid = snap->parts()[static_cast<size_t>(carried_i)].has_grid;
    grid = snap->parts()[static_cast<size_t>(carried_i)].grid;
  } else {
    SplitWallScope wall_ms(SplitWallSec::MakeState);
    WDS_PERF_SCOPE("shuriken.make_state");
    st = make_state(ctx, child, cb.index, cb.extra, eu, cb.time, cseed);
    st.birth_fade = applied_birth_fade(ctx, cb.time);
  }
  const float trail_mul = child.trail_lifetime_count > 0
                              ? (sys != nullptr ? sys->trail_hi : trail_lifetime_mul_hi(child))
                              : (st.trail_life_mul >= 0.0f ? st.trail_life_mul : child.trail_lifetime);
  float max_age = particle_keep_sim(child, st.life, trail_mul);
  if (age < 0.0f || age >= max_age || st.life <= 1e-8f) {
    return;
  }
  if (carried_i < 0) {
    st.event_emit = 1;
    st.event_wx = born.wx;
    st.event_wy = born.wy;
    st.event_wz = born.wz;
    st.birth_parent_z = born.wz;
    st.birth_origin_y_delta = born.birth_origin_y_delta;
    st.inherit_props = properties;
    if ((properties & kSplitEffectSubInheritSize) != 0) {
      st.size *= born.size;
      st.size_y *= born.size_y;
      st.size_z *= born.size_z;
    }
    if ((properties & kSplitEffectSubInheritRotation) != 0) {
      st.rot_z += born.roll;
      st.rot_x += born.tilt_x;
      st.rot_y += born.tilt_y;
    }
    if ((properties & kSplitEffectSubInheritLifetime) != 0) {
      const float rem = born.life > 1e-8f ? std::clamp(1.0f - born.age / born.life, 0.0f, 1.0f) : 0.0f;
      st.life *= rem;
    }
    if ((properties & kSplitEffectSubInheritColor) != 0) {
      st.inherit_r = born.r;
      st.inherit_g = born.g;
      st.inherit_b = born.b;
      st.inherit_a = born.a;
    }
    max_age = particle_keep_sim(child, st.life,
                               child.trail_lifetime_count > 0
                                   ? (sys != nullptr ? sys->trail_hi : trail_lifetime_mul_hi(child))
                                   : (st.trail_life_mul >= 0.0f ? st.trail_life_mul
                                                                : child.trail_lifetime));
    if (age < 0.0f || age >= max_age || st.life <= 1e-8f) {
      return;
    }
  }
  const float pose_age = age < st.life ? age : std::max(st.life - 1e-4f, 0.0f);
  if (do_fill_live) {
    const ParticleCacheKey key = make_cache_key(ctx, child, cb, cseed, st, child_idx, &g_tls_sys_hash);
    PoseIntegrator integ(st);
    if (has_grid != 0) {
      integ.import_carry_grid(grid);
    }
    integ.set_cache_key(key.valid != 0 ? &key : nullptr);
    WDS_PERF_COUNT("shuriken.integrate.child", 1);
    fill_live(integ, st, pose_age, ctx.want_trail, live, true, age);
    integ.export_grid(grid);
    has_grid = 1;
    stamp_evaluated_live(live, ctx, child, st, age, child_idx);
  }
  keep = 1;
  if (carried != nullptr) {
    carried->b = cb;
    carried->parent_b = parent_b;
    carried->st = st;
    carried->st.p = nullptr;
    carried->grid = grid;
    carried->seed = cseed;
    carried->emit_u = eu;
    carried->slot_i = slot_i;
    carried->has_grid = has_grid;
    carried->on_grid = 1;
  }
  write_dest_identity(dest_item, do_fill_live ? 1 : 0, pose_age);
}

void write_parent_event_pose(const SplitEffectParticle& parent, ParticleState& pst,
                             PoseIntegrator& integ, const ParticleCacheKey* key, const Birth& b,
                             const EmitContext& ctx, uint32_t parent_idx, uint32_t seed,
                             float event_t, uint8_t type, float& last_pose_age,
                             SubParentPose& pose) {
  const float age = std::max(0.0f, event_t - b.time);
  const float pose_age =
      type == kSplitEffectSubEmitDeath ? pst.life
                                       : (age < pst.life ? age : std::max(pst.life - 1e-4f, 0.0f));
  const float integ_age =
      type == kSplitEffectSubEmitDeath ? death_parent_pose_age(pst.life) : pose_age;
  SplitEffectLiveParticle live{};
  WDS_PERF_COUNT("shuriken.integrate.pose_at", 1);
  if (integ_age + 1e-7f < last_pose_age) {
    PoseIntegrator tmp(pst);
    tmp.set_cache_key(key);
    fill_live(tmp, pst, integ_age, false, live, false);
  } else {
    fill_live(integ, pst, integ_age, false, live, false);
    last_pose_age = integ_age;
  }
  live.effect_id = ctx.color_id;
  live.line_index = ctx.line_index;
  live.catalog_index = parent_idx;
  live.element_index = parent.element_index;
  live.seed = pst.seed;
  live.age = age;
  live.effective_distance =
      parent.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)
          ? emitter_travel(ctx.color_id, pst.birth_fade)
          : -1.0f;
  pose.event_t = event_t;
  pose.type = type;
  pose.wx = live.world_x;
  pose.wy = live.main_y;
  pose.wz = live.z;
  pose.size = live.current_size;
  pose.size_y = live.current_size_y;
  pose.size_z = live.current_size_z;
  pose.r = live.current_r;
  pose.g = live.current_g;
  pose.b = live.current_b;
  pose.a = live.current_a;
  if (type == kSplitEffectSubEmitBirth && std::fabs(event_t - b.time) <= 1e-5f) {
    const float ic_age = inherit_color_age_at_birth_frame_end(b.time);
    if (ic_age > pose_age + 1e-8f) {
      eval_particle_color_at_age(parent, pst, ic_age, pose.r, pose.g, pose.b, pose.a);
      eval_particle_size_at_age(parent, pst, ic_age, pose.size, pose.size_y, pose.size_z);
    }
  }
  pose.roll = live.roll;
  pose.tilt_x = live.tilt_x;
  pose.tilt_y = live.tilt_y;
  pose.life = pst.life;
  pose.age = pose_age;
  pose.duration_u = system_emit_u(parent, event_t);
  pose.seed = seed;
  pose.birth_origin_y_delta = pst.birth_origin_y_delta;
}

void hydrate_deferred_parent_poses(const EmitContext& ctx, SubemitFillPlan& plan,
                                   const uint8_t* need_fill, int n,
                                   std::vector<int>* newly_hydrated) {
  struct Ref {
    int item_i = 0;
    const SplitEffectParticle* parent = nullptr;
    uint32_t parent_idx = 0;
    Birth parent_b{};
    float event_t = 0.0f;
    uint8_t type = 0;
  };
  std::vector<Ref> refs;
  refs.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    if (need_fill[static_cast<size_t>(i)] == 0) {
      continue;
    }
    SubemitFillItem& item = plan.items[static_cast<size_t>(i)];
    if (item.pose_hydrated != 0 || item.parent == nullptr) {
      continue;
    }
    Ref r;
    r.item_i = i;
    r.parent = item.parent;
    r.parent_idx = item.parent_idx;
    r.parent_b = item.parent_b;
    r.event_t = item.born.event_t;
    r.type = item.born.type;
    refs.push_back(r);
  }
  if (refs.empty()) {
    return;
  }
  std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
    if (a.parent != b.parent) {
      return a.parent < b.parent;
    }
    if (a.parent_b.index != b.parent_b.index) {
      return a.parent_b.index < b.parent_b.index;
    }
    if (a.parent_b.extra != b.parent_b.extra) {
      return a.parent_b.extra < b.parent_b.extra;
    }
    if (a.event_t != b.event_t) {
      return a.event_t < b.event_t;
    }
    return a.type < b.type;
  });
  SplitWallScope wall_hy(SplitWallSec::PoseHydrate);
  size_t g0 = 0;
  while (g0 < refs.size()) {
    size_t g1 = g0 + 1;
    while (g1 < refs.size() && refs[g1].parent == refs[g0].parent &&
           refs[g1].parent_b.index == refs[g0].parent_b.index &&
           refs[g1].parent_b.extra == refs[g0].parent_b.extra) {
      ++g1;
    }
    const Ref& head = refs[g0];
    const SplitEffectParticle& parent = *head.parent;
    note_eval_system_hash(parent, ctx.color_id);
    const float emit_u = system_emit_u(parent, birth_curve_t(head.parent_b));
    const uint32_t seed = particle_seed(ctx, parent, head.parent_b);
    // Match primary realize's cache key. Subemit ctx has no catalog
    // override, so carry lookup must set it or catalog_index(bound copy)
    // is 0 and the snap misses. Use the snap's st only — do not import
    // the now_t grid. sample() pick_leq is by event k. hop, not 2022.3.
    EmitContext key_ctx = ctx;
    key_ctx.catalog_index_override = head.parent_idx;
    key_ctx.has_catalog_index_override = 1;
    SystemCarrySnapshot snap;
    PersistParticleView persist_parent;
    const SystemCarryKey parent_key = make_system_key(key_ctx, parent, 0);
    const bool persist_hit =
        persist_primary_eligible(parent) && persist_store_exists(parent_key) &&
        split_eval_persist_lookup_particle(parent_key, ctx.now_t, head.parent_b, seed,
                                           persist_parent);
    const bool have_snap =
        !persist_hit &&
        split_eval_carry_lookup_for_time(parent_key, ctx.now_t, snap);
    const int carried_i = have_snap ? carry_find_particle(snap, head.parent_b, seed) : -1;
    ParticleState pst;
    if (persist_hit) {
      pst = persist_parent.st;
      refresh_carried_state(pst, ctx, parent);
    } else if (carried_i >= 0) {
      pst = snap.parts()[static_cast<size_t>(carried_i)].st;
      refresh_carried_state(pst, ctx, parent);
    } else {
      SplitWallScope wall_ms(SplitWallSec::MakeState);
      WDS_PERF_SCOPE("shuriken.make_state");
      pst = make_state(ctx, parent, head.parent_b.index, head.parent_b.extra, emit_u,
                       head.parent_b.time, seed);
    }
    if (head.parent_b.parent_pose != 0) {
      pst.birth_parent_z = head.parent_b.parent_z;
    }
    pst.birth_fade = applied_birth_fade(ctx, head.parent_b.time);
    const ParticleCacheKey key =
        make_cache_key(ctx, parent, head.parent_b, seed, pst, head.parent_idx, &g_tls_sys_hash);
    PoseIntegrator integ(pst);
    integ.set_cache_key(key.valid != 0 ? &key : nullptr);
    float last_pose_age = -1.0f;
    for (size_t k = g0; k < g1; ++k) {
      SubParentPose pose;
      write_parent_event_pose(parent, pst, integ, key.valid != 0 ? &key : nullptr, head.parent_b,
                              ctx, head.parent_idx, seed, refs[k].event_t, refs[k].type,
                              last_pose_age, pose);
      SubemitFillItem& item = plan.items[static_cast<size_t>(refs[k].item_i)];
      item.born = pose;
      item.pose_hydrated = 1;
      if (newly_hydrated != nullptr) {
        newly_hydrated->push_back(refs[k].item_i);
      }
    }
    g0 = g1;
  }
}

void store_hydrated_walk_poses(const EmitContext& ctx, SubemitFillPlan& plan,
                               const std::vector<int>& newly) {
  if (newly.empty() || !split_eval_carry_enabled() || observers_installed() ||
      split_eval_carry_store_inhibited()) {
    return;
  }
  struct GKey {
    const SplitEffectParticle* parent = nullptr;
    int birth_index = 0;
    uint32_t birth_extra = 0;
    uint16_t slot_i = 0;
    bool operator<(const GKey& o) const noexcept {
      if (parent != o.parent) {
        return parent < o.parent;
      }
      if (birth_index != o.birth_index) {
        return birth_index < o.birth_index;
      }
      if (birth_extra != o.birth_extra) {
        return birth_extra < o.birth_extra;
      }
      return slot_i < o.slot_i;
    }
  };
  std::vector<GKey> keys;
  std::vector<int> item_is;
  keys.reserve(newly.size());
  item_is.reserve(newly.size());
  for (int i : newly) {
    if (i < 0 || i >= static_cast<int>(plan.items.size())) {
      continue;
    }
    const SubemitFillItem& item = plan.items[static_cast<size_t>(i)];
    if (item.pose_hydrated == 0 || item.parent == nullptr) {
      continue;
    }
    GKey k;
    k.parent = item.parent;
    k.birth_index = item.parent_b.index;
    k.birth_extra = item.parent_b.extra;
    k.slot_i = item.slot_i;
    keys.push_back(k);
    item_is.push_back(i);
  }
  if (keys.empty()) {
    return;
  }
  SplitWallScope wall_hy(SplitWallSec::PoseHydrate);
  std::vector<size_t> ord(keys.size());
  std::iota(ord.begin(), ord.end(), 0);
  std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return keys[a] < keys[b]; });
  size_t g0 = 0;
  while (g0 < ord.size()) {
    const GKey& head = keys[ord[g0]];
    size_t g1 = g0 + 1;
    while (g1 < ord.size() && !(head < keys[ord[g1]]) && !(keys[ord[g1]] < head)) {
      ++g1;
    }
    const SubemitFillItem& sample = plan.items[static_cast<size_t>(item_is[ord[g0]])];
    const SubemitWalkKey wkey = make_walk_key(ctx, *head.parent, sample.parent_b, head.slot_i);
    const int np = static_cast<int>(g1 - g0);
    thread_local std::vector<float> event_ts;
    thread_local std::vector<SubemitWalkPose> stored;
    event_ts.resize(static_cast<size_t>(np));
    stored.resize(static_cast<size_t>(np));
    for (int u = 0; u < np; ++u) {
      const SubemitFillItem& item = plan.items[static_cast<size_t>(item_is[ord[g0 + static_cast<size_t>(u)]])];
      event_ts[static_cast<size_t>(u)] = item.born.event_t;
      pose_to_stored(item.born, stored[static_cast<size_t>(u)]);
    }
    split_eval_walk_write_poses(wkey, event_ts.data(), stored.data(), np);
    g0 = g1;
  }
}

struct ChildPersistSession {
  SystemCarryKey key{};
  const SplitEffectParticle* child = nullptr;
  uint32_t child_idx = 0;
  PersistClaim claim{};
  ChildPersistFp fp{};
  uint8_t persist_ok = 0;
  uint8_t resume = 0;
  std::vector<int> item_is;
  std::vector<PrimaryEvalJob> jobs;
};

SystemCarryKey make_child_persist_key(const EmitContext& ctx, const SplitEffectParticle& child,
                                      uint32_t child_idx) {
  EmitContext key_ctx = ctx;
  key_ctx.catalog_index_override = child_idx;
  key_ctx.has_catalog_index_override = 1;
  return make_system_key(key_ctx, child, kPersistKindChild);
}

void begin_child_persist(const EmitContext& ctx, SubemitFillPlan& plan, const uint8_t* will_fill,
                         int n, std::vector<ChildPersistSession>& sessions, int& n_sessions,
                         std::vector<int>& session_of, std::vector<int>& slot_of) {
  for (int i = 0; i < n_sessions && i < static_cast<int>(sessions.size()); ++i) {
    persist_release(sessions[static_cast<size_t>(i)].claim);
    sessions[static_cast<size_t>(i)].item_is.clear();
    sessions[static_cast<size_t>(i)].jobs.clear();
  }
  n_sessions = 0;
  session_of.assign(static_cast<size_t>(n), -1);
  slot_of.assign(static_cast<size_t>(n), -1);
  if (observers_installed() || !split_eval_persist_enabled()) {
    return;
  }
  for (int i = 0; i < n; ++i) {
    if (will_fill[static_cast<size_t>(i)] == 0) {
      continue;
    }
    SubemitFillItem& item = plan.items[static_cast<size_t>(i)];
    if (item.child == nullptr || !persist_child_eligible(*item.child) ||
        !persist_child_can_hit(ctx, *item.child)) {
      continue;
    }
    const SystemCarryKey key = make_child_persist_key(ctx, *item.child, item.child_idx);
    int si = -1;
    for (int g = 0; g < n_sessions; ++g) {
      if (sessions[static_cast<size_t>(g)].key == key) {
        si = g;
        break;
      }
    }
    if (si < 0) {
      si = n_sessions;
      if (si >= static_cast<int>(sessions.size())) {
        sessions.resize(static_cast<size_t>(si) + 1);
      }
      ChildPersistSession& s = sessions[static_cast<size_t>(si)];
      s.key = key;
      s.child = item.child;
      s.child_idx = item.child_idx;
      s.fp = make_child_persist_fp(ctx, *item.child);
      s.claim = PersistClaim{};
      s.persist_ok = 1;
      s.resume = 0;
      s.item_is.clear();
      s.jobs.clear();
      if (s.item_is.capacity() < 64) {
        s.item_is.reserve(64);
      }
      if (s.jobs.capacity() < 64) {
        s.jobs.reserve(64);
      }
      split_eval_persist_claim(key, ctx.now_t, s.fp, s.claim);
      if (s.claim.kind == PersistClaimKind::Forward) {
        persist_cull_dead(s.claim, *item.child, ctx.now_t);
        s.resume = 1;
      }
      ++n_sessions;
    }
    ChildPersistSession& sess = sessions[static_cast<size_t>(si)];
    sess.item_is.push_back(i);
    session_of[static_cast<size_t>(i)] = si;
    if (item.persist_i >= 0) {
      slot_of[static_cast<size_t>(i)] = item.persist_i;
    }
  }
  auto child_item_lt = [&](const SubemitFillItem& a, const SubemitFillItem& b) noexcept {
    if (a.parent_b.index != b.parent_b.index) {
      return a.parent_b.index < b.parent_b.index;
    }
    if (a.parent_b.extra != b.parent_b.extra) {
      return a.parent_b.extra < b.parent_b.extra;
    }
    uint32_t ta = 0;
    uint32_t tb = 0;
    std::memcpy(&ta, &a.parent_b.time, sizeof(ta));
    std::memcpy(&tb, &b.parent_b.time, sizeof(tb));
    if (ta != tb) {
      return ta < tb;
    }
    if (a.slot_i != b.slot_i) {
      return a.slot_i < b.slot_i;
    }
    std::memcpy(&ta, &a.b.time, sizeof(ta));
    std::memcpy(&tb, &b.b.time, sizeof(tb));
    if (ta != tb) {
      return ta < tb;
    }
    return a.b.extra < b.b.extra;
  };
  auto slot_before_item = [&](const PersistSlot& s, const SubemitFillItem& it) noexcept {
    SubemitFillItem tmp{};
    tmp.parent_b = s.parent_b;
    tmp.b = s.b;
    tmp.slot_i = s.slot_i;
    return child_item_lt(tmp, it);
  };
  auto slot_matches_item = [&](const PersistSlot& s, const SubemitFillItem& it) noexcept {
    uint32_t ta = 0;
    uint32_t tb = 0;
    std::memcpy(&ta, &s.b.time, sizeof(ta));
    std::memcpy(&tb, &it.b.time, sizeof(tb));
    return s.is_child != 0 && s.seed == it.seed && s.slot_i == it.slot_i && ta == tb &&
           s.b.extra == it.b.extra && carry_birth_eq(s.parent_b, it.parent_b);
  };
  for (int g = 0; g < n_sessions; ++g) {
    ChildPersistSession& sess = sessions[static_cast<size_t>(g)];
    if (sess.resume == 0 || sess.item_is.empty()) {
      continue;
    }
    const int nparts = persist_slot_count(sess.claim);
    if (nparts <= 0) {
      continue;
    }
    std::vector<int> pord(static_cast<size_t>(nparts));
    for (int i = 0; i < nparts; ++i) {
      pord[static_cast<size_t>(i)] = i;
    }
    std::stable_sort(pord.begin(), pord.end(), [&](int a, int b) {
      const PersistSlot* sa = persist_slot_at(sess.claim, a);
      const PersistSlot* sb = persist_slot_at(sess.claim, b);
      if (sa == nullptr || sb == nullptr) {
        return a < b;
      }
      SubemitFillItem ia{};
      ia.parent_b = sa->parent_b;
      ia.b = sa->b;
      ia.slot_i = sa->slot_i;
      SubemitFillItem ib{};
      ib.parent_b = sb->parent_b;
      ib.b = sb->b;
      ib.slot_i = sb->slot_i;
      return child_item_lt(ia, ib);
    });
    std::vector<int> ord = sess.item_is;
    std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) {
      return child_item_lt(plan.items[static_cast<size_t>(a)], plan.items[static_cast<size_t>(b)]);
    });
    int pi = 0;
    for (int oi : ord) {
      if (slot_of[static_cast<size_t>(oi)] >= 0) {
        continue;
      }
      const SubemitFillItem& item = plan.items[static_cast<size_t>(oi)];
      while (pi < nparts) {
        const PersistSlot* slot = persist_slot_at(sess.claim, pord[static_cast<size_t>(pi)]);
        if (slot == nullptr) {
          ++pi;
          continue;
        }
        if (slot_matches_item(*slot, item)) {
          slot_of[static_cast<size_t>(oi)] = pord[static_cast<size_t>(pi)];
          ++pi;
          break;
        }
        if (slot_before_item(*slot, item)) {
          ++pi;
          continue;
        }
        break;
      }
    }
  }
  for (int g = 0; g < n_sessions; ++g) {
    ChildPersistSession& sess = sessions[static_cast<size_t>(g)];
    if (sess.jobs.capacity() < sess.item_is.size()) {
      sess.jobs.reserve(sess.item_is.size());
    }
  }
}

void job_from_child_carry(PrimaryEvalJob& job, const CarryParticle& cap) noexcept {
  job = PrimaryEvalJob{};
  job.b = cap.b;
  job.parent_b = cap.parent_b;
  job.slot_i = cap.slot_i;
  job.is_child = 1;
  job.st = cap.st;
  job.grid = cap.grid;
  job.has_grid = cap.has_grid;
  job.has_state = 1;
  job.seed = cap.seed;
  job.emit_u = cap.emit_u;
  job.alive = 1;
  job.do_fill = 1;
}

void finish_child_persist(std::vector<ChildPersistSession>& sessions, int n_sessions, float now,
                          const SubemitFillPlan& plan, const std::vector<PersistCand>* occ) {
  for (int i = 0; i < n_sessions && i < static_cast<int>(sessions.size()); ++i) {
    ChildPersistSession& s = sessions[static_cast<size_t>(i)];
    if (s.persist_ok != 0) {
      std::vector<PersistCand> cands;
      const uint32_t child_idx = s.child_idx;
      if (occ != nullptr && !occ->empty()) {
        cands.reserve(occ->size());
        for (const PersistCand& c : *occ) {
          if (c.child_idx == child_idx) {
            cands.push_back(c);
          }
        }
      } else {
        cands.reserve(plan.items.size());
        for (const SubemitFillItem& item : plan.items) {
          if (item.child == nullptr || s.child == nullptr || item.child != s.child) {
            continue;
          }
          PersistCand c;
          c.b = item.b;
          c.parent_b = item.parent_b;
          c.slot_i = item.slot_i;
          c.seed = item.seed;
          c.emit_u = item.emit_u;
          c.properties = item.properties;
          c.child_idx = item.child_idx;
          cands.push_back(c);
        }
      }
      if (s.claim.kind == PersistClaimKind::Forward) {
        persist_finish(s.claim, s.key, now, s.fp, s.jobs, cands);
      } else {
        persist_finish(s.claim, s.key, now, s.fp, std::vector<PrimaryEvalJob>{}, cands);
      }
    } else {
      persist_release(s.claim);
    }
    s.item_is.clear();
    s.jobs.clear();
  }
}

}  // namespace

int evaluate_subemitters_plan(const SplitEffectParticle* particles, uint16_t count, int element_slot,
                              const EmitContext& ctx, SubemitFillPlan& plan, int max_out,
                              SplitEffectLiveParticle* out,
                              std::vector<SplitEffectLiveParticle>* out_vec) {
  plan.items.clear();
  plan.persist_occ.clear();
  plan.kept = 0;
  SplitWallScope wall_subemit_plan(SplitWallSec::SubemitPlan);
  SplitDiagScope diag_subemit_plan(SplitDiagPhase::SubemitPlan);
  // Reserve for the rows this task can plausibly fill, not for the whole pool.
  // max_out is kSplitEffectParticlePool, so reserving it charges
  // 16384 * sizeof(SubemitFillItem) ~= 3.1 MB of *commit* per task; a task keeps
  // a few hundred rows (measured max 1536 on a 25-instance stress chart). On
  // macOS/Linux that is only address space (untouched pages are not resident),
  // but on Windows commit charge is a hard limit (RAM + page file), so the cap
  // is what keeps a many-instance scene from charging ~0.5 GB it never touches.
  // Capacity only: no reference into plan.items is held across a push_back, so
  // growth is safe and the kept set / vertex stream are unchanged.
  const std::size_t want =
      std::min<std::size_t>(static_cast<std::size_t>(max_out),
                            std::max<std::size_t>(256u, 4u * static_cast<std::size_t>(count)));
  if (plan.items.capacity() < want) {
    plan.items.reserve(want);
  }
  if (particles == nullptr || max_out <= 0 || count == 0) {
    return 0;
  }
  SplitWallScope wall_plan(SplitWallSec::SubemitSetup);
  const bool fill_now = out != nullptr || out_vec != nullptr;
  WDS_PERF_SCOPE("shuriken.subemit");
  // live dump v1 (reports/re-subemit-work-report.md):
  // collect now passes hydrate+bind copies. catalog_index() is pointer identity
  // into kSplitEffectParticles, so bound rows need effect-local + particle_begin.
  // Catalog pointers (tests) still use the table address.
  // Birth attached RoT/RoD + InheritSize current size + skip independent Play
  // for slot targets. Not a 2022.3 formula.
  auto idx_of = [&](const SplitEffectParticle& p) -> uint32_t {
    return bound_catalog_index(particles, count, ctx, p);
  };
  struct LayerBirths {
    uint16_t local = 0;
    std::vector<Birth> births;
  };

  std::vector<LayerBirths> queue;
  queue.reserve(static_cast<size_t>(count));
  for (uint16_t pi = 0; pi < count; ++pi) {
    const SplitEffectParticle& parent = particles[pi];
    if (static_cast<int>(parent.element_index) != element_slot) {
      continue;
    }
    if (parent.go_active == 0) {
      continue;
    }
    uint16_t nslots = 0;
    const SplitEffectSubEmitter* slots =
        subemitters_of(static_cast<uint16_t>(idx_of(parent)), &nslots);
    if (slots == nullptr || nslots == 0) {
      continue;
    }
    bool fire = false;
    for (uint16_t s = 0; s < nslots; ++s) {
      if (slots[s].type == kSplitEffectSubEmitBirth || slots[s].type == kSplitEffectSubEmitDeath) {
        fire = true;
        break;
      }
    }
    if (!fire) {
      continue;
    }
    LayerBirths layer;
    layer.local = pi;
    // Unity 2022.3 SubEmitters: Death/Birth spawn a separate child
    // ParticleSystem at the parent event
    // (Manual/PartSysSubEmitModule.html). The child Main startLifetime is
    // not the parent's. A Constant 0.0001 parent (OpenWDS 10220 GO1) dies
    // during Duration; children live 0.2–1.2s. Primary collect_births
    // drops the event list at duration+parent life — hydrate Constant
    // does not keep the unused minScalar. Same window for every
    // Birth/Death slot, not an ID.
    {
      SplitWallScope wall_births(SplitWallSec::Births);
      collect_births(parent, ctx.now_t, ctx.color_id, layer.births, &ctx, true);
    }
    {
      SplitWallScope wall_cap(SplitWallSec::CapSort);
      apply_max_particles_alive_cap(parent, ctx, layer.births);
    }
    if (!layer.births.empty()) {
      queue.push_back(std::move(layer));
    } else {
    }
  }

  const uint32_t base = idx_of(particles[0]);
  std::vector<ChildSysHoist> child_sys(static_cast<size_t>(count));
  for (uint16_t ci = 0; ci < count; ++ci) {
    if (static_cast<int>(particles[ci].element_index) != element_slot) {
      continue;
    }
    fill_child_sys_hoist(particles[ci], child_sys[ci]);
  }
  std::vector<ParentSysHoist> parent_sys(static_cast<size_t>(count));
  std::vector<ParentSlotHoist> parent_slot_flat;
  {
    size_t slot_total = 0;
    for (uint16_t ci = 0; ci < count; ++ci) {
      if (static_cast<int>(particles[ci].element_index) != element_slot) {
        continue;
      }
      uint16_t nslots = 0;
      (void)subemitters_of(static_cast<uint16_t>(idx_of(particles[ci])), &nslots);
      slot_total += static_cast<size_t>(nslots);
    }
    parent_slot_flat.resize(slot_total);
    size_t slot_off = 0;
    for (uint16_t ci = 0; ci < count; ++ci) {
      if (static_cast<int>(particles[ci].element_index) != element_slot) {
        continue;
      }
      uint16_t nslots = 0;
      const uint32_t pidx = idx_of(particles[ci]);
      (void)subemitters_of(static_cast<uint16_t>(pidx), &nslots);
      fill_parent_sys_hoist(parent_sys[ci], nslots > 0 ? parent_slot_flat.data() + slot_off : nullptr,
                            particles[ci], particles, count, base, pidx, element_slot,
                            child_sys[ci].const_life, &ctx);
      slot_off += static_cast<size_t>(nslots);
    }
  }
  std::vector<uint8_t> child_incr(static_cast<size_t>(count), 0);
  std::vector<int> child_cand_n(static_cast<size_t>(count), 0);
  std::vector<std::vector<PersistCand>> child_cands(static_cast<size_t>(count));
  // Incremental PLAN dest-strip/reconstruct is not byte-identical to remake
  // occupancy (persist-check 10516/10660/...). Realize still persist-hits.
  if (false && split_eval_persist_enabled() && !observers_installed() && !fill_now) {
    for (uint16_t ci = 0; ci < count; ++ci) {
      const SplitEffectParticle& child = particles[ci];
      if (static_cast<int>(child.element_index) != element_slot ||
          !persist_child_eligible(child) || !persist_child_can_hit(ctx, child)) {
        continue;
      }
      const SystemCarryKey key = make_child_persist_key(ctx, child, idx_of(child));
      const ChildPersistFp fp = make_child_persist_fp(ctx, child);
      if (!persist_peek_cands(key, fp, ctx.now_t, child_cands[ci]) || child_cands[ci].empty()) {
        continue;
      }
      child_incr[ci] = 1;
      child_cand_n[ci] = static_cast<int>(child_cands[ci].size());
    }
  }
  std::vector<std::vector<uint8_t>> child_cand_used(static_cast<size_t>(count));
  for (uint16_t ci = 0; ci < count; ++ci) {
    child_cand_used[ci].assign(child_cands[ci].size(), 0);
  }
  for (int depth = 0; depth < 6 && !queue.empty(); ++depth) {
    size_t ev_n = 0;
    for (const LayerBirths& layer : queue) {
      ev_n += layer.births.size();
    }
    std::vector<SubEventWork> events = take_event_vec(ev_n);
    {
      SplitWallScope wall_setup(SplitWallSec::SubemitSetup);
      size_t ei = 0;
      for (const LayerBirths& layer : queue) {
        const uint32_t parent_idx = idx_of(particles[layer.local]);
        uint16_t nslots = 0;
        const SplitEffectSubEmitter* slots =
            subemitters_of(static_cast<uint16_t>(parent_idx), &nslots);
        if (slots == nullptr || nslots == 0) {
          continue;
        }
        for (const Birth& b : layer.births) {
          events[ei].local = layer.local;
          events[ei].parent_idx = parent_idx;
          events[ei].b = b;
          if (nslots > 0 && events[ei].slots.capacity() < static_cast<size_t>(nslots)) {
            events[ei].slots.reserve(static_cast<size_t>(nslots));
          }
          ++ei;
        }
      }
      ev_n = ei;
    }

    struct EventCollectCtx {
      std::vector<SubEventWork>* events = nullptr;
      const SplitEffectParticle* particles = nullptr;
      uint16_t count = 0;
      int element_slot = 0;
      const EmitContext* ctx = nullptr;
      uint32_t base = 0;
      const SystemCarrySnapshot* parent_snaps = nullptr;
      const uint8_t* parent_ok = nullptr;
      const uint8_t* parent_persist = nullptr;
      const uint8_t* child_incr = nullptr;
      const int* child_cand_n = nullptr;
      const ParentSysHoist* parent_sys = nullptr;
      bool fill_now = false;
    } ectx;
    std::vector<SystemCarrySnapshot> parent_snaps(static_cast<size_t>(count));
    std::vector<uint8_t> parent_ok(static_cast<size_t>(count), 0);
    {
      SplitWallScope wall_setup2(SplitWallSec::SubemitSetup);
      if (split_eval_carry_enabled() && !observers_installed()) {
        std::vector<uint8_t> seen(static_cast<size_t>(count), 0);
        for (size_t ei = 0; ei < ev_n; ++ei) {
          const SubEventWork& ev = events[ei];
          if (ev.local >= count || seen[ev.local] != 0) {
            continue;
          }
          seen[ev.local] = 1;
          const SystemCarryKey key = make_system_key(ctx, particles[ev.local], 0);
          if (split_eval_carry_lookup_for_time(key, ctx.now_t, parent_snaps[ev.local])) {
            parent_ok[ev.local] = 1;
          }
        }
      }
    }
    ectx.events = &events;
    ectx.particles = particles;
    ectx.count = count;
    ectx.element_slot = element_slot;
    ectx.ctx = &ctx;
    ectx.base = base;
    ectx.parent_snaps = parent_snaps.data();
    ectx.parent_ok = parent_ok.data();
    ectx.fill_now = fill_now;
    std::vector<uint8_t> parent_persist(static_cast<size_t>(count), 0);
    {
      SplitWallScope wall_setup2b(SplitWallSec::SubemitSetup);
      if (split_eval_persist_enabled() && !observers_installed()) {
        std::vector<uint8_t> seen_p(static_cast<size_t>(count), 0);
        for (size_t ei = 0; ei < ev_n; ++ei) {
          const SubEventWork& ev = events[ei];
          if (ev.local >= count || seen_p[ev.local] != 0) {
            continue;
          }
          seen_p[ev.local] = 1;
          const SystemCarryKey pkey = make_system_key(ctx, particles[ev.local], 0);
          parent_persist[ev.local] = persist_store_exists(pkey) ? 1 : 0;
        }
      }
    }
    ectx.parent_persist = parent_persist.data();
    ectx.child_incr = child_incr.data();
    ectx.child_cand_n = child_cand_n.data();
    ectx.parent_sys = parent_sys.data();
    auto collect_one = [](size_t i, void* raw) {
      auto* c = static_cast<EventCollectCtx*>(raw);
      const SubEventWork& ev = (*c->events)[i];
      const SystemCarrySnapshot* snap = nullptr;
      if (c->parent_snaps != nullptr && c->parent_ok != nullptr && ev.local < c->count &&
          c->parent_ok[ev.local] != 0) {
        snap = &c->parent_snaps[ev.local];
      }
      const bool persist_store =
          c->parent_persist != nullptr && ev.local < c->count && c->parent_persist[ev.local] != 0;
      const ParentSysHoist* ph =
          c->parent_sys != nullptr && ev.local < c->count ? &c->parent_sys[ev.local] : nullptr;
      collect_subemit_parent_event((*c->events)[i], c->particles, c->count, c->element_slot, *c->ctx,
                                   c->base, snap, c->fill_now, persist_store, c->child_incr,
                                   c->child_cand_n, ph);
    };
    // Parent walks are independent (RoD is sequential *inside* one parent).
    // Nested fan-out is OK here: claim_depth!=0 would otherwise serialize
    // every parent onto the Subemit EvalTask worker (11711 11.6ms tail).
    if (ev_n >= 2 && !observers_installed() && split_collect_thread_count() > 1) {
      split_collect_parallel_for(0, ev_n, 1, collect_one, &ectx);
    } else {
      for (size_t i = 0; i < ev_n; ++i) {
        collect_one(i, &ectx);
      }
    }

    std::vector<std::vector<Birth>> child_event(static_cast<size_t>(count));
    struct ChildRef {
      size_t ev = 0;
      size_t sl = 0;
      size_t di = 0;
      int index = 0;
    };
    std::vector<ChildRef> refs;
    {
      SplitWallScope wall_setup3(SplitWallSec::SubemitSetup);
    for (size_t ei = 0; ei < ev_n; ++ei) {
      for (size_t si = 0; si < events[ei].slots.size(); ++si) {
        SubSlotOut& slot = events[ei].slots[si];
        if (slot.incr_resume == 0 || slot.local_child >= count ||
            child_incr[slot.local_child] == 0) {
          continue;
        }
        std::vector<Birth> extras = std::move(slot.births);
        std::vector<SubParentPose> extra_poses = std::move(slot.poses);
        std::vector<uint8_t> extra_ready = std::move(slot.pose_ready);
        std::vector<uint8_t> extra_hyd = std::move(slot.pose_hydrated);
        slot.births.clear();
        slot.poses.clear();
        slot.pose_ready.clear();
        slot.pose_hydrated.clear();
        auto& cands = child_cands[slot.local_child];
        auto& used = child_cand_used[slot.local_child];
        for (size_t ck = 0; ck < cands.size(); ++ck) {
          if (used[ck] != 0) {
            continue;
          }
          const PersistCand& cand = cands[ck];
          if (cand.slot_i != slot.slot_i || !carry_birth_eq(cand.parent_b, events[ei].b)) {
            continue;
          }
          used[ck] = 1;
          slot.births.push_back(cand.b);
          SubParentPose pose{};
          fill_identity_parent_pose(pose, cand.b.time, slot.type, 1.0f, events[ei].b.time, 0);
          slot.poses.push_back(pose);
          slot.pose_ready.push_back(1);
          slot.pose_hydrated.push_back(0);
        }
        for (size_t k = 0; k < extras.size(); ++k) {
          slot.births.push_back(extras[k]);
          if (k < extra_poses.size()) {
            slot.poses.push_back(extra_poses[k]);
          } else {
            SubParentPose pose{};
            fill_identity_parent_pose(pose, extras[k].time, slot.type, 1.0f, events[ei].b.time, 0);
            slot.poses.push_back(pose);
          }
          slot.pose_ready.push_back(k < extra_ready.size() ? extra_ready[k] : uint8_t(1));
          slot.pose_hydrated.push_back(k < extra_hyd.size() ? extra_hyd[k] : uint8_t(0));
        }
        slot.n_full_births = static_cast<int>(slot.births.size());
      }
    }

    for (size_t ei = 0; ei < ev_n; ++ei) {
      for (size_t si = 0; si < events[ei].slots.size(); ++si) {
        SubSlotOut& slot = events[ei].slots[si];
        auto& dest = child_event[slot.local_child];
        const size_t bn = slot_birth_n(slot);
        for (size_t di = 0; di < bn; ++di) {
          // Keep the per-parent emit identity (burst k / RoT extra 800+k /
          // RoD extra 900+k). Do not rewrite Birth.index to dest.size():
          // that remints particle_seed every time an earlier parent grows.
          const Birth cb = slot_birth_at(slot, di);
          dest.push_back(cb);
          refs.push_back(ChildRef{ei, si, di, cb.index});
        }
        if (fill_now) {
          slot.lives.reserve(bn);
          slot.lives.resize(bn);
        }
        slot.items.reserve(bn);
        slot.keep.reserve(bn);
        slot.items.resize(bn);
        slot.keep.assign(bn, 0);
      }
    }
    if (!fill_now && !plan.persist_occ.empty()) {
      for (size_t ei = 0; ei < ev_n; ++ei) {
        for (size_t si = 0; si < events[ei].slots.size(); ++si) {
          const SubSlotOut& slot = events[ei].slots[si];
          if (slot.local_child >= count) {
            continue;
          }
          const uint32_t child_idx = idx_of(particles[static_cast<uint16_t>(slot.local_child)]);
          const size_t bn = slot_birth_n(slot);
          for (size_t di = 0; di < bn; ++di) {
            PersistCand c;
            c.b = slot_birth_at(slot, di);
            c.parent_b = events[ei].b;
            c.slot_i = slot.slot_i;
            c.properties = slot.properties;
            c.child_idx = child_idx;
            plan.persist_occ.push_back(c);
          }
        }
      }
    }
    }
    if (!refs.empty()) {
      SplitWallScope wall_ccap(SplitWallSec::ChildCap);
      auto& ref_keep = g_alive_cap_tls.ref_keep;
      ref_keep.assign(refs.size(), 1);
      for (uint16_t ci = 0; ci < count; ++ci) {
        const SplitEffectParticle& child = particles[ci];
        const int cap = child.max_particles > 0 ? static_cast<int>(child.max_particles)
                                                : kSplitEffectParticlePool;
        auto& mine = g_alive_cap_tls.mine;
        mine.clear();
        for (size_t ri = 0; ri < refs.size(); ++ri) {
          if (events[refs[ri].ev].slots[refs[ri].sl].local_child == ci) {
            mine.push_back(ri);
          }
        }
        if (static_cast<int>(mine.size()) <= cap) {
          continue;
        }
        auto& cand = g_alive_cap_tls.cand;
        auto& death = g_alive_cap_tls.death;
        cand.resize(mine.size());
        death.resize(mine.size());
        const ChildSysHoist& ch = child_sys[ci];
        const float sim = ch.sim;
        for (size_t k = 0; k < mine.size(); ++k) {
          const ChildRef& r = refs[mine[k]];
          const SubSlotOut& slot = events[r.ev].slots[r.sl];
          cand[k] = slot_birth_at(slot, r.di);
          const float eu =
              r.di < slot.poses.size()
                  ? official_split_effect_sub_emit_u(slot.poses[r.di].age, slot.poses[r.di].life)
                  : 1.0f;
          const uint32_t cseed =
              child_emit_seed(ctx, child, cand[k], events[r.ev].b, slot.slot_i);
          float life =
              ch.const_life >= 0.0f ? ch.const_life : birth_start_life(child, ctx, cand[k], eu, cseed);
          if ((slot.properties & kSplitEffectSubInheritLifetime) != 0 &&
              r.di < slot.poses.size()) {
            const SubParentPose& born = slot.poses[r.di];
            const float rem =
                born.life > 1e-8f ? std::clamp(1.0f - born.age / born.life, 0.0f, 1.0f) : 0.0f;
            life *= rem;
          }
          death[k] = cand[k].time + std::max(life, 1e-4f) / sim;
        }
        auto& keep = g_alive_cap_tls.keep;
        mark_alive_cap(cap, cand, death, keep);
        for (size_t k = 0; k < mine.size(); ++k) {
          if (keep[k] == 0) {
            ref_keep[mine[k]] = 0;
          }
        }
      }
      size_t w = 0;
      for (size_t i = 0; i < refs.size(); ++i) {
        if (ref_keep[i] != 0) {
          refs[w++] = refs[i];
        }
      }
      refs.resize(w);
    }

    struct ChildFillCtx {
      std::vector<SubEventWork>* events = nullptr;
      const ChildRef* refs = nullptr;
      const SplitEffectParticle* particles = nullptr;
      uint16_t count = 0;
      const EmitContext* ctx = nullptr;
      const ChildSysHoist* child_sys = nullptr;
      bool fill_now = true;
    } cctx;
    cctx.events = &events;
    cctx.refs = refs.data();
    cctx.particles = particles;
    cctx.count = count;
    cctx.ctx = &ctx;
    cctx.child_sys = child_sys.data();
    cctx.fill_now = fill_now;
    auto fill_one = [](size_t i, void* raw) {
      SplitWallScope wall_cf(SplitWallSec::ChildFill);
      auto* c = static_cast<ChildFillCtx*>(raw);
      const ChildRef& r = c->refs[i];
      SubEventWork& ev = (*c->events)[r.ev];
      SubSlotOut& slot = ev.slots[r.sl];
      const SplitEffectParticle& child = c->particles[static_cast<uint16_t>(slot.local_child)];
      const uint32_t child_idx = bound_catalog_index(c->particles, c->count, *c->ctx, child);
      if (r.di >= slot.pose_ready.size() || slot.pose_ready[r.di] == 0) {
        slot.keep[r.di] = 0;
        return;
      }
      thread_local SplitEffectLiveParticle dummy;
      SplitEffectLiveParticle& dest = c->fill_now ? slot.lives[r.di] : dummy;
      const uint8_t hyd = r.di < slot.pose_hydrated.size() ? slot.pose_hydrated[r.di] : 0;
      const Birth cb = slot_birth_at(slot, r.di);
      const ChildSysHoist* sys =
          c->child_sys != nullptr && slot.local_child < c->count ? &c->child_sys[slot.local_child]
                                                                 : nullptr;
      fill_subemit_child(child, cb, slot.poses[r.di], slot.properties, *c->ctx,
                         child_idx, dest, slot.keep[r.di], nullptr, nullptr, ev.b, slot.slot_i,
                         c->fill_now, &slot.items[r.di], &c->particles[ev.local], ev.parent_idx, hyd,
                         sys);
    };
    if (should_parallel_eval(refs.size())) {
      split_collect_parallel_for(0, refs.size(), 1, fill_one, &cctx);
    } else {
      for (size_t i = 0; i < refs.size(); ++i) {
        fill_one(i, &cctx);
      }
    }

    {
      SplitWallScope wall_merge(SplitWallSec::SubemitMerge);
      for (size_t ei = 0; ei < ev_n; ++ei) {
        for (size_t si = 0; si < events[ei].slots.size(); ++si) {
          SubSlotOut& slot = events[ei].slots[si];
          if (slot.local_child >= count) {
            continue;
          }
          const SplitEffectParticle& child = particles[static_cast<uint16_t>(slot.local_child)];
          const int ribbon0 = plan.kept;
          const uint16_t stitch_group = static_cast<uint16_t>(ribbon0 & 0xffff);
          const uint8_t is_ribbon =
              (ctx.want_trail && child.trail_mode == 1) ? uint8_t(1) : uint8_t(0);
          int expect = 0;
          for (size_t di = 0; di < slot.keep.size(); ++di) {
            if (slot.keep[di] != 0) {
              ++expect;
            }
          }
          const int take = std::min(expect, std::max(0, max_out - plan.kept));
          if (out_vec != nullptr && take > 0 &&
              static_cast<int>(out_vec->size()) < plan.kept + take) {
            out_vec->resize(static_cast<size_t>(plan.kept + take));
          }
          SplitEffectLiveParticle* dest = out_vec != nullptr ? out_vec->data() : out;
          for (size_t di = 0; di < slot.keep.size(); ++di) {
            if (slot.keep[di] == 0) {
              continue;
            }
            if (plan.kept >= max_out) {
              break;
            }
            if (dest != nullptr) {
              dest[plan.kept] = std::move(slot.lives[di]);
            }
            if (!fill_now) {
              SubemitFillItem item = std::move(slot.items[di]);
              item.stitch_group = stitch_group;
              item.ribbon = is_ribbon;
              item.particle_trail = 0;
              item.child = &child;
              item.child_idx = idx_of(child);
              plan.items.push_back(std::move(item));
            }
            ++plan.kept;
          }
          if (dest != nullptr && ctx.want_trail && child.trail_mode == 1 && plan.kept > ribbon0) {
            stitch_ribbons(child, dest + ribbon0, plan.kept - ribbon0, ctx.line_index,
                           idx_of(child));
          }
        }
      }
    }
    give_event_vec(std::move(events), ev_n);

    std::vector<LayerBirths> next_q;
    for (uint16_t ci = 0; ci < count; ++ci) {
      if (child_event[ci].empty()) {
        continue;
      }
      uint16_t nslots = 0;
      const SplitEffectSubEmitter* slots =
          subemitters_of(static_cast<uint16_t>(idx_of(particles[ci])), &nslots);
      if (slots == nullptr || nslots == 0) {
        continue;
      }
      bool fire = false;
      for (uint16_t s = 0; s < nslots; ++s) {
        if (slots[s].type == kSplitEffectSubEmitBirth || slots[s].type == kSplitEffectSubEmitDeath) {
          fire = true;
          break;
        }
      }
      if (!fire) {
        continue;
      }
      LayerBirths layer;
      layer.local = ci;
      layer.births = std::move(child_event[ci]);
      next_q.push_back(std::move(layer));
    }
    queue.swap(next_q);
  }

  return plan.kept;
}

bool child_carry_time_lt(float a, float b) noexcept {
  uint32_t ua = 0;
  uint32_t ub = 0;
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua < ub;
}

// Sort child CarryParticle rows in the same order carry_find_child() probes:
// parent birth (index/extra/time), slot, child birth (index/extra/time).
bool child_carry_lt(const CarryParticle& a, const CarryParticle& b) noexcept {
  if (a.parent_b.index != b.parent_b.index) {
    return a.parent_b.index < b.parent_b.index;
  }
  if (a.parent_b.extra != b.parent_b.extra) {
    return a.parent_b.extra < b.parent_b.extra;
  }
  if (a.parent_b.time != b.parent_b.time) {
    return child_carry_time_lt(a.parent_b.time, b.parent_b.time);
  }
  if (a.slot_i != b.slot_i) {
    return a.slot_i < b.slot_i;
  }
  if (a.b.index != b.b.index) {
    return a.b.index < b.b.index;
  }
  if (a.b.extra != b.b.extra) {
    return a.b.extra < b.b.extra;
  }
  if (a.b.time != b.b.time) {
    return child_carry_time_lt(a.b.time, b.b.time);
  }
  return false;
}

int evaluate_subemitters_realize(const EmitContext& ctx, SubemitFillPlan& plan,
                                 const uint8_t* do_fill, SplitEffectLiveParticle* out, int max_out) {
  if (out == nullptr || max_out <= 0 || plan.kept <= 0) {
    return 0;
  }
  SplitWallScope wall_real(SplitWallSec::SubemitSetup);
  SplitWallScope wall_subemit_realize(SplitWallSec::SubemitRealize);
  SplitDiagScope diag_subemit_realize(SplitDiagPhase::SubemitRealize);
  int written = 0;
  uint16_t prev_group = 0xffffu;
  int ribbon0 = 0;
  const SplitEffectParticle* ribbon_child = nullptr;
  uint32_t ribbon_idx = 0;
  auto flush_stitch = [&]() {
    if (ribbon_child != nullptr && ctx.want_trail && ribbon_child->trail_mode == 1 &&
        written > ribbon0) {
      stitch_ribbons(*ribbon_child, out + ribbon0, written - ribbon0, ctx.line_index, ribbon_idx);
    }
  };
  const int n = std::min(plan.kept, static_cast<int>(plan.items.size()));
  // B1: one child-state checkpoint per plan/task. The plan is per bound parent
  // system, so all nonzero parent pointers should share the same key; if they
  // do not, skip the checkpoint rather than pair a child with the wrong parent.
  thread_local std::vector<CarryParticle> child_carry_parts;
  child_carry_parts.clear();
  SystemCarryKey child_carry_key{};
  SystemCarrySnapshot child_carry_snap;
  bool have_child_carry_key = false;
  bool have_child_carry_snap = false;
  if (n > 0 && split_eval_carry_rewind_enabled() && !observers_installed()) {
    const SubemitFillItem* key_item = nullptr;
    for (int i = 0; i < n; ++i) {
      if (plan.items[static_cast<size_t>(i)].parent != nullptr) {
        key_item = &plan.items[static_cast<size_t>(i)];
        break;
      }
    }
    if (key_item != nullptr) {
      bool consistent = true;
      for (int i = 0; i < n; ++i) {
        const SubemitFillItem& it = plan.items[static_cast<size_t>(i)];
        if (it.parent != nullptr &&
            (it.parent != key_item->parent || it.parent_idx != key_item->parent_idx)) {
          consistent = false;
          break;
        }
      }
      if (consistent) {
        EmitContext key_ctx = ctx;
        key_ctx.catalog_index_override = key_item->parent_idx;
        key_ctx.has_catalog_index_override = 1;
        child_carry_key = make_system_key(key_ctx, *key_item->parent, 1);
        have_child_carry_key = true;
        have_child_carry_snap =
            split_eval_carry_lookup_for_time(child_carry_key, ctx.now_t, child_carry_snap);
      }
    }
  }
  thread_local std::vector<uint8_t> will_fill;
  thread_local std::vector<ChildPersistSession> child_sessions;
  thread_local std::vector<int> child_session_of;
  thread_local std::vector<int> child_slot_of;
  thread_local int child_n_sessions = 0;
  {
    SplitWallScope wall_setup(SplitWallSec::SubemitSetup);
  std::vector<uint8_t> group_keep(static_cast<size_t>(n), 0);
  if (do_fill != nullptr) {
    uint16_t g = 0xffffu;
    int g0 = 0;
    uint8_t any = 0;
    auto mark = [&](int hi) {
      if (any != 0) {
        for (int j = g0; j < hi; ++j) {
          group_keep[static_cast<size_t>(j)] = 1;
        }
      }
    };
    for (int i = 0; i < n; ++i) {
      const SubemitFillItem& item = plan.items[static_cast<size_t>(i)];
      if (i == 0 || item.stitch_group != g) {
        mark(i);
        g = item.stitch_group;
        g0 = i;
        any = 0;
      }
      if (do_fill[i] != 0) {
        any = 1;
      }
    }
    mark(n);
  }
  will_fill.assign(static_cast<size_t>(n), 0);
  for (int i = 0; i < n; ++i) {
    const SubemitFillItem& item = plan.items[static_cast<size_t>(i)];
    const bool group_has_kept = do_fill == nullptr || group_keep[static_cast<size_t>(i)] != 0;
    if (do_fill == nullptr || do_fill[i] != 0 || (group_has_kept && item.ribbon != 0)) {
      will_fill[static_cast<size_t>(i)] = 1;
    }
  }
  begin_child_persist(ctx, plan, will_fill.data(), n, child_sessions, child_n_sessions,
                      child_session_of, child_slot_of);
  {
    thread_local std::vector<uint8_t> need;
    thread_local std::vector<int> newly;
    need.assign(static_cast<size_t>(n), 0);
    newly.clear();
    if (newly.capacity() < static_cast<size_t>(n)) {
      newly.reserve(static_cast<size_t>(n));
    }
    for (int i = 0; i < n; ++i) {
      if (will_fill[static_cast<size_t>(i)] != 0 && child_slot_of[static_cast<size_t>(i)] < 0) {
        need[static_cast<size_t>(i)] = 1;
      }
    }
    if (split_defer_rot_parent_pose()) {
      hydrate_deferred_parent_poses(ctx, plan, need.data(), n, &newly);
      store_hydrated_walk_poses(ctx, plan, newly);
    }
  }
  }
  for (int i = 0; i < n && written < max_out; ++i) {
    SubemitFillItem& item = plan.items[static_cast<size_t>(i)];
    if (i == 0 || item.stitch_group != prev_group) {
      flush_stitch();
      ribbon0 = written;
      ribbon_child = item.ribbon != 0 ? item.child : nullptr;
      ribbon_idx = item.child_idx;
      prev_group = item.stitch_group;
    }
    // stitch_ribbons reads the whole written chain (length / prefix UV /
    // arc t). A group with no cap-kept candidate skips the copy; any kept
    // ribbon member still force-fills neighbours. Particle-trail vertices
    // are per-particle (no stitch). hop.
    const bool fill = will_fill[static_cast<size_t>(i)] != 0;
    if (fill) {
      const int si = child_session_of[static_cast<size_t>(i)];
      const int persist_i = child_slot_of[static_cast<size_t>(i)];
      uint8_t keep = 0;
      if (persist_i >= 0 && si >= 0) {
        ChildPersistSession& sess = child_sessions[static_cast<size_t>(si)];
        const PersistSlot* slot = persist_slot_at(sess.claim, persist_i);
        if (slot != nullptr && item.child != nullptr) {
          PrimaryEvalJob job{};
          job.b = item.b;
          job.parent_b = item.parent_b;
          job.slot_i = item.slot_i;
          job.is_child = 1;
          job.seed = item.seed;
          job.emit_u = item.emit_u;
          job.age = item.age;
          job.pose_age = item.pose_age;
          adopt_stored_state(job, slot->st, slot->grid, slot->has_grid, ctx, *item.child);
          job.do_fill = 1;
          job.alive = 1;
          job.pose_age = job.age < job.st.life ? job.age : std::max(job.st.life - 1e-4f, 0.0f);
          // Child pose cache is keyed by parent walk, not child Birth.index.
          PoseIntegrator integ(job.st);
          if (job.has_grid != 0) {
            integ.import_carry_grid(job.grid);
          }
          if (IntegratorHot* chot = persist_claimed_hot(persist_i)) {
            integ.bind_hot(chot);
          }
          integ.set_cache_key(nullptr);
          WDS_PERF_COUNT("shuriken.integrate.child", 1);
          fill_live(integ, job.st, job.pose_age, ctx.want_trail, out[written], true, job.age);
          integ.export_grid(job.grid);
          job.has_grid = 1;
          stamp_evaluated_live(out[written], ctx, *item.child, job.st, job.age, item.child_idx);
          item.filled = 1;
          keep = 1;
          job.persist_i = persist_i;
          sess.jobs.push_back(job);
        }
      }
      if (keep == 0 && item.child != nullptr) {
        CarryParticle cap{};
        fill_subemit_child(*item.child, item.b, item.born, item.properties, ctx, item.child_idx,
                           out[written], keep,
                           have_child_carry_snap ? &child_carry_snap : nullptr, &cap,
                           item.parent_b, item.slot_i, true, &item);
        if (keep != 0) {
          if (have_child_carry_key) {
            child_carry_parts.push_back(cap);
          }
          if (si >= 0 && child_sessions[static_cast<size_t>(si)].persist_ok != 0) {
            PrimaryEvalJob job{};
            job_from_child_carry(job, cap);
            child_sessions[static_cast<size_t>(si)].jobs.push_back(job);
          }
        }
      }
      if (keep == 0) {
        out[written] = SplitEffectLiveParticle{};
        item.filled = 0;
      }
    } else {
      out[written] = SplitEffectLiveParticle{};
      item.filled = 0;
    }
    ++written;
  }
  finish_child_persist(child_sessions, child_n_sessions, ctx.now_t, plan, &plan.persist_occ);
  child_n_sessions = 0;
  if (have_child_carry_key && !child_carry_parts.empty()) {
    std::sort(child_carry_parts.begin(), child_carry_parts.end(), child_carry_lt);
    split_eval_carry_store(child_carry_key, ctx.now_t, std::move(child_carry_parts), 0);
  }
  flush_stitch();
  return written;
}

int evaluate_subemitters(const SplitEffectParticle* particles, uint16_t count, int element_slot,
                         const EmitContext& ctx, SplitEffectLiveParticle* out, int max_out) {
  SubemitFillPlan plan;
  return evaluate_subemitters_plan(particles, count, element_slot, ctx, plan, max_out, out);
}

void reset_split_eval_persistence_for_tests() noexcept {
  split_eval_persist_clear();
  split_eval_carry_clear();
  split_eval_cache_bump_epoch();
  split_collect_clear_dead_latches();
}

}  // namespace wds::chart_render::shuriken

#include <wds/chart_render/shuriken/evaluate.hpp>
#include "internal.hpp"
#include "split_wall_profile.hpp"

#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wds::chart_render::shuriken {
namespace {

// Safety only. Unity 2022.3 Main.maxParticles is a concurrent-alive cap
// applied after collect (evaluate.cpp), not a historical birth budget.
// https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysMainModule.html
int collect_list_cap() noexcept { return kSplitEffectParticlePool; }

float playback_delay(const SplitEffectParticle& p) noexcept {
  return start_delay_wall_clock(p);
}

void push_birth(std::vector<Birth>& out, float time, int index, uint32_t extra, int cap,
                float parent_z = 0.0f, bool parent_pose = false) {
  if (static_cast<int>(out.size()) >= cap) {
    return;
  }
  Birth b;
  b.time = time;
  b.index = index;
  b.extra = extra;
  b.parent_z = parent_z;
  b.parent_pose = parent_pose ? 1 : 0;
  out.push_back(b);
}

// live dump orb-11791-v1 hop, not 2022.3: every RateOverDistance birth in a
// frame takes startSize (and the other emit-time curves) at the frame-end
// system time. 11740_3..7 size.start matches frame end exactly; birth time
// is off by up to 1.7. Rate-over-time / bursts are not covered by that dump.
void push_rod_birth(std::vector<Birth>& out, float time, int index, uint32_t extra, int cap,
                    float parent_z = 0.0f, bool parent_pose = false) {
  const size_t before = out.size();
  push_birth(out, time, index, extra, cap, parent_z, parent_pose);
  if (out.size() > before) {
    out.back().curve_t = split_effect_frame_end_play_t(time);
  }
}

uint32_t fbits(float x) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &x, sizeof(u));
  return u != 0 ? u : 1u;
}

// live dump v1: bind copies are not addresses in kSplitEffectParticles, so
// catalog_index() is 0 for every system on a line. Auto-seed TwoCurves
// (2022/2023-class, 31 systems) then shared one pick/seed stream and sat
// on the same points. Identity is authored catalog fields that survive
// hydrate+bind (curve table slots + scalars). Manual same-seed siblings
// (10820/10820_1) keep one stream via random_seed. Not Unity auto hash,
// not 2022.3.
uint32_t rod_system_identity(const SplitEffectParticle& p) noexcept {
  if (p.auto_random_seed == 0) {
    const uint32_t seed = p.random_seed != 0 ? p.random_seed : 1u;
    return mix3(seed, 3u, 1u);
  }
  const uint32_t slots =
      mix3(static_cast<uint32_t>(p.rod_curve_begin) + 1u,
           static_cast<uint32_t>(p.rod_min_curve_begin) + 1u,
           static_cast<uint32_t>(p.size_curve_begin) + 1u);
  const uint32_t more =
      mix3(static_cast<uint32_t>(p.sol_begin) + 1u, static_cast<uint32_t>(p.col_color_begin) + 1u,
           static_cast<uint32_t>(p.element_index) + 1u);
  const uint32_t authored =
      mix3(fbits(p.size), fbits(p.rate_over_distance),
           mix3(fbits(p.rate_over_distance_min), fbits(p.color_r), fbits(p.local_pos_y)));
  return mix3(slots, more, authored);
}

uint32_t rod_random_seed(const EmitContext* ctx, const SplitEffectParticle& p,
                         int birth_index) noexcept {
  const uint32_t id = rod_system_identity(p);
  if (ctx != nullptr && ctx->parent_motion.rod_emitter_seed_candidate) {
    return mix3(stable_emitter_identity(*ctx), static_cast<uint32_t>(birth_index + 1), id);
  }
  return mix3(id, static_cast<uint32_t>(birth_index + 1), 3u);
}

// live dump v2 + v3 (re-overlap-v2 / re-overlap-v3-work-report.md):
// Independent ParticleSystems that share authored/controller seed must not
// share Birth.extra when birth sampling fields differ (2020/2021-class).
// OpenWDS Controller writes one Random.Range(0,1000) onto every !auto
// system on a line; bind copies also have catalog_index()==0.
// Identity is authored birth position / velocity / shape / emission
// fields that survive hydrate+bind. rotation3D is not a sampling field:
// 2013/2014-class (same shape/speed/rod/VOL, only rotation3D + startSize)
// stay one stream like 10820. Do not mix startSize or curve table slots
// on manual systems. Keep rot_z / rbs / sortingOrder so 2020/2021 stay
// split. Auto systems still mix rod_system_identity so 2022/2023 stay
// split. Not Unity auto hash, not 2022.3.
uint32_t sampling_system_identity(const SplitEffectParticle& p) noexcept {
  const uint32_t shape =
      mix3(mix3(static_cast<uint32_t>(p.shape_type) + 1u, fbits(p.shape_radius),
                fbits(p.shape_angle)),
           mix3(fbits(p.shape_scale_x), fbits(p.shape_scale_y), fbits(p.shape_scale_z)),
           mix3(fbits(p.shape_pos_x), fbits(p.local_pos_y), fbits(p.random_direction_amount)));
  const uint32_t vel =
      mix3(mix3(fbits(p.start_speed), fbits(p.start_speed_min), fbits(p.rate_over_distance)),
           mix3(fbits(p.vel_x), fbits(p.vel_y), fbits(p.vel_z)),
           mix3(fbits(p.rot_z), static_cast<uint32_t>(p.rbs_enabled) + 1u, 1u));
  const uint32_t rest =
      mix3(static_cast<uint32_t>(p.sorting_order) + 1u, fbits(p.rate_over_time),
           mix3(fbits(p.shape_rot_z), static_cast<uint32_t>(p.shape_enabled) + 1u,
                fbits(p.random_position_amount)));
  const uint32_t values = mix3(shape, vel, rest);
  if (p.auto_random_seed == 0) {
    const uint32_t seed = p.random_seed != 0 ? p.random_seed : 1u;
    return mix3(values, mix3(seed, 3u, 1u), 5u);
  }
  return mix3(values, rod_system_identity(p), 5u);
}

uint32_t birth_sampling_extra(const SplitEffectParticle& p, uint32_t tag) noexcept {
  return mix3(sampling_system_identity(p), tag != 0 ? tag : 1u, 7u);
}

// Closed-form |Δps.worldPosition| when the ParticleSystem is a child of
// LineHight and the only motion is OpenWDS scale.y (27 × fade × local axis).
// live dump v1 hop is still RoD × Σ|Δps.worldPosition| — this is that path,
// not dump-column lossyY / chord / max(wda, 2·Σdt). Not 2022.3.
float authored_rod_travel(const SplitEffectParticle& p, int32_t color_id,
                          float fade_scale) noexcept {
  const float axis = emit_local_y(color_id) + p.local_pos_y;
  return std::fabs(axis) * wds::chart_editor::kOfficialLineHeightScale *
         std::max(fade_scale, 0.0f);
}

// live dump v1 (synth-mesh-measure-v14 Wave 1 / Wave 4 + rod-work-report):
// distance is Σ|Δps.worldPosition| when parent_motion is set; otherwise the
// LineHight-scale closed form of that same path. Curve input is
// particleSystem.time / Duration. Integrate rate(u)·dS. Forbidden:
// lossyY / chord / max(wda, 2·Σdt). Not 2022.3.
float rod_parent_travel_at(const SplitEffectParticle& p, float play_t, float now_t,
                           int32_t color_id, const EmitContext* context) noexcept {
  if (context != nullptr && context->parent_motion.rod_motion_candidate) {
    const float total = std::max(context->parent_motion.distance_total, 0.0f);
    const float ups = std::max(context->parent_motion.units_per_second, 1e-6f);
    const float origin_play = now_t - total / ups;
    const float dt = play_t - origin_play;
    if (dt <= 0.0f) {
      return 0.0f;
    }
    return std::min(total, ups * dt);
  }
  const float grow_t =
      std::min(std::max(play_t, 0.0f), kSplitEffectFadeEndPlayT);
  return authored_rod_travel(p, color_id, fade_scale_at_play_t(grow_t));
}

// Travel is monotone in play time within one RoD step.
float rod_play_t_at_travel(const SplitEffectParticle& p, float lo, float hi, float target,
                           float now_t, int32_t color_id, const EmitContext* context) noexcept {
  for (int i = 0; i < 12; ++i) {
    const float mid = 0.5f * (lo + hi);
    if (rod_parent_travel_at(p, mid, now_t, color_id, context) < target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void collect_curve_rod_births(const SplitEffectParticle& p, float now_t, int32_t color_id,
                              std::vector<Birth>& out, const EmitContext* context, int cap,
                              int next) {
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = playback_delay(p);
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  const float now_play = now_t - delay;
  const float now_sim = now_play * speed;
  const float span = looping ? now_sim : std::min(now_sim, length);
  if (span <= 1e-6f) {
    return;
  }
  float frac = 0.0f;
  int k = 0;
  const uint32_t id = rod_system_identity(p);
  const uint32_t extra = birth_sampling_extra(p, 3u);
  const bool pose = context != nullptr && context->parent_motion.rod_motion_candidate &&
                    context->parent_motion.birth_pose_candidate;
  const float end_play = delay + span / speed;
  for (float play_t = delay; play_t < end_play && static_cast<int>(out.size()) < cap && k < cap;) {
    // live dump orb-11791-v1: RoD(u) is evaluated once per frame at the frame
    // end (11740_6/7 spacing = 1/RoD(frame-end u)). Not 2022.3.
    const float frame_end = split_effect_next_frame_play_t(play_t);
    const float play_t1 = std::min(frame_end, end_play);
    const float u = std::clamp((frame_end - delay) * speed / length, 0.0f, 1.0f);
    float rate = official_split_effect_eval_curve_keys(p.rod_curve_begin, p.rod_curve_count, u,
                                                       p.rate_over_distance);
    if (p.rod_min_curve_count > 0) {
      const float lo = official_split_effect_eval_curve_keys(
          p.rod_min_curve_begin, p.rod_min_curve_count, u, p.rate_over_distance_min);
      // hop / live dump align2-u1-unity-v1 1070_7: Evaluate min/max identical
      // across 7 lines, counts are not (play_t=0.119 n=1,3,3,1,1,3,2 vs
      // native 2×7). One r per instance (catalog + line/start), not
      // mix3(catalog, k+1, 3). Time-independent so persist / carry /
      // dead-skip remakes match. TwoCurves RoD only (1070_7, 10820_3).
      // TwoConstants RoD and Curve-only 1070_2 stay on their own paths.
      // Not 2022.3.
      const uint32_t inst =
          context != nullptr ? stable_emitter_identity(*context)
                             : mix3(static_cast<uint32_t>(color_id + 1), 1u, 1u);
      rate = pick(rate, lo, true, mix3(id, inst, 3u));
    }
    const float d0 = rod_parent_travel_at(p, play_t, now_t, color_id, context);
    float dS = rod_parent_travel_at(p, play_t1, now_t, color_id, context) - d0;
    if (dS < 0.0f) {
      dS = 0.0f;
    }
    // live dump v1: dN = RoD(u) × |Δps.worldPosition|. Not 2022.3.
    const float inc = rate > 0.0f ? split_effect_rod_seeds_from_ps_world_path(rate, dS) : 0.0f;
    float need = 1.0f - frac;
    frac += inc;
    while (frac >= 1.0f && static_cast<int>(out.size()) < cap && k < cap) {
      frac -= 1.0f;
      // Birth sits where the seed count crosses, same spacing as the constant
      // RoD path. Snapping to the step end quantized spacing to whole steps.
      const float s = inc > 1e-12f ? std::clamp(need / inc, 0.0f, 1.0f) : 1.0f;
      const float dist = d0 + s * dS;
      const float birth_t = rod_play_t_at_travel(p, play_t, play_t1, dist, now_t, color_id, context);
      if (pose) {
        const float parent_z = context->parent_motion.motion_origin_z + dist;
        push_rod_birth(out, birth_t, next + k, extra, cap, parent_z, true);
      } else {
        push_rod_birth(out, birth_t, next + k, extra, cap);
      }
      need += 1.0f;
      ++k;
    }
    play_t = play_t1;
  }
}

// live dump refix-b iso_rot_curve hop, not 2022.3: Rate over Time Curve uses
// the same 1/60 Play grid as RoD (split_effect_next_frame_play_t). One rate
// per frame at frame-end u; in-frame births are 1/rate apart. First birth is
// still when the accumulated count reaches 1. Isolate first 4 births
// 0.00340/0.00679/0.01019/0.01359 = 1/rate(u=1/60). Birth-time curves
// (startSize etc.) stay on Birth.time — isolate startSize/lifetime were
// constant, so frame-end curve_t is unproven for RoT. Constant RoT does not
// use this path.
void collect_curve_rot_births(const SplitEffectParticle& p, float now_t, std::vector<Birth>& out,
                              int cap, int next) {
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = playback_delay(p);
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  const float now_sim = (now_t - delay) * speed;
  const float span = looping ? now_sim : std::min(now_sim, length);
  if (span <= 1e-6f) {
    return;
  }
  const float end_play = delay + span / speed;
  const uint32_t extra = birth_sampling_extra(p, 2u);
  float frac = 0.0f;
  int k = 0;
  for (float play_t = delay; play_t < end_play && static_cast<int>(out.size()) < cap && k < cap;) {
    const float frame_end = split_effect_next_frame_play_t(play_t);
    const float play_t1 = std::min(frame_end, end_play);
    if (!(play_t1 > play_t)) {
      break;
    }
    float u = 0.0f;
    if (length > 1e-6f) {
      const float sim_fe = (frame_end - delay) * speed;
      if (looping) {
        float wrapped = std::fmod(sim_fe, length);
        if (wrapped < 0.0f) {
          wrapped += length;
        }
        u = wrapped / length;
      } else {
        u = std::clamp(sim_fe / length, 0.0f, 1.0f);
      }
    }
    const float rate = official_split_effect_eval_rate_over_time(p, u);
    const float dt_sim = (play_t1 - play_t) * speed;
    const float inc = rate > 0.0f ? rate * dt_sim : 0.0f;
    float need = 1.0f - frac;
    frac += inc;
    while (frac >= 1.0f && static_cast<int>(out.size()) < cap && k < cap) {
      frac -= 1.0f;
      const float s = inc > 1e-12f ? std::clamp(need / inc, 0.0f, 1.0f) : 1.0f;
      const float birth_t = play_t + s * (play_t1 - play_t);
      push_birth(out, birth_t, next + k, extra, cap);
      need += 1.0f;
      ++k;
    }
    play_t = play_t1;
  }
}

}  // namespace

void collect_births(const SplitEffectParticle& p, float now_t, int32_t color_id,
                    std::vector<Birth>& out, const EmitContext* context,
                    bool include_expired) {
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = playback_delay(p);
  const float now_sim = (now_t - delay) * speed;
  const float now = now_sim;
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  const float max_life = std::max(p.lifetime, p.lifetime_min);
  const float keep = particle_keep_sim_conservative(p, max_life);
  // Unity 2022.3 Main Looping: the system starts again at the end of Duration.
  // Official catalog: 0 / 1872. Non-looping still dies after duration + life.
  // SubEmitters keep listing those historical births after the parent is
  // gone — child ParticleSystems have their own Main startLifetime
  // (PartSysSubEmitModule + startLifetime). Primary evaluate leaves this
  // window in place.
  if (now < 0.0f || (!include_expired && !looping && now > length + keep)) {
    return;
  }
  const int cap = collect_list_cap();
  int next = static_cast<int>(out.size());

  const float rot = p.rate_over_time;
  if (p.rot_curve_count > 0) {
    collect_curve_rot_births(p, now_t, out, cap, next);
    next = static_cast<int>(out.size());
  } else if (rot > 1e-6f) {
    // live dump dump-pending-v1 hop (10600_8), not 2022.3: constant Rate over
    // Time first birth at 1/rate (k=1..n). Same rule as the curve RoT path
    // (first birth when accumulated count reaches 1). No t=0.
    // Wall clock is still delay + (k/rate)/speed (startDelay hop).
    const float span = looping ? now : std::min(now, length);
    const int n = std::min(cap, std::max(0, static_cast<int>(std::floor(span * rot))));
    for (int k = 0; k < n; ++k) {
      push_birth(out, delay + ((static_cast<float>(k) + 1.0f) / rot) / speed, next + k,
                 birth_sampling_extra(p, 2u), cap);
    }
    next = static_cast<int>(out.size());
  }

  float rod = p.rate_over_distance;
  if (rod <= 1e-6f &&
      p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)) {
    rod = p.emission;
  }
  const bool rod_two = p.rod_random != 0 && p.rate_over_distance_min > 1e-6f;
  if (p.rod_curve_count > 0) {
    collect_curve_rod_births(p, now_t, color_id, out, context, cap, next);
    next = static_cast<int>(out.size());
  } else if (rod > 1e-6f || rod_two) {
    if (context != nullptr && context->parent_motion.rod_motion_candidate &&
        p.rod_curve_count == 0) {
      // live dump v1 hop: distance_total is Σ|Δps.worldPosition|. Crossing /
      // first-interval order stay unverified. Not 2022.3.
      const float total = std::max(context->parent_motion.distance_total, 0.0f);
      const double speed_z = std::max<double>(context->parent_motion.units_per_second, 1e-6);
      auto birth_time_for_distance = [&](float distance) {
        const double sample = context->parent_motion.motion_origin_seconds +
                              static_cast<double>(distance) / speed_z;
        return now_t + static_cast<float>(sample - context->parent_motion.motion_now_seconds);
      };
      if (p.rod_curve_count == 0 && !rod_two) {
        // live dump v1: n ≈ RoD × Σ|Δps.worldPosition| (distance_total). Not 2022.3.
        constexpr float kDistanceEpsilon = 1e-5f;
        int k = 0;
        while (static_cast<int>(out.size()) < cap && k < cap) {
          const float distance = (static_cast<float>(k) + 1.0f) / rod;
          if (distance > total + kDistanceEpsilon) break;
          const float parent_z = context->parent_motion.motion_origin_z + distance;
          push_rod_birth(out, birth_time_for_distance(distance), next + k, birth_sampling_extra(p, 3u),
                     cap, parent_z, context->parent_motion.birth_pose_candidate);
          ++k;
        }
      } else if (rod_two) {
        float distance = 0.0f;
        int k = 0;
        while (static_cast<int>(out.size()) < cap && k < cap) {
          const float rate = pick(p.rate_over_distance, p.rate_over_distance_min, true,
                                  rod_random_seed(context, p, k));
          if (rate <= 1e-6f) break;
          distance += 1.0f / rate;
          if (distance > total + 1e-5f) break;
          const float parent_z = context->parent_motion.motion_origin_z + distance;
          push_rod_birth(out, birth_time_for_distance(distance), next + k, birth_sampling_extra(p, 3u),
                     cap, parent_z, context->parent_motion.birth_pose_candidate);
          ++k;
        }
      }
      next = static_cast<int>(out.size());
    } else {
    const float grow_t = std::min(now_t, kSplitEffectFadeEndPlayT);
    const float delay_t = std::min(delay, grow_t);
    if (grow_t > delay_t + 1e-4f && fade_scale_at_play_t(grow_t) > 1e-4f) {
      // live dump v1: travel is closed-form |Δps.worldPosition|; n ≈ RoD × travel.
      // Start Delay: playback has not started, so animator motion before
      // delay does not emit (Manual/PartSysMainModule.html startDelay).
      const float travel_now = authored_rod_travel(p, color_id, fade_scale_at_play_t(grow_t));
      const float travel_delay = authored_rod_travel(p, color_id, fade_scale_at_play_t(delay_t));
      const float travel = std::max(0.0f, travel_now - travel_delay);
      const float full_travel = std::max(authored_rod_travel(p, color_id, 1.0f), 1e-4f);
      const uint32_t extra = birth_sampling_extra(p, 3u);
      if (rod_two) {
        float acc = 0.0f;
        int k = 0;
        while (static_cast<int>(out.size()) < cap && k < cap) {
          const float rate = pick(p.rate_over_distance, p.rate_over_distance_min, true,
                                  rod_random_seed(context, p, k));
          if (rate <= 1e-6f) {
            break;
          }
          acc += 1.0f / rate;
          if (acc > travel) {
            break;
          }
          const float scale = std::clamp((travel_delay + acc) / full_travel, 0.0f, 1.0f);
          push_rod_birth(out, play_t_at_fade_scale(scale), next + k, extra, cap);
          ++k;
        }
      } else {
        int n = std::min(cap, std::max(0, static_cast<int>(std::floor(
            split_effect_rod_seeds_from_ps_world_path(rod, travel)))));
        for (int k = 0; k < n; ++k) {
          // live dump resid-iso-v2 (official 11581 fade, RoD∈{0.2,1,5},
          // point / World / startSpeed=0): travel-at-birth = (k+1)/RoD
          // from fade=0 origin. Same integer crossing as parent_motion.
          // (k+0.5) sat a half-grid early on every RoD. n = floor(RoD×
          // travel) unchanged. Not 2022.3.
          const float dist = travel_delay + (static_cast<float>(k) + 1.0f) / rod;
          const float scale = std::clamp(dist / full_travel, 0.0f, 1.0f);
          push_rod_birth(out, play_t_at_fade_scale(scale), next + k, extra, cap);
        }
      }
      next = static_cast<int>(out.size());
    }
    }
  }

  uint8_t burst_n = 0;
  const SplitEffectBurst* bursts = official_split_effect_bursts(p, &burst_n);
  if (bursts != nullptr && burst_n > 0) {
    for (uint8_t bi = 0; bi < burst_n; ++bi) {
      const SplitEffectBurst& b = bursts[bi];
      const int cycles = std::max(1, static_cast<int>(b.cycles));
      const float interval = b.interval > 1e-6f ? b.interval : 0.01f;
      const int count = std::max(0, static_cast<int>(std::lround(b.count)));
      for (int c = 0; c < cycles; ++c) {
        const float t0 = b.time + static_cast<float>(c) * interval;
        const int wraps = looping ? std::max(0, static_cast<int>(std::floor(now / length))) : 0;
        for (int w = 0; w <= wraps; ++w) {
          const float t = t0 + static_cast<float>(w) * length;
          if ((!looping && t > length + 1e-6f) || t > now_sim) {
            continue;
          }
          if (b.probability < 1.0f &&
              u01(mix3(static_cast<uint32_t>(color_id), static_cast<uint32_t>(bi + 1),
                       static_cast<uint32_t>(c + 11))) > b.probability) {
            continue;
          }
          for (int k = 0; k < count; ++k) {
            push_birth(out, delay + t / speed, next,
                       birth_sampling_extra(
                           p, (4u + bi) * 17u + static_cast<uint32_t>(c + w * 32)),
                       cap);
            ++next;
          }
        }
      }
    }
  } else if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::Burst) &&
             p.emission > 0.0f) {
    const int n = std::min(cap, std::max(1, static_cast<int>(std::lround(p.emission))));
    for (int k = 0; k < n; ++k) {
      push_birth(out, delay, next + k, birth_sampling_extra(p, 1u), cap);
    }
  }
}

bool collect_births_window_open(const SplitEffectParticle& p, float now_t) noexcept {
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = playback_delay(p);
  const float now_sim = (now_t - delay) * speed;
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  const float max_life = std::max(p.lifetime, p.lifetime_min);
  const float keep = particle_keep_sim_conservative(p, max_life);
  if (now_sim < 0.0f) {
    return false;
  }
  if (!looping && now_sim > length + keep) {
    return false;
  }
  return true;
}

bool collect_births_can_extend(const SplitEffectParticle& p) noexcept {
  if (p.go_active == 0 || p.rot_curve_count > 0 || p.rod_curve_count > 0) {
    return false;
  }
  // Concurrent maxParticles needs the full time-ordered candidate list.
  // Carry only keeps still-in-window births, so incremental k-extend can
  // accept extras the remake would drop. Non-default caps remake; default
  // 1000 still extends until evaluate_plan sees size > cap.
  if (p.max_particles > 0 && p.max_particles < 1000) {
    return false;
  }
  uint8_t burst_n = 0;
  const SplitEffectBurst* bursts = official_split_effect_bursts(p, &burst_n);
  if (bursts != nullptr && burst_n > 0) {
    return false;
  }
  if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::Burst) && p.emission > 0.0f) {
    return false;
  }
  const bool has_rot = p.rate_over_time > 1e-6f;
  float rod = p.rate_over_distance;
  if (rod <= 1e-6f &&
      p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)) {
    rod = p.emission;
  }
  const bool rod_two = p.rod_random != 0 && p.rate_over_distance_min > 1e-6f;
  const bool has_rod = rod > 1e-6f || rod_two;
  // Mixed RoT+RoD shares one next_birth index. Constant-RoD-only (10380 /
  // 10660 / 11711_1) is the play-extend case: after fade-in travel is frozen
  // so new k = 0; during fade-in n = floor(RoD × travel) grows monotonically.
  if (has_rot && has_rod) {
    return false;
  }
  if (rod_two) {
    return false;
  }
  if (has_rot) {
    return true;
  }
  return has_rod;
}

void collect_births_extend(const SplitEffectParticle& p, float now_t, int32_t color_id,
                           int first_index, std::vector<Birth>& out, const EmitContext* context) {
  (void)color_id;
  (void)context;
  if (!collect_births_can_extend(p)) {
    collect_births(p, now_t, color_id, out, context, false);
    return;
  }
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = playback_delay(p);
  const float now_sim = (now_t - delay) * speed;
  const float now = now_sim;
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  const float max_life = std::max(p.lifetime, p.lifetime_min);
  const float keep = particle_keep_sim_conservative(p, max_life);
  if (now < 0.0f || (!looping && now > length + keep)) {
    return;
  }
  const int cap = collect_list_cap();
  const int start = std::max(0, first_index);
  if (p.rate_over_time > 1e-6f) {
    const float rot = p.rate_over_time;
    const float span = looping ? now : std::min(now, length);
    const int n = std::min(cap, std::max(0, static_cast<int>(std::floor(span * rot))));
    for (int k = start; k < n; ++k) {
      push_birth(out, delay + ((static_cast<float>(k) + 1.0f) / rot) / speed, k,
                 birth_sampling_extra(p, 2u), cap);
    }
    return;
  }
  float rod = p.rate_over_distance;
  if (rod <= 1e-6f &&
      p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)) {
    rod = p.emission;
  }
  if (rod <= 1e-6f) {
    return;
  }
  const float now_wall = now_t;
  const float grow_t = std::min(now_wall, kSplitEffectFadeEndPlayT);
  const float delay_t = std::min(delay, grow_t);
  if (!(grow_t > delay_t + 1e-4f && fade_scale_at_play_t(grow_t) > 1e-4f)) {
    return;
  }
  const float travel_now = authored_rod_travel(p, color_id, fade_scale_at_play_t(grow_t));
  const float travel_delay = authored_rod_travel(p, color_id, fade_scale_at_play_t(delay_t));
  const float travel = std::max(0.0f, travel_now - travel_delay);
  const float full_travel = std::max(authored_rod_travel(p, color_id, 1.0f), 1e-4f);
  const uint32_t extra = birth_sampling_extra(p, 3u);
  const int n = std::min(cap, std::max(0, static_cast<int>(std::floor(
      split_effect_rod_seeds_from_ps_world_path(rod, travel)))));
  for (int k = start; k < n; ++k) {
    // live dump resid-iso-v2 hop, not 2022.3. See collect_births RoD.
    const float dist = travel_delay + (static_cast<float>(k) + 1.0f) / rod;
    const float scale = std::clamp(dist / full_travel, 0.0f, 1.0f);
    push_rod_birth(out, play_t_at_fade_scale(scale), k, extra, cap);
  }
}

void collect_event_bursts(const SplitEffectParticle& p, float event_t, int32_t color_id,
                          int start_index, std::vector<Birth>& out) {
  SplitWallScope wall_births(SplitWallSec::Births);
  const int cap = collect_list_cap();
  int next = start_index;
  uint8_t burst_n = 0;
  const SplitEffectBurst* bursts = official_split_effect_bursts(p, &burst_n);
  if (bursts != nullptr && burst_n > 0) {
    for (uint8_t bi = 0; bi < burst_n; ++bi) {
      const SplitEffectBurst& b = bursts[bi];
      const int count = std::max(0, static_cast<int>(std::lround(b.count)));
      if (b.probability < 1.0f &&
          u01(mix3(static_cast<uint32_t>(color_id), static_cast<uint32_t>(bi + 1),
                   701u)) > b.probability) {
        continue;
      }
      for (int k = 0; k < count; ++k) {
        push_birth(out, event_t, next,
                   birth_sampling_extra(p, (4u + bi) * 17u + 700u + static_cast<uint32_t>(k)),
                   cap);
        ++next;
      }
    }
    return;
  }
  if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::Burst) && p.emission > 0.0f) {
    const int n = std::min(cap, std::max(1, static_cast<int>(std::lround(p.emission))));
    for (int k = 0; k < n; ++k) {
      push_birth(out, event_t, next + k, birth_sampling_extra(p, 701u), cap);
    }
  }
}

}  // namespace wds::chart_render::shuriken

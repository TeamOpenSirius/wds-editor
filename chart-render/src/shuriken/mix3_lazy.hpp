#pragma once

// Wave 3.5c: skip mix3 when the result is unread. hop / perf, not 2022.3.
// WDS_MIX3_LAZY=0 restores the eager path. Per-item WDS_MIX3_LAZY_{A,B,C,D,E}=0
// turns that item off. WDS_MIX3_LAZY_VERIFY=1 runs eager+lazy and counts
// mismatches on fields that readers actually consume.

#include "internal.hpp"

#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/common/perf_stages.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

namespace wds::chart_render::shuriken {

inline bool mix3_env_on_default(const char* name, bool default_on) noexcept {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_on;
  }
  const std::string_view v(raw);
  if (v == "0" || v == "off" || v == "false" || v == "OFF") {
    return false;
  }
  return true;
}

inline bool mix3_lazy_master() noexcept {
  static const bool on = mix3_env_on_default("WDS_MIX3_LAZY", true);
  return on;
}

inline bool mix3_lazy_a() noexcept {
  static const bool on = mix3_lazy_master() && mix3_env_on_default("WDS_MIX3_LAZY_A", true);
  return on;
}

inline bool mix3_lazy_b() noexcept {
  static const bool on = mix3_lazy_master() && mix3_env_on_default("WDS_MIX3_LAZY_B", true);
  return on;
}

inline bool mix3_lazy_c() noexcept {
  static const bool on = mix3_lazy_master() && mix3_env_on_default("WDS_MIX3_LAZY_C", true);
  return on;
}

inline bool mix3_lazy_d() noexcept {
  static const bool on = mix3_lazy_master() && mix3_env_on_default("WDS_MIX3_LAZY_D", true);
  return on;
}

inline bool mix3_lazy_e() noexcept {
  static const bool on = mix3_lazy_master() && mix3_env_on_default("WDS_MIX3_LAZY_E", true);
  return on;
}

inline bool mix3_lazy_verify() noexcept {
  static const bool on = mix3_env_on_default("WDS_MIX3_LAZY_VERIFY", false);
  return on;
}

inline std::atomic<uint64_t> g_mix3_verify_fail{0};
inline std::atomic<uint64_t> g_mix3_verify_ok{0};

inline void mix3_verify_note(bool ok, const char* where) noexcept {
  static std::once_flag once;
  std::call_once(once, [] {
    std::atexit([] {
      std::fprintf(stderr, "WDS_MIX3_LAZY_VERIFY fail=%llu ok=%llu\n",
                   static_cast<unsigned long long>(g_mix3_verify_fail.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_mix3_verify_ok.load(std::memory_order_relaxed)));
    });
  });
  if (ok) {
    g_mix3_verify_ok.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint64_t n = g_mix3_verify_fail.fetch_add(1, std::memory_order_relaxed) + 1;
  WDS_PERF_COUNT("mix3.verify_fail", 1);
  if (n <= 32) {
    std::fprintf(stderr, "WDS_MIX3_LAZY_VERIFY mismatch at %s (n=%llu)\n", where,
                 static_cast<unsigned long long>(n));
  }
}

inline uint64_t mix3_verify_fail_count() noexcept {
  return g_mix3_verify_fail.load(std::memory_order_relaxed);
}

// Shared with integrate.cpp / modules.cpp / catalog readers. Skip mix3 iff
// every reader of the field ignores the random u (eval_minmax_axis random==0
// returns hi; module flag off never loads the field).

inline bool particle_reads_vol_u(const SplitEffectParticle& p) noexcept {
  if ((p.flags & kSplitEffectFlagVelocity) == 0) {
    return false;
  }
  return p.vel_x_random != 0 || p.vel_y_random != 0 || p.vel_z_random != 0;
}

inline bool particle_reads_force_u(const SplitEffectParticle& p) noexcept {
  if (p.force_enabled != 0 && p.force_randomize == 0 &&
      (p.force_x_random != 0 || p.force_y_random != 0 || p.force_z_random != 0)) {
    return true;
  }
  if (p.clamp_enabled != 0 && p.clamp_drag_random != 0) {
    return true;
  }
  if (p.clamp_enabled != 0 && p.clamp_separate == 0 && p.clamp_speed_random != 0) {
    return true;
  }
  return false;
}

inline bool particle_reads_noise_u(const SplitEffectParticle& p) noexcept {
  if ((p.flags & kSplitEffectFlagNoise) == 0) {
    return false;
  }
  if (p.noise_strength_random != 0) {
    return true;
  }
  if (p.noise_separate_axes != 0 &&
      (p.noise_strength_y_random != 0 || p.noise_strength_z_random != 0)) {
    return true;
  }
  return false;
}

inline bool particle_reads_orbital_u(const SplitEffectParticle& p) noexcept {
  return p.orbital_x_random != 0 || p.orbital_y_random != 0 || p.orbital_z_random != 0;
}

inline bool particle_reads_col_u(const SplitEffectParticle& p) noexcept {
  // fill_trails copies st.col_u into live trail_color_u *before* the Trails
  // flag early-out, and collect sets want_trail=true. Golden hashes that
  // field on every particle (1010/1020 trail_scalars). Always sample.
  (void)p;
  return true;
}

inline bool particle_reads_start_color_u(const SplitEffectParticle& p) noexcept {
  // Gradient (1) / TwoGradients (3) sample time at emit_u, not start_color_u.
  if (p.start_color_state == 1 || p.start_color_state == 3) {
    return false;
  }
  if (p.start_color_state == 2) {
    return false;
  }
  if (p.start_color_state == 4) {
    return true;
  }
  return p.color_random != 0;
}

inline bool particle_reads_start_u_hash(const SplitEffectParticle& p) noexcept {
  return p.uv_frame_count > 0 && p.start_frame_random != 0;
}

inline bool particle_reads_fot_u_hash(const SplitEffectParticle& p) noexcept {
  if (p.uv_frame_count == 0) {
    return false;
  }
  if (p.fot_min_count > 0) {
    return true;
  }
  if (p.fot_count > 0) {
    return false;
  }
  return p.fot_random != 0;
}

inline bool particle_reads_trail_life_hash(const SplitEffectParticle& p) noexcept {
  return (p.flags & kSplitEffectFlagTrails) != 0 && p.trail_lifetime_random != 0;
}

inline bool particle_reads_start_color_lerp16(const SplitEffectParticle& p) noexcept {
  return p.start_color_state == 2 || p.start_color_state == 3;
}

inline bool particle_reads_rol_u(const SplitEffectParticle& p) noexcept {
  if (p.rot_ol_enabled == 0) {
    return false;
  }
  if (p.rot_ol_separate == 0) {
    return p.rot_ol_random != 0;
  }
  return p.rot_ol_random != 0 || p.rot_ol_x_random != 0 || p.rot_ol_y_random != 0;
}

inline bool particle_reads_sol_u21(const SplitEffectParticle& p) noexcept {
  if ((p.flags & kSplitEffectFlagSizeOverLifetime) == 0) {
    return false;
  }
  const bool sol_separate = p.sol_y_count > 0 || p.sol_z_count > 0 ||
                            (p.sol_y > 1e-4f && std::fabs(p.sol_y - 1.0f) > 1e-4f);
  const bool sol_two_curves =
      p.sol_min_count > 0 || p.sol_y_min_count > 0 || p.sol_z_min_count > 0;
  if (sol_separate && sol_two_curves) {
    return false;
  }
  return p.sol_min_count > 0;
}

inline float pick_maybe_hash(float max_v, float min_v, bool random, uint32_t seed, uint32_t salt,
                             uint32_t extra, bool skip_unused) noexcept {
  if (skip_unused && !random) {
    return max_v;
  }
  WDS_MIX3_COUNT("mix3.ms.pick", 1);
  return pick(max_v, min_v, random, mix3(seed, salt, extra));
}

inline bool bits_eq(float a, float b) noexcept {
  uint32_t ua = 0;
  uint32_t ub = 0;
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua == ub;
}

inline void mix3_verify_state(const ParticleState& lazy, const ParticleState& eager,
                              const SplitEffectParticle& p) noexcept {
  bool ok = true;
  auto chk = [&](float a, float b, bool need) {
    if (need && !bits_eq(a, b)) {
      ok = false;
    }
  };
  chk(lazy.life, eager.life, true);
  chk(lazy.size, eager.size, true);
  chk(lazy.size_y, eager.size_y, true);
  chk(lazy.size_z, eager.size_z, true);
  chk(lazy.speed, eager.speed, true);
  chk(lazy.spawn_x, eager.spawn_x, true);
  chk(lazy.spawn_y, eager.spawn_y, true);
  chk(lazy.spawn_z, eager.spawn_z, true);
  chk(lazy.dir_x, eager.dir_x, true);
  chk(lazy.dir_y, eager.dir_y, true);
  chk(lazy.dir_z, eager.dir_z, true);
  chk(lazy.rot_z, eager.rot_z, true);
  chk(lazy.rot_x, eager.rot_x, p.rotation_3d != 0);
  chk(lazy.rot_y, eager.rot_y, p.rotation_3d != 0);
  chk(lazy.inherit_k, eager.inherit_k, p.inherit_enabled != 0);
  chk(lazy.vol_ux, eager.vol_ux, particle_reads_vol_u(p));
  chk(lazy.vol_uy, eager.vol_uy, particle_reads_vol_u(p));
  chk(lazy.vol_uz, eager.vol_uz, particle_reads_vol_u(p));
  chk(lazy.force_ux, eager.force_ux, particle_reads_force_u(p));
  chk(lazy.force_uy, eager.force_uy, particle_reads_force_u(p));
  chk(lazy.force_uz, eager.force_uz, particle_reads_force_u(p));
  chk(lazy.noise_ux, eager.noise_ux, particle_reads_noise_u(p));
  chk(lazy.noise_uy, eager.noise_uy, particle_reads_noise_u(p));
  chk(lazy.noise_uz, eager.noise_uz, particle_reads_noise_u(p));
  chk(lazy.orb_x, eager.orb_x, true);
  chk(lazy.orb_y, eager.orb_y, true);
  chk(lazy.orb_z, eager.orb_z, true);
  chk(lazy.col_u, eager.col_u, particle_reads_col_u(p));
  chk(lazy.start_color_u, eager.start_color_u, particle_reads_start_color_u(p));
  chk(lazy.start_u, eager.start_u, p.uv_frame_count > 0);
  chk(lazy.fot_u, eager.fot_u, p.uv_frame_count > 0);
  chk(lazy.trail_life_mul, eager.trail_life_mul, (p.flags & kSplitEffectFlagTrails) != 0);
  mix3_verify_note(ok, "make_state");
}

inline uint32_t mix3_cached_auto_inner(uint32_t extra, uint32_t ps_i, uint32_t gen) noexcept {
  if (!mix3_lazy_c()) {
    return mix3(extra, ps_i, gen);
  }
  struct Cache {
    uint32_t extra = 0;
    uint32_t ps_i = 0;
    uint32_t gen = 0;
    uint32_t mix = 0;
    uint8_t valid = 0;
  };
  static thread_local Cache c{};
  if (c.valid != 0 && c.extra == extra && c.ps_i == ps_i && c.gen == gen) {
    return c.mix;
  }
  c.extra = extra;
  c.ps_i = ps_i;
  c.gen = gen;
  c.mix = mix3(extra, ps_i, gen);
  c.valid = 1;
  return c.mix;
}

}  // namespace wds::chart_render::shuriken

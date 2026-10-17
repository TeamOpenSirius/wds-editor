#pragma once

#include <wds/chart_render/shuriken/curve.hpp>
#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/shuriken/evaluate.hpp>
#include <wds/chart_render/split_effect_particles.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wds::chart_render::shuriken {

struct EmitContext;

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDegToRad = kPi / 180.0f;

struct Birth {
  float time = 0.0f;
  int index = 0;
  uint32_t extra = 0;
  float parent_z = 0.0f;
  uint8_t parent_pose = 0;
  // Play time for emit-time curves (startSize, startColor, ...). < 0 uses
  // `time`; RateOverDistance births set the frame end (push_rod_birth).
  // Curve RoT does not set this (refix-b isolate startSize/lifetime constant).
  float curve_t = -1.0f;
};

inline float birth_curve_t(const Birth& b) noexcept {
  return b.curve_t >= 0.0f ? b.curve_t : b.time;
}

struct ParticleState {
  int32_t color_id = 0;
  int32_t line_index = 0;
  // Main X (SetLineX). integrate_pose writes world_x = line_x + lx.
  float line_x = 0.0f;
  float fade_scale = 1.0f;
  float birth_fade = 1.0f;
  float line_alpha = 1.0f;
  const SplitEffectParticle* p = nullptr;
  uint32_t seed = 0;
  float life = 1.0f;
  float size = 1.0f;
  float size_y = 1.0f;
  float size_z = 1.0f;
  float speed = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  float orb_x = 0.0f;
  float orb_y = 0.0f;
  float orb_z = 0.0f;
  float orb_ox = 0.0f;
  float orb_oy = 0.0f;
  float orb_oz = 0.0f;
  float radial = 0.0f;
  float spawn_x = 0.0f;
  float spawn_y = 0.0f;
  float spawn_z = 0.0f;
  float dir_x = 0.0f;
  float dir_y = 1.0f;
  float dir_z = 0.0f;
  float start_u = 0.0f;
  float fot_u = 0.0f;
  float col_u = 0.0f;
  float start_color_u = 0.0f;
  float rot_z = 0.0f;
  float rot_x = 0.0f;
  float rot_y = 0.0f;
  float birth_t = 0.0f;
  int32_t birth_index = 0;
  float birth_parent_z = 0.0f;
  float birth_origin_y_delta = 0.0f;
  float inherit_k = 0.0f;
  // live dump v1: Inherit emitter_v when set. Else integrate uses line-fade Y.
  float emitter_vx = 0.0f;
  float emitter_vy = 0.0f;
  float emitter_vz = 0.0f;
  uint8_t emitter_vel_set = 0;
  float force_ux = 0.0f;
  float force_uy = 0.0f;
  float force_uz = 0.0f;
  float vol_ux = 0.0f;
  float vol_uy = 0.0f;
  float vol_uz = 0.0f;
  // Unity 2022.3 MinMaxCurve.Evaluate time for Main curves (startSize).
  // Sub-emitters: InheritDuration uses the parent system's age/duration;
  // otherwise the child system's own age/duration.
  float emit_u = 0.0f;
  // Unity 2022.3 Sub Emitters: event Emit at the parent particle pose.
  uint8_t event_emit = 0;
  uint8_t inherit_props = 0;
  float event_wx = 0.0f;
  float event_wy = 0.0f;
  float event_wz = 0.0f;
  float inherit_r = 1.0f;
  float inherit_g = 1.0f;
  float inherit_b = 1.0f;
  float inherit_a = 1.0f;
  // Unity 2022.3 MinMaxCurve lerpFactor for Noise strength (TwoConstants /
  // TwoCurves). Stream from kSaltNoiseStrength; independent of VOL salt 4.
  float noise_ux = 0.0f;
  float noise_uy = 0.0f;
  float noise_uz = 0.0f;
  // Trails.lifetime TwoConstants pick. < 0 → use catalog trail_lifetime max.
  float trail_life_mul = -1.0f;
};

// Observation-only snapshot of the velocity terms already computed by
// integrate_pose. All vectors are in the native PS-local/LaneGroup basis.
// stored is Particle.velocity after Force/Gravity/Limit/Drag; vol and noise
// are the current-frame animated terms; inherited is Current-mode inherit
// (Initial is already in stored); total is the sum consumed by the position
// step. Orbital/radial post-step transforms are deliberately outside this
// snapshot.
struct IntegrationVelocityTrace {
  float stored_x = 0.0f;
  float stored_y = 0.0f;
  float stored_z = 0.0f;
  float vol_x = 0.0f;
  float vol_y = 0.0f;
  float vol_z = 0.0f;
  float noise_x = 0.0f;
  float noise_y = 0.0f;
  float noise_z = 0.0f;
  // Curl × positionAmount, before Strength. Hold keeps this; Strength is a
  // lifetime curve and is re-applied each step (2022.3 Noise Manual).
  float noise_unit_x = 0.0f;
  float noise_unit_y = 0.0f;
  float noise_unit_z = 0.0f;
  float inherited_x = 0.0f;
  float inherited_y = 0.0f;
  float inherited_z = 0.0f;
  float total_x = 0.0f;
  float total_y = 0.0f;
  float total_z = 0.0f;
};

// Observation-only per-substep record.  This exposes the substeps that the
// native evaluator actually executes without changing the integration
// equations or their ordering.  Production callers leave the observer null.
struct IntegrationStepTrace {
  int step_index = 0;
  int step_count = 0;
  float step_start = 0.0f;
  float step_end = 0.0f;
  float dt = 0.0f;
  IntegrationVelocityTrace velocity{};
};

using IntegrationObserver = void (*)(const ParticleState&, float age,
                                      const IntegrationVelocityTrace&) noexcept;

using IntegrationStepObserver = void (*)(const ParticleState&, float age,
                                          const IntegrationStepTrace&) noexcept;

// Null by default. This is used only by standalone observation probes and is
// intentionally not consulted by production callers unless installed.
void set_integration_observer(IntegrationObserver observer) noexcept;

// live dump v1 observation (write-intents/physics.md). Last integrate_pose
// stored / VOL / Noise / total in LaneGroup. fill_live copies these onto
// SplitEffectLiveParticle; dump reads live. Not a 2022.3 equation.
const IntegrationVelocityTrace& last_integration_velocity() noexcept;
void set_last_integration_velocity(const IntegrationVelocityTrace& t) noexcept;

// Null by default; intended only for timestep diagnostics.
void set_integration_step_observer(IntegrationStepObserver observer) noexcept;

bool integration_observers_installed() noexcept;

// Same 32-step xorshift as evaluate.cpp. Inlined so shape / emit / make_state
// do not pay a cross-TU call per mix (sample: mix3 11–17% of collect).
inline uint32_t mix3(uint32_t a, uint32_t b, uint32_t c) noexcept {
  return xorshift128_r32_mix3(a, b, c);
}

// Stable logical emitter identity used by temporary random-stream candidates.
// This is the same line seed already used by the !autoRandomSeed path.
uint32_t stable_emitter_identity(const EmitContext& ctx) noexcept;

inline float u01(uint32_t h) noexcept {
  // UnityEngine.Random.value: inclusive [0, 1].
  return static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

inline float pick(float max_v, float min_v, bool random, uint32_t h) noexcept {
  if (!random) {
    return max_v;
  }
  return min_v + (max_v - min_v) * u01(h);
}

// mix3(seed, extra, salt) with nearby salts is one xorshift family, not
// independent uniforms. Cube-reject Sphere using 20/21/22 collapsed to the
// +++ / --- octants (live dump v19 10220 1_1 native xz half-plane empty).
// Advance a stream: each draw rehashes the previous output. hop, not 2022.3.
inline uint32_t rng_or1(uint32_t x) noexcept { return x != 0 ? x : 1u; }

inline uint32_t rng_start(uint32_t seed, uint32_t extra, uint32_t salt) noexcept {
  return mix3(rng_or1(seed), rng_or1(extra), rng_or1(salt));
}

inline uint32_t rng_next(uint32_t& rng, uint32_t seed, uint32_t extra) noexcept {
  rng = mix3(rng, rng_or1(seed), rng_or1(extra));
  return rng;
}

inline float rng_next_u01(uint32_t& rng, uint32_t seed, uint32_t extra) noexcept {
  return u01(rng_next(rng, seed, extra));
}

// New MinMaxCurve birth randoms. Far from existing mix3 salts (1–23, 40–43,
// 70, 72, 91–93, 101) so they are not the same xorshift family.
inline constexpr uint32_t kSaltStartSpeedTwoCurves = 50u;
inline constexpr uint32_t kSaltFotTwoCurves = 60u;
inline constexpr uint32_t kSaltNoiseStrength = 110u;
inline constexpr uint32_t kSaltOrbital = 130u;
inline constexpr uint32_t kSaltTrailLifetime = 150u;
inline constexpr uint32_t kSaltCustom1 = 170u;
inline constexpr uint32_t kSaltSolAxes = 180u;

inline void rng_fill_u01_3(uint32_t seed, uint32_t extra, uint32_t salt0, float& ux, float& uy,
                           float& uz) noexcept {
  uint32_t rng = rng_start(seed, extra, salt0);
  ux = rng_next_u01(rng, seed, extra);
  uy = rng_next_u01(rng, seed, extra);
  uz = rng_next_u01(rng, seed, extra);
}

// live dump 11423-birth-v1 hop (not 2022.3). OpenWDS Main.startDelay is
// consumed on ParticleSystem.time: time = now_t * simulationSpeed − delay.
// Wall wait is therefore delay / speed. 2022.3 simulationSpeed is "the
// entire system"; startDelay is "seconds" without this algebra.
inline float start_delay_wall_clock(const SplitEffectParticle& p) noexcept {
  const float delay = std::max(official_split_effect_start_delay(p), 0.0f);
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  return delay / speed;
}

// No new births after Duration. Looping never closes. Keep-window aging of
// already-born particles is a separate can_emit check (length + life).
inline bool primary_emission_closed(const SplitEffectParticle& p, float now_t) noexcept {
  if ((p.flags & kSplitEffectFlagLooping) != 0) {
    return false;
  }
  const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  const float delay = start_delay_wall_clock(p);
  const float now_sim = (now_t - delay) * speed;
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  return now_sim > length;
}

// Applied birth fade shared by primary and sub-emitter paths.  The optional
// temporary replay clock only changes this diagnostic candidate; production
// callers leave it disabled and retain the authored birth-time clock.
float applied_birth_fade(const EmitContext& ctx, float birth_t) noexcept;
float invert_fade_in_scale(float scale) noexcept;
// ParticleSystem play time at which LineHight reaches `scale`.
inline float play_t_at_fade_scale(float scale) noexcept {
  return invert_fade_in_scale(scale) + kSplitEffectAnimatorLagSec;
}
float emit_local_y(int32_t color_id) noexcept;
float emitter_main_y(int32_t color_id, float fade_scale) noexcept;
float emitter_travel(int32_t color_id, float fade_scale) noexcept;
// Unity Transform chain used by local_y_to_main_y. origin + signed_lever*fade
// is main_y; signed_lever is d(main_y)/d(fade). OpenWDS lineHight.localPosition
// / localRotation; live dump refix-d hop for the derivative, not 2022.3.
void emitter_main_pose(int32_t color_id, float local_y, float& origin,
                       float& signed_lever) noexcept;
// d(main_y)/d(ly) in apply_epilogue (main_y = origin + slope*ly). Same sign as
// signed_lever when local_y>0.
inline float emitter_main_y_from_ly_slope(int32_t color_id) noexcept {
  float origin = 0.0f;
  float lever = 0.0f;
  emitter_main_pose(color_id, 1.0f, origin, lever);
  return lever < 0.0f ? -1.0f : 1.0f;
}

void collect_births(const SplitEffectParticle& p, float now_t, int32_t color_id,
                    std::vector<Birth>& out, const EmitContext* context = nullptr,
                    bool include_expired = false);
bool collect_births_can_extend(const SplitEffectParticle& p) noexcept;
bool collect_births_window_open(const SplitEffectParticle& p, float now_t) noexcept;
void collect_births_extend(const SplitEffectParticle& p, float now_t, int32_t color_id,
                           int first_index, std::vector<Birth>& out,
                           const EmitContext* context = nullptr);

// Unity 2022.3 Trails.lifetime is a multiplier of startLifetime (Manual
// PartSysTrailsModule + TrailModule.lifetime). Conservative hi for windows
// that run before per-particle pick (dead-skip / collect). Curve mode uses
// the same 5-sample peak as axis_peak_abs (live dump dump-pending-v2 hop,
// not 2022.3).
inline float trail_lifetime_mul_hi(const SplitEffectParticle& p) noexcept {
  float m = std::max(p.trail_lifetime, 0.0f);
  if (p.trail_lifetime_random != 0) {
    m = std::max(m, p.trail_lifetime_min);
  }
  static constexpr float kT[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
  for (float t : kT) {
    if (p.trail_lifetime_count > 0) {
      m = std::max(m, std::fabs(official_split_effect_eval_curve_keys(
                          p.trail_lifetime_begin, p.trail_lifetime_count, t, p.trail_lifetime)));
    }
    if (p.trail_lifetime_min_count > 0) {
      m = std::max(m, std::fabs(official_split_effect_eval_curve_keys(
                          p.trail_lifetime_min_begin, p.trail_lifetime_min_count, t,
                          p.trail_lifetime_min)));
    }
  }
  return m;
}

// Unity 2022.3 TrailModule.dieWithParticles=false: each trail persists until
// its points expire (t_rec + Lifetime×startLifetime). Ribbon has no per-trail
// points independent of particles, so keep=life+L×life is Mode=Particle (0)
// only. Ribbon dieWith=false drops the particle at death (leaves the chain).
// dieWith=true stays `life`.
// https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysTrailsModule.html
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.TrailModule-dieWithParticles.html
inline float particle_keep_sim(const SplitEffectParticle& p, float life,
                               float trail_mul) noexcept {
  life = std::max(life, 1e-4f);
  if ((p.flags & kSplitEffectFlagTrails) != 0 && p.trail_die_with == 0 &&
      p.trail_mode == 0) {
    return life + std::max(trail_mul, 0.0f) * life;
  }
  return life;
}

inline float particle_keep_sim_conservative(const SplitEffectParticle& p,
                                            float max_life) noexcept {
  return particle_keep_sim(p, max_life, trail_lifetime_mul_hi(p));
}

// Unity 2022.3 Sub Emitters: Death/Collision/Trigger/Manual (and Birth extra
// Emit) use only the child's burst settings, at the parent event time.
void collect_event_bursts(const SplitEffectParticle& p, float event_t, int32_t color_id,
                          int start_index, std::vector<Birth>& out);

void sample_shape(const SplitEffectParticle& p, uint32_t seed, uint32_t extra, float birth_t,
                  int birth_index, float& px, float& py, float& pz, float& dx, float& dy,
                  float& dz) noexcept;

float integration_candidate_hz() noexcept;
float integration_candidate_age(float age) noexcept;
bool split_integrator_is_legacy() noexcept;

struct ParticleCacheKey;

// Pose / velocity at a full grid step k (never a tail). sample() advances
// to k then integrates the exact remaining tail to now_t.
//
// IntegratorHot is the only R+W state of run_step / advance_to. It lives in
// a contiguous array (persist SoA or a 128 B-aligned frame-arena batch).
// 32 B divides 64 so a record never straddles a 64 B line. trace is cold.
struct alignas(32) IntegratorHot {
  float lx = 0.0f;
  float ly = 0.0f;
  float lz = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  int next_step = 0;
  std::uint32_t pad = 0;
};
static_assert(sizeof(IntegratorHot) == 32, "hot pose is 32 B");
static_assert(64 % sizeof(IntegratorHot) == 0, "hot size divides 64");
inline constexpr int kIntegratorHotPerLine = 128 / static_cast<int>(sizeof(IntegratorHot));

inline int align_hot_index(int i) noexcept {
  const int s = kIntegratorHotPerLine;
  return (i / s) * s;
}

inline int align_hot_count(int n) noexcept {
  const int s = kIntegratorHotPerLine;
  return n <= 0 ? 0 : ((n + s - 1) / s) * s;
}

struct IntegratorGridState {
  float lx = 0.0f;
  float ly = 0.0f;
  float lz = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  int next_step = 0;
  std::uint32_t hot_pad = 0;
  IntegrationVelocityTrace trace{};
};
static_assert(offsetof(IntegratorGridState, trace) == 32, "hot pose is 32 B");
static_assert(alignof(IntegratorGridState) >= 4, "");

// Both types are layout-compatible 32 B poses (see the static_assert above). The void* casts state that this is
// a raw byte copy, which keeps GCC's -Wclass-memaccess quiet.
inline void copy_grid_hot(const IntegratorGridState& g, IntegratorHot& h) noexcept {
  std::memcpy(static_cast<void*>(&h), static_cast<const void*>(&g), sizeof(IntegratorHot));
}
inline void copy_hot_grid(const IntegratorHot& h, IntegratorGridState& g) noexcept {
  std::memcpy(static_cast<void*>(&g), static_cast<const void*>(&h), sizeof(IntegratorHot));
}

class PoseIntegrator {
 public:
  explicit PoseIntegrator(const ParticleState& st, IntegratorHot* hot = nullptr) noexcept;
  // After ctor / import, point run_step at an element of a contiguous hot
  // array. Copies the current pose into dest. dest must stay 32 B aligned.
  void bind_hot(IntegratorHot* dest) noexcept;
  IntegratorHot& hot() noexcept { return *hot_; }
  const IntegratorHot& hot() const noexcept { return *hot_; }
  ~PoseIntegrator();
  PoseIntegrator(const PoseIntegrator&) = delete;
  PoseIntegrator& operator=(const PoseIntegrator&) = delete;
  void advance_to(int k) noexcept;
  void sample(float age, float& world_x, float& main_y, float& world_z,
              IntegrationVelocityTrace* trace = nullptr) noexcept;
  void finite_difference(float age, float dt, float x0, float y0, float z0, float& vx, float& vy,
                         float& vz) noexcept;
  void set_cache_key(const ParticleCacheKey* key) noexcept { cache_key_ = key; }
  const ParticleCacheKey* cache_key() const noexcept { return cache_key_; }
  void export_grid(IntegratorGridState& out) const noexcept;
  void import_grid(const IntegratorGridState& in) noexcept;
  // Current-frame world pose of the birth grid (no rewind, no extra step).
  void birth_world(float& world_x, float& main_y, float& world_z) const noexcept;
  // Carry snapshot already holds this pose. Mark stored so play does not
  // rewrite the pose cache every frame. Pose-cache hydrate must NOT use this
  // (it would thin checkpoints and regress cold / carry-off).
  void import_carry_grid(const IntegratorGridState& in) noexcept;
  int grid_next_step() const noexcept { return hot_->next_step; }
  // hop (not 2022.3): record each full-grid state during the live walk so
  // PerParticle bent trails can emit vertices without a second 0→age pass.
  // Separate from the pose cache (do not sample(0) on the live integrator).
  void begin_grid_log() noexcept;
  const IntegratorGridState* grid_log_data() const noexcept {
    return grid_log_ != nullptr && !grid_log_->empty() ? grid_log_->data() : nullptr;
  }
  int grid_log_size() const noexcept {
    return grid_log_ != nullptr ? static_cast<int>(grid_log_->size()) : 0;
  }
  int grid_log_origin_step() const noexcept { return grid_log_origin_; }

 private:
  void reset_to_birth() noexcept;
  void run_step(int i, float t_mid, float dt, float t_end_for_noise, int step_count,
                float step_start, float step_end, const float* noise_override = nullptr) noexcept;
  template <uint32_t Mask>
  void run_step_masked(int i, float t_mid, float dt, float t_end_for_noise, int step_count,
                       float step_start, float step_end, const float* noise_override) noexcept;
  void apply_epilogue(float lx, float ly, float lz, float& world_x, float& main_y,
                      float& world_z) const noexcept;
  void sample_legacy(float age, float& world_x, float& main_y, float& world_z,
                     IntegrationVelocityTrace* trace) noexcept;
  void inherit_emitter_v(float play_t, float& evx, float& evy, float& evz) const noexcept;
  void seed_trace_from_stored() noexcept;
  void publish_world_velocity(IntegrationVelocityTrace& dst) const noexcept;
  void publish_sample(float age, IntegrationVelocityTrace* trace) noexcept;
  void maybe_restore(int k) noexcept;
  void maybe_store() noexcept;
  void flush_pending_stores() noexcept;
  bool can_closed_form() const noexcept;
  void apply_closed_form(int i0, int i1) noexcept;

  const ParticleState* st_ = nullptr;
  const ParticleCacheKey* cache_key_ = nullptr;
  static constexpr int kPendingStores = 8;
  // Uninitialized: maybe_store writes via export_grid before flush reads.
  alignas(IntegratorGridState) unsigned char pending_raw_[sizeof(IntegratorGridState) *
                                                          kPendingStores];
  int pending_store_n_ = 0;
  // Persist SoA (hots vector) is a separate array from this embed.
  // Do not alignas(128) this stack embed: PoseIntegrator is built per
  // particle per frame and 128-align forces caller stack realignment.
  IntegratorHot embedded_{};
  IntegratorHot* hot_ = &embedded_;
  float birth_lx_ = 0.0f;
  float birth_ly_ = 0.0f;
  float birth_lz_ = 0.0f;
  float birth_vx_ = 0.0f;
  float birth_vy_ = 0.0f;
  float birth_vz_ = 0.0f;
  int next_step_ = 0;
  int last_stored_step_ = -1;
  IntegrationVelocityTrace trace_{};
  float grav_y_ = 0.0f;
  float grav_z_ = 0.0f;
  float hz_ = 60.0f;
  float h_ = 1.0f / 60.0f;
  float birth_x_scale_ = 1.0f;
  float birth_y_scale_ = 1.0f;
  float live_y_scale_ = 1.0f;
  float observe_age_ = 0.0f;
  bool local_sim_ = true;
  bool legacy_ = false;
  bool force_after_limit_ = false;
  bool noise_before_limit_ = false;
  bool vol_before_limit_ = false;
  bool vol_before_force_ = false;
  int noise_hold_ = 1;
  bool noise_hold_spread_ = true;
  uint8_t has_vol_ = 0;
  uint8_t has_force_ = 0;
  uint8_t has_noise_ = 0;
  uint8_t has_inherit_cur_ = 0;
  uint8_t has_clamp_ = 0;
  uint8_t vol_world_ = 0;
  uint8_t force_world_ = 0;
  uint8_t authored_local_native_ = 1;
  uint8_t step_mask_ = 15;
  uint8_t noise_position_ = 0;
  uint8_t noise_phase_ = 0;
  uint8_t animated_feedback_ = 0;
  uint8_t curve_hint_[12]{};
  std::vector<IntegratorGridState>* grid_log_ = nullptr;
  int grid_log_origin_ = 0;
};

// Stable per-particle identity for the incremental integrator cache.
// Uniqueness: within one (color_id, line_index, line_start_ms, catalog_index,
// line_play_gen, epoch), collect_births / subemit remapping assign unique
// (birth_index, birth_extra). Overlapping instances differ by start_ms (or
// are bit-identical at stagger 0). input_hash covers integration inputs so a
// bind/chart change misses instead of reusing a stale pose.
struct ParticleCacheKey {
  uint32_t epoch = 0;
  int32_t color_id = 0;
  int32_t line_index = 0;
  int64_t line_start_ms = 0;
  uint32_t catalog_index = 0;
  int32_t birth_index = 0;
  uint32_t birth_extra = 0;
  uint32_t line_play_gen = 0;
  uint32_t seed = 0;
  uint64_t input_hash = 0;
  uint8_t valid = 0;
};

inline bool operator==(const ParticleCacheKey& a, const ParticleCacheKey& b) noexcept {
  return a.epoch == b.epoch && a.color_id == b.color_id && a.line_index == b.line_index &&
         a.line_start_ms == b.line_start_ms && a.catalog_index == b.catalog_index &&
         a.birth_index == b.birth_index && a.birth_extra == b.birth_extra &&
         a.line_play_gen == b.line_play_gen && a.seed == b.seed && a.input_hash == b.input_hash &&
         a.valid == b.valid;
}

void integrate_pose(const ParticleState& st, float age, float& world_x, float& main_y,
                    float& world_z) noexcept;

void apply_noise(const ParticleState& st, float age, float& lx, float& ly, float& lz) noexcept;

void eval_noise_velocity(const ParticleState& st, float age, float lx, float ly, float lz,
                         float& vx, float& vy, float& vz, float* unit_x = nullptr,
                         float* unit_y = nullptr, float* unit_z = nullptr) noexcept;
// hop (not 2022.3): analytic curl is inline in modules_an.inl.hpp with
// scoped contract-off (Clang fp pragma / GCC optimize / MSVC precise).
void eval_noise_velocity_an(const ParticleState& st, float age, float lx, float ly, float lz,
                            float& vx, float& vy, float& vz, float* unit_x = nullptr,
                            float* unit_y = nullptr, float* unit_z = nullptr) noexcept;

// Unity 2022.3 Noise Strength MinMaxCurve at normalized particle age t01.
void eval_noise_strength_axes(const ParticleState& st, float t01, float& sx, float& sy,
                              float& sz) noexcept;

inline void finish_noise_vel(float ux, float uy, float uz, float sx, float sy, float sz, float& vx,
                             float& vy, float& vz, float* unit_x, float* unit_y,
                             float* unit_z) noexcept {
  if (unit_x != nullptr) {
    *unit_x = ux;
  }
  if (unit_y != nullptr) {
    *unit_y = uy;
  }
  if (unit_z != nullptr) {
    *unit_z = uz;
  }
  vx = ux * sx;
  vy = uy * sy;
  vz = uz * sz;
}

enum class NoiseExperiment : uint8_t {
  NativeCurrent,
  BridsonVectorPotential,
  FrequencyDampedCurl,
  QualitySharedSample,
};
enum class NoisePhaseExperiment : uint8_t { Current, Age, Seed };
bool split_noise_exact_enabled() noexcept;
bool split_noise_vol_enabled() noexcept;
int split_noise_hold() noexcept;
// hop (not 2022.3): per-system hold N from noise-coord rate + cost-aware
// densify. Default on. WDS_SPLIT_NOISE_HOLD_ADAPT=0 or WDS_SPLIT_NOISE_HOLD=N
// restores uniform. WDS_SPLIT_NOISE_HOLD_RAISE=0/1 forces densify off/on.
int split_noise_hold_for(const SplitEffectParticle& p) noexcept;
int split_noise_hold_for(const SplitEffectParticle& p, int32_t effect_id) noexcept;
float split_noise_coord_rate(const SplitEffectParticle& p) noexcept;
float split_noise_hold_target_travel() noexcept;
float split_noise_hold_tvis() noexcept;
float split_noise_est_live(const SplitEffectParticle& p, int32_t effect_id = 0) noexcept;
float split_noise_est_frame_work(const SplitEffectParticle& p, int hold_n,
                                 int32_t effect_id = 0) noexcept;
// hop: stagger hold-N evals by birth_index % N on the play-time grid.
// WDS_SPLIT_NOISE_HOLD_SPREAD=0 restores (local_step % N). Default on.
bool split_noise_hold_spread() noexcept;
bool split_noise_an_simd_enabled() noexcept;
int split_walk_hz() noexcept;
// hop (not 2022.3): PerParticle trail path is a constant-velocity line
// (lerp exact) when no module can bend it or change speed along age.
// Otherwise fill_trails walks PoseIntegrator once. Not 2022.3.
bool split_particle_trail_path_straight(const SplitEffectParticle& p) noexcept;
bool split_particle_trail_path_straight(const ParticleState& st) noexcept;
// hop (not 2022.3): linear closed-form pose jump. One-shot T=age vs
// stepwise T=h is ULP-divergent, so remake≠play. Default off.
// WDS_SPLIT_CLOSED_FORM=1 enables (visual-only; fails carry-check).
bool split_closed_form() noexcept;
NoiseExperiment noise_experiment() noexcept;
NoisePhaseExperiment noise_phase_experiment() noexcept;
float noise_seed_phase(uint32_t seed, uint32_t axis) noexcept;
void set_split_noise_exact_for_tests(int on) noexcept;
void set_split_noise_neon_for_tests(int on) noexcept;
void set_split_noise_vol_for_tests(int on) noexcept;
void set_split_noise_hold_for_tests(int steps) noexcept;
void set_split_walk_hz_for_tests(int hz) noexcept;
void eval_noise_velocity_vol_q(float px, float py, float pz, float freq, float scroll,
                               float phase_x, float phase_y, float phase_z, int oct, float oct_scale,
                               float oct_mult, float sx, float sy, float sz, float amt, float& vx,
                               float& vy, float& vz) noexcept;

// Test-only frozen scalar (pre-optimization). Production uses the bit-exact
// optimized path. Do not call the reference from integrate.
float perlin3_reference(float x, float y, float z) noexcept;
float perlin3_optimized(float x, float y, float z) noexcept;
float fbm_reference(float x, float y, float z, int octaves, float lac, float gain) noexcept;
float fbm_optimized(float x, float y, float z, int octaves, float lac, float gain) noexcept;
void eval_noise_velocity_reference(const ParticleState& st, float age, float lx, float ly,
                                   float lz, float& vx, float& vy, float& vz) noexcept;

// Same dimensionless Perlin as curl n_at, at the current Main pose.
float eval_noise_n(const ParticleState& st, float age, float world_x, float main_y,
                   float world_z) noexcept;

void eval_sol_axes(const SplitEffectParticle& p, float t01, float ux, float uy, float uz, float& sx,
                   float& sy, float& sz) noexcept;

void lane_to_world(float lx, float ly, float lz, float& wx, float& wy, float& wz) noexcept;

void eval_col(const SplitEffectParticle& p, float t01, float col_u, float& r, float& g, float& b,
              float& a) noexcept;

float eval_rotation_over_life(const SplitEffectParticle& p, float age, float life, float u01,
                              uint8_t axis) noexcept;

uint32_t catalog_index(const SplitEffectParticle& p) noexcept;

float eval_minmax_axis(float max_v, float min_v, uint8_t random, uint16_t begin, uint8_t count,
                       uint16_t min_begin, uint8_t min_count, float t01, float u) noexcept;

void world_force_to_lane(float wx, float wy, float wz, float& lx, float& ly, float& lz) noexcept;

float emitter_inherit_speed(int32_t color_id, float play_t) noexcept;

void set_rol_cache_key(const ParticleCacheKey* key) noexcept;

void fill_trails(PoseIntegrator& integ, const ParticleState& st, float age,
                 SplitEffectLiveParticle& out) noexcept;

void stitch_ribbons(const SplitEffectParticle& p, SplitEffectLiveParticle* out, int n,
                    int32_t line_index, uint32_t system_id) noexcept;

void fill_live(const ParticleState& st, float age, bool want_trail, SplitEffectLiveParticle& out);

void fill_live(PoseIntegrator& integ, const ParticleState& st, float age, bool want_trail,
               SplitEffectLiveParticle& out, bool want_fd = true, float trail_clock = -1.0f);
void fill_live_after_pose(PoseIntegrator& integ, const ParticleState& st, float age,
                          bool want_trail, SplitEffectLiveParticle& out, bool want_fd,
                          float trail_clock = -1.0f);

struct PrimaryEvalJob {
  Birth b{};
  float raw_age = 0.0f;
  float age = 0.0f;
  float emit_u = 0.0f;
  uint32_t seed = 0;
  ParticleState st{};
  IntegratorGridState grid{};
  float pose_age = 0.0f;
  uint8_t alive = 0;
  uint8_t alpha_keep = 0;
  uint8_t do_fill = 1;
  uint8_t has_grid = 0;
  uint8_t has_state = 0;
  uint8_t is_child = 0;
  uint16_t slot_i = 0;
  Birth parent_b{};
  uint32_t write_slot = 0xffffffffu;
  int32_t persist_i = -1;
};

void evaluate_plan(const SplitEffectParticle& p, const EmitContext& ctx,
                   std::vector<PrimaryEvalJob>& jobs);
void evaluate_realize_job(const SplitEffectParticle& p, const EmitContext& ctx, PrimaryEvalJob& job,
                          SplitEffectLiveParticle& dest, IntegratorHot* hot = nullptr);

int evaluate_realize(const SplitEffectParticle& p, const EmitContext& ctx,
                     std::vector<PrimaryEvalJob>& jobs, SplitEffectLiveParticle* out, int max_out,
                     bool write_unfilled_stamps = true);
void evaluate_drop_primary_carry(const EmitContext& ctx, const SplitEffectParticle& p) noexcept;

// Parent event pose at a child birth. Same fields as evaluate.cpp's former
// local SubParentPose (Birth inherit / Death frame-end).
struct SubParentPose {
  float event_t = 0.0f;
  uint8_t type = 0;
  float wx = 0.0f;
  float wy = 0.0f;
  float wz = 0.0f;
  float size = 1.0f;
  float size_y = 1.0f;
  float size_z = 1.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
  float roll = 0.0f;
  float tilt_x = 0.0f;
  float tilt_y = 0.0f;
  float life = 1.0f;
  float age = 0.0f;
  float duration_u = 0.0f;
  uint32_t seed = 0;
  float birth_origin_y_delta = 0.0f;
};

struct SubemitFillItem {
  const SplitEffectParticle* child = nullptr;
  const SplitEffectParticle* parent = nullptr;
  uint32_t child_idx = 0;
  uint32_t parent_idx = 0;
  uint16_t stitch_group = 0;
  uint8_t ribbon = 0;
  uint8_t particle_trail = 0;
  // Identity only; preview-pool cap runs between plan and realize. hop.
  Birth b{};
  Birth parent_b{};
  SubParentPose born{};
  uint32_t seed = 0;
  float age = 0.0f;
  float pose_age = 0.0f;
  float emit_u = 0.0f;
  uint16_t slot_i = 0;
  uint8_t properties = 0;
  uint8_t ready = 0;
  uint8_t filled = 0;
  // 1 = PLAN already ran pose_at (RoD walk). 0 = identity stub; realize
  // hydrates after the preview-pool cap. hop, not 2022.3.
  uint8_t pose_hydrated = 0;
  int32_t persist_i = -1;
};

struct PersistCand {
  Birth b{};
  Birth parent_b{};
  uint16_t slot_i = 0;
  uint32_t seed = 0;
  float emit_u = 0.0f;
  uint8_t properties = 0;
  uint32_t child_idx = 0;
};

struct SubemitFillPlan {
  std::vector<SubemitFillItem> items;
  std::vector<PersistCand> persist_occ;
  int kept = 0;
};

int evaluate_subemitters_plan(const SplitEffectParticle* particles, uint16_t count, int element_slot,
                              const EmitContext& ctx, SubemitFillPlan& plan, int max_out,
                              SplitEffectLiveParticle* out = nullptr,
                              std::vector<SplitEffectLiveParticle>* out_vec = nullptr);
int evaluate_subemitters_realize(const EmitContext& ctx, SubemitFillPlan& plan,
                                 const uint8_t* do_fill, SplitEffectLiveParticle* out, int max_out);

bool primary_eval_can_emit(const SplitEffectParticle& p, float now_t) noexcept;
bool subemit_eval_can_emit(const SplitEffectParticle* particles, uint16_t count, int element_slot,
                           const EmitContext& ctx) noexcept;
void set_dead_system_skip(bool enabled) noexcept;
bool dead_system_skip_enabled() noexcept;
void split_collect_clear_dead_latches() noexcept;

}  // namespace wds::chart_render::shuriken

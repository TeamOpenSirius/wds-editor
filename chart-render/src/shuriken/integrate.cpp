#include "internal.hpp"
#include "modules_an.inl.hpp"
#include "split_eval_cache.hpp"
#include "split_wall_profile.hpp"

#include <wds/chart_render/shuriken/curve.hpp>
#include <wds/chart_render/split_effect_catalog.hpp>

#include <wds/chart_render/split_effect_particles.hpp>

#include <wds/common/perf_stages.hpp>

#include <wds/core/gimmick.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/official_split_x.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace wds::chart_render::shuriken {

namespace {

IntegrationObserver g_integration_observer = nullptr;
IntegrationStepObserver g_integration_step_observer = nullptr;
thread_local IntegrationVelocityTrace g_last_integration_velocity{};

bool force_world_candidate() noexcept {
  static const bool enabled = [] {
    const char* raw = std::getenv("OPENWDS_FORCE_SPACE_EXPERIMENT");
    return raw != nullptr && std::string_view(raw) == "world";
  }();
  return enabled;
}

// Observation-only authored-local module-space candidates.  The recovered
// 2020 assets author Force/VOL/Limit in local space while their ParticleSystem
// transform is under the +60 degree LaneGroup parent.  Native integration is
// expressed in the Main/LaneGroup basis, so a faithful local->world->lane
// conversion is algebraically an identity for the measured static hierarchy.
// Keep the conversion behind an environment switch so probe runs can prove
// that fact without changing the production path.  The *_as_lane variants are
// deliberately diagnostic "single rotation" hops: they model the common
// mistake of transforming an authored local vector to Unity World and then
// feeding that World vector into the native LaneGroup integrator.
enum class AuthoredLocalTransformExperiment : uint8_t {
  Native,
  AtBirthRoundTrip,
  CurrentRoundTrip,
  AtBirthWorldAsLane,
  CurrentWorldAsLane,
};

AuthoredLocalTransformExperiment authored_local_transform_experiment() noexcept {
  static const AuthoredLocalTransformExperiment mode = [] {
    const char* raw = std::getenv("OPENWDS_AUTHORED_LOCAL_TRANSFORM_EXPERIMENT");
    if (raw == nullptr) return AuthoredLocalTransformExperiment::Native;
    const std::string_view name(raw);
    if (name == "at_birth_roundtrip")
      return AuthoredLocalTransformExperiment::AtBirthRoundTrip;
    if (name == "current_roundtrip")
      return AuthoredLocalTransformExperiment::CurrentRoundTrip;
    if (name == "at_birth_world_as_lane")
      return AuthoredLocalTransformExperiment::AtBirthWorldAsLane;
    if (name == "current_world_as_lane")
      return AuthoredLocalTransformExperiment::CurrentWorldAsLane;
    return AuthoredLocalTransformExperiment::Native;
  }();
  return mode;
}

void authored_local_module_to_lane(float x, float y, float z, float& out_x, float& out_y,
                                   float& out_z) noexcept {
  const AuthoredLocalTransformExperiment mode = authored_local_transform_experiment();
  if (mode == AuthoredLocalTransformExperiment::Native) {
    out_x = x;
    out_y = y;
    out_z = z;
    return;
  }
  // The parent probe records a static PS/parent orientation of +60 degrees X
  // at both birth and current time.  A direction transform has no translation
  // component; Unity TransformDirection also ignores uniform parent scale.
  float wx = 0.0f, wy = 0.0f, wz = 0.0f;
  lane_to_world(x, y, z, wx, wy, wz);
  if (mode == AuthoredLocalTransformExperiment::AtBirthWorldAsLane ||
      mode == AuthoredLocalTransformExperiment::CurrentWorldAsLane) {
    out_x = wx;
    out_y = wy;
    out_z = wz;
    return;
  }
  // Correct conversion for an integrator whose state is Main/LaneGroup local:
  // local PS vector -> Unity World -> measured parent inverse.
  world_force_to_lane(wx, wy, wz, out_x, out_y, out_z);
}

bool vol_world_candidate() noexcept {
  static const bool enabled = [] {
    const char* raw = std::getenv("OPENWDS_VOL_SPACE_EXPERIMENT");
    return raw != nullptr && std::string_view(raw) == "world";
  }();
  return enabled;
}

bool module_order_has(std::string_view name) noexcept;

struct ModuleOrderFlags {
  bool force_after_limit = false;
  bool noise_before_limit = false;
  bool vol_before_limit = false;
  bool vol_before_force = false;
};

const ModuleOrderFlags& module_order_flags() noexcept {
  static const ModuleOrderFlags flags = [] {
    ModuleOrderFlags f;
    f.force_after_limit = module_order_has("force_after_limit");
    f.noise_before_limit = module_order_has("noise_before_limit");
    f.vol_before_limit = module_order_has("vol_before_limit");
    f.vol_before_force = module_order_has("vol_before_force");
    return f;
  }();
  return flags;
}

[[maybe_unused]] bool vol_before_force_candidate() noexcept {
  return module_order_flags().vol_before_force;
}

[[maybe_unused]] bool drag_total_candidate() noexcept {
  static const bool enabled = [] {
    const char* raw = std::getenv("OPENWDS_DRAG_TARGET_EXPERIMENT");
    return raw != nullptr && std::string_view(raw) == "total";
  }();
  return enabled;
}

bool drag_before_limit_candidate() noexcept {
  static const bool enabled = [] {
    const char* raw = std::getenv("OPENWDS_LIMIT_DRAG_ORDER_EXPERIMENT");
    return raw != nullptr && std::string_view(raw) == "drag_before_limit";
  }();
  return enabled;
}

bool module_order_has(std::string_view name) noexcept {
  static const std::string cached = []() -> std::string {
    const char* raw = std::getenv("OPENWDS_MODULE_ORDER_EXPERIMENT");
    return raw != nullptr ? std::string(raw) : std::string();
  }();
  if (cached.empty()) return false;
  const std::string_view value(cached);
  // Accept a single value and comma-separated combinations so the probe can
  // sweep bounded permutations without introducing a production setting.
  size_t start = 0;
  while (start <= value.size()) {
    const size_t end = value.find(',', start);
    const std::string_view item = value.substr(start, end == std::string_view::npos
                                                        ? value.size() - start
                                                        : end - start);
    if (item == name) return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

enum class NoiseSamplePositionExperiment : uint8_t {
  PreDisplacement,
  PostDisplacement,
  PostDisplacementFeedback,
};

NoiseSamplePositionExperiment noise_sample_position_experiment() noexcept {
  static const NoiseSamplePositionExperiment mode = [] {
    const char* raw = std::getenv("OPENWDS_NOISE_SAMPLE_POSITION");
    if (raw == nullptr) return NoiseSamplePositionExperiment::PreDisplacement;
    const std::string_view name(raw);
    if (name == "post_displacement") return NoiseSamplePositionExperiment::PostDisplacement;
    if (name == "post_displacement_feedback") {
      return NoiseSamplePositionExperiment::PostDisplacementFeedback;
    }
    return NoiseSamplePositionExperiment::PreDisplacement;
  }();
  return mode;
}

enum class NoisePhaseTimeExperiment : uint8_t {
  CurrentMidpoint,
  ParticleAge,
  StepStart,
};

NoisePhaseTimeExperiment noise_phase_time_experiment() noexcept {
  static const NoisePhaseTimeExperiment mode = [] {
    const char* raw = std::getenv("OPENWDS_NOISE_PHASE_EXPERIMENT");
    if (raw == nullptr) return NoisePhaseTimeExperiment::CurrentMidpoint;
    const std::string_view name(raw);
    if (name == "age") return NoisePhaseTimeExperiment::ParticleAge;
    if (name == "current") return NoisePhaseTimeExperiment::StepStart;
    // `seed` is a domain phase candidate handled in modules.cpp; retain the
    // current midpoint clock for that variant.
    return NoisePhaseTimeExperiment::CurrentMidpoint;
  }();
  return mode;
}

enum class AnimatedFeedbackExperiment : uint8_t {
  None,
  NoiseToStored,
  VolToStored,
  VolNoiseToStored,
};

AnimatedFeedbackExperiment animated_feedback_experiment() noexcept {
  static const AnimatedFeedbackExperiment mode = [] {
    const char* raw = std::getenv("OPENWDS_ANIMATED_FEEDBACK_EXPERIMENT");
    if (raw == nullptr) return AnimatedFeedbackExperiment::None;
    const std::string_view name(raw);
    if (name == "noise_to_stored") return AnimatedFeedbackExperiment::NoiseToStored;
    if (name == "vol_to_stored") return AnimatedFeedbackExperiment::VolToStored;
    if (name == "vol_noise_to_stored") return AnimatedFeedbackExperiment::VolNoiseToStored;
    return AnimatedFeedbackExperiment::None;
  }();
  return mode;
}

void rotate_axis(float ax, float ay, float az, float angle, float& x, float& y, float& z) noexcept {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  const float dot = ax * x + ay * y + az * z;
  const float cx = ay * z - az * y;
  const float cy = az * x - ax * z;
  const float cz = ax * y - ay * x;
  x = x * c + cx * s + ax * dot * (1.0f - c);
  y = y * c + cy * s + ay * dot * (1.0f - c);
  z = z * c + cz * s + az * dot * (1.0f - c);
}

void eval_force_lane(const ParticleState& st, float t01, uint32_t step, float& fx, float& fy,
                     float& fz) noexcept {
  const SplitEffectParticle& p = *st.p;
  fx = fy = fz = 0.0f;
  if (p.force_enabled == 0) {
    return;
  }
  float ux = st.force_ux;
  float uy = st.force_uy;
  float uz = st.force_uz;
  if (p.force_randomize != 0) {
    ux = u01(mix3(st.seed, step + 91u, 1u));
    uy = u01(mix3(st.seed, step + 92u, 2u));
    uz = u01(mix3(st.seed, step + 93u, 3u));
  }
  fx = eval_minmax_axis(p.force_x, p.force_x_min, p.force_x_random, p.force_x_begin, p.force_x_count,
                        p.force_x_min_begin, p.force_x_min_count, t01, ux);
  fy = eval_minmax_axis(p.force_y, p.force_y_min, p.force_y_random, p.force_y_begin, p.force_y_count,
                        p.force_y_min_begin, p.force_y_min_count, t01, uy);
  fz = eval_minmax_axis(p.force_z, p.force_z_min, p.force_z_random, p.force_z_begin, p.force_z_count,
                        p.force_z_min_begin, p.force_z_min_count, t01, uz);
  if (p.force_in_world != 0 || force_world_candidate()) {
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    world_force_to_lane(fx, fy, fz, lx, ly, lz);
    fx = lx;
    fy = ly;
    fz = lz;
  } else if (authored_local_transform_experiment() != AuthoredLocalTransformExperiment::Native) {
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    authored_local_module_to_lane(fx, fy, fz, lx, ly, lz);
    fx = lx;
    fy = ly;
    fz = lz;
  }
}

float eval_minmax_axis_hint(float max_v, float min_v, uint8_t random, uint16_t begin, uint8_t count,
                            uint16_t min_begin, uint8_t min_count, float t01, float u,
                            uint8_t& hi_hint, uint8_t& lo_hint) noexcept {
  float hi = max_v;
  if (count > 0) {
    using split_effect_catalog_detail::kSplitEffectCurveKeys;
    hi = eval_hermite_hint(reinterpret_cast<const HermiteKey*>(kSplitEffectCurveKeys + begin), count,
                           t01, max_v, hi_hint);
  }
  if (random == 0) {
    return hi;
  }
  float lo = min_v;
  if (min_count > 0) {
    using split_effect_catalog_detail::kSplitEffectCurveKeys;
    lo = eval_hermite_hint(reinterpret_cast<const HermiteKey*>(kSplitEffectCurveKeys + min_begin),
                           min_count, t01, min_v, lo_hint);
  }
  return lo + (hi - lo) * std::clamp(u, 0.0f, 1.0f);
}

void eval_force_lane_hint(const ParticleState& st, float t01, uint32_t step, float& fx, float& fy,
                          float& fz, uint8_t* hints, bool force_world,
                          bool authored_local_native) noexcept {
  const SplitEffectParticle& p = *st.p;
  fx = fy = fz = 0.0f;
  if (p.force_enabled == 0) {
    return;
  }
  float ux = st.force_ux;
  float uy = st.force_uy;
  float uz = st.force_uz;
  if (p.force_randomize != 0) {
    ux = u01(mix3(st.seed, step + 91u, 1u));
    uy = u01(mix3(st.seed, step + 92u, 2u));
    uz = u01(mix3(st.seed, step + 93u, 3u));
  }
  fx = eval_minmax_axis_hint(p.force_x, p.force_x_min, p.force_x_random, p.force_x_begin,
                             p.force_x_count, p.force_x_min_begin, p.force_x_min_count, t01, ux,
                             hints[0], hints[1]);
  fy = eval_minmax_axis_hint(p.force_y, p.force_y_min, p.force_y_random, p.force_y_begin,
                             p.force_y_count, p.force_y_min_begin, p.force_y_min_count, t01, uy,
                             hints[2], hints[3]);
  fz = eval_minmax_axis_hint(p.force_z, p.force_z_min, p.force_z_random, p.force_z_begin,
                             p.force_z_count, p.force_z_min_begin, p.force_z_min_count, t01, uz,
                             hints[4], hints[5]);
  if (force_world) {
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    world_force_to_lane(fx, fy, fz, lx, ly, lz);
    fx = lx;
    fy = ly;
    fz = lz;
  } else if (!authored_local_native) {
    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
    authored_local_module_to_lane(fx, fy, fz, lx, ly, lz);
    fx = lx;
    fy = ly;
    fz = lz;
  }
}

// live dump v23 WITHDRAW. Hamming-1 Unity has no F·τ birth kick on either
// side of the former AND-gate. Official Force / Limit pages have no birth
// impulse. Helper always returns false. Not 2022.3.
void apply_force_birth_impulse(const ParticleState& st, float& vx, float& vy,
                               float& vz) noexcept {
  const SplitEffectParticle& p = *st.p;
  // Predicate is unconditionally false (v23 withdraw) and ignores these
  // args. Evaluate Force only if a restored helper can apply.
  if (!official_split_effect_force_birth_impulse_applies(p.force_enabled, p.clamp_enabled,
                                                         p.clamp_dampen, p.clamp_drag, 0.0f, 0.0f,
                                                         0.0f)) {
    return;
  }
  float fx = 0.0f, fy = 0.0f, fz = 0.0f;
  eval_force_lane(st, 0.0f, 0, fx, fy, fz);
  if (!official_split_effect_force_birth_impulse_applies(p.force_enabled, p.clamp_enabled,
                                                         p.clamp_dampen, p.clamp_drag, fx, fy,
                                                         fz)) {
    return;
  }
  const float tau = kSplitEffectForceBirthImpulseTau;
  vx += fx * tau;
  vy += fy * tau;
  vz += fz * tau;
}

void apply_drag(const ParticleState& st, float t01, float dt, float size, float& vx, float& vy,
                float& vz) noexcept {
  // live dump force-drag-2020-class-v1. Not 2022.3.
  // Caller passes composed total (stored + VOL + Noise + inherit). mulVel
  // selects iso vs const-decel; k is not also multiplied by |v|.
  const SplitEffectParticle& p = *st.p;
  if (p.clamp_enabled == 0 || dt <= 1e-8f) {
    return;
  }
  float k = eval_minmax_axis(p.clamp_drag, p.clamp_drag_min, p.clamp_drag_random, p.clamp_drag_begin,
                             p.clamp_drag_count, 0, 0, t01, st.force_ux);
  if (p.clamp_mul_size != 0) {
    k *= size;
  }
  split_effect_limit_drag_on_total(k, dt, p.clamp_mul_vel, vx, vy, vz);
}

void apply_limit_velocity(const ParticleState& st, float t01, float dt, float size, float& vx,
                          float& vy, float& vz, bool include_drag = true) noexcept {
  const SplitEffectParticle& p = *st.p;
  if (p.clamp_enabled == 0 || dt <= 1e-8f) {
    return;
  }
  if (drag_before_limit_candidate()) {
    apply_drag(st, t01, dt, size, vx, vy, vz);
  }
  float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
  if (p.clamp_separate == 0) {
    const float limit =
        eval_minmax_axis(p.clamp_speed, p.clamp_speed_min, p.clamp_speed_random, p.clamp_speed_begin,
                         p.clamp_speed_count, 0, 0, t01, st.force_ux);
    if (speed > limit && speed > 1e-8f && p.clamp_dampen > 1e-8f) {
      const float target = speed + (limit - speed) * std::clamp(p.clamp_dampen, 0.0f, 1.0f);
      const float s = target / speed;
      vx *= s;
      vy *= s;
      vz *= s;
      speed = target;
    }
  } else {
    // Unity 2022.3 Limit Velocity Space is only available with Separate Axes.
    // World: limits are Unity world components. Official extract: 0 / 1100.
    float axv = vx, ayv = vy, azv = vz;
    if (p.clamp_in_world != 0) {
      lane_to_world(vx, vy, vz, axv, ayv, azv);
    } else if (authored_local_transform_experiment() ==
                   AuthoredLocalTransformExperiment::AtBirthWorldAsLane ||
               authored_local_transform_experiment() ==
                   AuthoredLocalTransformExperiment::CurrentWorldAsLane) {
      // Diagnostic single-rotation hop: treat the local-axis velocity after
      // PS->World conversion as if it were still in the native LaneGroup
      // basis.  This is intentionally a candidate, not production behavior.
      lane_to_world(vx, vy, vz, axv, ayv, azv);
    } else if (authored_local_transform_experiment() !=
                   AuthoredLocalTransformExperiment::Native) {
      // Correct local->world->lane round-trip.  It is identity for the
      // measured static parent but keeps this candidate explicit in traces.
      float wx = 0.0f, wy = 0.0f, wz = 0.0f;
      lane_to_world(vx, vy, vz, wx, wy, wz);
      world_force_to_lane(wx, wy, wz, axv, ayv, azv);
    }
    auto axis = [&](float& v, float lim) {
      const float a = std::fabs(v);
      if (a > lim && a > 1e-8f && p.clamp_dampen > 1e-8f) {
        const float target = a + (lim - a) * std::clamp(p.clamp_dampen, 0.0f, 1.0f);
        v *= target / a;
      }
    };
    axis(axv, p.clamp_x);
    axis(ayv, p.clamp_y);
    axis(azv, p.clamp_z);
    if (p.clamp_in_world != 0) {
      world_force_to_lane(axv, ayv, azv, vx, vy, vz);
    } else if (authored_local_transform_experiment() ==
                   AuthoredLocalTransformExperiment::AtBirthWorldAsLane ||
               authored_local_transform_experiment() ==
                   AuthoredLocalTransformExperiment::CurrentWorldAsLane) {
      vx = axv;
      vy = ayv;
      vz = azv;
    } else {
      vx = axv;
      vy = ayv;
      vz = azv;
    }
    speed = std::sqrt(vx * vx + vy * vy + vz * vz);
  }
  if (include_drag && !drag_before_limit_candidate()) {
    apply_drag(st, t01, dt, size, vx, vy, vz);
  }
}

bool noise_phase_forces_legacy() noexcept {
  return noise_phase_time_experiment() == NoisePhaseTimeExperiment::ParticleAge;
}

bool env_split_integrator_legacy() noexcept {
  static const bool legacy = [] {
    const char* raw = std::getenv("WDS_SPLIT_INTEGRATOR");
    return raw != nullptr && std::string_view(raw) == "legacy";
  }();
  return legacy;
}

enum class LegacyOverride : int8_t { Unset = -1, Grid = 0, Legacy = 1 };

LegacyOverride g_legacy_override = LegacyOverride::Unset;
float g_hz_override = 0.0f;

}  // namespace

bool split_integrator_is_legacy() noexcept {
  if (g_legacy_override != LegacyOverride::Unset) {
    return g_legacy_override == LegacyOverride::Legacy;
  }
  return env_split_integrator_legacy() || noise_phase_forces_legacy();
}

void set_split_integrator_legacy_for_tests(bool legacy) noexcept {
  g_legacy_override = legacy ? LegacyOverride::Legacy : LegacyOverride::Grid;
  bump_eval_system_hash_config_generation();
}

void set_integration_hz_for_tests(float hz) noexcept {
  g_hz_override = hz;
  bump_eval_system_hash_config_generation();
}

float integration_candidate_hz() noexcept {
  if (g_hz_override > 0.0f) {
    return g_hz_override;
  }
  static const float hz = [] {
    const char* raw = std::getenv("OPENWDS_INTEGRATION_CLOCK");
    if (raw != nullptr && (std::string_view(raw) == "fixed_50hz" ||
                           std::string_view(raw) == "fixed_50hz_accum")) {
      return 50.0f;
    }
    return 60.0f;
  }();
  return hz;
}

float integration_candidate_age(float age) noexcept {
  static const bool accumulator = [] {
    const char* raw = std::getenv("OPENWDS_INTEGRATION_CLOCK");
    return raw != nullptr && std::string_view(raw) == "fixed_50hz_accum";
  }();
  if (!accumulator || age <= 0.0f) return age;
  constexpr float kHz = 50.0f;
  return std::floor(age * kHz + 1e-4f) / kHz;
}

void set_integration_observer(IntegrationObserver observer) noexcept {
  g_integration_observer = observer;
}

const IntegrationVelocityTrace& last_integration_velocity() noexcept {
  return g_last_integration_velocity;
}

void set_last_integration_velocity(const IntegrationVelocityTrace& t) noexcept {
  g_last_integration_velocity = t;
}

void set_integration_step_observer(IntegrationStepObserver observer) noexcept {
  g_integration_step_observer = observer;
}

bool integration_observers_installed() noexcept {
  return g_integration_observer != nullptr || g_integration_step_observer != nullptr;
}

namespace {

constexpr float kGridIndexEps = 1e-4f;
constexpr float kTailEps = 1e-7f;

}  // namespace

void PoseIntegrator::inherit_emitter_v(float play_t, float& evx, float& evy, float& evz) const noexcept {
  const ParticleState& st = *st_;
  if (st.emitter_vel_set != 0) {
    evx = st.emitter_vx;
    evy = st.emitter_vy;
    evz = st.emitter_vz;
    return;
  }
  evx = 0.0f;
  // emitter_inherit_speed is d(main_y)/dt. Integration vy is ly; convert
  // with the same slope as apply_epilogue so inherit_ly stays |lever|*fade'.
  evy = emitter_main_y_from_ly_slope(st.color_id) * emitter_inherit_speed(st.color_id, play_t);
  evz = 0.0f;
}

void PoseIntegrator::seed_trace_from_stored() noexcept {
  trace_ = {};
  trace_.stored_x = hot_->vx;
  trace_.stored_y = hot_->vy;
  trace_.stored_z = hot_->vz;
  trace_.total_x = hot_->vx;
  trace_.total_y = hot_->vy;
  trace_.total_z = hot_->vz;
}

void PoseIntegrator::reset_to_birth() noexcept {
  hot_->lx = birth_lx_;
  hot_->ly = birth_ly_;
  hot_->lz = birth_lz_;
  hot_->vx = birth_vx_;
  hot_->vy = birth_vy_;
  hot_->vz = birth_vz_;
  hot_->next_step = 0;
  seed_trace_from_stored();
}

void PoseIntegrator::bind_hot(IntegratorHot* dest) noexcept {
  if (dest == nullptr || dest == hot_) {
    return;
  }
  *dest = *hot_;
  hot_ = dest;
}

PoseIntegrator::PoseIntegrator(const ParticleState& st, IntegratorHot* hot) noexcept : st_(&st) {
  hot_ = &embedded_;
  if (hot != nullptr) {
    hot_ = hot;
  }
  SplitWallScope wall_ctor(SplitWallSec::PoseCtor);
  const SplitEffectParticle& p = *st.p;
  // EVAL-GAP(Custom): Custom is preserved in catalog/serialization, but the
  // native evaluator has no MainModule.customSimulationSpace Transform source
  // (Unity 2022.3 MainModule.simulationSpace). It therefore follows the
  // existing non-Local path until that source is wired.
  local_sim_ = p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Local);
  birth_x_scale_ = wds::chart_editor::kOfficialSplitLineLocalScaleX;
  birth_y_scale_ = wds::chart_editor::kOfficialLineHeightScale * std::max(st.birth_fade, 0.0f);
  live_y_scale_ = wds::chart_editor::kOfficialLineHeightScale * std::max(st.fade_scale, 0.0f);
  hot_->lx = st.spawn_x;
  hot_->ly = st.spawn_y;
  hot_->lz = st.spawn_z;
  if (!local_sim_ && p.scaling_mode != static_cast<uint8_t>(SplitEffectScalingMode::Local)) {
    hot_->lx *= birth_x_scale_;
    hot_->ly *= birth_y_scale_;
  } else if (!local_sim_) {
    hot_->lx += p.local_pos_x * (birth_x_scale_ - 1.0f);
    hot_->ly += p.local_pos_y * (birth_y_scale_ - 1.0f);
  }

  hot_->vx = st.dir_x * st.speed + st.vx;
  hot_->vy = st.dir_y * st.speed + st.vy;
  hot_->vz = st.dir_z * st.speed + st.vz;
  apply_force_birth_impulse(st, hot_->vx, hot_->vy, hot_->vz);
  if (p.inherit_enabled != 0 && p.inherit_mode == 0) {
    float evx = 0.0f, evy = 0.0f, evz = 0.0f;
    inherit_emitter_v(st.birth_t, evx, evy, evz);
    float sax = 0.0f, say = 0.0f, saz = 0.0f, ax = 0.0f, ay = 0.0f, az = 0.0f;
    split_effect_inherit_velocity_terms(p.inherit_enabled, p.inherit_mode, st.inherit_k, evx, evy,
                                        evz, sax, say, saz, ax, ay, az);
    hot_->vx += sax;
    hot_->vy += say;
    hot_->vz += saz;
  }

  grav_y_ = 0.0f;
  grav_z_ = 0.0f;
  if (std::fabs(p.gravity_modifier) > 1e-8f) {
    wds::chart_editor::official_gravity_lane_accel(p.gravity_modifier, grav_y_, grav_z_);
  }

  hz_ = integration_candidate_hz();
  h_ = 1.0f / hz_;
  legacy_ = split_integrator_is_legacy();
  const ModuleOrderFlags& mo = module_order_flags();
  force_after_limit_ = mo.force_after_limit;
  noise_before_limit_ = mo.noise_before_limit;
  vol_before_limit_ = mo.vol_before_limit;
  vol_before_force_ = mo.vol_before_force;
  birth_lx_ = hot_->lx;
  birth_ly_ = hot_->ly;
  birth_lz_ = hot_->lz;
  birth_vx_ = hot_->vx;
  birth_vy_ = hot_->vy;
  birth_vz_ = hot_->vz;
  noise_hold_ = (st.p != nullptr) ? split_noise_hold_for(*st.p, st.color_id) : split_noise_hold();
  noise_hold_spread_ = split_noise_hold_spread();
  has_vol_ = (p.flags & kSplitEffectFlagVelocity) != 0 ? 1 : 0;
  has_force_ = p.force_enabled != 0 ? 1 : 0;
  has_noise_ = (p.flags & kSplitEffectFlagNoise) != 0 ? 1 : 0;
  has_inherit_cur_ = (p.inherit_enabled != 0 && p.inherit_mode != 0) ? 1 : 0;
  has_clamp_ = p.clamp_enabled != 0 ? 1 : 0;
  vol_world_ = (p.vol_in_world != 0 || vol_world_candidate()) ? 1 : 0;
  force_world_ = (p.force_in_world != 0 || force_world_candidate()) ? 1 : 0;
  authored_local_native_ =
      authored_local_transform_experiment() == AuthoredLocalTransformExperiment::Native ? 1 : 0;
  step_mask_ = static_cast<uint8_t>((has_vol_ != 0 ? 1u : 0u) | (has_force_ != 0 ? 2u : 0u) |
                                    (has_noise_ != 0 ? 4u : 0u) | (has_inherit_cur_ != 0 ? 8u : 0u));
  noise_position_ = static_cast<uint8_t>(noise_sample_position_experiment());
  noise_phase_ = static_cast<uint8_t>(noise_phase_time_experiment());
  animated_feedback_ = static_cast<uint8_t>(animated_feedback_experiment());
  for (uint8_t& h : curve_hint_) {
    h = 1;
  }
  seed_trace_from_stored();
}

PoseIntegrator::~PoseIntegrator() {
  flush_pending_stores();
}

void PoseIntegrator::run_step(int i, float t_mid, float dt, float t_end_for_noise, int step_count,
                              float step_start, float step_end,
                              const float* noise_override) noexcept {
  switch (step_mask_) {
#define WDS_RUN_STEP_CASE(M)                                                                      \
  case M:                                                                                         \
    run_step_masked<M>(i, t_mid, dt, t_end_for_noise, step_count, step_start, step_end,           \
                       noise_override);                                                           \
    return;
    WDS_RUN_STEP_CASE(0)
    WDS_RUN_STEP_CASE(1)
    WDS_RUN_STEP_CASE(2)
    WDS_RUN_STEP_CASE(3)
    WDS_RUN_STEP_CASE(4)
    WDS_RUN_STEP_CASE(5)
    WDS_RUN_STEP_CASE(6)
    WDS_RUN_STEP_CASE(7)
    WDS_RUN_STEP_CASE(8)
    WDS_RUN_STEP_CASE(9)
    WDS_RUN_STEP_CASE(10)
    WDS_RUN_STEP_CASE(11)
    WDS_RUN_STEP_CASE(12)
    WDS_RUN_STEP_CASE(13)
    WDS_RUN_STEP_CASE(14)
    WDS_RUN_STEP_CASE(15)
#undef WDS_RUN_STEP_CASE
    default:
      run_step_masked<15>(i, t_mid, dt, t_end_for_noise, step_count, step_start, step_end,
                          noise_override);
      return;
  }
}

template <uint32_t Mask>
void PoseIntegrator::run_step_masked(int i, float t_mid, float dt, float t_end_for_noise,
                                     int step_count, float step_start, float step_end,
                                     const float* noise_override) noexcept {
  constexpr bool kHasVol = (Mask & 1u) != 0;
  constexpr bool kHasForce = (Mask & 2u) != 0;
  constexpr bool kHasNoise = (Mask & 4u) != 0;
  constexpr bool kHasInherit = (Mask & 8u) != 0;
  const ParticleState& st = *st_;
  const SplitEffectParticle& p = *st.p;
  float& lx = hot_->lx;
  float& ly = hot_->ly;
  float& lz = hot_->lz;
  float& vx = hot_->vx;
  float& vy = hot_->vy;
  float& vz = hot_->vz;
  IntegrationVelocityTrace& trace = trace_;
  const float grav_y = grav_y_;
  const float grav_z = grav_z_;
  const float life = st.life > 1e-6f ? st.life : 1.0f;
  const bool force_after_limit = force_after_limit_;
  const bool noise_before_limit = noise_before_limit_;
  const bool vol_before_limit = vol_before_limit_;
  const auto noise_position = static_cast<NoiseSamplePositionExperiment>(noise_position_);
  const auto noise_phase = static_cast<NoisePhaseTimeExperiment>(noise_phase_);
  const auto animated_feedback = static_cast<AnimatedFeedbackExperiment>(animated_feedback_);
  const float t = t_mid;
  const float t01 = std::clamp(t / life, 0.0f, 1.0f);
  float fx = 0.0f, fy = 0.0f, fz = 0.0f;
  if constexpr (kHasForce) {
    eval_force_lane_hint(st, t01, static_cast<uint32_t>(i), fx, fy, fz, curve_hint_,
                         force_world_ != 0, authored_local_native_ != 0);
  }
  float volx = 0.0f, voly = 0.0f, volz = 0.0f;
  if constexpr (kHasVol) {
    volx = eval_minmax_axis_hint(p.vel_x, p.vel_x_min, p.vel_x_random, p.vel_x_begin, p.vel_x_count,
                                 p.vel_x_min_begin, p.vel_x_min_count, t01, st.vol_ux,
                                 curve_hint_[6], curve_hint_[7]);
    voly = eval_minmax_axis_hint(p.vel_y, p.vel_y_min, p.vel_y_random, p.vel_y_begin, p.vel_y_count,
                                 p.vel_y_min_begin, p.vel_y_min_count, t01, st.vol_uy,
                                 curve_hint_[8], curve_hint_[9]);
    volz = eval_minmax_axis_hint(p.vel_z, p.vel_z_min, p.vel_z_random, p.vel_z_begin, p.vel_z_count,
                                 p.vel_z_min_begin, p.vel_z_min_count, t01, st.vol_uz,
                                 curve_hint_[10], curve_hint_[11]);
    if (vol_world_ != 0) {
      float wx = 0.0f, wy = 0.0f, wz = 0.0f;
      world_force_to_lane(volx, voly, volz, wx, wy, wz);
      volx = wx;
      voly = wy;
      volz = wz;
    } else if (authored_local_native_ == 0) {
      float lvx = 0.0f, lvy = 0.0f, lvz = 0.0f;
      authored_local_module_to_lane(volx, voly, volz, lvx, lvy, lvz);
      volx = lvx;
      voly = lvy;
      volz = lvz;
    }
  }
  const bool vol_in_stored = vol_before_force_ || vol_before_limit;
  if (vol_before_force_ || vol_before_limit) {
    vx += volx;
    vy += voly;
    vz += volz;
  }
  if (!force_after_limit) {
    vx += fx * dt;
    vy += (fy + grav_y) * dt;
    vz += (fz + grav_z) * dt;
  }
  float inh_x = 0.0f, inh_y = 0.0f, inh_z = 0.0f;
  if constexpr (kHasInherit) {
    float evx = 0.0f, evy = 0.0f, evz = 0.0f;
    inherit_emitter_v(st.birth_t + t, evx, evy, evz);
    float sax = 0.0f, say = 0.0f, saz = 0.0f;
    split_effect_inherit_velocity_terms(p.inherit_enabled, p.inherit_mode, st.inherit_k, evx, evy,
                                        evz, sax, say, saz, inh_x, inh_y, inh_z);
  }
  const float noise_time = noise_phase == NoisePhaseTimeExperiment::ParticleAge
                               ? t_end_for_noise
                               : noise_phase == NoisePhaseTimeExperiment::StepStart
                                     ? step_start
                                     : t;
  float nvx = 0.0f, nvy = 0.0f, nvz = 0.0f;
  bool noise_in_stored = false;
  const int noise_hold = noise_hold_;
  // hop (not 2022.3). Old hold used the particle-local step i from birth
  // (i % N == 0). Same-age bursts then all eval on the same play frame.
  // Spread: phase = birth_index % N (balanced in one system; seed fallback).
  // Eval when (floor((birth_t + step_start)*hz) + phase) % N == 0. First
  // local step always evals so hold has a value and births do not sit on
  // zero noise. Pure identity + grid time: carry matches stateless.
  bool reuse_held_noise = false;
  if (noise_override == nullptr && noise_hold > 1) {
    if (!noise_hold_spread_) {
      reuse_held_noise = (i % noise_hold) != 0;
    } else if (i != 0) {
      const int g = static_cast<int>(
          std::floor(static_cast<double>(st.birth_t + step_start) * static_cast<double>(hz_) +
                     static_cast<double>(kGridIndexEps)));
      int phase = st.birth_index % noise_hold;
      if (phase < 0) {
        phase += noise_hold;
      }
      int slot = (g + phase) % noise_hold;
      if (slot < 0) {
        slot += noise_hold;
      }
      reuse_held_noise = slot != 0;
    }
  }
  auto eval_noise_at_candidate_position = [&](float& ox, float& oy, float& oz) {
    if (noise_override != nullptr) {
      ox = noise_override[0];
      oy = noise_override[1];
      oz = noise_override[2];
      return;
    }
    float sx = lx;
    float sy = ly;
    float sz = lz;
    if (noise_position != NoiseSamplePositionExperiment::PreDisplacement) {
      const float base_x = vx + (vol_in_stored ? 0.0f : volx) + inh_x;
      const float base_y = vy + (vol_in_stored ? 0.0f : voly) + inh_y;
      const float base_z = vz + (vol_in_stored ? 0.0f : volz) + inh_z;
      sx += base_x * dt;
      sy += base_y * dt;
      sz += base_z * dt;
    }
    if (!split_noise_exact_enabled() &&
        noise_experiment() != NoiseExperiment::QualitySharedSample) {
      eval_noise_velocity_an(st, noise_time, sx, sy, sz, ox, oy, oz, &trace.noise_unit_x,
                             &trace.noise_unit_y, &trace.noise_unit_z);
    } else {
      eval_noise_velocity(st, noise_time, sx, sy, sz, ox, oy, oz, &trace.noise_unit_x,
                          &trace.noise_unit_y, &trace.noise_unit_z);
    }
  };
  auto apply_held_noise_strength = [&]() {
    // Strength is a lifetime curve (2022.3 Noise Manual). Hold keeps unit
    // curl; re-evaluate strength at this step's t_mid.
    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    eval_noise_strength_axes(st, t01, sx, sy, sz);
    const float freq = p.noise_frequency > 1e-6f ? p.noise_frequency : 1.0f;
    const NoiseExperiment nexp = noise_experiment();
    if (p.noise_damping != 0 && nexp != NoiseExperiment::BridsonVectorPotential &&
        nexp != NoiseExperiment::QualitySharedSample) {
      sx /= freq;
      sy /= freq;
      sz /= freq;
    }
    nvx = trace.noise_unit_x * sx;
    nvy = trace.noise_unit_y * sy;
    nvz = trace.noise_unit_z * sz;
  };
  if (noise_before_limit) {
    if constexpr (kHasNoise) {
      if (reuse_held_noise) {
        apply_held_noise_strength();
      } else {
        eval_noise_at_candidate_position(nvx, nvy, nvz);
      }
    }
    vx += nvx;
    vy += nvy;
    vz += nvz;
    noise_in_stored = true;
  }
  if (force_after_limit) {
    vx += fx * dt;
    vy += (fy + grav_y) * dt;
    vz += (fz + grav_z) * dt;
  }
  if (!noise_before_limit) {
    if constexpr (kHasNoise) {
      if (reuse_held_noise) {
        apply_held_noise_strength();
      } else {
        eval_noise_at_candidate_position(nvx, nvy, nvz);
      }
    }
    if constexpr (kHasNoise) {
    if (noise_override == nullptr &&
        noise_position == NoiseSamplePositionExperiment::PostDisplacementFeedback) {
      const float base_x = vx + (vol_in_stored ? 0.0f : volx) + inh_x;
      const float base_y = vy + (vol_in_stored ? 0.0f : voly) + inh_y;
      const float base_z = vz + (vol_in_stored ? 0.0f : volz) + inh_z;
      if (!split_noise_exact_enabled() &&
          noise_experiment() != NoiseExperiment::QualitySharedSample) {
        eval_noise_velocity_an(st, noise_time, lx + (base_x + nvx) * dt,
                               ly + (base_y + nvy) * dt, lz + (base_z + nvz) * dt, nvx, nvy, nvz,
                               &trace.noise_unit_x, &trace.noise_unit_y, &trace.noise_unit_z);
      } else {
        eval_noise_velocity(st, noise_time, lx + (base_x + nvx) * dt, ly + (base_y + nvy) * dt,
                            lz + (base_z + nvz) * dt, nvx, nvy, nvz, &trace.noise_unit_x,
                            &trace.noise_unit_y, &trace.noise_unit_z);
      }
    }
    }
  }
  if (animated_feedback == AnimatedFeedbackExperiment::NoiseToStored ||
      animated_feedback == AnimatedFeedbackExperiment::VolNoiseToStored) {
    vx += nvx;
    vy += nvy;
    vz += nvz;
    noise_in_stored = true;
  }
  if (animated_feedback == AnimatedFeedbackExperiment::VolToStored ||
      animated_feedback == AnimatedFeedbackExperiment::VolNoiseToStored) {
    if (!vol_in_stored) {
      vx += volx;
      vy += voly;
      vz += volz;
    }
  }
  float tx = vx + (vol_in_stored ? 0.0f : volx) + inh_x + (noise_in_stored ? 0.0f : nvx);
  float ty = vy + (vol_in_stored ? 0.0f : voly) + inh_y + (noise_in_stored ? 0.0f : nvy);
  float tz = vz + (vol_in_stored ? 0.0f : volz) + inh_z + (noise_in_stored ? 0.0f : nvz);
  // live dump limit-total-v1. Not 2022.3. Speed clamp acts on the composed
  // total (11740_3/4/5 drag=0 pin |total| at 6.470 while |stored| ~14), then
  // drag; stored = total - anim below. dampen=0 leaves the total untouched.
  if (has_clamp_ != 0) {
    apply_limit_velocity(st, t01, dt, st.size, tx, ty, tz);
  }
  vx = tx - (vol_in_stored ? 0.0f : volx) - inh_x - (noise_in_stored ? 0.0f : nvx);
  vy = ty - (vol_in_stored ? 0.0f : voly) - inh_y - (noise_in_stored ? 0.0f : nvy);
  vz = tz - (vol_in_stored ? 0.0f : volz) - inh_z - (noise_in_stored ? 0.0f : nvz);
  trace.stored_x = vx;
  trace.stored_y = vy;
  trace.stored_z = vz;
  trace.vol_x = volx;
  trace.vol_y = voly;
  trace.vol_z = volz;
  trace.noise_x = nvx;
  trace.noise_y = nvy;
  trace.noise_z = nvz;
  trace.inherited_x = inh_x;
  trace.inherited_y = inh_y;
  trace.inherited_z = inh_z;
  trace.total_x = tx;
  trace.total_y = ty;
  trace.total_z = tz;
  if (g_integration_step_observer != nullptr) {
    IntegrationStepTrace step_trace;
    step_trace.step_index = i;
    step_trace.step_count = step_count;
    step_trace.step_start = step_start;
    step_trace.step_end = step_end;
    step_trace.dt = dt;
    step_trace.velocity = trace;
    g_integration_step_observer(st, observe_age_, step_trace);
  }
  float dpx = 0.0f, dpy = 0.0f, dpz = 0.0f;
  split_effect_speedmod_delta_position(tx, ty, tz, dt, p.vol_speed_mod, dpx, dpy, dpz);
  lx += dpx;
  ly += dpy;
  lz += dpz;
  if (std::fabs(st.orb_x) > 1e-8f || std::fabs(st.orb_y) > 1e-8f || std::fabs(st.orb_z) > 1e-8f) {
    float rx = lx - st.orb_ox;
    float ry = ly - st.orb_oy;
    float rz = lz - st.orb_oz;
    if (std::fabs(st.orb_x) > 1e-8f) {
      rotate_axis(1.0f, 0.0f, 0.0f, st.orb_x * dt, rx, ry, rz);
    }
    if (std::fabs(st.orb_y) > 1e-8f) {
      rotate_axis(0.0f, 1.0f, 0.0f, st.orb_y * dt, rx, ry, rz);
    }
    if (std::fabs(st.orb_z) > 1e-8f) {
      rotate_axis(0.0f, 0.0f, 1.0f, st.orb_z * dt, rx, ry, rz);
    }
    lx = st.orb_ox + rx;
    ly = st.orb_oy + ry;
    lz = st.orb_oz + rz;
  }
  if (std::fabs(st.radial) > 1e-8f) {
    float ax = lx - st.orb_ox;
    float ay = ly - st.orb_oy;
    float az = lz - st.orb_oz;
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len > 1e-6f) {
      const float k = st.radial * dt / len;
      lx += ax * k;
      ly += ay * k;
      lz += az * k;
    }
  }
}

void PoseIntegrator::apply_epilogue(float lx, float ly, float lz, float& world_x, float& main_y,
                                    float& world_z) const noexcept {
  const ParticleState& st = *st_;
  const SplitEffectParticle& p = *st.p;
  // Distinguish Unity Transform vs ParticleSystemScalingMode:
  // 1) System Transform localPosition through the parent chain is
  //    TransformPoint (ScriptReference/Transform.TransformPoint.html).
  //    Parent LineHight 27×fade and Line localScale.x=0.1 apply. hop
  //    birth main_y = origin+(2.5+local_pos_y)×27×fade, live dump
  //    11423-birth-v1. Do not drop this.
  // 2) ParticleSystemScalingMode.Local: "Scale the Particle System using
  //    only its own transform scale. (Ignores parent scale)."
  //    (ScriptReference/ParticleSystemScalingMode.html). Shape position /
  //    radius / scale, particle size, and velocity ignore parent 27×fade.
  // Hierarchy multiplies the whole PS-local offset (Shape + motion +
  // localPos). Local-sim + Local scaling keeps Shape/motion in PS-local
  // and only adds localPos×(parent_scale−1), same algebra as World+Local
  // in the ctor. Catalog is Local on every official system.
  const bool local_scaling =
      p.scaling_mode == static_cast<uint8_t>(SplitEffectScalingMode::Local);
  if (local_sim_ && !local_scaling) {
    ly *= live_y_scale_;
  } else if (local_sim_) {
    lx += p.local_pos_x * (birth_x_scale_ - 1.0f);
    ly += p.local_pos_y * (live_y_scale_ - 1.0f);
  }

  const float origin_fade = local_sim_ ? st.fade_scale : st.birth_fade;
  world_x = st.line_x + lx;
  const float origin = emitter_main_y(st.color_id, origin_fade);
  const float y_slope = emitter_main_y_from_ly_slope(st.color_id);
  main_y = origin + y_slope * ly;
  main_y += st.birth_origin_y_delta;
  world_z = lz + st.birth_parent_z;
  if (st.event_emit != 0) {
    // Event baseline is the child Transform pose, not Shape/motion.
    float loc_x = p.local_pos_x * birth_x_scale_;
    float loc_y = p.local_pos_y * (local_sim_ ? live_y_scale_ : birth_y_scale_);
    const float origin_with_pose = origin + st.birth_origin_y_delta;
    const float wy0 = origin_with_pose + y_slope * loc_y;
    world_x += st.event_wx - (st.line_x + loc_x);
    main_y += st.event_wy - wy0;
    world_z += st.event_wz - p.local_pos_z - st.birth_parent_z;
  }
}

void PoseIntegrator::sample_legacy(float age, float& world_x, float& main_y, float& world_z,
                                   IntegrationVelocityTrace* trace) noexcept {
  reset_to_birth();
  const float t_end = std::max(age, 0.0f);
  const int steps = std::max(1, static_cast<int>(std::ceil(t_end * hz_)));
  const float dt = t_end / static_cast<float>(steps);
  for (int i = 0; i < steps; ++i) {
    const float t_mid = dt * (static_cast<float>(i) + 0.5f);
    const float t_start = dt * static_cast<float>(i);
    const float t_end_step = dt * static_cast<float>(i + 1);
    run_step(i, t_mid, dt, t_end, steps, t_start, t_end_step);
  }
  apply_epilogue(hot_->lx, hot_->ly, hot_->lz, world_x, main_y, world_z);
  publish_sample(observe_age_, trace);
  WDS_PERF_COUNT("shuriken.integrate.steps", steps);
#if WDS_DIAG_ENABLED
  split_diag_add_count(SplitDiagCount::Steps, steps);
#endif  // WDS_DIAG_ENABLED
  reset_to_birth();
}

void PoseIntegrator::publish_world_velocity(IntegrationVelocityTrace& dst) const noexcept {
  dst = trace_;
  // World composition: published Y is d(main_y)/dt = slope * ly_vel.
  // Identity slope=+1 (golden byte-identical). live dump refix-d hop.
  const float s = emitter_main_y_from_ly_slope(st_->color_id);
  dst.stored_y *= s;
  dst.vol_y *= s;
  dst.noise_y *= s;
  dst.inherited_y *= s;
  dst.total_y *= s;
}

void PoseIntegrator::publish_sample(float age, IntegrationVelocityTrace* trace) noexcept {
  publish_world_velocity(g_last_integration_velocity);
  if (g_integration_observer != nullptr) {
    g_integration_observer(*st_, age, g_last_integration_velocity);
  }
  if (trace != nullptr) {
    publish_world_velocity(*trace);
  }
}

void PoseIntegrator::birth_world(float& world_x, float& main_y, float& world_z) const noexcept {
  apply_epilogue(birth_lx_, birth_ly_, birth_lz_, world_x, main_y, world_z);
}

void PoseIntegrator::begin_grid_log() noexcept {
  static thread_local std::vector<IntegratorGridState> log;
  log.clear();
  log.reserve(128);
  grid_log_ = &log;
  grid_log_origin_ = -1;
}

void PoseIntegrator::export_grid(IntegratorGridState& out) const noexcept {
  copy_hot_grid(*hot_, out);
  out.trace = trace_;
}

void PoseIntegrator::import_grid(const IntegratorGridState& in) noexcept {
  copy_grid_hot(in, *hot_);
  // Hydrate is not a store. Leave last_stored_step_ so the live pose after
  // advance is written once (play resume / 120 Hz RoD / pose_at). Thinning
  // this on cache hydrate regressed cold and carry-off.
  trace_ = in.trace;
}

void PoseIntegrator::import_carry_grid(const IntegratorGridState& in) noexcept {
  import_grid(in);
  last_stored_step_ = in.next_step;
}

void PoseIntegrator::maybe_restore(int k) noexcept {
  if (cache_key_ == nullptr || cache_key_->valid == 0 || legacy_ || k <= 0) {
    return;
  }
  // One hydrate per particle (or after a rewind to birth). Forward samples
  // on the same integrator — FD, play resume, 120 Hz RoD, pose_at — already
  // hold hot_->next_step and must not lock the shard again.
  if (hot_->next_step > 0 && hot_->next_step <= k) {
    return;
  }
  IntegratorGridState st;
  if (!split_eval_cache_lookup(*cache_key_, k, st)) {
    return;
  }
  if (st.next_step > hot_->next_step && st.next_step <= k) {
    import_grid(st);
  }
}

void PoseIntegrator::maybe_store() noexcept {
  if (cache_key_ == nullptr || cache_key_->valid == 0 || legacy_) {
    return;
  }
  // First checkpoint on this integrator is the live pose — always keep it.
  // Using hot_->next_step as the first gap (old) skipped every particle younger
  // than stride (~0.5 s at 60 Hz), so play walked from birth every frame.
  const int stride = split_eval_cache_stride();
  const int min_gap = stride > 1 ? stride : 2;
  if (last_stored_step_ >= 0 && hot_->next_step - last_stored_step_ < min_gap) {
    return;
  }
  if (pending_store_n_ >= kPendingStores) {
    flush_pending_stores();
  }
  auto* slot = reinterpret_cast<IntegratorGridState*>(
      pending_raw_ + static_cast<size_t>(pending_store_n_) * sizeof(IntegratorGridState));
  export_grid(*slot);
  ++pending_store_n_;
  last_stored_step_ = hot_->next_step;
}

void PoseIntegrator::flush_pending_stores() noexcept {
  if (pending_store_n_ <= 0 || cache_key_ == nullptr || cache_key_->valid == 0) {
    pending_store_n_ = 0;
    return;
  }
  split_eval_cache_store_many(*cache_key_,
                              reinterpret_cast<IntegratorGridState*>(pending_raw_), pending_store_n_);
  pending_store_n_ = 0;
}

bool PoseIntegrator::can_closed_form() const noexcept {
  if (!split_closed_form()) {
    return false;
  }
  if (legacy_ || st_ == nullptr || st_->p == nullptr) {
    return false;
  }
  if (g_integration_step_observer != nullptr) {
    return false;
  }
  if (vol_before_force_ || vol_before_limit_ || force_after_limit_ || noise_before_limit_) {
    return false;
  }
  const ParticleState& st = *st_;
  const SplitEffectParticle& p = *st.p;
  if ((p.flags & kSplitEffectFlagNoise) != 0 &&
      (std::fabs(p.noise_strength) > 1e-8f || std::fabs(p.noise_strength_y) > 1e-8f ||
       std::fabs(p.noise_strength_z) > 1e-8f || p.noise_strength_count > 0 ||
       p.noise_strength_y_count > 0 || p.noise_strength_z_count > 0)) {
    return false;
  }
  if (p.clamp_enabled != 0) {
    return false;
  }
  if (std::fabs(st.orb_x) > 1e-8f || std::fabs(st.orb_y) > 1e-8f || std::fabs(st.orb_z) > 1e-8f ||
      std::fabs(st.radial) > 1e-8f) {
    return false;
  }
  if (p.inherit_enabled != 0 && p.inherit_mode != 0) {
    return false;
  }
  if (p.force_randomize != 0) {
    return false;
  }
  if (p.force_enabled != 0 &&
      (p.force_x_count > 0 || p.force_y_count > 0 || p.force_z_count > 0 ||
       p.force_x_min_count > 0 || p.force_y_min_count > 0 || p.force_z_min_count > 0)) {
    return false;
  }
  if ((p.flags & kSplitEffectFlagVelocity) != 0 &&
      (p.vel_x_count > 0 || p.vel_y_count > 0 || p.vel_z_count > 0 || p.vel_x_min_count > 0 ||
       p.vel_y_min_count > 0 || p.vel_z_min_count > 0)) {
    return false;
  }
  return true;
}

void PoseIntegrator::apply_closed_form(int i0, int i1) noexcept {
  const int n = i1 - i0;
  if (n <= 0) {
    return;
  }
  const ParticleState& st = *st_;
  const SplitEffectParticle& p = *st.p;
  const float T = h_ * static_cast<float>(n);
  const float t_mid = h_ * (static_cast<float>(i0) + 0.5f * static_cast<float>(n));
  const float life = st.life > 1e-6f ? st.life : 1.0f;
  const float t01 = std::clamp(t_mid / life, 0.0f, 1.0f);
  float fx = 0.0f, fy = 0.0f, fz = 0.0f;
  eval_force_lane(st, t01, static_cast<uint32_t>(i0), fx, fy, fz);
  const float ax = fx;
  const float ay = fy + grav_y_;
  const float az = fz + grav_z_;
  float volx = 0.0f, voly = 0.0f, volz = 0.0f;
  if ((p.flags & kSplitEffectFlagVelocity) != 0) {
    volx = eval_minmax_axis(p.vel_x, p.vel_x_min, p.vel_x_random, p.vel_x_begin, p.vel_x_count,
                            p.vel_x_min_begin, p.vel_x_min_count, t01, st.vol_ux);
    voly = eval_minmax_axis(p.vel_y, p.vel_y_min, p.vel_y_random, p.vel_y_begin, p.vel_y_count,
                            p.vel_y_min_begin, p.vel_y_min_count, t01, st.vol_uy);
    volz = eval_minmax_axis(p.vel_z, p.vel_z_min, p.vel_z_random, p.vel_z_begin, p.vel_z_count,
                            p.vel_z_min_begin, p.vel_z_min_count, t01, st.vol_uz);
    if (p.vol_in_world != 0 || vol_world_candidate()) {
      float wx = 0.0f, wy = 0.0f, wz = 0.0f;
      world_force_to_lane(volx, voly, volz, wx, wy, wz);
      volx = wx;
      voly = wy;
      volz = wz;
    } else if (authored_local_transform_experiment() !=
               AuthoredLocalTransformExperiment::Native) {
      float lx = 0.0f, ly = 0.0f, lz = 0.0f;
      authored_local_module_to_lane(volx, voly, volz, lx, ly, lz);
      volx = lx;
      voly = ly;
      volz = lz;
    }
  }
  const float sm = p.vol_speed_mod;
  float dpx = 0.0f, dpy = 0.0f, dpz = 0.0f;
  split_effect_speedmod_delta_position(hot_->vx + volx, hot_->vy + voly, hot_->vz + volz, T, sm, dpx, dpy, dpz);
  dpx += 0.5f * ax * T * T * sm;
  dpy += 0.5f * ay * T * T * sm;
  dpz += 0.5f * az * T * T * sm;
  hot_->lx += dpx;
  hot_->ly += dpy;
  hot_->lz += dpz;
  hot_->vx += ax * T;
  hot_->vy += ay * T;
  hot_->vz += az * T;
  trace_.stored_x = hot_->vx;
  trace_.stored_y = hot_->vy;
  trace_.stored_z = hot_->vz;
  trace_.vol_x = volx;
  trace_.vol_y = voly;
  trace_.vol_z = volz;
  trace_.noise_x = 0.0f;
  trace_.noise_y = 0.0f;
  trace_.noise_z = 0.0f;
  trace_.inherited_x = 0.0f;
  trace_.inherited_y = 0.0f;
  trace_.inherited_z = 0.0f;
  trace_.total_x = hot_->vx + volx;
  trace_.total_y = hot_->vy + voly;
  trace_.total_z = hot_->vz + volz;
}

void PoseIntegrator::advance_to(int k) noexcept {
  if (legacy_ || k <= hot_->next_step) {
    return;
  }
  if (k < 0) {
    return;
  }
  if (can_closed_form()) {
    if (grid_log_ != nullptr) {
      grid_log_->clear();
    }
    apply_closed_form(hot_->next_step, k);
    hot_->next_step = k;
    return;
  }
  if (grid_log_ != nullptr && grid_log_->empty()) {
    grid_log_origin_ = hot_->next_step;
    IntegratorGridState cur;
    export_grid(cur);
    grid_log_->push_back(cur);
  }
  const int step_count = k;
  for (int i = hot_->next_step; i < k; ++i) {
    const float t_mid = h_ * (static_cast<float>(i) + 0.5f);
    const float t_start = h_ * static_cast<float>(i);
    const float t_end = h_ * static_cast<float>(i + 1);
    run_step(i, t_mid, h_, t_end, step_count, t_start, t_end);
    if (grid_log_ != nullptr) {
      hot_->next_step = i + 1;
      IntegratorGridState cur;
      export_grid(cur);
      grid_log_->push_back(cur);
    }
  }
  hot_->next_step = k;
}

void PoseIntegrator::sample(float age, float& world_x, float& main_y, float& world_z,
                            IntegrationVelocityTrace* trace) noexcept {
  observe_age_ = age;
  const float t_end = std::max(integration_candidate_age(age), 0.0f);
  if (legacy_) {
    sample_legacy(t_end, world_x, main_y, world_z, trace);
    return;
  }
  const int k = t_end <= 0.0f ? 0 : static_cast<int>(std::floor(t_end * hz_ + kGridIndexEps));
  float r = t_end - static_cast<float>(k) * h_;
  if (r <= kTailEps) {
    r = 0.0f;
  }
  if (k < hot_->next_step) {
    reset_to_birth();
  }
  maybe_restore(k);
  const int before = hot_->next_step;
  advance_to(k);
  int ran = std::max(0, hot_->next_step - before);
  if (r > 0.0f || k == 0) {
    // Exact tail, or the age-0 / on-grid birth sample. Old equal-split always
    // ran max(1, ceil(age*hz)) steps, so age 0 was one dt=0 evaluation that
    // filled VOL/Noise on the trace without moving the pose. Stretch reads
    // that trace. k>=1 && r==0 keeps the last full grid step (midpoint
    // (k-0.5)*h, same as equal-split when age is exactly k/hz).
    const float slx = hot_->lx;
    const float sly = hot_->ly;
    const float slz = hot_->lz;
    const float svx = hot_->vx;
    const float svy = hot_->vy;
    const float svz = hot_->vz;
    const IntegrationVelocityTrace strace = trace_;
    const int snext = hot_->next_step;
    const float t_start = static_cast<float>(k) * h_;
    const float dt = r;
    const float t_mid = t_start + 0.5f * dt;
    run_step(k, t_mid, dt, t_end, std::max(k, 1), t_start, t_start + dt);
    apply_epilogue(hot_->lx, hot_->ly, hot_->lz, world_x, main_y, world_z);
    publish_sample(age, trace);
    hot_->lx = slx;
    hot_->ly = sly;
    hot_->lz = slz;
    hot_->vx = svx;
    hot_->vy = svy;
    hot_->vz = svz;
    trace_ = strace;
    hot_->next_step = snext;
    ++ran;
  } else {
    apply_epilogue(hot_->lx, hot_->ly, hot_->lz, world_x, main_y, world_z);
    publish_sample(age, trace);
  }
  maybe_store();
  WDS_PERF_COUNT("shuriken.integrate.steps", ran);
#if WDS_DIAG_ENABLED
  split_diag_add_count(SplitDiagCount::Steps, ran);
#endif  // WDS_DIAG_ENABLED
}

void PoseIntegrator::finite_difference(float age, float dt, float x0, float y0, float z0, float& vx,
                                       float& vy, float& vz) noexcept {
  if (dt <= 1e-8f) {
    vx = 0.0f;
    vy = 0.0f;
    vz = 0.0f;
    return;
  }
  const IntegrationVelocityTrace saved_last = g_last_integration_velocity;
  if (legacy_) {
    float x1 = x0;
    float y1 = y0;
    float z1 = z0;
    sample(age + dt, x1, y1, z1, nullptr);
    vx = (x1 - x0) / dt;
    vy = (y1 - y0) / dt;
    vz = (z1 - z0) / dt;
    g_last_integration_velocity = saved_last;
    return;
  }
  const float t_end = std::max(integration_candidate_age(age), 0.0f);
  const int k = t_end <= 0.0f ? 0 : static_cast<int>(std::floor(t_end * hz_ + kGridIndexEps));
  float r = t_end - static_cast<float>(k) * h_;
  if (r <= kTailEps) {
    r = 0.0f;
  }
  if (k < hot_->next_step) {
    reset_to_birth();
  }
  maybe_restore(k);
  advance_to(k);
  const float slx = hot_->lx;
  const float sly = hot_->ly;
  const float slz = hot_->lz;
  const float svx = hot_->vx;
  const float svy = hot_->vy;
  const float svz = hot_->vz;
  const IntegrationVelocityTrace strace = trace_;
  const int snext = hot_->next_step;
  const float tail = r + dt;
  const float t_start = static_cast<float>(k) * h_;
  run_step(k, t_start + 0.5f * tail, tail, t_end + dt, k + 1, t_start, t_end + dt);
  float x1 = x0;
  float y1 = y0;
  float z1 = z0;
  apply_epilogue(hot_->lx, hot_->ly, hot_->lz, x1, y1, z1);
  hot_->lx = slx;
  hot_->ly = sly;
  hot_->lz = slz;
  hot_->vx = svx;
  hot_->vy = svy;
  hot_->vz = svz;
  trace_ = strace;
  hot_->next_step = snext;
  g_last_integration_velocity = saved_last;
  vx = (x1 - x0) / dt;
  vy = (y1 - y0) / dt;
  vz = (z1 - z0) / dt;
}

void integrate_pose(const ParticleState& st, float age, float& world_x, float& main_y,
                    float& world_z) noexcept {
  PoseIntegrator integ(st);
  integ.sample(age, world_x, main_y, world_z, nullptr);
}

}  // namespace wds::chart_render::shuriken

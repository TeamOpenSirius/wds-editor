#pragma once

#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/core/split_fade.hpp>

#include <cmath>
#include <cstdint>

namespace wds::chart_render::shuriken {

// Unity frame on OpenWDS default IsLowFrameRate=true (60 Hz). Frames are
// counted from ParticleSystem.Play (play time 0).
inline constexpr float kSplitEffectFrameSec = 1.0f / 60.0f;

// End of the frame that contains play time t.
inline float split_effect_frame_end_play_t(float t) noexcept {
  return std::ceil(t / kSplitEffectFrameSec - 1e-3f) * kSplitEffectFrameSec;
}

// First frame boundary strictly after play time t.
inline float split_effect_next_frame_play_t(float t) noexcept {
  return (std::floor(t / kSplitEffectFrameSec + 1e-3f) + 1.0f) * kSplitEffectFrameSec;
}

// live dump orb-11791-v1 hop, not 2022.3. OnFadeIn calls ParticleSystem.Play
// and Animator.Play("SplitEffect_fadeIn_anim", 0, 0) together, yet in every
// sample LineHight sits one frame behind particleSystem.time (11791 0.0169 s,
// 11740 0.0167 s; 11791_1 burst at ps 0.1 lands at fade(0.083)).
inline constexpr float kSplitEffectAnimatorLagSec = kSplitEffectFrameSec;
// Play time at which LineHight stops growing.
inline constexpr float kSplitEffectFadeEndPlayT =
    wds::chart_editor::kOfficialSplitFadeInSec + kSplitEffectAnimatorLagSec;

// LineHight fade scale at ParticleSystem play time t (animator lag applied).
float fade_scale_at_play_t(double t) noexcept;

struct EmitContext {
  int32_t color_id = 0;
  int32_t line_index = 0;
  int64_t line_start_ms = 0;
  // Main X from official_split_line_world_x (OpenWDS SetLineX).
  float line_x = 0.0f;
  // Live fade for Local origin (element 2.5×27×fade). World uses birth_fade.
  // scalingMode=Local: parent 27×fade applies to Transform localPosition
  // (TransformPoint) only, not to Shape / motion offsets.
  float fade_scale = 1.0f;
  float line_alpha = 1.0f;
  float now_t = 0.0f;
  bool want_trail = true;
  SplitEffectParentMotion parent_motion{};
  // One Play() generation for the whole split line. collect bumps this when
  // now_t re-enters kSplitEffectLinePlayWindowSec, not per particle.
  uint32_t line_play_gen = 1;
  // collect bind copies are not addresses in kSplitEffectParticles, so
  // catalog_index(p) is 0. push_evaluated fills this with particle_begin +
  // local so Ribbon chains stay one ParticleSystem (Unity ribbonCount=1).
  uint32_t catalog_index_override = 0;
  uint8_t has_catalog_index_override = 0;
};

// Observation-only capability state.  Unity 2022.3 Custom simulation space
// requires MainModule.customSimulationSpace, which is not present in the
// native catalog/evaluator.  The evaluator keeps its existing fallback path;
// this status only lets diagnostic callers label that limitation.
enum class RuntimeCapabilityStatus : uint8_t {
  Supported = 0,
  CustomSimulationSpaceUnavailable = 1,
};

using RuntimeCapabilityObserver = void (*)(const SplitEffectParticle&, const EmitContext&,
                                            RuntimeCapabilityStatus) noexcept;

RuntimeCapabilityStatus runtime_capability_status(const SplitEffectParticle& p) noexcept;

// Null by default.  Installing an observer has no effect on particle output
// and is intended for probes/tests that need to detect an EVAL-GAP.
void set_runtime_capability_observer(RuntimeCapabilityObserver observer) noexcept;

// True when a capability or integration observer is installed. evaluate() and
// evaluate_subemitters() stay single-threaded so callbacks are not re-entered
// from the collect pool.
bool observers_installed() noexcept;

// Deterministic Play+Simulate(now_t) for one catalog ParticleSystem.
// Writes at most max_out live particles; returns the count written.
int evaluate(const SplitEffectParticle& p, const EmitContext& ctx, SplitEffectLiveParticle* out,
             int max_out);

// Unity 2022.3 Sub Emitters Birth/Death at the parent particle pose.
// Collision/Trigger/Manual are catalogued but not fired.
// Birth schedule (live dump v1, re-subemit-work-report): bursts plus the
// child's own RoT/RoD attached to the parent particle. RoD hop is
// RoD × Σ|Δemitter.world| (same as primary emit). Death stays burst-only
// (2022.3 Sub Emitters page). playOnAwake children that are slot targets are
// not independently Played — collect skips evaluate() for those rows.
// Main curve u: parent particle age / startLifetime at the child's emission
// (live dump count-v1 hop, not 2022.3). Death → 1. Parent births stay
// listed after the parent particle dies so child startLifetime can finish
// (2022.3 SubEmitters are separate ParticleSystems).
int evaluate_subemitters(const SplitEffectParticle* particles, uint16_t count, int element_slot,
                         const EmitContext& ctx, SplitEffectLiveParticle* out, int max_out);

// In-process A/B for the resumable integrator. Overrides WDS_SPLIT_INTEGRATOR
// (legacy = old equal-split from birth; default / false = fixed 60 Hz grid).
void set_split_integrator_legacy_for_tests(bool legacy) noexcept;

// hz <= 0 restores the env/default clock (60, or OPENWDS_INTEGRATION_CLOCK).
void set_integration_hz_for_tests(float hz) noexcept;

// hop A/B for Noise curl (not 2022.3). 1 = 12-sample FD (golden-grid-v2-ref);
// 0 = analytic-gradient curl; -1 = env (unset = analytic; WDS_SPLIT_NOISE_EXACT=1 = FD).
void set_split_noise_exact_for_tests(int on) noexcept;
// hop: 1 = 4-wide SIMD Perlin (NEON/SSE/AVX2 via WDS_SPLIT_SIMD); 0 = scalar;
// -1 = env (unset = on when SIMD != scalar; WDS_SPLIT_NOISE_NEON=0 forces off).
void set_split_noise_neon_for_tests(int on) noexcept;

// Drop carry + pose-cache epoch so an A/B collect cannot resume the other mode.
void reset_split_eval_persistence_for_tests() noexcept;

}  // namespace wds::chart_render::shuriken

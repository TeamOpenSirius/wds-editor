#pragma once

#include "internal.hpp"
#include "split_eval_carry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wds::chart_render::shuriken {

// Persistent live-particle store per evaluated system instance.
// hop, not 2022.3. Default off; WDS_SPLIT_PERSIST=1 opts in.
//
// Primaries: resume the 1/60 grid. Children (Birth / RoT / burst / Death /
// RoD): persist when the recorded make_state / parent-pose inputs that
// class actually reads are bit-identical. World does not read current
// fade_scale (origin_fade = birth_fade; run_step ignores event_w / fade),
// so fade is omitted from that class's fingerprint and fade-in can hit.
// Local keeps fade in the fingerprint; while fade bits still change the
// store cannot hit — skip lookup and record (no remake-frame penalty).

inline constexpr uint8_t kPersistKindPrimary = 0;
inline constexpr uint8_t kPersistKindChild = 1;

struct SplitEvalPersistStats {
  std::size_t bytes_used = 0;
  std::size_t systems = 0;
  std::size_t particles = 0;
  std::size_t budget_bytes = 0;
  bool enabled = true;
  float rebuild_gap = 0.0f;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t rebuilds = 0;
  std::uint64_t commits = 0;
};

enum class PersistClaimKind : uint8_t {
  Miss = 0,
  Forward = 1,
  Rebuild = 2,
};

// Bit-identical snapshot of every ctx field child make_state + parent pose
// (and RoD walk, when included) read. Enumerated from evaluate.cpp /
// emit.cpp / integrate.cpp, not guessed. now_t is the advance target, not
// an input. line_alpha is refresh-only (not in apply_epilogue).
struct ChildPersistFp {
  uint32_t fade_scale_bits = 0;
  uint32_t line_x_bits = 0;
  uint8_t birth_pose_candidate = 0;
  uint32_t birth_origin_y_delta_bits = 0;
  uint8_t emitter_vel_candidate = 0;
  uint32_t emitter_vx_bits = 0;
  uint32_t emitter_vy_bits = 0;
  uint32_t emitter_vz_bits = 0;
  uint8_t rod_motion_candidate = 0;
  uint8_t rod_emitter_seed_candidate = 0;
  uint32_t distance_total_bits = 0;
  uint64_t motion_origin_seconds_bits = 0;
  uint64_t motion_now_seconds_bits = 0;
  uint32_t units_per_second_bits = 0;
  uint32_t motion_origin_z_bits = 0;
  uint8_t simulation_play_clock_candidate = 0;
  uint64_t simulation_play_origin_seconds_bits = 0;
  uint64_t visual_fade_origin_seconds_bits = 0;
  uint8_t want = 0;
};

inline uint32_t persist_f32_bits(float f) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

inline uint64_t persist_f64_bits(double f) noexcept {
  uint64_t u = 0;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

inline bool persist_sim_reads_live_fade(const SplitEffectParticle& p) noexcept {
  return p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Local);
}

inline ChildPersistFp make_child_persist_fp(const EmitContext& ctx,
                                            const SplitEffectParticle& p) noexcept {
  ChildPersistFp fp;
  fp.want = 1;
  // World / Custom: apply_epilogue origin_fade = birth_fade; run_step does
  // not read fade_scale. Local: origin uses current fade (element 2.5×27×
  // fade). scalingMode=Local still reads fade for Transform localPosition
  // × parent scale; Shape / motion offsets do not.
  fp.fade_scale_bits =
      persist_sim_reads_live_fade(p) ? persist_f32_bits(ctx.fade_scale) : 0u;
  fp.line_x_bits = persist_f32_bits(ctx.line_x);
  const SplitEffectParentMotion& m = ctx.parent_motion;
  fp.birth_pose_candidate = m.birth_pose_candidate ? 1 : 0;
  fp.birth_origin_y_delta_bits = persist_f32_bits(m.birth_origin_y_delta);
  fp.emitter_vel_candidate = m.emitter_vel_candidate ? 1 : 0;
  fp.emitter_vx_bits = persist_f32_bits(m.emitter_vx);
  fp.emitter_vy_bits = persist_f32_bits(m.emitter_vy);
  fp.emitter_vz_bits = persist_f32_bits(m.emitter_vz);
  fp.rod_motion_candidate = m.rod_motion_candidate ? 1 : 0;
  fp.rod_emitter_seed_candidate = m.rod_emitter_seed_candidate ? 1 : 0;
  fp.distance_total_bits = persist_f32_bits(m.distance_total);
  fp.motion_origin_seconds_bits = persist_f64_bits(m.motion_origin_seconds);
  fp.motion_now_seconds_bits = persist_f64_bits(m.motion_now_seconds);
  fp.units_per_second_bits = persist_f32_bits(m.units_per_second);
  fp.motion_origin_z_bits = persist_f32_bits(m.motion_origin_z);
  fp.simulation_play_clock_candidate = m.simulation_play_clock_candidate ? 1 : 0;
  fp.simulation_play_origin_seconds_bits = persist_f64_bits(m.simulation_play_origin_seconds);
  fp.visual_fade_origin_seconds_bits = persist_f64_bits(m.visual_fade_origin_seconds);
  return fp;
}

inline bool child_persist_fp_eq(const ChildPersistFp& a, const ChildPersistFp& b) noexcept {
  return a.want == b.want && a.fade_scale_bits == b.fade_scale_bits &&
         a.line_x_bits == b.line_x_bits && a.birth_pose_candidate == b.birth_pose_candidate &&
         a.birth_origin_y_delta_bits == b.birth_origin_y_delta_bits &&
         a.emitter_vel_candidate == b.emitter_vel_candidate &&
         a.emitter_vx_bits == b.emitter_vx_bits && a.emitter_vy_bits == b.emitter_vy_bits &&
         a.emitter_vz_bits == b.emitter_vz_bits && a.rod_motion_candidate == b.rod_motion_candidate &&
         a.rod_emitter_seed_candidate == b.rod_emitter_seed_candidate &&
         a.distance_total_bits == b.distance_total_bits &&
         a.motion_origin_seconds_bits == b.motion_origin_seconds_bits &&
         a.motion_now_seconds_bits == b.motion_now_seconds_bits &&
         a.units_per_second_bits == b.units_per_second_bits &&
         a.motion_origin_z_bits == b.motion_origin_z_bits &&
         a.simulation_play_clock_candidate == b.simulation_play_clock_candidate &&
         a.simulation_play_origin_seconds_bits == b.simulation_play_origin_seconds_bits &&
         a.visual_fade_origin_seconds_bits == b.visual_fade_origin_seconds_bits;
}

// LineHight fade is constant 1.0 after kSplitEffectFadeEndPlayT. Local
// children read that live fade, so a changing-fade frame cannot hit.
inline bool persist_child_fade_stable(const EmitContext& ctx) noexcept {
  constexpr uint32_t kOne = 0x3f800000u;
  return persist_f32_bits(ctx.fade_scale) == kOne &&
         ctx.now_t + 1e-6f >= kSplitEffectFadeEndPlayT;
}

// World: fade not in fp → can hit during fade-in. Local: only after fade
// bits freeze. When this is false, skip persist claim/lookup/record.
inline bool persist_child_can_hit(const EmitContext& ctx, const SplitEffectParticle& p) noexcept {
  if (persist_sim_reads_live_fade(p)) {
    return persist_child_fade_stable(ctx);
  }
  return true;
}

struct alignas(64) PersistSlot {
  IntegratorGridState grid{};
  Birth b{};
  Birth parent_b{};
  uint32_t seed = 0;
  float emit_u = 0.0f;
  ParticleState st{};
  uint16_t slot_i = 0;
  uint8_t has_grid = 0;
  uint8_t is_child = 0;
  uint32_t gen = 1;
};
static_assert(sizeof(PersistSlot) % 64 == 0, "view size stays 64-aligned");

// Cold persist fields (not the integrate-loop R+W pose). Stored in a
// separate std::vector from IntegratorHot.
struct PersistCold {
  Birth b{};
  Birth parent_b{};
  uint32_t seed = 0;
  float emit_u = 0.0f;
  ParticleState st{};
  IntegrationVelocityTrace trace{};
  uint16_t slot_i = 0;
  uint8_t has_grid = 0;
  uint8_t is_child = 0;
  uint32_t gen = 1;
};

struct PersistClaim {
  uint32_t store_id = 0xffffffffu;
  uint32_t gen = 0;
  float prev_now = -1.0e30f;
  PersistClaimKind kind = PersistClaimKind::Miss;
};

struct PersistParticleView {
  ParticleState st{};
  IntegratorGridState grid{};
  uint8_t has_grid = 0;
};

bool split_eval_persist_enabled() noexcept;
void split_eval_persist_clear() noexcept;
void split_eval_persist_precommit(std::size_t stores, std::size_t slots_each) noexcept;
bool split_eval_persist_precommit_one_store(std::size_t slots_each) noexcept;
void split_eval_persist_set_enabled_for_tests(int on) noexcept;
SplitEvalPersistStats split_eval_persist_stats() noexcept;

bool persist_primary_eligible(const SplitEffectParticle& p) noexcept;
bool persist_child_eligible(const SplitEffectParticle& p) noexcept;

// Claim exclusive ownership of the store for this task/frame. Backward time
// or instance change drops the store immediately (Rebuild) and does not cull
// or merge. fp != null (child): mismatch vs recorded inputs also Rebuild.
void split_eval_persist_claim(const SystemCarryKey& key, float now, PersistClaim& out) noexcept;
void split_eval_persist_claim(const SystemCarryKey& key, float now, const ChildPersistFp& fp,
                              PersistClaim& out) noexcept;

const PersistSlot* persist_slot_at(const PersistClaim& c, int idx) noexcept;
IntegratorHot* persist_claimed_hot(int idx) noexcept;
int persist_slot_count(const PersistClaim& c) noexcept;
int persist_find_slot(const PersistClaim& c, const Birth& b, uint32_t seed) noexcept;
int persist_find_child_slot(const PersistClaim& c, const Birth& b, uint32_t seed,
                            const Birth& parent_b, uint16_t slot_i) noexcept;

void persist_cull_dead(PersistClaim& c, const SplitEffectParticle& p, float now) noexcept;

void persist_commit(PersistClaim& c, float now, const std::vector<PrimaryEvalJob>& jobs) noexcept;
void persist_finish(PersistClaim& c, const SystemCarryKey& key, float now,
                    const std::vector<PrimaryEvalJob>& jobs) noexcept;
void persist_finish(PersistClaim& c, const SystemCarryKey& key, float now, const ChildPersistFp& fp,
                    const std::vector<PrimaryEvalJob>& jobs) noexcept;
void persist_finish(PersistClaim& c, const SystemCarryKey& key, float now, const ChildPersistFp& fp,
                    const std::vector<PrimaryEvalJob>& jobs,
                    const std::vector<PersistCand>& cands) noexcept;
void persist_release(PersistClaim& c) noexcept;
// Miss: record key+now so the next frame can Forward, without copying slots.
// First frame stays remake-cost (10850 persist=1 first-window spike).
void persist_touch_empty(const SystemCarryKey& key, float now) noexcept;

bool persist_store_exists(const SystemCarryKey& key) noexcept;
bool persist_peek_cands(const SystemCarryKey& key, const ChildPersistFp& fp, float now,
                        std::vector<PersistCand>& out) noexcept;

bool split_eval_persist_lookup_particle(const SystemCarryKey& key, float now, const Birth& b,
                                        uint32_t seed, PersistParticleView& out) noexcept;

}  // namespace wds::chart_render::shuriken

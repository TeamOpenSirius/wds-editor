#pragma once

#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/core/preview_snapshot.hpp>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace wds::chart_render {

namespace openwds_split_effects {
class Controller;
}

// Temporary replay-only parent-motion contract.  Production callers leave
// this disabled and retain the stateless authored fade/travel evaluator.
// The crossing and update-order behavior is a candidate, not a Unity parity
// claim; the temporary adapter records it explicitly in its introspection.
struct SplitEffectParentMotion {
  bool rod_motion_candidate = false;
  // Temporary replay-only candidate: derive the RoD random draw stream from
  // the stable logical emitter identity (line + chart start + catalog system)
  // instead of sharing one catalog-only sequence across all lines. This is
  // disabled by default until the Unity per-system draw lifetime is verified.
  bool rod_emitter_seed_candidate = false;
  bool birth_pose_candidate = false;
  // live dump v1 hop: Σ|Δps.worldPosition| of the ParticleSystem Transform.
  // Not lossyY / chord / worldDistanceAccum / max(wda, 2·Σdt). Not 2022.3.
  float distance_total = 0.0f;
  double motion_origin_seconds = 0.0;
  double motion_now_seconds = 0.0;
  float units_per_second = 10.0f;
  float motion_origin_z = 0.0f;
  float birth_origin_y_delta = 0.0f;
  bool simulation_play_clock_candidate = false;
  double simulation_play_origin_seconds = 0.0;
  // Authored visual Animator/LineHight origin on the event timeline. This is
  // independent of the temporary particle simulation clock.
  double visual_fade_origin_seconds = 0.0;
  // live dump v1 (synth-infer-v15 Wave 1 / Wave 5). Explicit emitter velocity
  // for Inherit (Transform / known parent_vel). When unset, integrate uses
  // official line-fade Y. Not 2022.3.
  bool emitter_vel_candidate = false;
  float emitter_vx = 0.0f;
  float emitter_vy = 0.0f;
  float emitter_vz = 0.0f;
};

// Preview draw-pool hop (not Unity Main.maxParticles). Unity 2022.3
// maxParticles is "in the system at once"; emit/evaluate apply that as a
// concurrent-alive cap (drop a birth that would exceed it, do not defer).
// 16384 sits just above the official-chart peak ~15500.
// apply_over_cap: first-come between splits, per-line share, then
// birth-ordered selection (ribbon = one contiguous newest/head run).
// Note hit VFX (BomSquare / BomFlare) are a separate uncapped list.
inline constexpr int kSplitEffectParticlePool = 16384;
// Preview seed window in now_t seconds (t=0 is fadeIn / OnFadeIn Play).
// Engineering pad (hop, not 2022.3), same upper bound as
// kSplitEffectFadeinCollectCapSec. Official catalog looping is 0; conservative
// tree last-alive max is 9.0s. Draw of the line plate is not gated here.
// Unity 2022.3 useAutoRandomSeed remints on Play(), not seek/pause. Preview
// remints when a line leaves this window and later re-enters; a line still
// on screen past 10s keeps the last generation.
inline constexpr float kSplitEffectLinePlayWindowSec = 10.0f;
// Engineering cap (hop, not 2022.3) on fade-in extras collect. OnFadeIn
// Play()s every LineEffect once; catalog looping is 0. Conservative
// tree last-alive max is 9.0s (11710/11711/11712). This is not Unity
// Duration. Same 10s bound as the seed remint window above. Draw of the
// line plate is independent. Raise both if a future catalog tree exceeds 10s.
inline constexpr float kSplitEffectFadeinCollectCapSec = 10.0f;
inline bool split_effect_fadein_extras_open(float now_t) noexcept {
  return now_t >= 0.0f && now_t <= kSplitEffectFadeinCollectCapSec;
}
// Storage cap only. Unity Trails has no published max; official Particle-mode
// worst path is ~481 verts (startSpeed 80 / minVertex 0.2).
inline constexpr int kSplitEffectTrailMaxPoints = 512;

// Non-owning trail channel. Backing store is a per-worker / per-frame arena so
// LiveParticle stays cheap to default-construct, move, and destroy.
struct SplitEffectTrailChan {
  float* data = nullptr;
  uint16_t n = 0;

  float& operator[](size_t i) noexcept { return data[i]; }
  const float& operator[](size_t i) const noexcept { return data[i]; }
  size_t size() const noexcept { return n; }
  bool empty() const noexcept { return n == 0; }
  float* begin() noexcept { return data; }
  float* end() noexcept { return data + n; }
  const float* begin() const noexcept { return data; }
  const float* end() const noexcept { return data + n; }
  const float* data_ptr() const noexcept { return data; }
  void clear() noexcept {
    data = nullptr;
    n = 0;
  }
};

struct SplitEffectTrailArena {
  void reset() noexcept;
  float* alloc(size_t n);
  void reserve_chunks(size_t n);
  bool reserve_one_chunk();
  size_t chunk_count() const noexcept;

  static constexpr size_t kChunk = 65536;

 private:
  struct Chunk {
    std::unique_ptr<float[]> mem;
    size_t cap = 0;
    size_t used = 0;
  };
  std::vector<Chunk> chunks_;
  size_t cur_ = 0;
};

struct SplitEffectLiveParticle;

void split_effect_trail_begin_frame() noexcept;
SplitEffectTrailArena& split_effect_thread_trails();
void split_effect_clear_live_trails(SplitEffectLiveParticle& p) noexcept;
void split_effect_alloc_live_trails_xyz(SplitEffectLiveParticle& p, uint16_t n);
void split_effect_alloc_live_trails_attrs(SplitEffectLiveParticle& p, uint16_t n);
void split_effect_adopt_live_trails(SplitEffectTrailArena& dst, SplitEffectLiveParticle& p);

// One reconstructed catalog particle (Play once at OnFadeIn, looping=false).
struct SplitEffectLiveParticle {
  // LaneGroup / Main X (SetLineX / _Line{n}.localPosition.x + PS-local lx).
  // Historical name: not Unity World. Do not multiply LaneGroup or 27 again.
  float world_x = 0.0f;
  float main_y = 0.0f;
  float z = 0.0f;
  float birth_t = 0.0f;
  // Pairing provenance for Unity/native trajectory probes. These fields are
  // observational metadata and do not affect rendering or integration.
  int32_t effect_id = 0;
  int32_t line_index = 0;
  uint32_t catalog_index = 0;
  uint8_t element_index = 0;
  uint32_t seed = 0;
  float age = 0.0f;
  // Sampled Main.startLifetime and the live remaining lifetime. These are
  // observation fields populated from ParticleState; they do not affect
  // integration or rendering.
  float sampled_lifetime = 0.0f;
  float remaining_lifetime = 0.0f;
  // GetCurrentSize analogue: start × SOL only (live dump v1 / v19).
  // Noise sizeAmount apply measure 未证实; draw `size` is also start × SOL.
  // Observation may be 0. Not a 2022.3 sizeAmount equation.
  float current_size = 0.0f;
  float current_size_y = 0.0f;
  float current_size_z = 0.0f;
  // live dump v1 observation (re-observe-writer). Birth startSize / startColor
  // and ROL+RBS+noise angular velocity (rad/s). Dump converts rotation and
  // ω to degrees. Not a 2022.3 equation.
  float start_size = 0.0f;
  float start_size_y = 0.0f;
  float start_size_z = 0.0f;
  float start_r = 1.0f;
  float start_g = 1.0f;
  float start_b = 1.0f;
  float start_a = 1.0f;
  // live dump v2: GetCurrentColor = Color32(start × COL). Custom1 brightness
  // hop stays on draw `r/g/b/a` (same split as sizeAmount vs current_size).
  // Not a 2022.3 equation.
  float current_r = 1.0f;
  float current_g = 1.0f;
  float current_b = 1.0f;
  float current_a = 1.0f;
  float ang_vx = 0.0f;
  float ang_vy = 0.0f;
  float ang_vz = 0.0f;
  // Applied birth-pose provenance for temporary replay diagnostics.
  float birth_fade = 1.0f;
  float birth_parent_z = 0.0f;
  float birth_origin_y_delta = 0.0f;
  // Distance along the authored LineHight motion at birth. -1 means the
  // system is not rate-over-distance (or the value is not defined).
  float effective_distance = -1.0f;
  float size = 0.0f;
  float max_particle_size = 0.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
  uint16_t uv_name_index = kSplitEffectNoUvName;
  uint8_t blend = 0;
  uint8_t sorting_order = 1;
  uint16_t flags = 0;
  uint8_t render_mode = 0;
  // Main-space finite difference (evaluate fill_live). Trail
  // minVertexDistance consumes this triple. Stretch kettlewell uses
  // stored/total via official_split_effect_stretch_applied_length_scale
  // (live dump v22: authored LS × Sprite.textureRect h/w; 11320 ID
  // branch withdrawn 2026-10-08), not this
  // pose derivative.
  float vel_x = 0.0f;
  float vel_y = 0.0f;
  float vel_z = 0.0f;
  // live dump v1 observation (write-intents/physics.md). integrate_pose
  // stored / VOL / Noise / total in LaneGroup. Dump reads these fields
  // instead of pairing observer hits by pointer catalog_index.
  // Not a 2022.3 equation.
  float stored_vx = 0.0f;
  float stored_vy = 0.0f;
  float stored_vz = 0.0f;
  float vol_vx = 0.0f;
  float vol_vy = 0.0f;
  float vol_vz = 0.0f;
  float noise_vx = 0.0f;
  float noise_vy = 0.0f;
  float noise_vz = 0.0f;
  float total_vx = 0.0f;
  float total_vy = 0.0f;
  float total_vz = 0.0f;
  float length_scale = 2.0f;
  float velocity_scale = 0.0f;
  float pivot_x = 0.0f;
  float pivot_y = 0.0f;
  float pivot_z = 0.0f;
  float custom1_x = 0.0f;
  float custom1_y = 0.0f;
  // Birth Main-curve time (startSize TwoCurves). Dump only.
  float emit_u = 0.0f;
  uint8_t inherit_props = 0;
  float butterfly_wing_x = 0.0f;
  float sol_y = 1.0f;
  // startSizeY × SOL Y (no UV aspect). Stretch length is width × Length Scale
  // (Unity 2022.3 lengthScale vs width). 3D Y is height, not along-velocity.
  float size_y = 1.0f;
  // startSize × SOL Z. Unity 2022.3 Size over Lifetime: "Z will only be used
  // for Mesh particles."
  float size_z = 1.0f;
  uint8_t size_3d = 0;
  float roll = 0.0f;
  float tilt_x = 0.0f;
  float tilt_y = 0.0f;
  uint8_t render_alignment = 0;
  uint16_t trail_points = 0;
  SplitEffectTrailChan trail_x;
  SplitEffectTrailChan trail_y;
  SplitEffectTrailChan trail_z;
  // Particle-mode widthOverTrail evaluated at each point (index 0=head).
  // Ribbon mode keeps its per-particle width in trail_width; stitch_ribbons
  // evaluates the curve at 3D arc/total after particles are ordered by age.
  SplitEffectTrailChan trail_widths;
  SplitEffectTrailChan trail_color_r;
  SplitEffectTrailChan trail_color_g;
  SplitEffectTrailChan trail_color_b;
  SplitEffectTrailChan trail_color_a;
  float trail_width = 0.0f;
  // Ribbon chain (Unity TrailRenderer / Trails module, 0 corner vertices).
  // One id per evaluate() per split line; vertex 0 is oldest. 0 count =
  // Particle-mode path.
  uint32_t trail_chain = 0;
  uint16_t trail_vertex = 0;
  uint16_t trail_count = 0;
  float trail_r = 1.0f;
  float trail_g = 1.0f;
  float trail_b = 1.0f;
  float trail_a = 1.0f;
  // textureMode Stretch: V along the whole ribbon. hop (align2-n6): 0=oldest
  // living particle, 1=newest. Not 2022.3 (page does not name the ends).
  float trail_u0 = 0.0f;
  float trail_u1 = 1.0f;
  // Copy of catalog Trails colorOverTrail keys for Particle-mode per-vertex
  // eval (Ribbon applies them in stitch_ribbons).
  uint16_t trail_over_alpha_begin = 0;
  uint8_t trail_over_alpha_count = 0;
  uint16_t trail_over_color_begin = 0;
  uint8_t trail_over_color_count = 0;
  uint16_t trail_over_min_alpha_begin = 0;
  uint8_t trail_over_min_alpha_count = 0;
  uint16_t trail_over_min_color_begin = 0;
  uint8_t trail_over_min_color_count = 0;
  uint8_t trail_over_state = 0;
  uint8_t trail_over_mode = 0;
  uint8_t trail_over_min_mode = 0;
  float trail_over_r = 1.0f;
  float trail_over_g = 1.0f;
  float trail_over_b = 1.0f;
  float trail_over_r_min = 1.0f;
  float trail_over_g_min = 1.0f;
  float trail_over_b_min = 1.0f;
  float trail_color_u = 0.0f;
  float trail_over_a0 = 1.0f;
  float trail_over_a1 = 1.0f;
  // Approved unofficial Stretch hops (not Unity 2022.3). Copied from catalog
  // by evaluate fill_live when that hop has landed; defaults keep Freeform
  // off and cameraVelocityScale at 0.
  float camera_velocity_scale = 0.0f;
  uint8_t freeform_stretching = 0;
  uint8_t rotate_with_stretch = 0;
  float noise_rotation_amount = 0.0f;
  float noise_size_amount = 0.0f;
  // live dump psr-flip-v1 hop (not 2022.3). Birth-stable Bernoulli bits from
  // catalog flip_x/y × seed. Billboard+Stretch swap atlas U/V corners.
  // Mesh / Trails unproven — leave 0 and do not apply.
  uint8_t flip_u = 0;
  uint8_t flip_v = 0;
};

// Caller-owned state for continuous preview playback.  The normal collector
// remains stateless and is the API to use for seek/scrub and deterministic
// snapshots.  A playback caller should reset this state at Play and at any
// seek, then call the wrapper once per monotonically advancing frame.
struct SplitEffectPlaybackState {
  double last_now_sec = -std::numeric_limits<double>::infinity();

  void reset() noexcept { last_now_sec = -std::numeric_limits<double>::infinity(); }
};

// Reconstruct live particles for one snapshot split. Gating is only
// official_split_effect_for_each_emit. Returns count written to out (≤ max_out,
// pool 16384). Over-cap keeps a round-robin mix per line and ParticleSystem so
// later layers (petals / thorns) are not evicted by earlier RoD fog.
// Seek before Play yields 0. Non-looping dies after duration + lifetime.
int collect_split_effect_particles(const wds::chart_editor::PreviewSplitLaneInstance& split,
                                   int quality, double now_sec, SplitEffectLiveParticle* out,
                                   int max_out,
                                   const SplitEffectParentMotion* parent_motion = nullptr);

// Continuous-play adapter for the production preview.  It calls the
// stateless collector.  First-sample RateOverDistance particles are kept:
// OpenWDS OnFadeIn then Simulate includes the first LineHight
// |Δps.worldPosition| (live dump pin).  The caller owns state and must call
// state.reset() on Play and seek.  Calls for a non-monotonic timeline are
// rejected with zero output until the caller resets the state, avoiding an
// implicit seek policy.
int collect_split_effect_particles_playback(
    const wds::chart_editor::PreviewSplitLaneInstance& split, int quality, double now_sec,
    SplitEffectPlaybackState& state, SplitEffectLiveParticle* out, int max_out);

// One preview frame of collect across every split, matching
// PlaybackPreviewView::draw_split_particles: same per-split call, shared
// remaining budget, same order. Particles of split k land at split_begin[k]
// (production overwrites from index 0, then emits before the next split).
// Reuse the same struct across frames so pool keeps high-water capacity
// (grows by written, never pre-sized to the cap).
struct SplitEffectFrameCollect {
  std::vector<SplitEffectLiveParticle> pool;
  SplitEffectTrailArena trails;
  std::vector<int> split_begin;
  std::vector<int> split_count;
  int total = 0;
};

// THREAD CONTRACT: call from one thread at a time (the GUI/preview owner thread).
// Not safe for concurrent callers: auto-seed session, latches, controller cache and the
// shared RNG are unsynchronised process-global state. Internally fans out to the split
// collect pool. Debug builds assert on concurrent entry from a second thread.
void collect_split_effect_frame(
    const std::vector<wds::chart_editor::PreviewSplitLaneInstance>& splits, int quality,
    double now_sec, bool playing, SplitEffectPlaybackState& state, SplitEffectFrameCollect& io);

// Drops integrator checkpoints. Does not reset auto-seed session.
void split_effect_clear_caches() noexcept;

// pure time seek drops time-ordered state only; the eval cache key already carries
// the epoch and the full input hash, so retaining it is equivalent to a cold rebuild
// (proved by wds_split_effect_seek_cache_tests).
void split_effect_clear_seek_state() noexcept;

// Wake collect workers and keep them INTERACTIVE for hold_ms so the first
// parallel eval does not pay idle-core clock/QoS ramp. Callers must only
// arm when fade-in extras will actually parallelize (not on empty / past
// last-alive / past kSplitEffectFadeinCollectCapSec / hint-inline).
void split_effect_prepare_heavy_collect(int hold_ms = 50) noexcept;

// Previous frame work time in milliseconds (editor: tick+render; bench:
// collect). Drives idle-sliced cache reap. 0 = unknown.
void split_effect_note_prev_frame_ms(double frame_ms) noexcept;

// Conservative evaluate skip for systems that cannot produce a live particle
// at now_t. Default on. Does not touch Phase A bind / auto-seed.
void split_effect_set_dead_system_skip(bool enabled) noexcept;
bool split_effect_dead_system_skip_enabled() noexcept;

struct SplitEffectEvalCacheStats {
  std::size_t bytes_used = 0;
  std::size_t particle_entries = 0;
  std::size_t checkpoints = 0;
  std::uint32_t epoch = 0;
  bool enabled = true;
  std::size_t budget_bytes = 0;
  std::uint64_t lookup_ops = 0;
  std::uint64_t lookup_hits = 0;
  std::uint64_t lookup_contend = 0;
  std::uint64_t store_ops = 0;
  std::uint64_t store_contend = 0;
  // Diagnostics only (WDS_FRAME_DIAG). Extends the counters above; no
  // second store. hits+misses == lookup_ops when the cache is enabled.
  // Miss = no slot for the key. Stale = the slot existed and its key
  // changed (epoch / input hash), so find() replaced it — that is the only
  // real "invalidated" signal because keys carry the epoch.
  std::uint64_t lookup_misses = 0;
  std::uint64_t lookup_stale = 0;
  std::uint64_t rollup_hits = 0;
  std::uint64_t rollup_misses = 0;
  // Entry drops with the reason. Byte-budget eviction and idle reap are
  // the two ways an entry leaves the map.
  std::uint64_t evict_ops = 0;
  std::uint64_t evict_budget = 0;
  std::uint64_t evict_reap = 0;
  std::uint64_t evict_alloc = 0;
  std::uint64_t clear_ops = 0;
  std::uint64_t epoch_bumps = 0;
  std::size_t carry_bytes_used = 0;
  std::size_t carry_systems = 0;
  std::size_t carry_particles = 0;
  std::size_t carry_budget_bytes = 0;
  bool carry_enabled = true;
  float carry_max_gap = 0.25f;
  std::uint64_t carry_hits = 0;
  std::uint64_t carry_misses = 0;
  std::uint64_t carry_fallbacks = 0;
  std::uint64_t carry_commits = 0;
  std::uint64_t carry_walk_hits = 0;
  std::uint64_t carry_walk_misses = 0;
  std::uint64_t carry_walk_commits = 0;
  std::size_t persist_bytes_used = 0;
  std::size_t persist_systems = 0;
  std::size_t persist_particles = 0;
  std::size_t persist_budget_bytes = 0;
  bool persist_enabled = true;
  float persist_rebuild_gap = 0.0f;
  std::uint64_t persist_hits = 0;
  std::uint64_t persist_misses = 0;
  std::uint64_t persist_rebuilds = 0;
  std::uint64_t persist_commits = 0;
  int pool_cap = 0;
  int pool_participants = 0;
  int pool_last_call_participants = 0;
  int pool_frame_max_participants = 0;
  std::uint64_t pool_frame_wakeups = 0;
  int pool_p_cores = 0;
  int pool_e_cores = 0;
  const char* simd_path = "";
};

const char* split_effect_simd_name() noexcept;
void split_effect_set_simd_for_tests(const char* name) noexcept;

SplitEffectEvalCacheStats split_effect_eval_cache_stats() noexcept;

// Diagnostics only (WDS_FRAME_DIAG). Nothing here allocates, and nothing
// reads a clock or writes a counter unless wds::common::perf::enabled().
// ---------------------------------------------------------------------------
// Diagnostics-only counters for the internal helpers that this task may not
// extend in their own headers (split_collect_pool.hpp / split_eval_cache.hpp
// are outside the edit boundary). Declared here, defined in those .cpp files.
// Every field is written only when wds::common::perf::enabled().
// ---------------------------------------------------------------------------
namespace shuriken {
struct SplitEvalCacheDiag {
  std::uint64_t lookup_misses = 0;   // no checkpoint for this key
  std::uint64_t lookup_stale = 0;    // slot present, key replaced (epoch)
  std::uint64_t rollup_hits = 0;     // C16 rollup reused
  std::uint64_t rollup_misses = 0;   // C16 rolled up into the run
  std::uint64_t evict_ops = 0;       // budget + reap + clear
  std::uint64_t evict_budget = 0;    // dropped for the byte budget
  std::uint64_t evict_reap = 0;      // dropped by idle reap (untouched gen)
  std::uint64_t evict_clear = 0;     // dropped by clear()/epoch bump
  std::uint64_t clear_ops = 0;
  std::uint64_t epoch_bumps = 0;
};
void split_eval_cache_diag(SplitEvalCacheDiag& out) noexcept;
}  // namespace shuriken

// Pool counters for one frame; see split_collect_pool.cpp. tasks/wakeups are
// per frame, the rest are per frame too except POOLSTAT's cumulative view.
struct SplitCollectPoolDiag {
  std::uint64_t tasks = 0;             // pool jobs published this frame
  std::uint64_t wakeups = 0;           // cv_wake_.notify_one calls
  std::uint64_t wait_us = 0;           // caller blocked in cv_done_ (µs)
  std::uint64_t spin_us = 0;           // caller ran claims instead (µs)
  std::uint64_t inline_calls = 0;      // hint below the 512 µs threshold
  std::uint64_t serial_calls = 0;      // cap<=1 / single item: no pool
  std::uint64_t run_inline_calls = 0;  // kMaxJobs exhausted: ran inline
  std::uint64_t par_calls = 0;         // calls that published a Job
  std::uint64_t sum_item_us = 0;       // Σ top-level claim µs
  std::uint64_t wall_us = 0;           // Σ wall µs of those waves
  // Who ran the work: claims/item counts for the calling (GUI) thread vs the
  // pool workers. items_caller / (items_caller + items_worker) is the share of
  // the collect the GUI thread did itself.
  std::uint64_t claims_caller = 0;
  std::uint64_t claims_worker = 0;
  std::uint64_t items_caller = 0;
  std::uint64_t items_worker = 0;
  std::uint64_t join_us = 0;           // caller blocked in cv_done_ (µs)
};

SplitCollectPoolDiag split_collect_pool_diag_frame() noexcept;

struct SplitCollectPoolStat {
  std::uint64_t tasks = 0;
  std::uint64_t participants_max = 0;
  std::uint64_t wait_us_sum = 0;
  std::uint64_t spin_us_sum = 0;
  std::uint64_t inline_calls = 0;
  std::uint64_t serial_calls = 0;
};

// Cumulative since the last split_collect_diag_reset().
SplitCollectPoolStat split_collect_pool_stat() noexcept;
void split_collect_diag_reset() noexcept;

struct SplitEffectPhaseDiag {
  std::uint64_t frame = 0;
  int splits = 0;
  int systems = 0;
  int particles = 0;
  int playing = 0;
  int quality = 0;
  // Last completed collect was a post-seek/post-epoch cold rebuild.
  int cold = 0;
  // Stage totals for the last completed collect (µs). Nested scopes are
  // included in their parent, e.g. evaluate includes make_state / integrate.
  std::int64_t collect_us = 0;
  std::int64_t prepare_us = 0;
  std::int64_t evaluate_us = 0;
  std::int64_t assemble_us = 0;
  std::int64_t integrate_us = 0;
  std::int64_t subemit_us = 0;
  std::int64_t subemit_plan_us = 0;
  std::int64_t subemit_realize_us = 0;
  std::int64_t realize_us = 0;
  std::int64_t copyout_us = 0;
  std::int64_t compact_us = 0;
  std::int64_t traverse_us = 0;
  std::int64_t stitch_us = 0;
  std::int64_t cache_lookup_us = 0;
  // Cumulative cache counters sampled at the end of that frame.
  std::uint64_t cache_lookup_hits = 0;
  std::uint64_t cache_lookup_misses = 0;
  std::uint64_t cache_evict = 0;
  std::uint64_t cache_bytes = 0;
  // Pool counters for that frame.
  std::uint64_t pool_tasks = 0;
  std::uint64_t pool_wait_us = 0;
  int pool_participants = 0;
  double pool_par_eff = 0.0;
  // Who ran the work: items claimed by the calling (GUI) thread vs pool
  // workers, and the µs the caller blocked in the join.
  std::uint64_t pool_claims_caller = 0;
  std::uint64_t pool_claims_worker = 0;
  std::uint64_t pool_items_caller = 0;
  std::uint64_t pool_items_worker = 0;
  std::uint64_t pool_join_us = 0;
  std::uint64_t pool_inline_calls = 0;
  std::uint64_t pool_serial_calls = 0;
  std::uint64_t pool_sum_item_us = 0;
  std::uint64_t pool_wall_us = 0;
  // Cache detail (same counters as above, split by outcome / drop reason).
  std::uint64_t cache_lookup_ops = 0;
  std::uint64_t cache_lookup_stale = 0;
  std::uint64_t cache_evict_budget = 0;
  std::uint64_t cache_evict_reap = 0;
  std::uint64_t cache_evict_clear = 0;
  std::uint64_t cache_epoch_bumps = 0;
};

// Diagnostics only (WDS_FRAME_DIAG). Per-frame split-phase snapshot plus the
// SPLITPHASE / POOLSTAT line formatters, so a caller outside chart-render (the
// Qt FrameDiagLogger) can place the lines in its own once-per-second block
// without changing this module's scope counters.
// Last completed collect. Zeroed struct when perf::enabled() is false.
SplitEffectPhaseDiag split_effect_last_phase_diag() noexcept;
// One `SPLITPHASE ...` line (no trailing newline) into buf; returns length,
// 0 when diagnostics are off. Safe to call every frame.
std::size_t format_split_effect_phase_line(char* buf, std::size_t cap) noexcept;
// One `POOLSTAT ...` line (no trailing newline); returns length, 0 when off.
std::size_t format_split_effect_poolstat_line(char* buf, std::size_t cap) noexcept;

// Per-frame stage accumulator behind the SPLITPHASE line. The stages nest:
// Evaluate includes Realize, Realize includes Integrate/Stitch/CacheLookup,
// Subemit includes its own Integrate. Collect covers Prepare + Evaluate +
// Assemble + Copyout. A single clock read per scope, and only when
// wds::common::perf::enabled(). Nothing here feeds particle math, cache
// keys, or scheduling.
enum class SplitDiagPhase : int {
  Collect = 0,
  Prepare,
  Evaluate,
  Integrate,
  Subemit,
  SubemitPlan,
  SubemitRealize,
  Realize,
  Copyout,
  Assemble,
  // Candidate walk in assemble_split: per-task output -> ordered rows.
  Compact,
  // Per-particle trail / ribbon vertex realization (fill_trails).
  Traverse,
  Stitch,
  CacheLookup,
  Count
};

// Counters: Systems / Particles count per-frame work; Steps counts fixed-step
// integrations; Tasks counts evaluate tasks; StepsCold is the post-seek redo.
enum class SplitDiagCount : int {
  Systems = 0,
  Particles,
  Steps,
  Tasks,
  Count
};

void split_diag_add_ns(SplitDiagPhase phase, std::int64_t ns) noexcept;
void split_diag_add_count(SplitDiagCount counter, std::int64_t n) noexcept;

class SplitDiagScope {
#if !WDS_DIAG_ENABLED
 public:
  explicit constexpr SplitDiagScope(SplitDiagPhase) noexcept {}
#else
 public:
  explicit SplitDiagScope(SplitDiagPhase phase) noexcept;
  ~SplitDiagScope();
  SplitDiagScope(const SplitDiagScope&) = delete;
  SplitDiagScope& operator=(const SplitDiagScope&) = delete;

 private:
  SplitDiagPhase phase_;
  bool on_ = false;
  std::chrono::steady_clock::time_point start_{};
#endif  // WDS_DIAG_ENABLED
};

// -1 restores env (default on). 0/1 force off/on for in-process A/B.
void split_effect_set_eval_carry_enabled(int enabled) noexcept;

// seconds <= 0 restores env/default (0.25 unless WDS_SPLIT_CARRY_GAP).
void split_effect_set_eval_carry_max_gap(float seconds) noexcept;
void split_effect_set_eval_carry_store_inhibited(bool inhibited) noexcept;

// -1 restores env (default off). 0/1 force off/on. WDS_SPLIT_PERSIST=1 opts in.
void split_effect_set_eval_persist_enabled(int enabled) noexcept;

// First-seek buffer sizes. Not particle state. WDS_PRECOMMIT=0 skips.
// Bound is a function of the document: concurrent official splits at any
// one chart time (sweep), raised to a catalog floor so an empty project
// can still accept a later heavy edit. High-water only — delete does not
// shrink. Touch is idle-sliced (same leftover-vsync budget as cache reap).
struct SplitEffectBoundNote {
  int32_t color_id = 0;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
};

struct SplitEffectCatalogWeight {
  int32_t id = 0;
  int live = 0;
  int trail_pts = 0;
  int systems = 0;
  int trail_systems = 0;
};

struct SplitEffectPrecommitPlan {
  int particle_cap = 0;
  int trail_chunks = 0;
  int cache_slots_per_shard = 0;
  int cache_entries_per_shard = 0;
  int carry_entries_per_shard = 0;
  int walk_entries_per_shard = 0;
  int persist_stores = 0;
  int persist_slots = 0;
  std::size_t draw_verts = 0;
  std::size_t bytes_estimate = 0;
  int32_t heaviest_id = 0;
  int heaviest_live = 0;
  int concurrent_instances = 0;
  int floor_instances = 0;
};

struct SplitEffectPrecommitStats {
  SplitEffectPrecommitPlan plan{};
  std::size_t bytes_touched = 0;
  int64_t wall_us = 0;
  int64_t request_us = 0;
  long minflt = 0;
  long rss_kb_before = 0;
  long rss_kb_after = 0;
  int pending = 0;
};

SplitEffectCatalogWeight split_effect_catalog_weight(int32_t color_id);
SplitEffectCatalogWeight split_effect_heaviest_catalog_weight();
int split_effect_precommit_floor_instances() noexcept;

SplitEffectPrecommitPlan split_effect_floor_buffer_bound();
SplitEffectPrecommitPlan split_effect_estimate_buffer_bound(const SplitEffectBoundNote* notes,
                                                            int n, int64_t appear_ms);
// Back-compat: each id is one concurrent instance (all overlap) + floor.
SplitEffectPrecommitPlan split_effect_plan_precommit(const int32_t* color_ids, int n);

void split_effect_raise_precommit(const SplitEffectPrecommitPlan& plan);
bool split_effect_precommit_pending() noexcept;
// Owner-thread slice. Seek/collect never wait; missing pages fault as today.
int split_effect_pump_precommit(SplitEffectFrameCollect* io, int budget_us);
// Leftover vsync µs using the same 16.7−prev−1 ms rule as idle cache reap.
int split_effect_idle_precommit_budget_us() noexcept;
SplitEffectPrecommitStats split_effect_precommit_stats() noexcept;
// Drain remaining slices (chart-load one-shot). Still not particle state.
SplitEffectPrecommitStats split_effect_precommit_buffers(const SplitEffectPrecommitPlan& plan,
                                                         SplitEffectFrameCollect* io);

// Unity 2022.3 useAutoRandomSeed: a new system seed each Play(). Preview
// remints when a line leaves kSplitEffectLinePlayWindowSec and later
// re-enters. Call on 「从开头开始播放」. Seek / pause do not reset.
void reset_split_effect_auto_seed_session(bool invalidate_eval_cache = true) noexcept;

// Locates the split-effect particle data directory (`resources/`) at runtime.
// A directory D is valid iff
//   D/game_splitlaneelement_assets_spliteffectelements/spliteffectelements10050.bundle.objects.tsv
// exists. Candidates, in order: cwd/resources, exe_dir/resources,
// exe_dir/../Resources/resources (macOS .app), then `<ancestor>/resources` for up to
// 6 parents of exe_dir, then of cwd (lets build-tree binaries find repo/resources).
// Returns a lexically normalized UTF-8 path, or "" when nothing valid exists.
std::string find_split_effects_resource_root(const std::filesystem::path& exe_dir,
                                             const std::filesystem::path& cwd);

// Explicit override for the root used by split_effects_resource_root(). Empty clears
// the override (and drops the cached auto-detected root).
void set_split_effects_resource_root(std::string root);

// Root handed to the split-effect controllers: the override if set, else the cached
// find_split_effects_resource_root(executable_dir(), current_path()), else "resources".
std::string split_effects_resource_root();

// THREAD CONTRACT: owner thread only, one thread at a time (shares the unsynchronised
// controller cache with collect_split_effect_frame).
// Hydrated controller for an official effect id. Dump writers that need to
// mutate modules before bind should take a non-const ParticleSystem from
// Controller::lines() (not catalog.inc).
openwds_split_effects::Controller* official_split_effect_controller(int32_t effect_id);

// Dump-only. collect binds this instance instead of the effect-id cache.
// Null clears. Not a production preview API; not a catalog generate change.
void set_split_effect_collect_controller_override(
    openwds_split_effects::Controller* controller) noexcept;

}  // namespace wds::chart_render

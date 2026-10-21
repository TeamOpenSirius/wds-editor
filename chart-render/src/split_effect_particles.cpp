#include <wds/chart_render/split_effect_particles.hpp>

#include "split_collect_pool.hpp"
#include "split_over_cap.hpp"
#include "split_wall_profile.hpp"
#include "shuriken/internal.hpp"
#include "shuriken/simd_dispatch.hpp"
#include "shuriken/split_eval_cache.hpp"
#include "shuriken/split_eval_carry.hpp"
#include "shuriken/split_eval_persist.hpp"

#include <wds/common/log.hpp>
#include <wds/common/minflt_stage.hpp>
#include <wds/common/page_touch.hpp>
#include <wds/common/perf_stages.hpp>
#include <wds/common/utf8_path.hpp>
#include <wds/chart_render/openwds_shuriken_bind.hpp>
#include <wds/chart_render/openwds_split_effects/controller.hpp>
#include <wds/chart_render/shuriken/evaluate.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/official_split_x.hpp>
#include <wds/core/split_effect_gate.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wds::chart_render {
namespace {

#ifndef NDEBUG
// Debug-only detector for concurrent entry into the single-owner-thread API. Same-thread
// re-entry (collect -> playback -> controller cache) is allowed.
std::atomic<int> g_owner_depth{0};
std::atomic<std::thread::id> g_owner_thread{std::thread::id{}};

class OwnerThreadGuard {
 public:
  OwnerThreadGuard() noexcept {
    const std::thread::id me = std::this_thread::get_id();
    if (g_owner_depth.fetch_add(1, std::memory_order_acq_rel) == 0) {
      g_owner_thread.store(me, std::memory_order_release);
    } else {
      assert(g_owner_thread.load(std::memory_order_acquire) == me &&
             "split effect API entered concurrently from two threads");
    }
  }
  ~OwnerThreadGuard() {
    if (g_owner_depth.load(std::memory_order_acquire) == 1) {
      g_owner_thread.store(std::thread::id{}, std::memory_order_release);
    }
    g_owner_depth.fetch_sub(1, std::memory_order_acq_rel);
  }
  OwnerThreadGuard(const OwnerThreadGuard&) = delete;
  OwnerThreadGuard& operator=(const OwnerThreadGuard&) = delete;
};
#else
class OwnerThreadGuard {
 public:
  OwnerThreadGuard() noexcept {}
};
#endif

std::atomic<uint64_t> g_trail_frame_gen{1};

struct ThreadTrail {
  SplitEffectTrailArena arena;
  uint64_t gen = 0;
};

ThreadTrail& thread_trail() {
  thread_local ThreadTrail tls;
  const uint64_t gen = g_trail_frame_gen.load(std::memory_order_relaxed);
  if (tls.gen != gen) {
    tls.arena.reset();
    tls.gen = gen;
  }
  return tls;
}

// ---------------------------------------------------------------------------
// SPLITPHASE stage accumulator (diagnostics only).
//
// The stage scopes run on the collect caller AND on pool workers (evaluate /
// integrate / realize are per-task), so every slot here is a relaxed atomic.
// Nothing is touched unless wds::common::perf::enabled(): with WDS_FRAME_DIAG
// unset no clock is read, no atomic is written, and the per-frame reset is a
// single load. Not part of any scheduling or particle decision.
//
// Nesting (do not add the columns up blindly):
//   Collect  ⊃ Prepare + Evaluate + Assemble + Copyout
//   Evaluate ⊃ Realize ⊃ { Integrate, Trails, Stitch, CacheLookup, MakeState }
//   Subemit  ⊃ its own Integrate / Trails
// so Integrate is a subset of Evaluate, and Subemit runs in the same wave.
// ---------------------------------------------------------------------------
constexpr int kDiagPhaseN = static_cast<int>(SplitDiagPhase::Count);
constexpr int kDiagCountN = static_cast<int>(SplitDiagCount::Count);

struct SplitDiagFrame {
  std::atomic<int64_t> phase_ns[kDiagPhaseN]{};
  std::atomic<int64_t> count[kDiagCountN]{};
  std::atomic<int> splits{0};
  std::atomic<int> particles{0};
  std::atomic<int> playing{0};
  std::atomic<int> quality{0};
  std::atomic<uint64_t> frame{0};
  // Cold (post-seek / post-epoch) redo marker, set by collect.
  std::atomic<int> cold{0};
  std::atomic<int> cold_reason{0};  // 0 none, 1 seek, 2 doc, 3 preload

  void reset_stages() noexcept {
    for (int i = 0; i < kDiagPhaseN; ++i) {
      phase_ns[i].store(0, std::memory_order_relaxed);
    }
    for (int i = 0; i < kDiagCountN; ++i) {
      count[i].store(0, std::memory_order_relaxed);
    }
    splits.store(0, std::memory_order_relaxed);
    particles.store(0, std::memory_order_relaxed);
    playing.store(0, std::memory_order_relaxed);
    quality.store(0, std::memory_order_relaxed);
    cold.store(0, std::memory_order_relaxed);
    cold_reason.store(0, std::memory_order_relaxed);
  }
};

SplitDiagFrame g_diag{};
std::atomic<int64_t> g_diag_last_phase_ns[kDiagPhaseN]{};
std::atomic<int> g_diag_last_systems{0};
std::atomic<int> g_diag_last_splits{0};
std::atomic<int> g_diag_last_particles{0};
std::atomic<int> g_diag_last_playing{0};
std::atomic<int> g_diag_last_cold{0};
std::atomic<uint64_t> g_diag_last_frame{0};
// Published snapshot of the last completed collect. Filled on the owner
// thread; worker threads only ever contribute to g_diag above.
uint64_t g_split_frame_seq = 0;

// Diagnostics only: why the next collect is cold (0 none, 1 seek, 2 doc,
// 3 preload). Set by the invalidation entry points, consumed by collect.
std::atomic<int> g_cold_reason{0};
// Set by split_effect_prepare_heavy_collect (seek pre-roll) so the very next
// cold collect can label itself even when the epoch also changed.
std::atomic<int> g_preload_armed{0};

// Set once per second to emit the POOLSTAT rollup (diagnostics only).
std::atomic<int64_t> g_poolstat_last_ns{0};

const char* cold_reason_name(int r) noexcept {
  switch (r) {
    case 1:
      return "seek";
    case 2:
      return "doc";
    case 3:
      return "preload";
    default:
      return "none";
  }
}

}  // namespace

void SplitEffectTrailArena::reset() noexcept {
  for (Chunk& c : chunks_) {
    c.used = 0;
  }
  cur_ = 0;
}

void SplitEffectTrailArena::reserve_chunks(size_t n) {
  while (chunks_.size() < n) {
    reserve_one_chunk();
  }
}

bool SplitEffectTrailArena::reserve_one_chunk() {
  Chunk extra;
  extra.mem = std::make_unique<float[]>(kChunk);
  extra.cap = kChunk;
  extra.used = 0;
  if (extra.mem) {
    wds::common::touch_pages(extra.mem.get(), extra.cap * sizeof(float));
  }
  chunks_.push_back(std::move(extra));
  return true;
}

size_t SplitEffectTrailArena::chunk_count() const noexcept {
  return chunks_.size();
}

float* SplitEffectTrailArena::alloc(size_t n) {
  if (n == 0) {
    return nullptr;
  }
  if (n > kChunk) {
    Chunk extra;
    extra.mem = std::make_unique<float[]>(n);
    extra.cap = n;
    extra.used = n;
    chunks_.push_back(std::move(extra));
    return chunks_.back().mem.get();
  }
  if (chunks_.empty()) {
    Chunk first;
    first.mem = std::make_unique<float[]>(kChunk);
    first.cap = kChunk;
    chunks_.push_back(std::move(first));
    cur_ = 0;
  }
  // Advance past chunks that cannot fit n (full, or an oversized chunk with used == cap).
  while (chunks_[cur_].used + n > chunks_[cur_].cap) {
    ++cur_;
    if (cur_ >= chunks_.size()) {
      Chunk next;
      next.mem = std::make_unique<float[]>(kChunk);
      next.cap = kChunk;
      chunks_.push_back(std::move(next));
    }
  }
  float* p = chunks_[cur_].mem.get() + chunks_[cur_].used;
  chunks_[cur_].used += n;
  return p;
}

void split_effect_trail_begin_frame() noexcept {
  g_trail_frame_gen.fetch_add(1, std::memory_order_relaxed);
}

SplitEffectTrailArena& split_effect_thread_trails() {
  return thread_trail().arena;
}

void split_effect_clear_live_trails(SplitEffectLiveParticle& p) noexcept {
  p.trail_points = 0;
  p.trail_x.clear();
  p.trail_y.clear();
  p.trail_z.clear();
  p.trail_widths.clear();
  p.trail_color_r.clear();
  p.trail_color_g.clear();
  p.trail_color_b.clear();
  p.trail_color_a.clear();
}

void split_effect_alloc_live_trails_xyz(SplitEffectLiveParticle& p, uint16_t n) {
  SplitEffectTrailArena& a = split_effect_thread_trails();
  p.trail_points = n;
  p.trail_x.data = a.alloc(n);
  p.trail_x.n = n;
  p.trail_y.data = a.alloc(n);
  p.trail_y.n = n;
  p.trail_z.data = a.alloc(n);
  p.trail_z.n = n;
}

void split_effect_alloc_live_trails_attrs(SplitEffectLiveParticle& p, uint16_t n) {
  SplitEffectTrailArena& a = split_effect_thread_trails();
  p.trail_widths.data = a.alloc(n);
  p.trail_widths.n = n;
  p.trail_color_r.data = a.alloc(n);
  p.trail_color_r.n = n;
  p.trail_color_g.data = a.alloc(n);
  p.trail_color_g.n = n;
  p.trail_color_b.data = a.alloc(n);
  p.trail_color_b.n = n;
  p.trail_color_a.data = a.alloc(n);
  p.trail_color_a.n = n;
}

namespace {
void copy_chan(SplitEffectTrailArena& dst, SplitEffectTrailChan& ch) {
  if (ch.n == 0 || ch.data == nullptr) {
    ch.clear();
    return;
  }
  float* p = dst.alloc(ch.n);
  std::memcpy(p, ch.data, static_cast<size_t>(ch.n) * sizeof(float));
  ch.data = p;
}
}  // namespace

void split_effect_adopt_live_trails(SplitEffectTrailArena& dst, SplitEffectLiveParticle& p) {
  copy_chan(dst, p.trail_x);
  copy_chan(dst, p.trail_y);
  copy_chan(dst, p.trail_z);
  copy_chan(dst, p.trail_widths);
  copy_chan(dst, p.trail_color_r);
  copy_chan(dst, p.trail_color_g);
  copy_chan(dst, p.trail_color_b);
  copy_chan(dst, p.trail_color_a);
}

namespace {

int emitter_priority(const SplitEffectParticle& p) noexcept {
  int pr = 0;
  if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::Burst)) {
    pr += 0;
  } else if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverTime)) {
    pr += 1;
  } else {
    pr += 2;
  }
  if ((p.flags & kSplitEffectFlagTrails) != 0) {
    pr += 10;
  }
  if (p.emission < 1.0f && p.rate_over_distance < 1.0f && p.rate_over_time < 1.0f) {
    pr += 5;
  }
  return pr;
}

using Candidate = OverCapCandidate;

using LineKey = std::tuple<int32_t, int32_t, int64_t>;

struct LinePlay {
  uint32_t gen = 0;
  bool active = false;
};

struct AutoSeedSession {
  std::map<LineKey, LinePlay> lines;
  std::set<LineKey> seen;
  double batch_now = 0.0;
  bool has_batch = false;
};

AutoSeedSession& auto_seed_session() {
  static AutoSeedSession session;
  return session;
}

void begin_auto_seed_batch(double now_sec) {
  AutoSeedSession& s = auto_seed_session();
  // us/1e6 vs ms/1000 can differ by an ulp at the same chart T. A strict
  // == would close the play window and remint useAutoRandomSeed on seek.
  if (s.has_batch && std::abs(s.batch_now - now_sec) < 1e-7) {
    return;
  }
  if (s.has_batch) {
    for (auto& kv : s.lines) {
      if (kv.second.active && s.seen.find(kv.first) == s.seen.end()) {
        kv.second.active = false;
      }
    }
    s.seen.clear();
  }
  s.batch_now = now_sec;
  s.has_batch = true;
}

uint32_t touch_line_play(int32_t color_id, int32_t line_index, int64_t start_ms, double now_sec,
                         bool inside, bool remint) {
  begin_auto_seed_batch(now_sec);
  AutoSeedSession& s = auto_seed_session();
  const LineKey key{color_id, line_index, start_ms};
  s.seen.insert(key);
  LinePlay& st = s.lines[key];
  if (inside) {
    if (!st.active) {
      // Unity 2022.3 useAutoRandomSeed remints on Play(), not seek/pause.
      // A paused remake at T must keep the current gen so capture order
      // cannot change seeds (subset vs full 318 at the same chart T).
      if (remint || st.gen == 0) {
        st.gen += 1u;
      }
      st.active = true;
    }
  } else {
    st.active = false;
  }
  return st.gen != 0 ? st.gen : 1u;
}

using DeadLatchKey = std::pair<int32_t, int64_t>;

std::map<DeadLatchKey, float>& dead_latches() {
  static std::map<DeadLatchKey, float> m;
  return m;
}

void clear_dead_latches() noexcept { dead_latches().clear(); }

bool is_subemitter_target(uint32_t catalog_i) noexcept;

bool catalog_effect_may_emit(const SplitEffectCatalogEntry& entry, float now_t) {
  uint16_t n = 0;
  const SplitEffectParticle* ps = official_split_effect_particles(entry, &n);
  if (ps == nullptr || n == 0) {
    return false;
  }
  uint8_t slots[256]{};
  for (uint16_t i = 0; i < n; ++i) {
    if (is_subemitter_target(static_cast<uint32_t>(entry.particle_begin) + i)) {
      continue;
    }
    if (shuriken::primary_eval_can_emit(ps[i], now_t)) {
      return true;
    }
    slots[ps[i].element_index] = 1;
  }
  shuriken::EmitContext ctx;
  ctx.now_t = now_t;
  for (int s = 0; s < 256; ++s) {
    if (slots[s] != 0 && shuriken::subemit_eval_can_emit(ps, n, s, ctx)) {
      return true;
    }
  }
  return false;
}

bool catalog_effect_emission_closed(const SplitEffectCatalogEntry& entry, float now_t) {
  uint16_t n = 0;
  const SplitEffectParticle* ps = official_split_effect_particles(entry, &n);
  if (ps == nullptr || n == 0) {
    return true;
  }
  for (uint16_t i = 0; i < n; ++i) {
    if (is_subemitter_target(static_cast<uint32_t>(entry.particle_begin) + i)) {
      continue;
    }
    if (!shuriken::primary_emission_closed(ps[i], now_t)) {
      return false;
    }
  }
  return true;
}

void mark_split_lines(int quality, const wds::chart_editor::PreviewSplitLaneInstance& split,
                      int32_t color_id, double now_sec, bool inside, bool remint) {
  if (split.split_count < 1) {
    return;
  }
  wds::chart_editor::official_split_effect_for_each_emit(
      quality, split.split_lane_type, split.split_count, [&](int32_t index) {
        touch_line_play(color_id, index, split.start_ms, now_sec, inside, remint);
      });
}

constexpr const char* kSplitEffectsMarkerRel =
    "game_splitlaneelement_assets_spliteffectelements/"
    "spliteffectelements10050.bundle.objects.tsv";
constexpr int kSplitEffectsWalkUpLevels = 6;

bool is_split_effects_root(const std::filesystem::path& dir) {
  return wds::common::is_regular_file_utf8(
      wds::common::path_to_utf8(dir / std::filesystem::path(kSplitEffectsMarkerRel)));
}

struct SplitEffectsRootState {
  std::mutex mutex;
  bool has_override = false;
  std::string override_root;
  bool has_cache = false;
  std::string cached_root;
};

SplitEffectsRootState& split_effects_root_state() {
  static SplitEffectsRootState state;
  return state;
}

openwds_split_effects::Controller* g_collect_controller_override = nullptr;

using ControllerCache =
    std::map<int32_t, std::unique_ptr<openwds_split_effects::Controller>>;

ControllerCache& controller_cache() {
  static ControllerCache cache;
  return cache;
}

}  // namespace

std::string find_split_effects_resource_root(const std::filesystem::path& exe_dir,
                                             const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  const auto accept = [](const fs::path& dir, std::string& out) {
    if (!is_split_effects_root(dir)) {
      return false;
    }
    out = wds::common::path_to_utf8(dir.lexically_normal());
    return true;
  };
  const auto walk_up = [&](const fs::path& start, std::string& out) {
    fs::path p = start;
    for (int i = 0; i < kSplitEffectsWalkUpLevels; ++i) {
      const fs::path parent = p.parent_path();
      if (parent.empty() || parent == p) {
        break;
      }
      p = parent;
      if (accept(p / "resources", out)) {
        return true;
      }
    }
    return false;
  };

  std::string out;
  if (!cwd.empty() && accept(cwd / "resources", out)) {
    return out;
  }
  if (!exe_dir.empty()) {
    if (accept(exe_dir / "resources", out) ||
        accept(exe_dir / ".." / "Resources" / "resources", out) || walk_up(exe_dir, out)) {
      return out;
    }
  }
  if (!cwd.empty() && walk_up(cwd, out)) {
    return out;
  }
  return {};
}

void set_split_effects_resource_root(std::string root) {
  SplitEffectsRootState& st = split_effects_root_state();
  std::lock_guard<std::mutex> lock(st.mutex);
  st.has_override = !root.empty();
  st.override_root = std::move(root);
  st.has_cache = false;
  st.cached_root.clear();
}

std::string split_effects_resource_root() {
  SplitEffectsRootState& st = split_effects_root_state();
  std::lock_guard<std::mutex> lock(st.mutex);
  if (st.has_override) {
    return st.override_root;
  }
  if (!st.has_cache) {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    st.cached_root = find_split_effects_resource_root(
        wds::common::executable_dir(nullptr), ec ? std::filesystem::path{} : cwd);
    st.has_cache = true;
    if (st.cached_root.empty()) {
      WDS_LOG("split effects: resources/ not found (cwd/exe/ancestors)\n");
    } else {
      WDS_LOG("split effects: resources root %s\n", st.cached_root.c_str());
    }
  }
  return st.cached_root.empty() ? std::string("resources") : st.cached_root;
}

openwds_split_effects::Controller* official_split_effect_controller(int32_t effect_id);

namespace {

openwds_split_effects::Controller* cached_controller(int32_t color_id) {
  if (g_collect_controller_override != nullptr) {
    return g_collect_controller_override;
  }
  return official_split_effect_controller(color_id);
}

}  // namespace

openwds_split_effects::Controller* official_split_effect_controller(int32_t effect_id) {
  const OwnerThreadGuard owner_guard;
  ControllerCache& cache = controller_cache();
  const auto it = cache.find(effect_id);
  if (it != cache.end()) {
    return it->second.get();
  }
  auto ctrl = std::make_unique<openwds_split_effects::Controller>(effect_id,
                                                                 split_effects_resource_root());
  if (!ctrl->valid() || !ctrl->initialize(false, 100, 10, 100)) {
    cache.emplace(effect_id, nullptr);
    return nullptr;
  }
  auto* raw = ctrl.get();
  cache.emplace(effect_id, std::move(ctrl));
  return raw;
}

void set_split_effect_collect_controller_override(
    openwds_split_effects::Controller* controller) noexcept {
  const OwnerThreadGuard owner_guard;
  g_collect_controller_override = controller;
  shuriken::split_eval_cache_bump_epoch();
}

namespace {

void append_line_systems(const openwds_split_effects::OpenWdsLineState& line,
                         std::vector<const unity::ParticleSystem*>& out) {
  for (const auto& ps : line.particle_systems) {
    if (ps) {
      out.push_back(ps.get());
    }
  }
}

void systems_for_slot(const openwds_split_effects::Controller* ctrl, int slot, size_t oracle_count,
                      std::vector<const unity::ParticleSystem*>& out) {
  out.clear();
  if (ctrl == nullptr) {
    return;
  }
  const auto& lines = ctrl->lines();
  if (slot >= 0 && static_cast<size_t>(slot) < lines.size() &&
      !lines[static_cast<size_t>(slot)].particle_systems.empty()) {
    append_line_systems(lines[static_cast<size_t>(slot)], out);
    return;
  }
  for (const auto& line : lines) {
    if (oracle_count > 0 && line.particle_systems.size() == oracle_count) {
      append_line_systems(line, out);
      return;
    }
  }
}

void oracle_for_slot(const SplitEffectParticle* particles, uint16_t pcount, int slot,
                     std::vector<const SplitEffectParticle*>& out) {
  out.clear();
  for (uint16_t pi = 0; pi < pcount; ++pi) {
    if (static_cast<int>(particles[pi].element_index) == slot) {
      out.push_back(&particles[pi]);
    }
  }
}

// live dump v1 (reports/re-subemit-work-report.md): a catalog row that is a
// Sub Emitters child is not independently Played. Unity sidecar children
// keep isEmitting=false even when playOnAwake is serialized true
// (11321_4 = 3×40, not 40+120). 118 Birth/Death slots share this, not an ID.
const std::vector<bool>& subemitter_target_bitset() {
  static const std::vector<bool> bits = [] {
    using split_effect_catalog_detail::kSplitEffectSubEmitterCount;
    using split_effect_catalog_detail::kSplitEffectSubEmitters;
    size_t max_child = 0;
    for (uint16_t i = 0; i < kSplitEffectSubEmitterCount; ++i) {
      const uint32_t child = static_cast<uint32_t>(kSplitEffectSubEmitters[i].child_index);
      max_child = std::max(max_child, static_cast<size_t>(child));
    }
    std::vector<bool> out(max_child + 1, false);
    for (uint16_t i = 0; i < kSplitEffectSubEmitterCount; ++i) {
      out[static_cast<size_t>(kSplitEffectSubEmitters[i].child_index)] = true;
    }
    return out;
  }();
  return bits;
}

bool is_subemitter_target(uint32_t catalog_i) noexcept {
  const std::vector<bool>& bits = subemitter_target_bitset();
  return catalog_i < bits.size() && bits[catalog_i];
}

uint32_t catalog_of_bound_row(const SplitEffectParticle& p,
                              const uint32_t* catalog_index_override) noexcept {
  if (catalog_index_override != nullptr) {
    return *catalog_index_override;
  }
  using split_effect_catalog_detail::kSplitEffectParticles;
  const auto* begin = kSplitEffectParticles;
  const ptrdiff_t n = static_cast<ptrdiff_t>(std::size(kSplitEffectParticles));
  const ptrdiff_t i = &p - begin;
  if (i >= 0 && i < n) {
    return static_cast<uint32_t>(i);
  }
  return 0;
}

// Primary evaluate already hydrate+binds. Subemit must walk the same bound rows
// (not a second catalog-only array). Slot topology stays the catalog table.
void bind_effect_particles(const SplitEffectParticle* particles, uint16_t pcount,
                           const std::vector<const SplitEffectParticle*>& oracle,
                           const std::vector<const unity::ParticleSystem*>& systems,
                           std::vector<SplitEffectParticle>& bound) {
  bound.assign(particles, particles + pcount);
  if (systems.empty()) {
    return;
  }
  for (size_t j = 0; j < systems.size(); ++j) {
    if (systems[j] == nullptr) {
      continue;
    }
    const SplitEffectParticle* ora = j < oracle.size() ? oracle[j] : nullptr;
    SplitEffectParticle row;
    bind_from_unity(*systems[j], row, ora);
    if (ora != nullptr) {
      const ptrdiff_t local = ora - particles;
      if (local >= 0 && local < static_cast<ptrdiff_t>(pcount)) {
        bound[static_cast<size_t>(local)] = row;
      }
    }
  }
}

enum class EvalKind : uint8_t { Primary, Subemit };

struct EvalTask {
  EvalKind kind = EvalKind::Primary;
  int32_t color_id = 0;
  int32_t line_index = 0;
  int64_t start_ms = 0;
  uint16_t layer = 0;
  int priority = 0;
  uint32_t catalog_i = 0;
  uint8_t has_catalog = 0;
  int element_slot = 0;
  uint16_t bound_count = 0;
  shuriken::EmitContext ctx;
  SplitEffectParticle primary{};
  std::vector<SplitEffectParticle> bound;
  std::vector<SplitEffectLiveParticle> out;
  std::vector<shuriken::PrimaryEvalJob> plan;
  shuriken::SubemitFillPlan sub_plan;
  std::vector<uint8_t> sub_do_fill;
  uint8_t sub_fill_marked = 0;
  uint8_t write_stamps = 1;
  int written = 0;
  uint32_t last_cost_us = 1;
};

struct SplitPrep {
  bool visible = false;
  bool has_work = false;
  size_t task_begin = 0;
  size_t task_end = 0;
  std::vector<Candidate> cap_kept;
  uint8_t cap_marked = 0;
  int32_t color_id = 0;
  int64_t start_ms = 0;
  uint32_t line_play_gen = 0;
  float now_t = 0.0f;
  const SplitEffectCatalogEntry* entry = nullptr;
};

struct FrameScratch {
  std::vector<EvalTask> tasks;
  size_t task_used = 0;
  std::vector<SplitPrep> preps;
  std::vector<Candidate> candidates;
  std::vector<Candidate> kept;
  std::vector<size_t> cap_head;
  std::vector<size_t> cap_lim;
  std::vector<SplitEffectParticle> bind_bound;
  std::vector<SplitEffectParticle> bound_stack;
  std::vector<const SplitEffectParticle*> bound_oracle;
  std::vector<const SplitEffectParticle*> oracle;
  std::vector<const unity::ParticleSystem*> systems;
  std::vector<size_t> order;
  std::unordered_map<uint64_t, uint32_t> costs;
  std::vector<float> leak_u0;
  std::vector<float> leak_u1;

  EvalTask& acquire_task() {
    if (task_used == tasks.size()) {
      tasks.emplace_back();
      EvalTask& n = tasks.back();
      n.plan.reserve(256);
      n.out.reserve(256);
      n.bound.reserve(16);
    }
    EvalTask& t = tasks[task_used++];
    t.kind = EvalKind::Primary;
    t.has_catalog = 0;
    t.catalog_i = 0;
    t.written = 0;
    t.bound_count = 0;
    t.plan.clear();
    t.bound.clear();
    t.sub_plan.items.clear();
    t.sub_plan.kept = 0;
    t.sub_do_fill.clear();
    t.sub_fill_marked = 0;
    t.write_stamps = 1;
    return t;
  }
};

FrameScratch& frame_scratch() {
  thread_local FrameScratch scratch;
  if (scratch.tasks.capacity() < 128) {
    scratch.tasks.reserve(128);
  }
  return scratch;
}

uint64_t task_cost_key(const EvalTask& t) noexcept {
  uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(t.color_id));
  h = h * 1315423911u + static_cast<uint32_t>(t.line_index);
  h = h * 1315423911u + static_cast<uint32_t>(t.start_ms);
  h = h * 1315423911u + (t.kind == EvalKind::Subemit ? 0x80000000u : 0u) + t.layer;
  return h;
}

shuriken::EmitContext task_emit_ctx(const EvalTask& task) {
  shuriken::EmitContext ectx = task.ctx;
  if (task.has_catalog != 0) {
    ectx.catalog_index_override = task.catalog_i;
    ectx.has_catalog_index_override = 1;
  }
  return ectx;
}

struct ParallelEvalCtx {
  EvalTask* tasks = nullptr;
  const size_t* order = nullptr;
};

void apply_over_cap(FrameScratch& fs, int cap);

// Diagnostics-only frame publisher (defined below, after the collect code).
void diag_finish_frame(bool frame_scope) noexcept;

void apply_tmp_reuse_leak(FrameScratch& fs, size_t begin, size_t end) {
  // evaluate() does not write trail_u0/u1 except ribbon stitch (vertex>0).
  // Leak arrays grow to this frame's max written, never the pool cap.
  size_t need = 0;
  for (size_t ti = begin; ti < end; ++ti) {
    if (fs.tasks[ti].written > 0) {
      need = std::max(need, static_cast<size_t>(fs.tasks[ti].written));
    }
  }
  if (need == 0) {
    return;
  }
  if (fs.leak_u0.size() < need) {
    fs.leak_u0.resize(need);
    fs.leak_u1.resize(need);
  }
  std::fill_n(fs.leak_u0.begin(), need, 0.0f);
  std::fill_n(fs.leak_u1.begin(), need, 1.0f);
  for (size_t ti = begin; ti < end; ++ti) {
    EvalTask& t = fs.tasks[ti];
    for (int i = 0; i < t.written; ++i) {
      if (t.kind == EvalKind::Subemit && static_cast<size_t>(i) < t.sub_plan.items.size() &&
          t.sub_plan.items[static_cast<size_t>(i)].filled == 0) {
        continue;
      }
      SplitEffectLiveParticle& p = t.out[static_cast<size_t>(i)];
      if (!(p.trail_count > 0 && p.trail_vertex > 0)) {
        p.trail_u0 = fs.leak_u0[static_cast<size_t>(i)];
        p.trail_u1 = fs.leak_u1[static_cast<size_t>(i)];
      }
      fs.leak_u0[static_cast<size_t>(i)] = p.trail_u0;
      fs.leak_u1[static_cast<size_t>(i)] = p.trail_u1;
    }
  }
}

void write_subemit_plan(EvalTask& task) {
  const int n = shuriken::evaluate_subemitters_plan(
      task.bound.data(), task.bound_count, task.element_slot, task.ctx, task.sub_plan,
      kSplitEffectParticlePool, nullptr, nullptr);
  task.written = n > 0 ? n : 0;
  // Diagnostics only: how many SubemitFillItems a task actually keeps, versus the
  // kSplitEffectParticlePool that evaluate_subemitters_plan reserves for it.
#if WDS_DIAG_ENABLED
  if (wds::common::perf::enabled()) {
    static std::atomic<int> calls{0};
    static std::atomic<int> max_items{0};
    static std::atomic<int> sum_items{0};
    static std::atomic<int> max_cap_items{0};
    static std::atomic<int64_t> last_sec{0};
    const int items = static_cast<int>(task.sub_plan.items.size());
    calls.fetch_add(1, std::memory_order_relaxed);
    sum_items.fetch_add(items, std::memory_order_relaxed);
    int prev = max_items.load(std::memory_order_relaxed);
    while (items > prev && !max_items.compare_exchange_weak(prev, items)) {
    }
    const int cap_items = static_cast<int>(task.sub_plan.items.capacity());
    prev = max_cap_items.load(std::memory_order_relaxed);
    while (cap_items > prev && !max_cap_items.compare_exchange_weak(prev, cap_items)) {
    }
    const int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    int64_t last = last_sec.load(std::memory_order_relaxed);
    if (now_sec != last &&
        last_sec.compare_exchange_strong(last, now_sec, std::memory_order_relaxed)) {
      const int c = calls.exchange(0, std::memory_order_relaxed);
      const int mx = max_items.exchange(0, std::memory_order_relaxed);
      const int sm = sum_items.exchange(0, std::memory_order_relaxed);
      std::fprintf(stderr,
                   "SUBMITPLAN_SEC tasks=%d max_items=%d mean_items=%d reserved=%d cap=%d item_bytes=%zu\n",
                   c, mx, c > 0 ? sm / c : 0,
                   max_cap_items.exchange(0, std::memory_order_relaxed), kSplitEffectParticlePool,
                   sizeof(shuriken::SubemitFillItem));
    }
  }
#endif  // WDS_DIAG_ENABLED
}

void write_subemit_realize(EvalTask& task) {
  if (task.sub_plan.kept <= 0) {
    task.written = 0;
    return;
  }
  const int keep = task.sub_plan.kept;
  if (task.out.size() < static_cast<size_t>(keep)) {
    SplitWallScope wall_grow(SplitWallSec::VecGrow);
    task.out.resize(static_cast<size_t>(keep));
  }
  const uint8_t* do_fill = nullptr;
  if (task.sub_fill_marked != 0 && task.sub_do_fill.size() >= static_cast<size_t>(keep)) {
    do_fill = task.sub_do_fill.data();
  }
  const int n = shuriken::evaluate_subemitters_realize(task.ctx, task.sub_plan, do_fill,
                                                       task.out.data(), keep);
  task.written = n;
}

bool eval_task_timing() noexcept {
#if WDS_DIAG_ENABLED
  return split_wall_enabled() || wds::common::perf::enabled();
#else
  return false;
#endif  // WDS_DIAG_ENABLED
}

void finish_eval_task_cost(EvalTask& task, std::chrono::steady_clock::time_point t0,
                           bool add) noexcept {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  const auto us = ns / 1000;
  const uint32_t part = us <= 0 ? 1u : static_cast<uint32_t>(us > 0xffffffffll ? 0xffffffffu : us);
  if (add) {
    const uint64_t sum = static_cast<uint64_t>(task.last_cost_us) + part;
    task.last_cost_us = sum > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(sum);
  } else {
    task.last_cost_us = part;
  }
#if WDS_DIAG_ENABLED
  split_wall_note_task(ns);
#endif  // WDS_DIAG_ENABLED
}

void run_one_eval_plan(EvalTask& task) {
  // Do not write last_cost_us here. Plan microseconds are tiny; using them
  // as the realize work_hint under-schedules (2 workers) on Noise+Particle-
  // trail. last_cost_us stays the previous realize weight, or 1 → formula.
#if WDS_DIAG_ENABLED
  const bool time = eval_task_timing();
  const auto t0 = time ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  split_wall_begin_task();
#endif  // WDS_DIAG_ENABLED
  {
    SplitWallScope wall_task(SplitWallSec::TaskSetup);
    if (task.kind == EvalKind::Primary) {
      WDS_PERF_SCOPE("split.collect.evaluate");
      SplitDiagScope diag_eval(SplitDiagPhase::Evaluate);
#if WDS_DIAG_ENABLED
      split_diag_add_count(SplitDiagCount::Systems, 1);
#endif  // WDS_DIAG_ENABLED
      bool can = false;
      {
        SplitWallScope wall_gate(SplitWallSec::PlanGate);
        can = shuriken::primary_eval_can_emit(task.primary, task.ctx.now_t);
      }
      if (!can) {
        task.written = 0;
      } else {
        shuriken::evaluate_plan(task.primary, task_emit_ctx(task), task.plan);
        task.written = 0;
      }
    } else {
      WDS_PERF_SCOPE("split.collect.subemit");
      SplitDiagScope diag_sub(SplitDiagPhase::Subemit);
      bool can = false;
      {
        SplitWallScope wall_gate(SplitWallSec::PlanGate);
        can = shuriken::subemit_eval_can_emit(task.bound.data(), task.bound_count, task.element_slot,
                                              task.ctx);
      }
      if (!can) {
        task.written = 0;
      } else {
        write_subemit_plan(task);
      }
    }
  }
#if WDS_DIAG_ENABLED
  if (time) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    split_wall_note_task(ns);
  }
#endif  // WDS_DIAG_ENABLED
}

void run_one_eval_realize(EvalTask& task) {
  if (task.kind != EvalKind::Primary) {
    return;
  }
  if (task.plan.empty()) {
    task.written = 0;
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
#if WDS_DIAG_ENABLED
  split_wall_begin_task();
#endif  // WDS_DIAG_ENABLED
  {
    SplitWallScope wall_task(SplitWallSec::TaskSetup);
    WDS_PERF_SCOPE("split.collect.evaluate");
    // Realize nests inside Evaluate on purpose (see the SPLITPHASE nesting
    // note); the column is the part of Evaluate that actually fills live
    // particles, so evaluate - realize is the birth/plan/traverse share.
    SplitDiagScope diag_real(SplitDiagPhase::Realize);
    int keep = 0;
    {
      SplitWallScope wall_keep(SplitWallSec::RealizeMatch);
      for (const shuriken::PrimaryEvalJob& job : task.plan) {
        if (job.alive == 0 || job.alpha_keep == 0) {
          continue;
        }
        if (task.write_stamps == 0 && job.do_fill == 0) {
          continue;
        }
        ++keep;
      }
    }
    if (keep > kSplitEffectParticlePool) {
      keep = kSplitEffectParticlePool;
    }
    if (keep <= 0) {
      task.written = 0;
      shuriken::evaluate_drop_primary_carry(task_emit_ctx(task), task.primary);
    } else {
      if (task.out.size() < static_cast<size_t>(keep)) {
        SplitWallScope wall_grow(SplitWallSec::VecGrow);
        task.out.resize(static_cast<size_t>(keep));
      }
      const int n = shuriken::evaluate_realize(task.primary, task_emit_ctx(task), task.plan,
                                               task.out.data(), keep, task.write_stamps != 0);
#if WDS_DIAG_ENABLED
      split_diag_add_count(SplitDiagCount::Particles, n);
#endif  // WDS_DIAG_ENABLED
      if (task.has_catalog != 0) {
        for (int i = 0; i < n; ++i) {
          task.out[static_cast<size_t>(i)].catalog_index = task.catalog_i;
        }
      }
      task.written = n;
    }
  }
  finish_eval_task_cost(task, t0, false);
}

void mark_primary_fills(FrameScratch& fs, size_t begin, size_t end, int cap) {
  fs.candidates.clear();
  for (size_t ti = begin; ti < end; ++ti) {
    EvalTask& t = fs.tasks[ti];
    if (t.kind == EvalKind::Primary) {
      for (uint32_t j = 0; j < static_cast<uint32_t>(t.plan.size()); ++j) {
        const shuriken::PrimaryEvalJob& job = t.plan[j];
        if (job.alive == 0 || job.alpha_keep == 0) {
          continue;
        }
        Candidate c;
        c.priority = t.priority;
        c.line_index = t.line_index;
        c.layer = t.layer;
        c.task_i = static_cast<uint32_t>(ti);
        c.job_i = j;
        c.birth = job.b.index;
        c.sys = 0;
        c.primary = 1;
        c.ribbon = t.primary.trail_mode == 1 ? 1 : 0;
        fs.candidates.push_back(std::move(c));
      }
    } else {
      const int n = std::min(t.written, static_cast<int>(t.sub_plan.items.size()));
      for (int i = 0; i < n; ++i) {
        const shuriken::SubemitFillItem& item = t.sub_plan.items[static_cast<size_t>(i)];
        if (item.ready == 0) {
          continue;
        }
        Candidate c;
        c.priority = t.priority;
        c.line_index = t.line_index;
        c.layer = t.layer;
        c.task_i = static_cast<uint32_t>(ti);
        c.job_i = static_cast<uint32_t>(i);
        c.birth = static_cast<int32_t>(i);
        c.sys = item.child_idx;
        c.primary = 0;
        c.ribbon = item.ribbon;
        fs.candidates.push_back(c);
      }
    }
  }
  apply_over_cap(fs, cap);
  for (size_t ti = begin; ti < end; ++ti) {
    EvalTask& t = fs.tasks[ti];
    if (t.kind == EvalKind::Primary) {
      for (shuriken::PrimaryEvalJob& job : t.plan) {
        job.do_fill = 0;
      }
    } else {
      t.sub_do_fill.assign(t.sub_plan.items.size(), 0);
      t.sub_fill_marked = 1;
    }
  }
  for (const Candidate& c : fs.candidates) {
    EvalTask& t = fs.tasks[c.task_i];
    if (c.primary != 0) {
      if (c.job_i < t.plan.size()) {
        shuriken::PrimaryEvalJob& job = t.plan[c.job_i];
        if (job.alive != 0 && job.alpha_keep != 0) {
          job.do_fill = 1;
        }
      }
    } else if (c.job_i < t.sub_do_fill.size()) {
      t.sub_do_fill[c.job_i] = 1;
    }
  }
}

int store_cap_selection(FrameScratch& fs, SplitPrep& prep, int cap) {
  SplitWallScope wall_sel(SplitWallSec::CapSelect);
  mark_primary_fills(fs, prep.task_begin, prep.task_end, cap);
  const int kept = static_cast<int>(fs.candidates.size());
  for (size_t ti = prep.task_begin; ti < prep.task_end; ++ti) {
    // Realize only the kept run so ribbon stitch sees a hole-free chain.
    fs.tasks[ti].write_stamps = 0;
  }
  prep.cap_marked = 1;
  prep.cap_kept.swap(fs.candidates);
  return kept;
}

void run_one_eval_subemit_realize(EvalTask& task) {
  if (task.kind != EvalKind::Subemit) {
    return;
  }
#if WDS_DIAG_ENABLED
  const bool time = eval_task_timing();
  const auto t0 = time ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  split_wall_begin_task();
#endif  // WDS_DIAG_ENABLED
  {
    SplitWallScope wall_task(SplitWallSec::TaskSetup);
    WDS_PERF_SCOPE("split.collect.subemit");
    SplitDiagScope diag_sub(SplitDiagPhase::Subemit);
    write_subemit_realize(task);
#if WDS_DIAG_ENABLED
    split_diag_add_count(SplitDiagCount::Particles, task.written);
#endif  // WDS_DIAG_ENABLED
  }
#if WDS_DIAG_ENABLED
  if (time) {
    finish_eval_task_cost(task, t0, true);
  }
#endif  // WDS_DIAG_ENABLED
}

uint64_t estimate_realize_cost(const EvalTask& t) noexcept {
  if (t.last_cost_us > 1) {
    return t.last_cost_us;
  }
  uint64_t njob = 1;
  if (t.kind == EvalKind::Primary) {
    if (t.write_stamps == 0) {
      njob = 0;
      for (const shuriken::PrimaryEvalJob& job : t.plan) {
        if (job.do_fill != 0) {
          ++njob;
        }
      }
      if (njob == 0) {
        njob = 1;
      }
    } else if (!t.plan.empty()) {
      njob = static_cast<uint64_t>(t.plan.size());
    } else {
      // First-touch PLAN: plan is still empty so njob=1 under-counts births.
      // max_particles is a work-unit stand-in for this collect only; the
      // next realize yields to last_cost_us. Not a cache / prewarm.
      const int mp = t.primary.max_particles > 0 ? static_cast<int>(t.primary.max_particles) : 64;
      njob = static_cast<uint64_t>(mp > 1024 ? 1024 : (mp < 64 ? 64 : mp));
    }
  } else {
    if (t.sub_fill_marked != 0 && !t.sub_do_fill.empty()) {
      njob = 0;
      for (uint8_t f : t.sub_do_fill) {
        if (f != 0) {
          ++njob;
        }
      }
      if (njob == 0) {
        njob = 1;
      }
    } else if (!t.sub_plan.items.empty()) {
      njob = static_cast<uint64_t>(t.sub_plan.items.size());
    } else {
      // First-touch subemit PLAN walks parent poses (pose_at × births).
      njob = 256;
    }
  }
  uint64_t steps = 1;
  if (t.ctx.now_t > 0.0f) {
    steps = static_cast<uint64_t>(t.ctx.now_t * 60.0f + 0.5f);
  }
  if (steps < 1) {
    steps = 1;
  }
  uint64_t hint = njob * steps;
  // Particle-trail mode=0 re-samples n ages per kept particle, n∈[3,512].
  // njob×now_t×60 at the standard window (now_t 0.7–1.7) sits in the
  // 512–8000 band → 2 workers. Interleaved A/B: forcing cap recovered
  // 10660 play/scrub/cold −1.3/−6.9/−3.9 ms; 11621 (no trails) was noise.
  // hop schedule, not 2022.3.
  if (t.kind == EvalKind::Primary && (t.primary.flags & kSplitEffectFlagTrails) != 0 &&
      t.primary.trail_mode == 0) {
    hint *= 16ull;
    if (hint < 8000ull) {
      hint = 8000ull;
    }
  }
  return hint;
}

uint64_t eval_work_hint(FrameScratch& fs, size_t n) noexcept {
  uint64_t h = 0;
  for (size_t i = 0; i < n; ++i) {
    h += estimate_realize_cost(fs.tasks[fs.order[i]]);
  }
  return h;
}

void note_eval_work(FrameScratch& fs, size_t n) noexcept {
  split_collect_note_work(n, eval_work_hint(fs, n));
}

void sort_eval_order(FrameScratch& fs) {
  std::sort(fs.order.begin(), fs.order.end(), [&](size_t a, size_t b) {
    return estimate_realize_cost(fs.tasks[a]) > estimate_realize_cost(fs.tasks[b]);
  });
}

void run_eval_tasks(FrameScratch& fs, SplitPrep& prep, int cap) {
  const size_t begin = prep.task_begin;
  const size_t end = prep.task_end;
  const size_t n = end - begin;
  if (n == 0) {
    return;
  }
  fs.order.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const size_t ti = begin + i;
    EvalTask& t = fs.tasks[ti];
    const auto it = fs.costs.find(task_cost_key(t));
    if (it != fs.costs.end()) {
      t.last_cost_us = it->second;
    }
    fs.order[i] = ti;
  }
  sort_eval_order(fs);
  note_eval_work(fs, n);
  const uint64_t work_hint = eval_work_hint(fs, n);
  ParallelEvalCtx ctx;
  ctx.tasks = fs.tasks.data();
  ctx.order = fs.order.data();
#if WDS_DIAG_ENABLED
  const bool time_b = split_wall_enabled();
  const auto b0 = time_b ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif  // WDS_DIAG_ENABLED
  split_collect_parallel_for(0, n, 1, [](size_t i, void* raw) {
    auto* c = static_cast<ParallelEvalCtx*>(raw);
    run_one_eval_plan(c->tasks[c->order[i]]);
  }, &ctx, work_hint);
  store_cap_selection(fs, prep, cap);
  sort_eval_order(fs);
  note_eval_work(fs, n);
  const uint64_t realize_hint = eval_work_hint(fs, n);
  split_collect_parallel_for(0, n, 1, [](size_t i, void* raw) {
    auto* c = static_cast<ParallelEvalCtx*>(raw);
    run_one_eval_realize(c->tasks[c->order[i]]);
  }, &ctx, realize_hint);
  split_collect_parallel_for(0, n, 1, [](size_t i, void* raw) {
    auto* c = static_cast<ParallelEvalCtx*>(raw);
    run_one_eval_subemit_realize(c->tasks[c->order[i]]);
  }, &ctx, realize_hint);
#if WDS_DIAG_ENABLED
  if (time_b) {
    const auto b1 = std::chrono::steady_clock::now();
    split_wall_note_phase_b(static_cast<int>(n), split_collect_last_participants(),
                            std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count());
  }
#endif  // WDS_DIAG_ENABLED
  for (size_t i = begin; i < end; ++i) {
    fs.costs[task_cost_key(fs.tasks[i])] = fs.tasks[i].last_cost_us;
  }
}

void run_eval_tasks_all(FrameScratch& fs, std::vector<SplitPrep>& preps, int cap) {
  fs.order.clear();
  for (const SplitPrep& prep : preps) {
    if (!prep.has_work) {
      continue;
    }
    for (size_t ti = prep.task_begin; ti < prep.task_end; ++ti) {
      EvalTask& t = fs.tasks[ti];
      const auto it = fs.costs.find(task_cost_key(t));
      if (it != fs.costs.end()) {
        t.last_cost_us = it->second;
      }
      fs.order.push_back(ti);
    }
  }
  const size_t n = fs.order.size();
  if (n == 0) {
    return;
  }
  {
    SplitWallScope wall_setup(SplitWallSec::TaskSetup);
    sort_eval_order(fs);
  }
  note_eval_work(fs, n);
  const uint64_t work_hint = eval_work_hint(fs, n);
  ParallelEvalCtx ctx;
  ctx.tasks = fs.tasks.data();
  ctx.order = fs.order.data();
#if WDS_DIAG_ENABLED
  const bool time_b = split_wall_enabled();
  const auto b0 = time_b ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif  // WDS_DIAG_ENABLED
  split_collect_parallel_for(0, n, 1, [](size_t i, void* raw) {
    auto* c = static_cast<ParallelEvalCtx*>(raw);
    run_one_eval_plan(c->tasks[c->order[i]]);
  }, &ctx, work_hint);
  int remaining = cap;
  for (SplitPrep& prep : preps) {
    if (!prep.has_work) {
      continue;
    }
    remaining -= store_cap_selection(fs, prep, remaining);
    if (remaining < 0) {
      remaining = 0;
    }
  }
  sort_eval_order(fs);
  note_eval_work(fs, n);
  const uint64_t realize_hint = eval_work_hint(fs, n);
  split_collect_parallel_for(0, n, 1, [](size_t i, void* raw) {
    auto* c = static_cast<ParallelEvalCtx*>(raw);
    run_one_eval_realize(c->tasks[c->order[i]]);
  }, &ctx, realize_hint);
  split_collect_parallel_for(0, n, 1, [](size_t i, void* raw) {
    auto* c = static_cast<ParallelEvalCtx*>(raw);
    run_one_eval_subemit_realize(c->tasks[c->order[i]]);
  }, &ctx, realize_hint);
#if WDS_DIAG_ENABLED
  if (time_b) {
    const auto b1 = std::chrono::steady_clock::now();
    split_wall_note_phase_b(static_cast<int>(n), split_collect_last_participants(),
                            std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count());
  }
#endif  // WDS_DIAG_ENABLED
  for (size_t i = 0; i < n; ++i) {
    const size_t ti = fs.order[i];
    fs.costs[task_cost_key(fs.tasks[ti])] = fs.tasks[ti].last_cost_us;
  }
}

struct SplitClock {
  int32_t color_id = 0;
  float now_t = 0.0f;
  float fade_scale = 1.0f;
  bool visible = false;
  bool seed_inside = false;
};

SplitClock make_split_clock(const wds::chart_editor::PreviewSplitLaneInstance& split,
                            double now_sec, const SplitEffectParentMotion* parent_motion) {
  SplitClock c;
  c.color_id = split.scratch_length != 0 ? split.scratch_length : 1;
  const double play_sec = parent_motion != nullptr &&
                                  parent_motion->simulation_play_clock_candidate
                              ? parent_motion->simulation_play_origin_seconds
                              : static_cast<double>(split.start_ms) / 1000.0 -
                                    static_cast<double>(wds::chart_editor::kOfficialSplitFadeInSec);
  c.now_t = static_cast<float>(now_sec - play_sec);
  c.visible = split.should_show && split.split_count >= 1 && c.now_t >= 0.0f;
  c.seed_inside = c.visible && c.now_t <= kSplitEffectLinePlayWindowSec;
  if (c.visible) {
    c.fade_scale = shuriken::fade_scale_at_play_t(c.now_t);
  }
  return c;
}

void prepare_split_tasks(FrameScratch& fs, SplitPrep& prep,
                         const wds::chart_editor::PreviewSplitLaneInstance& split, int quality,
                         double now_sec, const SplitEffectParentMotion* parent_motion,
                         bool playing) {
  prep = SplitPrep{};
  prep.task_begin = fs.task_used;
  prep.task_end = fs.task_used;
  const SplitClock clock = make_split_clock(split, now_sec, parent_motion);
  prep.visible = clock.visible;
  prep.color_id = clock.color_id;
  prep.start_ms = split.start_ms;
  prep.now_t = clock.now_t;
  mark_split_lines(quality, split, clock.color_id, now_sec, clock.seed_inside, playing);
  if (!clock.visible) {
    return;
  }

  const auto* entry = official_split_effect(clock.color_id);
  if (entry == nullptr || entry->particle_count == 0) {
    return;
  }
  prep.entry = entry;
  if (playing) {
    const DeadLatchKey key{clock.color_id, split.start_ms};
    auto& latches = dead_latches();
    const auto it = latches.find(key);
    if (it != latches.end()) {
      if (clock.now_t + 1e-6f < it->second) {
        latches.erase(it);
      } else {
        return;
      }
    }
  }
  if (!catalog_effect_may_emit(*entry, clock.now_t)) {
    return;
  }
  uint16_t pcount = 0;
  const SplitEffectParticle* particles = official_split_effect_particles(*entry, &pcount);
  if (particles == nullptr || pcount == 0) {
    return;
  }

  const float line_alpha = std::clamp(split.split_line_alpha, 0.0f, 1.0f);
  const uint8_t element_count = std::max<uint8_t>(entry->element_count, 1);
  const openwds_split_effects::Controller* ctrl = cached_controller(clock.color_id);
  prep.has_work = true;

  wds::chart_editor::official_split_effect_for_each_emit(
      quality, split.split_lane_type, split.split_count, [&](int32_t index) {
        float world_x = 0.0f;
        if (!wds::chart_editor::official_split_line_world_x(split.split_count, index, world_x)) {
          return;
        }
        const int slot = wds::chart_editor::split_color_slot(clock.color_id, split.split_count,
                                                            index) %
                         static_cast<int>(element_count);
        shuriken::EmitContext ctx;
        ctx.color_id = clock.color_id;
        ctx.line_index = index;
        ctx.line_start_ms = split.start_ms;
        ctx.line_x = world_x;
        ctx.fade_scale = clock.fade_scale;
        ctx.line_alpha = line_alpha;
        ctx.now_t = clock.now_t;
        ctx.want_trail = true;
        if (parent_motion != nullptr) {
          ctx.parent_motion = *parent_motion;
        }
        ctx.line_play_gen = touch_line_play(clock.color_id, index, split.start_ms, now_sec,
                                           clock.seed_inside, playing);
        oracle_for_slot(particles, pcount, slot, fs.oracle);
        systems_for_slot(ctrl, slot, fs.oracle.size(), fs.systems);
        fs.bind_bound.clear();
        fs.bound_stack.clear();
        fs.bound_oracle.clear();
        {
          WDS_PERF_SCOPE("split.collect.bind");
          bind_effect_particles(particles, pcount, fs.oracle, fs.systems, fs.bind_bound);
          if (!fs.systems.empty()) {
            fs.bound_stack.resize(fs.systems.size());
            fs.bound_oracle.resize(fs.systems.size());
            for (size_t j = 0; j < fs.systems.size(); ++j) {
              fs.bound_oracle[j] = j < fs.oracle.size() ? fs.oracle[j] : nullptr;
              bind_from_unity(*fs.systems[j], fs.bound_stack[j], fs.bound_oracle[j]);
            }
          }
        }
        if (!fs.systems.empty()) {
          for (size_t j = 0; j < fs.systems.size(); ++j) {
            const SplitEffectParticle* ora = fs.bound_oracle[j];
            const uint16_t layer = ora != nullptr ? static_cast<uint16_t>(ora - particles)
                                                  : static_cast<uint16_t>(j);
            const uint32_t catalog_i =
                ora != nullptr ? static_cast<uint32_t>(entry->particle_begin) +
                                     static_cast<uint32_t>(ora - particles)
                               : 0u;
            const uint32_t* cat = ora != nullptr ? &catalog_i : nullptr;
            if (is_subemitter_target(catalog_of_bound_row(fs.bound_stack[j], cat))) {
              continue;
            }
            EvalTask& t = fs.acquire_task();
            t.kind = EvalKind::Primary;
            t.color_id = clock.color_id;
            t.line_index = index;
            t.start_ms = split.start_ms;
            t.layer = layer;
            t.priority = emitter_priority(fs.bound_stack[j]);
            t.primary = fs.bound_stack[j];
            t.ctx = ctx;
            if (cat != nullptr) {
              t.catalog_i = catalog_i;
              t.has_catalog = 1;
            }
          }
        } else {
          for (size_t j = 0; j < fs.oracle.size(); ++j) {
            const SplitEffectParticle& p = *fs.oracle[j];
            if (is_subemitter_target(catalog_of_bound_row(p, nullptr))) {
              continue;
            }
            EvalTask& t = fs.acquire_task();
            t.kind = EvalKind::Primary;
            t.color_id = clock.color_id;
            t.line_index = index;
            t.start_ms = split.start_ms;
            t.layer = static_cast<uint16_t>(fs.oracle[j] - particles);
            t.priority = emitter_priority(p);
            t.primary = p;
            t.ctx = ctx;
          }
        }
        EvalTask& sub = fs.acquire_task();
        sub.kind = EvalKind::Subemit;
        sub.color_id = clock.color_id;
        sub.line_index = index;
        sub.start_ms = split.start_ms;
        sub.layer = pcount;
        sub.priority = 3;
        sub.ctx = ctx;
        sub.element_slot = slot;
        sub.bound_count = pcount;
        sub.bound = fs.bind_bound;
      });
  prep.task_end = fs.task_used;
}

// Preview-pool hop, not Unity 2022.3 maxParticles. See split_over_cap.hpp.
void apply_over_cap(FrameScratch& fs, int cap) {
  WDS_PERF_SCOPE("split.collect.cap");
  apply_over_cap_select(fs.candidates, cap, over_cap_legacy_env());
}

bool split_tasks_have_life(const FrameScratch& fs, const SplitPrep& prep) {
  for (size_t ti = prep.task_begin; ti < prep.task_end; ++ti) {
    const EvalTask& t = fs.tasks[ti];
    if (t.kind == EvalKind::Primary) {
      for (const shuriken::PrimaryEvalJob& job : t.plan) {
        if (job.alive != 0) {
          return true;
        }
      }
    } else if (t.written > 0) {
      return true;
    }
  }
  return false;
}

int assemble_split(FrameScratch& fs, SplitPrep& prep, SplitEffectLiveParticle* out, int max_out,
                   SplitEffectTrailArena* out_trails,
                   std::vector<SplitEffectLiveParticle>* pool = nullptr, int offset = 0) {
  SplitWallScope wall_asm(SplitWallSec::Assemble);
  if (max_out <= 0 || !prep.has_work) {
    return 0;
  }
  int evaluated = 0;
  const int cap = std::min(max_out, kSplitEffectParticlePool);
  const bool use_mark = prep.cap_marked != 0;
  if (use_mark) {
    SplitWallScope wall_compact(SplitWallSec::Compaction);
    fs.candidates.swap(prep.cap_kept);
    for (size_t ti = prep.task_begin; ti < prep.task_end; ++ti) {
      const EvalTask& t = fs.tasks[ti];
      if (t.kind == EvalKind::Primary) {
        for (const shuriken::PrimaryEvalJob& job : t.plan) {
          if (job.alive != 0 && job.alpha_keep != 0) {
            ++evaluated;
          }
        }
      } else {
        evaluated += t.written;
      }
    }
  } else {
    SplitWallScope wall_compact(SplitWallSec::Compaction);
    // Compact = the candidate walk that turns per-task output into the
    // ordered DrawBatch rows (unmarked branch). The copy into the pool is
    // Copyout; per-particle trail vertices are Traverse (in evaluate.cpp).
    SplitDiagScope diag_compact(SplitDiagPhase::Compact);
    fs.candidates.clear();
    for (size_t ti = prep.task_begin; ti < prep.task_end; ++ti) {
      EvalTask& t = fs.tasks[ti];
      for (int i = 0; i < t.written; ++i) {
        Candidate c;
        c.priority = t.priority;
        c.line_index = t.line_index;
        c.layer = t.layer;
        c.task_i = static_cast<uint32_t>(ti);
        c.job_i = static_cast<uint32_t>(i);
        c.birth = static_cast<int32_t>(i);
        c.sys = t.out[static_cast<size_t>(i)].catalog_index;
        c.ribbon = t.out[static_cast<size_t>(i)].trail_count > 1 ? 1 : 0;
        c.primary = t.kind == EvalKind::Primary ? 1 : 0;
        fs.candidates.push_back(c);
      }
    }
    evaluated = static_cast<int>(fs.candidates.size());
    SplitWallScope wall_cap(SplitWallSec::Cap);
    apply_over_cap(fs, cap);
  }
  int want = static_cast<int>(fs.candidates.size());
  if (want > cap) {
    want = cap;
  }
  if (pool != nullptr) {
    if (want > 0 && static_cast<int>(pool->size()) < offset + want) {
      pool->resize(static_cast<size_t>(offset + want));
    }
    out = want > 0 ? pool->data() + offset : nullptr;
  }
  if (out == nullptr || want <= 0) {
#if WDS_DIAG_ENABLED
    split_wall_note_candidates(evaluated, 0);
#endif  // WDS_DIAG_ENABLED
    WDS_PERF_COUNT("split.particles", 0);
    return 0;
  }
  int written = 0;
  {
    WDS_PERF_SCOPE("split.collect.copyout");
    SplitWallScope wall_copy(SplitWallSec::Copyout);
    SplitDiagScope diag_copy(SplitDiagPhase::Copyout);
    for (auto& c : fs.candidates) {
      if (written >= cap) {
        break;
      }
      const EvalTask& src = fs.tasks[c.task_i];
      if (use_mark && c.primary != 0) {
        if (c.job_i >= static_cast<uint32_t>(src.plan.size())) {
          continue;
        }
        const uint32_t slot = src.plan[c.job_i].write_slot;
        if (slot == 0xffffffffu || slot >= static_cast<uint32_t>(src.written)) {
          continue;
        }
        out[written] = src.out[slot];
      } else {
        if (c.job_i >= static_cast<uint32_t>(src.written)) {
          continue;
        }
        out[written] = src.out[c.job_i];
      }
      if (out_trails != nullptr) {
        split_effect_adopt_live_trails(*out_trails, out[written]);
      }
      ++written;
    }
  }
#if WDS_DIAG_ENABLED
  split_wall_note_candidates(evaluated, written);
#endif  // WDS_DIAG_ENABLED
  WDS_PERF_COUNT("split.particles", written);
  return written;
}

SplitEffectTrailArena& one_split_out_trails() {
  thread_local SplitEffectTrailArena arenas[2];
  thread_local int flip = 0;
  flip ^= 1;
  arenas[flip].reset();
  return arenas[flip];
}

int collect_one_split(const wds::chart_editor::PreviewSplitLaneInstance& split, int quality,
                      double now_sec, SplitEffectLiveParticle* out, int max_out,
                      const SplitEffectParentMotion* parent_motion) {
  if (out == nullptr || max_out <= 0) {
    return 0;
  }
  SplitEffectTrailArena& trails = one_split_out_trails();
  split_effect_trail_begin_frame();
  FrameScratch& fs = frame_scratch();
  fs.task_used = 0;
  fs.preps.assign(1, SplitPrep{});
#if WDS_DIAG_ENABLED
  const bool diag = wds::common::perf::enabled();
  if (diag) {
    g_diag.reset_stages();
    g_diag.frame.store(++g_split_frame_seq, std::memory_order_relaxed);
    g_diag.playing.store(0, std::memory_order_relaxed);
    g_diag.quality.store(quality, std::memory_order_relaxed);
    g_diag.splits.store(1, std::memory_order_relaxed);
  }
#endif  // WDS_DIAG_ENABLED
  {
    WDS_PERF_SCOPE("split.collect");
    SplitDiagScope diag_col(SplitDiagPhase::Collect);
    prepare_split_tasks(fs, fs.preps[0], split, quality, now_sec, parent_motion, false);
    if (!fs.preps[0].visible || !fs.preps[0].has_work) {
      diag_finish_frame(false);
      return 0;
    }
    run_eval_tasks(fs, fs.preps[0], max_out);
    apply_tmp_reuse_leak(fs, fs.preps[0].task_begin, fs.preps[0].task_end);
    const int n = assemble_split(fs, fs.preps[0], out, max_out, &trails);
    shuriken::split_eval_carry_set_store_inhibited(false);
#if WDS_DIAG_ENABLED
    if (diag) {
      g_diag.particles.store(n, std::memory_order_relaxed);
    }
#endif  // WDS_DIAG_ENABLED
    diag_finish_frame(false);
    return n;
  }
}

}  // namespace

int collect_split_effect_particles(const wds::chart_editor::PreviewSplitLaneInstance& split,
                                   int quality, double now_sec, SplitEffectLiveParticle* out,
                                   int max_out,
                                   const SplitEffectParentMotion* parent_motion) {
  return collect_one_split(split, quality, now_sec, out, max_out, parent_motion);
}

int collect_split_effect_particles_playback(
    const wds::chart_editor::PreviewSplitLaneInstance& split, int quality, double now_sec,
    SplitEffectPlaybackState& state, SplitEffectLiveParticle* out, int max_out) {
  const OwnerThreadGuard owner_guard;
  if (out == nullptr || max_out <= 0) {
    return 0;
  }

  // The adapter is intentionally a monotonic-play API.  A seek/scrub must be
  // explicit (state.reset()), so the normal stateless collector remains the
  // source of truth for arbitrary timeline access.
  if (std::isfinite(state.last_now_sec) && now_sec + 1e-9 < state.last_now_sec) {
    return 0;
  }

  // Keep the first sample's RateOverDistance identities. OpenWDS OnFadeIn
  // plays fadeIn then Simulate includes that first LineHight
  // |Δps.worldPosition| (probe pin; 10030/2020 live dump). A first-sample
  // skip only fired when play started mid-fade, which hid the near-camera
  // World RoD halo and left far Birth sparks. hop withdrawn, not 2022.3.
  const int written = collect_split_effect_particles(split, quality, now_sec, out, max_out);
  state.last_now_sec = now_sec;
  return written;
}

void reset_split_effect_auto_seed_session(bool invalidate_eval_cache) noexcept {
  auto_seed_session() = AutoSeedSession{};
  if (invalidate_eval_cache) {
    shuriken::split_eval_cache_bump_epoch();
#if WDS_DIAG_ENABLED
    if (wds::common::perf::enabled()) {
      g_cold_reason.store(2, std::memory_order_relaxed);  // doc / Play restart
    }
#endif  // WDS_DIAG_ENABLED
  }
}

void split_effect_clear_caches() noexcept {
  shuriken::split_eval_cache_bump_epoch();
  shuriken::split_eval_carry_clear();
  shuriken::split_eval_persist_clear();
  clear_dead_latches();
  // Diagnostics only: remember *why* the next collect is cold so SPLITCOLD
  // can name it. Does not change when invalidation happens.
#if WDS_DIAG_ENABLED
  if (wds::common::perf::enabled()) {
    g_cold_reason.store(1, std::memory_order_relaxed);  // seek
  }
#endif  // WDS_DIAG_ENABLED
}

void split_effect_clear_seek_state() noexcept {
  // B1: keep the bounded rewind history across a seek; only the current
  // forward snapshot is invalid. Doc/quality/epoch invalidation still calls
  // split_eval_carry_clear() (full).
  shuriken::split_eval_carry_clear_current_keep_rewind();
  shuriken::split_eval_persist_clear();
  clear_dead_latches();
#if WDS_DIAG_ENABLED
  if (wds::common::perf::enabled()) {
    g_cold_reason.store(1, std::memory_order_relaxed);  // seek
  }
#endif  // WDS_DIAG_ENABLED
}

void split_effect_prepare_heavy_collect(int hold_ms) noexcept {
  split_collect_prepare_heavy(hold_ms);
  // Diagnostics only: the seek path calls this right after
  // split_effect_clear_caches(), so the next cold collect is a seek pre-roll
  // rather than an ordinary doc invalidation.
#if WDS_DIAG_ENABLED
  if (wds::common::perf::enabled()) {
    g_preload_armed.store(1, std::memory_order_relaxed);
  }
#endif  // WDS_DIAG_ENABLED
}

#if WDS_DIAG_ENABLED
void split_diag_add_ns(SplitDiagPhase phase, std::int64_t ns) noexcept {
  const int i = static_cast<int>(phase);
  if (i < 0 || i >= kDiagPhaseN || ns <= 0 || !wds::common::perf::enabled()) {
    return;
  }
  g_diag.phase_ns[i].fetch_add(ns, std::memory_order_relaxed);
}

void split_diag_add_count(SplitDiagCount counter, std::int64_t n) noexcept {
  const int i = static_cast<int>(counter);
  if (i < 0 || i >= kDiagCountN || n <= 0 || !wds::common::perf::enabled()) {
    return;
  }
  g_diag.count[i].fetch_add(n, std::memory_order_relaxed);
}
#endif  // WDS_DIAG_ENABLED

#if WDS_DIAG_ENABLED
SplitDiagScope::SplitDiagScope(SplitDiagPhase phase) noexcept : phase_(phase) {
  if (!wds::common::perf::enabled()) {
    return;
  }
  on_ = true;
  start_ = std::chrono::steady_clock::now();
}

SplitDiagScope::~SplitDiagScope() {
  if (!on_) {
    return;
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start_)
                      .count();
  split_diag_add_ns(phase_, ns);
}
#endif  // WDS_DIAG_ENABLED

namespace {

// Owner-thread: fold this frame's atomics into the published snapshot.
void diag_publish_frame() noexcept {
  for (int i = 0; i < kDiagPhaseN; ++i) {
    g_diag_last_phase_ns[i].store(g_diag.phase_ns[i].load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
  }
  // systems = evaluate tasks this frame (one per bound ParticleSystem).
  g_diag_last_systems.store(
      static_cast<int>(g_diag.count[static_cast<int>(SplitDiagCount::Systems)].load(
          std::memory_order_relaxed)),
      std::memory_order_relaxed);
  g_diag_last_splits.store(g_diag.splits.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
  g_diag_last_particles.store(g_diag.particles.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
  g_diag_last_playing.store(g_diag.playing.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
  g_diag_last_cold.store(g_diag.cold.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
  g_diag_last_frame.store(g_diag.frame.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
}

}  // namespace

SplitEffectPhaseDiag split_effect_last_phase_diag() noexcept {
  SplitEffectPhaseDiag d;
#if WDS_DIAG_ENABLED
  if (!wds::common::perf::enabled()) {
    return d;
  }
  const shuriken::SplitEvalCacheStats cs = shuriken::split_eval_cache_stats();
  d.frame = g_diag_last_frame.load(std::memory_order_relaxed);
  d.splits = g_diag_last_splits.load(std::memory_order_relaxed);
  d.systems = g_diag_last_systems.load(std::memory_order_relaxed);
  d.particles = g_diag_last_particles.load(std::memory_order_relaxed);
  d.playing = g_diag_last_playing.load(std::memory_order_relaxed);
  d.cold = g_diag_last_cold.load(std::memory_order_relaxed);
  auto us = [](SplitDiagPhase p) -> std::int64_t {
    const std::int64_t ns =
        g_diag_last_phase_ns[static_cast<int>(p)].load(std::memory_order_relaxed);
    return ns > 0 ? ns / 1000 : 0;
  };
  d.collect_us = us(SplitDiagPhase::Collect);
  d.prepare_us = us(SplitDiagPhase::Prepare);
  d.evaluate_us = us(SplitDiagPhase::Evaluate);
  d.assemble_us = us(SplitDiagPhase::Assemble);
  d.integrate_us = us(SplitDiagPhase::Integrate);
  d.subemit_us = us(SplitDiagPhase::Subemit);
  d.subemit_plan_us = us(SplitDiagPhase::SubemitPlan);
  d.subemit_realize_us = us(SplitDiagPhase::SubemitRealize);
  d.realize_us = us(SplitDiagPhase::Realize);
  d.copyout_us = us(SplitDiagPhase::Copyout);
  d.compact_us = us(SplitDiagPhase::Compact);
  d.traverse_us = us(SplitDiagPhase::Traverse);
  d.stitch_us = us(SplitDiagPhase::Stitch);
  d.cache_lookup_us = us(SplitDiagPhase::CacheLookup);
  shuriken::SplitEvalCacheDiag cd;
  shuriken::split_eval_cache_diag(cd);
  d.cache_lookup_hits = cs.lookup_hits;
  d.cache_lookup_misses = cd.lookup_misses;
  d.cache_evict = cd.evict_ops;
  d.cache_bytes = cs.bytes_used;
  d.cache_lookup_ops = cs.lookup_ops;
  d.cache_lookup_stale = cd.lookup_stale;
  d.cache_evict_budget = cd.evict_budget;
  d.cache_evict_reap = cd.evict_reap;
  d.cache_evict_clear = cd.evict_clear;
  d.cache_epoch_bumps = cd.epoch_bumps;
  const SplitCollectPoolDiag pd = split_collect_pool_diag_frame();
  d.pool_tasks = pd.tasks;
  d.pool_wait_us = pd.wait_us;
  d.pool_claims_caller = pd.claims_caller;
  d.pool_claims_worker = pd.claims_worker;
  d.pool_items_caller = pd.items_caller;
  d.pool_items_worker = pd.items_worker;
  d.pool_join_us = pd.join_us;
  d.pool_inline_calls = pd.inline_calls;
  d.pool_serial_calls = pd.serial_calls;
  d.pool_sum_item_us = pd.sum_item_us;
  d.pool_wall_us = pd.wall_us;
  d.pool_participants = split_collect_last_call_participants();
  const double wall = static_cast<double>(pd.wall_us);
  const double sum_item = static_cast<double>(pd.sum_item_us);
  // Parallel efficiency = how much of the summed per-claim work ran
  // concurrently: sum_item / (wall * participants), clamped to [0,1]. Both
  // sides are µs on the same core class, so the ratio is usable, not exact.
  const int participants =
      split_collect_frame_max_participants() > 0 ? split_collect_frame_max_participants() : 1;
  const double cap = wall * static_cast<double>(participants);
  if (wall > 0.0 && cap > 0.0) {
    double eff = sum_item / cap;
    if (!(eff >= 0.0)) {
      eff = 0.0;
    }
    if (eff > 1.0) {
      eff = 1.0;
    }
    d.pool_par_eff = eff;
  }
  return d;
#else
  return d;
#endif  // WDS_DIAG_ENABLED
}

std::size_t format_split_effect_phase_line(char* buf, std::size_t cap) noexcept {
#if WDS_DIAG_ENABLED
  if (buf == nullptr || cap == 0 || !wds::common::perf::enabled()) {
    return 0;
  }
  const SplitEffectPhaseDiag d = split_effect_last_phase_diag();
  // Every value is clamped non-negative by split_effect_last_phase_diag(), so
  // the line never prints a negative or NaN. The first three lines of fields
  // are the contracted set; the rest is appended so the prefix stays stable.
  const int n = std::snprintf(
      buf, cap,
      "SPLITPHASE frame=%llu playing=%d splits=%d systems=%d particles=%d "
      "collect_ms=%.3f evaluate_ms=%.3f integrate_ms=%.3f subemit_ms=%.3f realize_ms=%.3f "
      "copyout_ms=%.3f cache_hit=%llu cache_miss=%llu cache_bytes=%llu pool_tasks=%llu "
      "pool_wait_us=%llu pool_par_eff=%.3f "
      "prepare_ms=%.3f assemble_ms=%.3f compact_ms=%.3f traverse_ms=%.3f stitch_ms=%.3f "
      "cache_lu_ms=%.3f "
      "cache_ops=%llu cache_stale=%llu cache_evict_budget=%llu cache_evict_reap=%llu "
      "cache_evict_clear=%llu cache_epoch_bumps=%llu "
      "pool_claims_caller=%llu pool_claims_worker=%llu pool_items_caller=%llu "
      "pool_items_worker=%llu pool_join_us=%llu pool_sum_item_us=%llu pool_wall_us=%llu "
      "pool_inline=%llu pool_serial=%llu pool_participants=%d "
      "subemit_plan_ms=%.3f subemit_realize_ms=%.3f",
      static_cast<unsigned long long>(d.frame), d.playing, d.splits, d.systems, d.particles,
      static_cast<double>(d.collect_us) / 1000.0, static_cast<double>(d.evaluate_us) / 1000.0,
      static_cast<double>(d.integrate_us) / 1000.0, static_cast<double>(d.subemit_us) / 1000.0,
      static_cast<double>(d.realize_us) / 1000.0, static_cast<double>(d.copyout_us) / 1000.0,
      static_cast<unsigned long long>(d.cache_lookup_hits),
      static_cast<unsigned long long>(d.cache_lookup_misses),
      static_cast<unsigned long long>(d.cache_bytes),
      static_cast<unsigned long long>(d.pool_tasks),
      static_cast<unsigned long long>(d.pool_wait_us), d.pool_par_eff,
      static_cast<double>(d.prepare_us) / 1000.0, static_cast<double>(d.assemble_us) / 1000.0,
      static_cast<double>(d.compact_us) / 1000.0, static_cast<double>(d.traverse_us) / 1000.0,
      static_cast<double>(d.stitch_us) / 1000.0,
      static_cast<double>(d.cache_lookup_us) / 1000.0,
      static_cast<unsigned long long>(d.cache_lookup_ops),
      static_cast<unsigned long long>(d.cache_lookup_stale),
      static_cast<unsigned long long>(d.cache_evict_budget),
      static_cast<unsigned long long>(d.cache_evict_reap),
      static_cast<unsigned long long>(d.cache_evict_clear),
      static_cast<unsigned long long>(d.cache_epoch_bumps),
      static_cast<unsigned long long>(d.pool_claims_caller),
      static_cast<unsigned long long>(d.pool_claims_worker),
      static_cast<unsigned long long>(d.pool_items_caller),
      static_cast<unsigned long long>(d.pool_items_worker),
      static_cast<unsigned long long>(d.pool_join_us),
      static_cast<unsigned long long>(d.pool_sum_item_us),
      static_cast<unsigned long long>(d.pool_wall_us),
      static_cast<unsigned long long>(d.pool_inline_calls),
      static_cast<unsigned long long>(d.pool_serial_calls), d.pool_participants,
      static_cast<double>(d.subemit_plan_us) / 1000.0,
      static_cast<double>(d.subemit_realize_us) / 1000.0);
  if (n < 0) {
    return 0;
  }
  return static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1;
#else
  (void)buf;
  (void)cap;
  return 0;
#endif  // WDS_DIAG_ENABLED
}

std::size_t format_split_effect_poolstat_line(char* buf, std::size_t cap) noexcept {
#if WDS_DIAG_ENABLED
  if (buf == nullptr || cap == 0 || !wds::common::perf::enabled()) {
    return 0;
  }
  const SplitCollectPoolStat s = split_collect_pool_stat();
  const int n = std::snprintf(
      buf, cap, "POOLSTAT tasks=%llu participants_max=%llu wait_us_sum=%llu spin_us_sum=%llu "
                "inline=%llu serial=%llu",
      static_cast<unsigned long long>(s.tasks),
      static_cast<unsigned long long>(s.participants_max),
      static_cast<unsigned long long>(s.wait_us_sum),
      static_cast<unsigned long long>(s.spin_us_sum),
      static_cast<unsigned long long>(s.inline_calls),
      static_cast<unsigned long long>(s.serial_calls));
  if (n < 0) {
    return 0;
  }
  return static_cast<std::size_t>(n) < cap ? static_cast<std::size_t>(n) : cap - 1;
#else
  (void)buf;
  (void)cap;
  return 0;
#endif  // WDS_DIAG_ENABLED
}

void split_effect_note_prev_frame_ms(double frame_ms) noexcept {
  shuriken::split_eval_cache_note_frame_ms(frame_ms);
}

namespace shuriken {
void split_collect_clear_dead_latches() noexcept { clear_dead_latches(); }
}  // namespace shuriken

void split_effect_set_dead_system_skip(bool enabled) noexcept {
  shuriken::set_dead_system_skip(enabled);
}

bool split_effect_dead_system_skip_enabled() noexcept {
  return shuriken::dead_system_skip_enabled();
}

SplitEffectEvalCacheStats split_effect_eval_cache_stats() noexcept {
  const shuriken::SplitEvalCacheStats s = shuriken::split_eval_cache_stats();
  const shuriken::SplitEvalCarryStats c = shuriken::split_eval_carry_stats();
  SplitEffectEvalCacheStats out;
  out.bytes_used = s.bytes_used;
  out.particle_entries = s.particle_entries;
  out.checkpoints = s.checkpoints;
  out.epoch = s.epoch;
  out.enabled = s.enabled;
  out.budget_bytes = s.budget_bytes;
  out.lookup_ops = s.lookup_ops;
  out.lookup_hits = s.lookup_hits;
  out.lookup_contend = s.lookup_contend;
  out.store_ops = s.store_ops;
  out.store_contend = s.store_contend;
  out.carry_bytes_used = c.bytes_used;
  out.carry_systems = c.systems;
  out.carry_particles = c.particles;
  out.carry_budget_bytes = c.budget_bytes;
  out.carry_enabled = c.enabled;
  out.carry_max_gap = c.max_gap;
  out.carry_hits = c.hits;
  out.carry_misses = c.misses;
  out.carry_fallbacks = c.fallbacks;
  out.carry_commits = c.commits;
  out.carry_walk_hits = c.walk_hits;
  out.carry_walk_misses = c.walk_misses;
  out.carry_walk_commits = c.walk_commits;
  const shuriken::SplitEvalPersistStats pe = shuriken::split_eval_persist_stats();
  out.persist_bytes_used = pe.bytes_used;
  out.persist_systems = pe.systems;
  out.persist_particles = pe.particles;
  out.persist_budget_bytes = pe.budget_bytes;
  out.persist_enabled = pe.enabled;
  out.persist_rebuild_gap = pe.rebuild_gap;
  out.persist_hits = pe.hits;
  out.persist_misses = pe.misses;
  out.persist_rebuilds = pe.rebuilds;
  out.persist_commits = pe.commits;
  out.pool_cap = split_collect_pool_cap();
  out.pool_participants = split_collect_last_call_participants();
  out.pool_last_call_participants = split_collect_last_call_participants();
  out.pool_frame_max_participants = split_collect_frame_max_participants();
  out.pool_frame_wakeups = split_collect_frame_wakeups();
  out.pool_p_cores = split_collect_p_cores();
  out.pool_e_cores = split_collect_e_cores();
  out.simd_path = split_simd_name();
  return out;
}

const char* split_effect_simd_name() noexcept {
  return split_simd_name();
}

void split_effect_set_simd_for_tests(const char* name) noexcept {
  split_simd_force_for_tests(name);
}

void split_effect_set_eval_carry_enabled(int enabled) noexcept {
  shuriken::split_eval_carry_set_enabled_for_tests(enabled);
}

void split_effect_set_eval_carry_max_gap(float seconds) noexcept {
  shuriken::split_eval_carry_set_max_gap_for_tests(seconds);
}

void split_effect_set_eval_carry_store_inhibited(bool inhibited) noexcept {
  shuriken::split_eval_carry_set_store_inhibited(inhibited);
}

void split_effect_set_eval_persist_enabled(int enabled) noexcept {
  shuriken::split_eval_persist_set_enabled_for_tests(enabled);
}

namespace {

long precommit_rss_kb() noexcept {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return 0;
  }
  return static_cast<long>(pmc.WorkingSetSize / 1024);
#else
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<long>(ru.ru_maxrss / 1024);
#else
  return ru.ru_maxrss;
#endif
#endif
}

long precommit_minflt() noexcept {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return 0;
  }
  return static_cast<long>(pmc.PageFaultCount);
#else
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return 0;
  }
  return ru.ru_minflt;
#endif
}

// Buffer precommit is OFF by default.
//
// It materialised every planned buffer up front (resize + touch_pages) so the
// first seek would not pay the faults. Measured on the heavy chart: 7.0 ms and
// 6 333 minor faults in the load frame, ~101 MB of resident pages (footprint
// 767 MB with it, 666 MB without), of which only the part a frame actually
// writes is ever needed. Keeping it off lowers total minor faults by ~14 %
// (33 590 -> 28 764) and leaves frame times unchanged (collect p50 3.70 vs
// 3.79 ms, per-second wall p95 16.7 vs 16.7 ms, wall_max 17.3 vs 17.4 ms), i.e.
// the faults simply move to the pages the renderer really touches.
//
// WDS_PRECOMMIT=1 restores the old behaviour for A/B measurement.
bool precommit_enabled() noexcept {
#if WDS_DIAG_ENABLED
  static const bool on = [] {
    const char* v = std::getenv("WDS_PRECOMMIT");
    return v != nullptr && v[0] == '1' && v[1] == '\0';
  }();
  return on;
#else
  // Release: no env read, no string, no branch.
  return false;
#endif
}

struct WorkerPrecommitCtx {
  int trail_chunks = 0;
};

void worker_precommit_fn(size_t, void* raw) {
  const auto* ctx = static_cast<const WorkerPrecommitCtx*>(raw);
  FrameScratch& fs = frame_scratch();
  fs.tasks.reserve(256);
  fs.preps.reserve(64);
  fs.candidates.reserve(512);
  fs.kept.reserve(512);
  fs.cap_head.reserve(64);
  fs.cap_lim.reserve(64);
  fs.bind_bound.reserve(64);
  fs.bound_stack.reserve(64);
  fs.oracle.reserve(64);
  fs.systems.reserve(64);
  fs.order.reserve(256);
  SplitEffectTrailArena& trails = split_effect_thread_trails();
  trails.reserve_chunks(static_cast<size_t>(std::max(ctx->trail_chunks, 0)));
  shuriken::split_eval_precommit_tls();
}

}  // namespace

namespace {

constexpr int kPrecommitFloorInstances = 2;
constexpr int kPoolGrowSlice = 512;

int next_pow2_at_least(int v, int min_v) noexcept {
  int n = min_v < 1 ? 1 : min_v;
  while (n < v) {
    if (n > (1 << 20)) {
      return n;
    }
    n *= 2;
  }
  return n;
}

void raise_plan(SplitEffectPrecommitPlan& dst, const SplitEffectPrecommitPlan& src) {
  dst.particle_cap = std::max(dst.particle_cap, src.particle_cap);
  dst.trail_chunks = std::max(dst.trail_chunks, src.trail_chunks);
  dst.cache_slots_per_shard = std::max(dst.cache_slots_per_shard, src.cache_slots_per_shard);
  dst.cache_entries_per_shard = std::max(dst.cache_entries_per_shard, src.cache_entries_per_shard);
  dst.carry_entries_per_shard = std::max(dst.carry_entries_per_shard, src.carry_entries_per_shard);
  dst.walk_entries_per_shard = std::max(dst.walk_entries_per_shard, src.walk_entries_per_shard);
  dst.persist_stores = std::max(dst.persist_stores, src.persist_stores);
  dst.persist_slots = std::max(dst.persist_slots, src.persist_slots);
  dst.draw_verts = std::max(dst.draw_verts, src.draw_verts);
  dst.bytes_estimate = std::max(dst.bytes_estimate, src.bytes_estimate);
  if (src.heaviest_live > dst.heaviest_live) {
    dst.heaviest_id = src.heaviest_id;
    dst.heaviest_live = src.heaviest_live;
  }
  dst.concurrent_instances = std::max(dst.concurrent_instances, src.concurrent_instances);
  dst.floor_instances = std::max(dst.floor_instances, src.floor_instances);
}

SplitEffectPrecommitPlan plan_from_weight(int live, int trail_pts, int systems, int instances) {
  SplitEffectPrecommitPlan plan;
  plan.particle_cap = std::min(kSplitEffectParticlePool, std::max(0, live));
  const int trail_floats = trail_pts * 8;
  const int chunks = (trail_floats + static_cast<int>(SplitEffectTrailArena::kChunk) - 1) /
                     static_cast<int>(SplitEffectTrailArena::kChunk);
  plan.trail_chunks = std::clamp(chunks, 2, 64);
  plan.cache_entries_per_shard =
      std::max(8, (plan.particle_cap + 255) / 256);
  plan.cache_slots_per_shard = next_pow2_at_least((plan.cache_entries_per_shard * 10 + 6) / 7, 16);
  plan.carry_entries_per_shard = std::max(2, (std::max(systems, 1) + 63) / 64);
  plan.walk_entries_per_shard = std::max(2, (plan.particle_cap + 1023) / 1024);
  if (shuriken::split_eval_persist_enabled()) {
    plan.persist_stores = std::max(4, systems);
    plan.persist_slots = 256;
  }
  // Catalog trail_pts can exceed any pictured frame. Observed peak is
  // 10660-class ~1.1M verts; keep a shared cap, not an ID branch.
  constexpr std::size_t kDrawVertsCap = 1200000;
  plan.draw_verts = static_cast<std::size_t>(plan.particle_cap) * 4u +
                    static_cast<std::size_t>(std::max(trail_pts, 0)) * 2u;
  if (plan.draw_verts > kDrawVertsCap) {
    plan.draw_verts = kDrawVertsCap;
  }
  plan.bytes_estimate =
      static_cast<std::size_t>(plan.particle_cap) * sizeof(SplitEffectLiveParticle) +
      static_cast<std::size_t>(plan.trail_chunks) * SplitEffectTrailArena::kChunk * sizeof(float) *
          4u +
      256ull * static_cast<std::size_t>(plan.cache_slots_per_shard) * 64ull +
      256ull * static_cast<std::size_t>(plan.cache_entries_per_shard) * 256ull +
      plan.draw_verts * 48ull;
  plan.concurrent_instances = instances;
  plan.floor_instances = kPrecommitFloorInstances;
  return plan;
}

struct PrecommitRt {
  SplitEffectPrecommitPlan desired{};
  SplitEffectPrecommitStats stats{};
  int cache_shard = 0;
  int carry_shard = 0;
  int walk_shard = 0;
  int persist_have = 0;
  int pool_have = 0;
  bool worker_tls = false;
  bool rss_started = false;
};

PrecommitRt& precommit_rt() {
  static PrecommitRt s;
  return s;
}

bool precommit_work_left(const PrecommitRt& rt, const SplitEffectFrameCollect* io) {
  const SplitEffectPrecommitPlan& d = rt.desired;
  if (d.particle_cap <= 0) {
    return false;
  }
  if (rt.pool_have < d.particle_cap) {
    return true;
  }
  if (io != nullptr && static_cast<int>(io->trails.chunk_count()) < d.trail_chunks) {
    return true;
  }
  if (rt.cache_shard < shuriken::split_eval_cache_shard_count()) {
    return true;
  }
  if (rt.carry_shard < shuriken::split_eval_carry_shard_count()) {
    return true;
  }
  if (rt.walk_shard < shuriken::split_eval_carry_walk_shard_count()) {
    return true;
  }
  if (rt.persist_have < d.persist_stores) {
    return true;
  }
  if (!rt.worker_tls) {
    return true;
  }
  return false;
}

}  // namespace

int split_effect_precommit_floor_instances() noexcept {
  return kPrecommitFloorInstances;
}

SplitEffectCatalogWeight split_effect_catalog_weight(int32_t color_id) {
  SplitEffectCatalogWeight w;
  w.id = color_id;
  const auto* e = official_split_effect(color_id);
  if (e == nullptr) {
    return w;
  }
  uint16_t count = 0;
  const auto* ps = official_split_effect_particles(*e, &count);
  for (uint16_t i = 0; i < count && ps != nullptr; ++i) {
    const SplitEffectParticle& p = ps[i];
    ++w.systems;
    const int mp = p.max_particles > 0 ? static_cast<int>(p.max_particles) : 0;
    w.live += mp;
    if ((p.flags & kSplitEffectFlagTrails) == 0) {
      continue;
    }
    ++w.trail_systems;
    const float life = std::max(p.lifetime, p.lifetime_min);
    const float mul = std::max(p.trail_lifetime, 1.0f);
    int pts = static_cast<int>(std::lround(static_cast<double>(life) * mul * 60.0));
    pts = std::clamp(pts, 3, 64);
    w.trail_pts += mp * pts;
  }
  return w;
}

SplitEffectCatalogWeight split_effect_heaviest_catalog_weight() {
  static SplitEffectCatalogWeight cached{};
  static std::once_flag once;
  std::call_once(once, [] {
    SplitEffectCatalogWeight best{};
    for (int32_t id : official_split_effect_ids()) {
      const SplitEffectCatalogWeight w = split_effect_catalog_weight(id);
      if (w.live > best.live || (w.live == best.live && w.trail_pts > best.trail_pts)) {
        best = w;
      }
    }
    cached = best;
  });
  return cached;
}

SplitEffectPrecommitPlan split_effect_floor_buffer_bound() {
  const SplitEffectCatalogWeight h = split_effect_heaviest_catalog_weight();
  SplitEffectPrecommitPlan plan =
      plan_from_weight(h.live * kPrecommitFloorInstances, h.trail_pts * kPrecommitFloorInstances,
                       h.systems * kPrecommitFloorInstances, kPrecommitFloorInstances);
  plan.heaviest_id = h.id;
  plan.heaviest_live = h.live;
  return plan;
}

SplitEffectPrecommitPlan split_effect_estimate_buffer_bound(const SplitEffectBoundNote* notes,
                                                            int n, int64_t appear_ms) {
  SplitEffectPrecommitPlan plan = split_effect_floor_buffer_bound();
  if (notes == nullptr || n <= 0) {
    return plan;
  }
  struct Ev {
    int64_t t = 0;
    int32_t id = 0;
    int sgn = 0;
  };
  std::vector<Ev> ev;
  ev.reserve(static_cast<std::size_t>(n) * 2u);
  for (int i = 0; i < n; ++i) {
    const int32_t id = notes[i].color_id;
    if (official_split_effect(id) == nullptr) {
      continue;
    }
    const int64_t t0 = notes[i].start_ms - appear_ms;
    const int64_t t1 = wds::chart_editor::official_split_destroy_ms(notes[i].start_ms, notes[i].end_ms);
    ev.push_back(Ev{t0, id, 1});
    ev.push_back(Ev{t1, id, -1});
  }
  if (ev.empty()) {
    return plan;
  }
  std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) {
    if (a.t != b.t) {
      return a.t < b.t;
    }
    return a.sgn < b.sgn;
  });
  int peak_live = 0;
  int peak_trail = 0;
  int peak_sys = 0;
  int peak_n = 0;
  int live = 0;
  int trail = 0;
  int sys = 0;
  int inst = 0;
  for (const Ev& e : ev) {
    const SplitEffectCatalogWeight w = split_effect_catalog_weight(e.id);
    const int c = e.sgn;
    inst += c;
    live += w.live * c;
    trail += w.trail_pts * c;
    sys += w.systems * c;
    if (live > peak_live || (live == peak_live && trail > peak_trail)) {
      peak_live = live;
      peak_trail = trail;
      peak_sys = sys;
      peak_n = inst;
    }
  }
  SplitEffectPrecommitPlan peak = plan_from_weight(peak_live, peak_trail, peak_sys, peak_n);
  peak.heaviest_id = plan.heaviest_id;
  peak.heaviest_live = plan.heaviest_live;
  raise_plan(plan, peak);
  return plan;
}

SplitEffectPrecommitPlan split_effect_plan_precommit(const int32_t* color_ids, int n) {
  if (color_ids == nullptr || n <= 0) {
    return split_effect_floor_buffer_bound();
  }
  std::vector<SplitEffectBoundNote> notes(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    notes[static_cast<std::size_t>(i)].color_id = color_ids[i];
    notes[static_cast<std::size_t>(i)].start_ms = 0;
    notes[static_cast<std::size_t>(i)].end_ms = 1;
  }
  return split_effect_estimate_buffer_bound(notes.data(), n, 0);
}

void split_effect_raise_precommit(const SplitEffectPrecommitPlan& plan) {
  if (!precommit_enabled()) {
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  PrecommitRt& rt = precommit_rt();
  if (plan.cache_entries_per_shard > rt.desired.cache_entries_per_shard ||
      plan.cache_slots_per_shard > rt.desired.cache_slots_per_shard) {
    rt.cache_shard = 0;
  }
  if (plan.carry_entries_per_shard > rt.desired.carry_entries_per_shard) {
    rt.carry_shard = 0;
  }
  if (plan.walk_entries_per_shard > rt.desired.walk_entries_per_shard) {
    rt.walk_shard = 0;
  }
  if (plan.trail_chunks > rt.desired.trail_chunks) {
    rt.worker_tls = false;
  }
  const bool first = rt.desired.particle_cap == 0;
  raise_plan(rt.desired, plan);
  rt.stats.plan = rt.desired;
  if (first && !rt.rss_started) {
    rt.stats.rss_kb_before = precommit_rss_kb();
    rt.rss_started = true;
  }
  rt.stats.request_us += std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
  rt.stats.pending = precommit_work_left(rt, nullptr) ? 1 : 0;
}

bool split_effect_precommit_pending() noexcept {
  return precommit_work_left(precommit_rt(), nullptr);
}

int split_effect_idle_precommit_budget_us() noexcept {
  constexpr double kFrameMs = 16.7;
  constexpr double kMarginMs = 1.0;
  const double prev = shuriken::split_eval_cache_prev_frame_ms();
  if (!(prev > 0.0 && prev + kMarginMs < kFrameMs)) {
    return 0;
  }
  return static_cast<int>((kFrameMs - prev - kMarginMs) * 1000.0);
}

int split_effect_pump_precommit(SplitEffectFrameCollect* io, int budget_us) {
  PrecommitRt& rt = precommit_rt();
  if (!precommit_enabled() || budget_us <= 0 || !precommit_work_left(rt, io)) {
    rt.stats.pending = 0;
    return 0;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const long flt0 = precommit_minflt();
  auto used_us = [&]() -> int {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
  };
  SplitEffectPrecommitPlan& d = rt.desired;
  while (used_us() < budget_us && precommit_work_left(rt, io)) {
    if (io != nullptr && rt.pool_have < d.particle_cap) {
      const int next = std::min(d.particle_cap, rt.pool_have + kPoolGrowSlice);
      if (io->pool.size() < static_cast<std::size_t>(next)) {
        io->pool.resize(static_cast<std::size_t>(next));
        wds::common::touch_pages(io->pool.data(), io->pool.size() * sizeof(SplitEffectLiveParticle));
        rt.stats.bytes_touched += static_cast<std::size_t>(next - rt.pool_have) *
                                  sizeof(SplitEffectLiveParticle);
      }
      io->pool.clear();
      rt.pool_have = next;
      continue;
    }
    if (io != nullptr && static_cast<int>(io->trails.chunk_count()) < d.trail_chunks) {
      io->trails.reserve_one_chunk();
      rt.stats.bytes_touched += SplitEffectTrailArena::kChunk * sizeof(float);
      continue;
    }
    if (rt.cache_shard < shuriken::split_eval_cache_shard_count()) {
      shuriken::split_eval_cache_precommit_one_shard(rt.cache_shard,
                                                     static_cast<std::size_t>(d.cache_slots_per_shard),
                                                     static_cast<std::size_t>(d.cache_entries_per_shard));
      ++rt.cache_shard;
      continue;
    }
    if (rt.carry_shard < shuriken::split_eval_carry_shard_count()) {
      shuriken::split_eval_carry_precommit_one_shard(
          rt.carry_shard, static_cast<std::size_t>(d.carry_entries_per_shard));
      ++rt.carry_shard;
      continue;
    }
    if (rt.walk_shard < shuriken::split_eval_carry_walk_shard_count()) {
      shuriken::split_eval_carry_precommit_one_walk(
          rt.walk_shard, static_cast<std::size_t>(d.walk_entries_per_shard));
      ++rt.walk_shard;
      continue;
    }
    if (rt.persist_have < d.persist_stores) {
      if (shuriken::split_eval_persist_precommit_one_store(static_cast<std::size_t>(d.persist_slots))) {
        ++rt.persist_have;
      } else {
        rt.persist_have = d.persist_stores;
      }
      continue;
    }
    if (!rt.worker_tls) {
      WorkerPrecommitCtx wctx;
      wctx.trail_chunks = d.trail_chunks;
      const int workers = std::max(1, split_collect_thread_count());
      split_collect_parallel_for(0, static_cast<size_t>(workers), 1, worker_precommit_fn, &wctx,
                                 99999);
      shuriken::split_eval_precommit_tls();
      rt.worker_tls = true;
      continue;
    }
    break;
  }
  const int spent = used_us();
  rt.stats.wall_us += spent;
  rt.stats.minflt += precommit_minflt() - flt0;
  rt.stats.rss_kb_after = precommit_rss_kb();
  rt.stats.plan = d;
  rt.stats.pending = precommit_work_left(rt, io) ? 1 : 0;
  return spent;
}

SplitEffectPrecommitStats split_effect_precommit_stats() noexcept {
  return precommit_rt().stats;
}

SplitEffectPrecommitStats split_effect_precommit_buffers(const SplitEffectPrecommitPlan& plan,
                                                         SplitEffectFrameCollect* io) {
  split_effect_raise_precommit(plan);
#if WDS_DIAG_ENABLED
  wds::common::minflt_stage("precommit.begin");
#endif  // WDS_DIAG_ENABLED
  while (split_effect_precommit_pending() ||
         (io != nullptr && precommit_work_left(precommit_rt(), io))) {
    if (split_effect_pump_precommit(io, 1000000) <= 0) {
      break;
    }
  }
#if WDS_DIAG_ENABLED
  wds::common::minflt_stage("precommit.end");
#endif  // WDS_DIAG_ENABLED
  SplitEffectPrecommitStats st = split_effect_precommit_stats();
  static const bool diag = [] {
    const char* v = std::getenv("WDS_FRAME_DIAG");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
  }();
  if (!diag) {
    return st;
  }
#if WDS_DIAG_ENABLED
  std::fprintf(stderr,
               "PRECOMMIT wall_us=%lld minflt=%ld rss_kb=%ld->%ld pool=%d trails=%d verts=%zu "
               "heaviest=%d floor_n=%d pending=%d\n",
               static_cast<long long>(st.wall_us), st.minflt, st.rss_kb_before, st.rss_kb_after,
               st.plan.particle_cap, st.plan.trail_chunks, st.plan.draw_verts, st.plan.heaviest_id,
               st.plan.floor_instances, st.pending);
#endif  // WDS_DIAG_ENABLED
  return st;
}


namespace {

// Diagnostics only. Publishes this collect's stage totals. Called from every
// exit path of collect_split_effect_frame so the SPLITPHASE line always names
// the last completed frame instead of a stale one. No-op when perf is off.
//
// frame_scope: true only for the whole-frame collect (one call per preview
// frame). It gates the SPLITPHASE / SPLITCOLD / POOLSTAT stderr lines; the
// per-split entry point (tests / bench / snapshot bake) publishes its counters
// without spamming a line per split.
void diag_finish_frame(bool frame_scope) noexcept {
#if WDS_DIAG_ENABLED
  if (!wds::common::perf::enabled()) {
    return;
  }
  diag_publish_frame();
  if (!frame_scope) {
    return;
  }
  // SPLITPHASE: exactly one line per preview frame. stderr (not WDS_LOG):
  // this build has WDS_ENABLE_LOGGING=0, so WDS_LOG only reaches the crash
  // ring and the harness would never see the line.
  {
    char buf[1536];
    const std::size_t n = format_split_effect_phase_line(buf, sizeof(buf));
    if (n > 0) {
      std::fprintf(stderr, "%s\n", buf);
    }
  }
  // SPLITCOLD: once per cold (post-seek / post-doc-invalidate / post-preload)
  // collect. The cold flag is what the existing epoch check already decided;
  // invalidation timing is untouched.
  const int reason = g_diag.cold_reason.load(std::memory_order_relaxed);
  if (reason != 0) {
    char buf[256];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "SPLITCOLD frame=%llu reason=%s systems=%d particles=%d steps=%lld evaluate_ms=%.3f",
        static_cast<unsigned long long>(g_diag.frame.load(std::memory_order_relaxed)),
        cold_reason_name(reason),
        static_cast<int>(g_diag.count[static_cast<int>(SplitDiagCount::Systems)].load(
            std::memory_order_relaxed)),
        static_cast<int>(g_diag.count[static_cast<int>(SplitDiagCount::Particles)].load(
            std::memory_order_relaxed)),
        static_cast<long long>(g_diag.count[static_cast<int>(SplitDiagCount::Steps)].load(
            std::memory_order_relaxed)),
        static_cast<double>(
            g_diag_last_phase_ns[static_cast<int>(SplitDiagPhase::Evaluate)].load(
                std::memory_order_relaxed)) /
            1e6);
    if (n > 0) {
      std::fprintf(stderr, "%s\n", buf);
    }
  }
  // POOLSTAT: cumulative pool counters once per wall-clock second, keyed off
  // the same 1 s cadence the editor's frame log uses. Emitted here because
  // chart-render cannot see the Qt logger's window.
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  const int64_t prev = g_poolstat_last_ns.load(std::memory_order_relaxed);
  if (prev == 0 || now_ns - prev >= 1000000000ll) {
    g_poolstat_last_ns.store(now_ns, std::memory_order_relaxed);
    char pbuf[256];
    const std::size_t pn = format_split_effect_poolstat_line(pbuf, sizeof(pbuf));
    if (pn > 0) {
      std::fprintf(stderr, "%s\n", pbuf);
    }
    split_collect_diag_reset();
  }
#endif  // WDS_DIAG_ENABLED
}

}  // namespace

void collect_split_effect_frame(
    const std::vector<wds::chart_editor::PreviewSplitLaneInstance>& splits, int quality,
    double now_sec, bool playing, SplitEffectPlaybackState& state, SplitEffectFrameCollect& io) {
  const OwnerThreadGuard owner_guard;
#if WDS_DIAG_ENABLED
  wds::common::minflt_stage("collect.begin");
#endif  // WDS_DIAG_ENABLED
  // Diagnostics: whole-collect stage total. Nested SplitDiagScopes add the
  // sub-stages; this one is the outer bound. Off => no clock read.
#if WDS_DIAG_ENABLED
  const bool diag = wds::common::perf::enabled();
  const auto diag_t0 = diag ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
  if (diag) {
    g_diag.reset_stages();
    g_diag.frame.store(++g_split_frame_seq, std::memory_order_relaxed);
    g_diag.playing.store(playing ? 1 : 0, std::memory_order_relaxed);
    g_diag.quality.store(quality, std::memory_order_relaxed);
    g_diag.splits.store(static_cast<int>(splits.size()), std::memory_order_relaxed);
  }
#endif  // WDS_DIAG_ENABLED
  split_collect_begin_frame();
  shuriken::begin_eval_system_hash_frame();
  {
    thread_local double tls_collect_now = std::numeric_limits<double>::quiet_NaN();
    thread_local std::uint32_t tls_epoch = 0;
    const std::uint32_t epoch = shuriken::split_eval_cache_epoch();
    const bool backward = std::isfinite(tls_collect_now) && now_sec + 1e-9 < tls_collect_now;
    const bool cold = epoch != tls_epoch || !std::isfinite(tls_collect_now);
    split_collect_set_burst(cold || backward);
    if (cold || backward) {
      split_collect_prepare_heavy(50);
    }
    if (backward) {
      shuriken::split_eval_persist_clear();
    }
#if WDS_DIAG_ENABLED
    if (diag) {
      // Invalidation semantics are untouched; this only labels what the
      // existing epoch/backward check already decided:
      //   seek    - split_effect_clear_caches() (chart_preview_panel seek)
      //   doc     - reset_split_effect_auto_seed_session(true) / quality switch
      //             / the first collect of the session (nothing cached yet)
      //   preload - the seek pre-roll armed it and no explicit marker arrived
      //   seek    - fallback: a timeline jump with no explicit invalidation
      int reason = 0;
      if (cold || backward) {
        reason = g_cold_reason.exchange(0, std::memory_order_relaxed);
        if (reason == 0) {
          reason = g_preload_armed.exchange(0, std::memory_order_relaxed) != 0 ? 3 : 1;
        }
        if (!std::isfinite(tls_collect_now)) {
          reason = 2;  // first collect of the session
        }
      }
      g_preload_armed.store(0, std::memory_order_relaxed);
      g_diag.cold.store(reason != 0 ? 1 : 0, std::memory_order_relaxed);
      g_diag.cold_reason.store(reason, std::memory_order_relaxed);
    }
#endif  // WDS_DIAG_ENABLED
    tls_collect_now = now_sec;
    tls_epoch = epoch;
  }
  static int last_quality = quality;
  if (last_quality != quality) {
    last_quality = quality;
    shuriken::split_eval_cache_bump_epoch();
    shuriken::split_eval_persist_clear();
    clear_dead_latches();
    // Diagnostics only: a quality switch is a document-side invalidation.
    // The next collect's cold flag picks this up via the epoch change.
#if WDS_DIAG_ENABLED
    if (diag) {
      g_cold_reason.store(2, std::memory_order_relaxed);
    }
#endif  // WDS_DIAG_ENABLED
  }
  const int cap = kSplitEffectParticlePool;
  if (io.pool.capacity() < static_cast<size_t>(cap)) {
    io.pool.reserve(static_cast<size_t>(cap));
  }
#if WDS_DIAG_ENABLED
  wds::common::minflt_stage("collect.after_pool");
#endif  // WDS_DIAG_ENABLED
  io.split_begin.assign(splits.size(), 0);
  io.split_count.assign(splits.size(), 0);
  io.total = 0;
  if (splits.empty()) {
    // Still exactly one SPLITPHASE line for this frame (all zeros).
    diag_finish_frame(true);
    return;
  }
  if (playing && std::isfinite(state.last_now_sec) &&
      now_sec + 1e-9 < state.last_now_sec) {
    diag_finish_frame(true);
    return;
  }
  if (!playing) {
    clear_dead_latches();
  }
  if (playing) {
    shuriken::split_eval_cache_begin_play_frame();
  }

  FrameScratch& fs = frame_scratch();
  fs.task_used = 0;
  fs.preps.assign(splits.size(), SplitPrep{});
  io.trails.reset();
  split_effect_trail_begin_frame();

  int remaining = cap;
  int offset = 0;
  int last_visited = -1;
#if WDS_DIAG_ENABLED
  split_wall_begin_frame();
#endif  // WDS_DIAG_ENABLED
  {
    SplitWallScope wall_frame(SplitWallSec::Frame);
#if WDS_DIAG_ENABLED
    split_wall_note_splits(static_cast<int>(splits.size()));
#endif  // WDS_DIAG_ENABLED
    // A stays per-split (seed / bind order). Plan+realize of every split
    // share one pool wave; mark still walks splits in order with the same
    // remaining budget so assemble keeps the identical set.
    {
      SplitWallScope wall_a(SplitWallSec::PhaseA);
      SplitDiagScope diag_a(SplitDiagPhase::Prepare);
      for (size_t k = 0; k < splits.size(); ++k) {
        prepare_split_tasks(fs, fs.preps[k], splits[k], quality, now_sec, nullptr, playing);
      }
    }
    {
      SplitWallScope wall_b(SplitWallSec::PhaseB);
      SplitDiagScope diag_b(SplitDiagPhase::Evaluate);
      run_eval_tasks_all(fs, fs.preps, remaining);
    }
    for (size_t k = 0; k < splits.size(); ++k) {
      if (remaining <= 0) {
        break;
      }
      io.split_begin[k] = offset;
      {
        SplitWallScope wall_c(SplitWallSec::PhaseC);
        if (fs.preps[k].has_work) {
          SplitWallScope wall_leak(SplitWallSec::Leak);
          apply_tmp_reuse_leak(fs, fs.preps[k].task_begin, fs.preps[k].task_end);
        }
        int n = 0;
        {
          SplitDiagScope diag_asm(SplitDiagPhase::Assemble);
          n = assemble_split(fs, fs.preps[k], nullptr, remaining, &io.trails, &io.pool, offset);
        }
        io.split_count[k] = n;
        io.total += n;
        remaining -= n;
        offset += n;
        // Skip later binds only after every in-life job is gone (alive, not
        // just alpha-culled) and Duration has closed. Conservative keep
        // (may_emit) stays true for a while after the last particle dies;
        // remaking RoD + include_expired parents there is the empty-frame
        // collect tax. Looping never latches (emission never closes).
        if (playing && n == 0 && fs.preps[k].entry != nullptr &&
            shuriken::dead_system_skip_enabled() && !shuriken::observers_installed() &&
            catalog_effect_emission_closed(*fs.preps[k].entry, fs.preps[k].now_t) &&
            !split_tasks_have_life(fs, fs.preps[k])) {
          dead_latches()[DeadLatchKey{fs.preps[k].color_id, fs.preps[k].start_ms}] =
              fs.preps[k].now_t;
        }
      }
      last_visited = static_cast<int>(k);
    }
  }
#if WDS_DIAG_ENABLED
  split_wall_end_frame();
#endif  // WDS_DIAG_ENABLED
#if WDS_DIAG_ENABLED
  wds::common::minflt_stage("collect.after_eval");
#endif  // WDS_DIAG_ENABLED
  shuriken::split_eval_carry_set_store_inhibited(false);

  if (playing && last_visited >= 0) {
    state.last_now_sec = now_sec;
  }
  if (playing) {
    shuriken::split_eval_cache_reap_unused();
  }
#if WDS_DIAG_ENABLED
  if (diag) {
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - diag_t0)
                           .count();
    split_diag_add_ns(SplitDiagPhase::Collect, ns);
    g_diag.particles.store(io.total, std::memory_order_relaxed);
  }
#endif  // WDS_DIAG_ENABLED
  diag_finish_frame(true);
#if WDS_DIAG_ENABLED
  wds::common::minflt_stage("collect.end");
#endif  // WDS_DIAG_ENABLED
}

}  // namespace wds::chart_render

#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/preview_snapshot.hpp>
#include <wds/core/split_fade.hpp>

#include <shuriken/split_eval_persist.hpp>
#include <split_collect_pool.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

using wds::chart_editor::PreviewSplitLaneInstance;
using wds::chart_editor::SplitLaneType;
using wds::chart_render::SplitEffectFrameCollect;
using wds::chart_render::SplitEffectLiveParticle;
using wds::chart_render::SplitEffectPlaybackState;

constexpr int64_t kStartMs = 10000;

PreviewSplitLaneInstance make_split(int32_t effect_id, int32_t split_count, int64_t start_ms,
                                    int32_t source_note_id) {
  PreviewSplitLaneInstance split;
  const int32_t src = source_note_id != 0 ? source_note_id : effect_id;
  split.apply_identity(src, split_count, SplitLaneType::Full, effect_id, start_ms, start_ms + 30000);
  split.apply_state(true, false, split_count + 1);
  split.apply_animation(1.0f, 0.0f, 1.0f, 0.0f, 1);
  split.should_show = true;
  split.split_line_alpha = 1.0f;
  return split;
}

std::vector<PreviewSplitLaneInstance> make_instances(int32_t effect_id, int instances,
                                                     double stagger_sec) {
  std::vector<PreviewSplitLaneInstance> out;
  for (int i = 0; i < instances; ++i) {
    const int64_t start_ms =
        kStartMs + static_cast<int64_t>(std::llround(stagger_sec * 1000.0 * static_cast<double>(i)));
    out.push_back(make_split(effect_id, 6, start_ms, effect_id * 10 + i + 1));
  }
  return out;
}

double play_origin_sec(int64_t start_ms) {
  return static_cast<double>(start_ms) / 1000.0 -
         static_cast<double>(wds::chart_editor::kOfficialSplitFadeInSec);
}

double now_sec_from_now_t(double now_t) {
  return play_origin_sec(kStartMs) + now_t;
}

bool bits_eq(float a, float b) {
  uint32_t ua = 0;
  uint32_t ub = 0;
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua == ub;
}

bool vec_eq(const wds::chart_render::SplitEffectTrailChan& a,
            const wds::chart_render::SplitEffectTrailChan& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (!bits_eq(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

bool live_eq(const SplitEffectLiveParticle& a, const SplitEffectLiveParticle& b) {
  return a.line_index == b.line_index && a.catalog_index == b.catalog_index && a.seed == b.seed &&
         a.element_index == b.element_index && a.trail_points == b.trail_points &&
         bits_eq(a.world_x, b.world_x) && bits_eq(a.main_y, b.main_y) && bits_eq(a.z, b.z) &&
         bits_eq(a.age, b.age) && bits_eq(a.birth_t, b.birth_t) && bits_eq(a.size, b.size) &&
         bits_eq(a.size_y, b.size_y) && bits_eq(a.roll, b.roll) && bits_eq(a.r, b.r) &&
         bits_eq(a.g, b.g) && bits_eq(a.b, b.b) && bits_eq(a.a, b.a) && bits_eq(a.vel_x, b.vel_x) &&
         bits_eq(a.vel_y, b.vel_y) && bits_eq(a.vel_z, b.vel_z) && bits_eq(a.stored_vx, b.stored_vx) &&
         bits_eq(a.total_vx, b.total_vx) && bits_eq(a.total_vy, b.total_vy) &&
         bits_eq(a.total_vz, b.total_vz) && vec_eq(a.trail_x, b.trail_x) &&
         vec_eq(a.trail_y, b.trail_y) && vec_eq(a.trail_z, b.trail_z);
}

const char* frame_diff(const SplitEffectFrameCollect& a, const SplitEffectFrameCollect& b) {
  if (a.total != b.total) {
    static char buf[80];
    std::snprintf(buf, sizeof(buf), "count %d vs %d", a.total, b.total);
    return buf;
  }
  for (int i = 0; i < a.total; ++i) {
    const auto& pa = a.pool[static_cast<size_t>(i)];
    const auto& pb = b.pool[static_cast<size_t>(i)];
    if (!live_eq(pa, pb)) {
      static char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "i=%d line=%d/%d cat=%u/%u seed=%u/%u age=%.5f/%.5f x=%.6g/%.6g y=%.6g/%.6g", i,
                    pa.line_index, pb.line_index, pa.catalog_index, pb.catalog_index, pa.seed,
                    pb.seed, pa.age, pb.age, pa.world_x, pb.world_x, pa.main_y, pb.main_y);
      return buf;
    }
  }
  return nullptr;
}

bool frame_eq(const SplitEffectFrameCollect& a, const SplitEffectFrameCollect& b) {
  return frame_diff(a, b) == nullptr;
}

void collect_at(const std::vector<PreviewSplitLaneInstance>& splits, double now_t,
                SplitEffectFrameCollect& io) {
  SplitEffectPlaybackState state;
  wds::chart_render::collect_split_effect_frame(splits, 0, now_sec_from_now_t(now_t), false, state,
                                               io);
}

std::vector<double> seek_sequence(int32_t effect_id) {
  const double times[] = {0.20, 0.50, 1.00, 1.50};
  std::vector<double> seq;
  for (double t : times) {
    seq.push_back(t);
  }
  for (int i = 3; i >= 0; --i) {
    seq.push_back(times[i]);
  }
  std::mt19937 rng(static_cast<uint32_t>(effect_id) * 17u + 3u);
  std::vector<int> order{0, 1, 2, 3};
  std::shuffle(order.begin(), order.end(), rng);
  for (int idx : order) {
    seq.push_back(times[idx]);
  }
  return seq;
}

void test_effect_seeks(int32_t effect_id, int instances, double stagger) {
  if (wds::chart_render::official_split_effect(effect_id) == nullptr) {
    std::fprintf(stderr, "skip unknown effect %d\n", effect_id);
    return;
  }
  const auto splits = make_instances(effect_id, instances, stagger);
  const std::vector<double> seq = seek_sequence(effect_id);

  wds::chart_render::reset_split_effect_auto_seed_session();
  wds::chart_render::split_effect_clear_caches();
  std::vector<SplitEffectFrameCollect> cached(seq.size());
  for (size_t i = 0; i < seq.size(); ++i) {
    collect_at(splits, seq[i], cached[i]);
  }

  wds::chart_render::reset_split_effect_auto_seed_session();
  std::vector<SplitEffectFrameCollect> fresh(seq.size());
  for (size_t i = 0; i < seq.size(); ++i) {
    wds::chart_render::split_effect_clear_caches();
    collect_at(splits, seq[i], fresh[i]);
  }

  for (size_t i = 0; i < seq.size(); ++i) {
    if (const char* d = frame_diff(cached[i], fresh[i])) {
      std::fprintf(stderr, "seek effect=%d i=%zu t=%.2f %s\n", effect_id, i, seq[i], d);
      CHECK(false);
    }
  }
}

void test_effect_seeks_retain_cache(int32_t effect_id, int instances, double stagger) {
  if (wds::chart_render::official_split_effect(effect_id) == nullptr) {
    std::fprintf(stderr, "skip unknown effect %d\n", effect_id);
    return;
  }
  const auto splits = make_instances(effect_id, instances, stagger);
  const std::vector<double> seq = seek_sequence(effect_id);

  // Reference: every collect is a fresh cold rebuild (full clear between each).
  wds::chart_render::reset_split_effect_auto_seed_session();
  std::vector<SplitEffectFrameCollect> fresh(seq.size());
  for (size_t i = 0; i < seq.size(); ++i) {
    wds::chart_render::split_effect_clear_caches();
    collect_at(splits, seq[i], fresh[i]);
  }

  // Seek path: drop only time-ordered state between collects; keep the eval
  // cache. Must match the fresh-with-full-clear reference frame for frame.
  wds::chart_render::reset_split_effect_auto_seed_session();
  wds::chart_render::split_effect_clear_caches();
  std::vector<SplitEffectFrameCollect> cached(seq.size());
  for (size_t i = 0; i < seq.size(); ++i) {
    wds::chart_render::split_effect_clear_seek_state();
    collect_at(splits, seq[i], cached[i]);
  }

  for (size_t i = 0; i < seq.size(); ++i) {
    if (const char* d = frame_diff(cached[i], fresh[i])) {
      std::fprintf(stderr, "seek-retain effect=%d i=%zu t=%.2f %s\n", effect_id, i, seq[i], d);
      CHECK(false);
    }
  }

  // The retained cache must stay bounded after the full sequence.
  const auto stats = wds::chart_render::split_effect_eval_cache_stats();
  CHECK(stats.bytes_used <= 64ull * 1024ull * 1024ull);  // <= 64 MB
}

void test_fadein_cap_skips_collect() {
  using wds::chart_render::split_effect_fadein_extras_open;
  CHECK(wds::chart_render::kSplitEffectLinePlayWindowSec ==
        wds::chart_render::kSplitEffectFadeinCollectCapSec);
  CHECK(split_effect_fadein_extras_open(0.0f));
  CHECK(split_effect_fadein_extras_open(10.0f));
  CHECK(!split_effect_fadein_extras_open(-1.0e-3f));
  CHECK(!split_effect_fadein_extras_open(10.001f));

  if (wds::chart_render::official_split_effect(11710) == nullptr) {
    std::fprintf(stderr, "11710 missing from catalog\n");
    CHECK(false);
    return;
  }
  const auto splits = make_instances(11710, 1, 0.0);
  wds::chart_render::reset_split_effect_auto_seed_session(true);
  wds::chart_render::split_effect_clear_caches();
  SplitEffectFrameCollect inside;
  collect_at(splits, 0.7, inside);
  CHECK(inside.total > 0);

  wds::chart_render::reset_split_effect_auto_seed_session(true);
  wds::chart_render::split_effect_clear_caches();
  SplitEffectFrameCollect past;
  collect_at(splits, 10.5, past);
  CHECK(past.total == 0);
  const auto after_past = wds::chart_render::split_effect_eval_cache_stats();
  CHECK(after_past.pool_frame_wakeups == 0);
  CHECK(after_past.pool_frame_max_participants == 0);
}

void test_cold_empty_does_not_arm_heavy() {
  wds::chart_render::reset_split_effect_auto_seed_session(true);
  wds::chart_render::split_effect_clear_caches();
  std::vector<PreviewSplitLaneInstance> empty;
  SplitEffectFrameCollect io;
  collect_at(empty, 1.0, io);
  CHECK(io.total == 0);
  const auto after_cold = wds::chart_render::split_effect_eval_cache_stats();
  CHECK(after_cold.pool_last_call_participants == 0);
  CHECK(after_cold.pool_frame_max_participants == 0);
  CHECK(after_cold.pool_frame_wakeups == 0);

  const auto heavy = make_instances(10380, 4, 0.2);
  collect_at(heavy, 1.0, io);
  CHECK(io.total > 0);
  collect_at(empty, 0.5, io);
  CHECK(io.total == 0);
  const auto after_back = wds::chart_render::split_effect_eval_cache_stats();
  CHECK(after_back.pool_frame_wakeups == 0);
  CHECK(after_back.pool_frame_max_participants == 0);
}

void test_empty_collect_wakes_nobody() {
  const auto heavy = make_instances(10380, 4, 0.2);
  wds::chart_render::split_effect_clear_caches();
  wds::chart_render::reset_split_effect_auto_seed_session();
  SplitEffectFrameCollect io;
  collect_at(heavy, 1.0, io);
  CHECK(io.total > 0);
  const auto after_heavy = wds::chart_render::split_effect_eval_cache_stats();
  CHECK(after_heavy.pool_frame_max_participants >= 1);

  std::vector<PreviewSplitLaneInstance> empty;
  SplitEffectPlaybackState state;
  collect_at(empty, 1.0, io);
  const auto after_empty = wds::chart_render::split_effect_eval_cache_stats();
  CHECK(after_empty.pool_last_call_participants == 0);
  CHECK(after_empty.pool_frame_max_participants == 0);
  CHECK(after_empty.pool_frame_wakeups == 0);

  int dummy = 0;
  wds::chart_render::split_collect_parallel_for(
      6,
      [](size_t, void* raw) {
        *static_cast<int*>(raw) += 1;
      },
      &dummy);
  CHECK(dummy == 6);
  CHECK(wds::chart_render::split_collect_last_call_participants() == 1);
  CHECK(wds::chart_render::split_collect_frame_wakeups() == 0);
}

void test_identity_no_collision_across_instances() {
  const auto splits = make_instances(11621, 4, 0.2);
  wds::chart_render::split_effect_clear_caches();
  wds::chart_render::reset_split_effect_auto_seed_session();
  SplitEffectFrameCollect io;
  collect_at(splits, 1.0, io);
  CHECK(io.total > 0);
  const auto stats = wds::chart_render::split_effect_eval_cache_stats();
  CHECK(stats.enabled);
  CHECK(stats.particle_entries > 0);
}

// Budget pressure + concurrent claim/finish so eviction (trim_budget under the
// shard lock) interleaves with commits. Global counters must not drift to
// absurd values and must read zero after a clear.
void test_persist_budget_pressure_counters() {
  namespace sh = wds::chart_render::shuriken;
  sh::split_eval_persist_set_enabled_for_tests(1);
  sh::split_eval_persist_clear();
  const auto base = sh::split_eval_persist_stats();
  CHECK(base.bytes_used == 0 && base.systems == 0 && base.particles == 0);

  constexpr int kThreads = 4;
  constexpr int kKeys = 48;
  constexpr int kRounds = 6;
  constexpr int kJobs = 96;
  std::vector<std::thread> pool;
  for (int t = 0; t < kThreads; ++t) {
    pool.emplace_back([t] {
      std::vector<sh::PrimaryEvalJob> jobs(static_cast<std::size_t>(kJobs));
      for (int round = 0; round < kRounds; ++round) {
        const float now = 1.0f + 0.01f * static_cast<float>(round);
        for (int k = 0; k < kKeys; ++k) {
          sh::SystemCarryKey key;
          key.line_index = t * kKeys + k;
          key.catalog_index = static_cast<uint32_t>(k);
          key.kind = sh::kPersistKindPrimary;
          const int n = 8 + ((k * 7 + round * 13) % (kJobs - 8));
          for (int j = 0; j < kJobs; ++j) {
            sh::PrimaryEvalJob& job = jobs[static_cast<std::size_t>(j)];
            job.b.index = j;
            job.b.time = 0.5f;
            job.seed = static_cast<uint32_t>(j);
            job.has_state = j < n ? 1 : 0;
            job.alive = j < n ? 1 : 0;
          }
          sh::PersistClaim claim;
          sh::split_eval_persist_claim(key, now, claim);
          sh::persist_finish(claim, key, now, jobs);
        }
      }
    });
  }
  for (std::thread& th : pool) {
    th.join();
  }

  const auto s = sh::split_eval_persist_stats();
  const std::size_t max_systems = static_cast<std::size_t>(kThreads) * kKeys;
  const std::size_t max_particles = max_systems * static_cast<std::size_t>(kJobs);
  CHECK(s.commits > 0);
  CHECK(s.systems <= max_systems);
  CHECK(s.particles <= max_particles);
  // Eviction keeps at most one over-budget store per shard; anything near
  // size_t wrap-around means an underflow.
  CHECK(s.bytes_used <= s.budget_bytes * 2);
  CHECK(s.systems > 0 || s.bytes_used == 0);

  sh::split_eval_persist_clear();
  const auto z = sh::split_eval_persist_stats();
  CHECK(z.bytes_used == 0 && z.systems == 0 && z.particles == 0);
  sh::split_eval_persist_set_enabled_for_tests(-1);
}

}  // namespace

int main() {
  // Small persist budget so the pressure test evicts; read when the registry
  // is first created, so set it before anything touches the persist store.
#ifdef _WIN32
  _putenv_s("WDS_SPLIT_PERSIST_MB", "1");
#else
  setenv("WDS_SPLIT_PERSIST_MB", "1", 1);
#endif
  test_persist_budget_pressure_counters();
  test_effect_seeks(11621, 4, 0.2);
  test_effect_seeks(10660, 1, 0.0);
  test_effect_seeks(10380, 1, 0.0);
  test_effect_seeks(10516, 1, 0.0);
  test_effect_seeks(10600, 1, 0.0);
  test_effect_seeks_retain_cache(11621, 4, 0.2);
  test_effect_seeks_retain_cache(10660, 1, 0.0);
  test_effect_seeks_retain_cache(10380, 1, 0.0);
  test_effect_seeks_retain_cache(10516, 1, 0.0);
  test_effect_seeks_retain_cache(10600, 1, 0.0);
  test_identity_no_collision_across_instances();
  test_fadein_cap_skips_collect();
  test_cold_empty_does_not_arm_heavy();
  test_empty_collect_wakes_nobody();
  if (failures == 0) {
    std::puts("split_effect_seek_cache_tests passed");
    return 0;
  }
  std::fprintf(stderr, "%d seek-cache test(s) failed\n", failures);
  return 1;
}

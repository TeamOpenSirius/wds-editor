// Scale behaviour of the split-line pose cache: many keys must stay correct,
// stay inside the byte budget (eviction) and keep per-store cost flat.

#include <shuriken/split_eval_cache.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using wds::chart_render::shuriken::IntegratorGridState;
using wds::chart_render::shuriken::ParticleCacheKey;

int failures = 0;

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

ParticleCacheKey make_key(std::uint32_t epoch, int i) {
  ParticleCacheKey k;
  k.epoch = epoch;
  k.color_id = i & 7;
  k.line_index = i >> 3;
  k.line_start_ms = 1000;
  k.catalog_index = 3;
  k.birth_index = i;
  k.seed = 17;
  k.input_hash = 0x1234;
  k.valid = 1;
  return k;
}

IntegratorGridState make_state(int i) {
  IntegratorGridState st;
  st.lx = static_cast<float>(i);
  st.next_step = 30;
  return st;
}

double store_many(std::uint32_t epoch, int first, int count) {
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = first; i < first + count; ++i) {
    wds::chart_render::shuriken::split_eval_cache_store(make_key(epoch, i), make_state(i));
  }
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() /
         static_cast<double>(count);
}

}  // namespace

int main() {
  using namespace wds::chart_render::shuriken;
  // 4 MB budget: far fewer bytes than the keys below, so eviction runs on
  // nearly every store once the cache is warm.
#ifdef _WIN32
  _putenv_s("WDS_SPLIT_CACHE_MB", "4");
#else
  setenv("WDS_SPLIT_CACHE_MB", "4", 1);
#endif
  CHECK(split_eval_cache_enabled());
  const std::uint32_t epoch = split_eval_cache_epoch();

  constexpr int kKeys = 200000;
  const double us_early = store_many(epoch, 0, 20000);
  const double us_late = store_many(epoch, 20000, kKeys - 20000);
  std::printf("store us/op: first 20k %.3f, rest %.3f\n", us_early, us_late);
  // Per-store cost must not grow with the number of stored/evicted keys.
  CHECK(us_late < 50.0 * (us_early > 0.05 ? us_early : 0.05));
  CHECK(us_late < 200.0);

  const SplitEvalCacheStats s = split_eval_cache_stats();
  CHECK(s.particle_entries > 0);
  CHECK(s.particle_entries < static_cast<std::size_t>(kKeys));
  CHECK(s.bytes_used <= s.budget_bytes + 256u * 4096u);
  CHECK(s.bytes_used >= s.particle_entries * 256u);

  // Whatever is still cached must be the right entry, and the newest keys of
  // each shard must have survived (LRU-ish eviction).
  int hits = 0;
  int recent_hits = 0;
  for (int i = 0; i < kKeys; ++i) {
    IntegratorGridState out;
    if (split_eval_cache_lookup(make_key(epoch, i), 1000, out)) {
      ++hits;
      CHECK(out.next_step == 30);
      CHECK(out.lx == static_cast<float>(i));
      if (i >= kKeys - 1000) {
        ++recent_hits;
      }
    }
  }
  CHECK(hits > 0);
  CHECK(recent_hits > 900);
  CHECK(static_cast<std::size_t>(hits) <= s.particle_entries);

  split_eval_cache_clear();
  const SplitEvalCacheStats z = split_eval_cache_stats();
  CHECK(z.bytes_used == 0 && z.particle_entries == 0);
  // Refill after clear (recycled entries) keeps byte accounting consistent.
  store_many(epoch, 0, 50000);
  const SplitEvalCacheStats r = split_eval_cache_stats();
  CHECK(r.bytes_used >= r.particle_entries * 256u);
  CHECK(r.bytes_used <= r.budget_bytes + 256u * 4096u);

  if (failures == 0) {
    std::puts("split_eval_cache_scale_tests passed");
    return 0;
  }
  std::fprintf(stderr, "%d scale test(s) failed\n", failures);
  return 1;
}

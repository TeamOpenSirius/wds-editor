#include "split_eval_cache.hpp"
#include "split_eval_carry.hpp"
#include "split_eval_persist.hpp"
#include "split_wall_profile.hpp"

#include <wds/common/page_touch.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace wds::chart_render::shuriken {
namespace {

constexpr int kShards = 256;
constexpr int kDefaultStride = 30;
constexpr int kMaxAligned = 20;
constexpr std::size_t kEntryOverhead = 256;
constexpr std::size_t kMinCap = 8;

struct KeyHash {
  std::size_t operator()(const ParticleCacheKey& k) const noexcept {
    std::size_t h = k.epoch;
    h = h * 1315423911u + static_cast<std::size_t>(k.color_id);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_index);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_start_ms);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_start_ms >> 32);
    h = h * 1315423911u + static_cast<std::size_t>(k.catalog_index);
    h = h * 1315423911u + static_cast<std::size_t>(k.birth_index);
    h = h * 1315423911u + static_cast<std::size_t>(k.birth_extra);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_play_gen);
    h = h * 1315423911u + static_cast<std::size_t>(k.seed);
    h ^= static_cast<std::size_t>(k.input_hash) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }
};

std::uint64_t key_hash64(const ParticleCacheKey& k) noexcept {
  const std::uint64_t h = static_cast<std::uint64_t>(KeyHash{}(k));
  return h == 0 ? 1 : h;
}

int parse_int_env(const char* name, int fallback) noexcept {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long v = std::strtol(raw, &end, 10);
  if (end == raw) {
    return fallback;
  }
  return static_cast<int>(v);
}

bool parse_enabled() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_CACHE");
  if (raw == nullptr || raw[0] == '\0') {
    return true;
  }
  const std::string_view v(raw);
  return v != "0" && v != "off" && v != "false" && v != "OFF";
}

std::size_t parse_budget() noexcept {
  const int mb = parse_int_env("WDS_SPLIT_CACHE_MB", 256);
  if (mb <= 0) {
    return 1;
  }
  return static_cast<std::size_t>(mb) * 1024ull * 1024ull;
}

int parse_stride() noexcept {
  const int s = parse_int_env("WDS_SPLIT_CACHE_STRIDE", kDefaultStride);
  return s < 1 ? kDefaultStride : s;
}

struct Entry {
  IntegratorGridState high{};
  IntegratorGridState recent{};
  IntegratorGridState event{};
  std::vector<IntegratorGridState> aligned;
  bool has_high = false;
  bool has_recent = false;
  bool has_event = false;
  float rol_acc[3] = {0.0f, 0.0f, 0.0f};
  int rol_k[3] = {-1, -1, -1};
  std::uint64_t tick = 0;
  std::atomic<std::uint64_t> used_gen{0};
};

struct FlatSlot {
  std::uint64_t hash = 0;
  ParticleCacheKey key{};
  Entry* entry = nullptr;
};

struct FlatMap {
  std::vector<FlatSlot> slots;
  std::size_t size = 0;

  bool empty() const noexcept { return size == 0; }

  void clear() {
    // Keep the bucket allocation. Seek must drop keys/results, not the
    // pages: first collect after seek would otherwise grow from kMinCap
    // and refault the whole table (memory reuse, not a result cache).
    for (FlatSlot& s : slots) {
      s.hash = 0;
      s.entry = nullptr;
    }
    size = 0;
  }

  FlatSlot* find(std::uint64_t h, const ParticleCacheKey& key) noexcept {
    if (slots.empty()) {
      return nullptr;
    }
    const std::size_t mask = slots.size() - 1;
    std::size_t i = static_cast<std::size_t>(h) & mask;
    for (;;) {
      FlatSlot& s = slots[i];
      if (s.hash == 0) {
        return nullptr;
      }
      if (s.hash == h && s.key == key) {
        return &s;
      }
      i = (i + 1) & mask;
    }
  }

  void grow() {
    const std::size_t n = slots.empty() ? kMinCap : slots.size() * 2;
    std::vector<FlatSlot> next(n);
    const std::size_t mask = n - 1;
    for (FlatSlot& s : slots) {
      if (s.hash == 0) {
        continue;
      }
      std::size_t i = static_cast<std::size_t>(s.hash) & mask;
      while (next[i].hash != 0) {
        i = (i + 1) & mask;
      }
      next[i] = s;
    }
    slots.swap(next);
  }

  FlatSlot* insert(std::uint64_t h, const ParticleCacheKey& key, Entry* e) {
    if (slots.empty() || (size + 1) * 10 > slots.size() * 7) {
      grow();
    }
    const std::size_t mask = slots.size() - 1;
    std::size_t i = static_cast<std::size_t>(h) & mask;
    for (;;) {
      FlatSlot& s = slots[i];
      if (s.hash == 0) {
        s.hash = h;
        s.key = key;
        s.entry = e;
        ++size;
        return &s;
      }
      if (s.hash == h && s.key == key) {
        return &s;
      }
      i = (i + 1) & mask;
    }
  }

  void erase_at(std::size_t i) {
    if (slots.empty() || i >= slots.size() || slots[i].hash == 0) {
      return;
    }
    const std::size_t mask = slots.size() - 1;
    --size;
    for (;;) {
      slots[i] = FlatSlot{};
      std::size_t j = i;
      for (;;) {
        j = (j + 1) & mask;
        if (slots[j].hash == 0) {
          return;
        }
        const std::size_t home = static_cast<std::size_t>(slots[j].hash) & mask;
        if (((i - home) & mask) < ((j - home) & mask)) {
          slots[i] = slots[j];
          i = j;
          break;
        }
      }
    }
  }
};

void reset_entry(Entry& e) noexcept {
  e.has_high = false;
  e.has_recent = false;
  e.has_event = false;
  e.aligned.clear();
  e.rol_acc[0] = e.rol_acc[1] = e.rol_acc[2] = 0.0f;
  e.rol_k[0] = e.rol_k[1] = e.rol_k[2] = -1;
  e.tick = 0;
  e.used_gen.store(0, std::memory_order_relaxed);
}

struct Shard {
  std::shared_mutex mu;
  std::uint64_t tick = 1;
  std::vector<Entry*> entry_free;
  FlatMap map;
  std::atomic<std::size_t> bytes{0};
  std::atomic<std::size_t> particles{0};
  std::atomic<std::size_t> checkpoints{0};
  std::atomic<std::uint64_t> lookup_ops{0};
  std::atomic<std::uint64_t> lookup_hits{0};
  std::atomic<std::uint64_t> lookup_contend{0};
  std::atomic<std::uint64_t> store_ops{0};
  std::atomic<std::uint64_t> store_contend{0};
};

Entry* take_entry(Shard& sh) {
  if (!sh.entry_free.empty()) {
    Entry* e = sh.entry_free.back();
    sh.entry_free.pop_back();
    reset_entry(*e);
    return e;
  }
  return new Entry();
}

void give_entry(Shard& sh, Entry* e) {
  if (e == nullptr) {
    return;
  }
  if (sh.entry_free.size() >= 8192) {
    delete e;
    return;
  }
  sh.entry_free.push_back(e);
}

struct Cache {
  Shard shards[kShards];
  std::atomic<std::uint32_t> epoch{1};
  std::atomic<std::uint64_t> play_gen{1};
  const bool enabled = parse_enabled();
  const std::size_t budget = parse_budget();
  const int stride = parse_stride();
};

std::atomic<Cache*> g_cache{nullptr};
std::mutex g_init_mu;

#ifndef _WIN32
void after_fork_child() {
  g_cache.store(nullptr, std::memory_order_relaxed);
}

void ensure_atfork() {
  static std::once_flag flag;
  std::call_once(flag, [] { pthread_atfork(nullptr, nullptr, after_fork_child); });
}
#endif

Cache& cache() {
#ifndef _WIN32
  ensure_atfork();
#endif
  Cache* p = g_cache.load(std::memory_order_acquire);
  if (p == nullptr) {
    std::lock_guard<std::mutex> lock(g_init_mu);
    p = g_cache.load(std::memory_order_relaxed);
    if (p == nullptr) {
      p = new Cache();
      g_cache.store(p, std::memory_order_release);
    }
  }
  return *p;
}

int shard_of(std::uint64_t h) noexcept {
  return static_cast<int>(h & static_cast<std::uint64_t>(kShards - 1));
}

std::unique_lock<std::shared_mutex> lock_shard(Shard& sh) {
  std::unique_lock<std::shared_mutex> lk(sh.mu, std::try_to_lock);
  if (!lk.owns_lock()) {
    SplitWallScope wall_lock(SplitWallSec::LockWait);
    lk = std::unique_lock<std::shared_mutex>(sh.mu);
    sh.store_contend.fetch_add(1, std::memory_order_relaxed);
  }
  sh.store_ops.fetch_add(1, std::memory_order_relaxed);
  return lk;
}

std::shared_lock<std::shared_mutex> lock_shard_shared(Shard& sh) {
  std::shared_lock<std::shared_mutex> lk(sh.mu, std::try_to_lock);
  if (!lk.owns_lock()) {
    SplitWallScope wall_lock(SplitWallSec::LockWait);
    lk = std::shared_lock<std::shared_mutex>(sh.mu);
    sh.lookup_contend.fetch_add(1, std::memory_order_relaxed);
  }
  sh.lookup_ops.fetch_add(1, std::memory_order_relaxed);
  return lk;
}

std::size_t entry_bytes(const Entry& e) noexcept {
  return kEntryOverhead + e.aligned.capacity() * sizeof(IntegratorGridState);
}

void sat_sub(std::atomic<std::size_t>& a, std::size_t d) noexcept {
  std::size_t cur = a.load(std::memory_order_relaxed);
  for (;;) {
    const std::size_t next = cur >= d ? cur - d : 0;
    if (a.compare_exchange_weak(cur, next, std::memory_order_relaxed)) {
      return;
    }
  }
}

void account_replace(Shard& sh, std::size_t old_b, std::size_t new_b, int old_cps,
                     int new_cps) noexcept {
  if (new_b >= old_b) {
    sh.bytes.fetch_add(new_b - old_b, std::memory_order_relaxed);
  } else {
    sat_sub(sh.bytes, old_b - new_b);
  }
  if (new_cps >= old_cps) {
    sh.checkpoints.fetch_add(static_cast<std::size_t>(new_cps - old_cps),
                             std::memory_order_relaxed);
  } else {
    sat_sub(sh.checkpoints, static_cast<std::size_t>(old_cps - new_cps));
  }
}

int count_cps(const Entry& e) noexcept {
  return static_cast<int>(e.aligned.size()) + (e.has_high ? 1 : 0) + (e.has_recent ? 1 : 0) +
         (e.has_event ? 1 : 0);
}

void drop_entry_account(Shard& sh, Entry& e) noexcept {
  sat_sub(sh.bytes, entry_bytes(e));
  sat_sub(sh.particles, 1);
  const int cps = count_cps(e);
  if (cps > 0) {
    sat_sub(sh.checkpoints, static_cast<std::size_t>(cps));
  }
}

void evict_one(Shard& sh) {
  if (sh.map.empty()) {
    return;
  }
  std::size_t victim = static_cast<std::size_t>(-1);
  std::uint64_t oldest = ~0ull;
  for (std::size_t i = 0; i < sh.map.slots.size(); ++i) {
    const FlatSlot& s = sh.map.slots[i];
    if (s.hash == 0 || s.entry == nullptr) {
      continue;
    }
    if (s.entry->tick < oldest) {
      oldest = s.entry->tick;
      victim = i;
    }
  }
  if (victim == static_cast<std::size_t>(-1)) {
    return;
  }
  Entry* e = sh.map.slots[victim].entry;
  drop_entry_account(sh, *e);
  give_entry(sh, e);
  sh.map.erase_at(victim);
}

std::size_t cache_bytes(const Cache& c) noexcept {
  std::size_t n = 0;
  for (int i = 0; i < kShards; ++i) {
    n += c.shards[i].bytes.load(std::memory_order_relaxed);
  }
  return n;
}

void evict_until_fit(Cache& c, Shard& sh, std::size_t extra) {
  const std::size_t shard_budget = c.budget / static_cast<std::size_t>(kShards);
  if (sh.bytes.load(std::memory_order_relaxed) + extra <= shard_budget) {
    return;
  }
  while (cache_bytes(c) + extra > c.budget && !sh.map.empty()) {
    evict_one(sh);
    if (sh.map.size <= 1) {
      break;
    }
  }
}

const IntegratorGridState* pick_leq(const Entry& e, int k_target) noexcept {
  const IntegratorGridState* best = nullptr;
  auto consider = [&](const IntegratorGridState& st, bool have) {
    if (!have || st.next_step > k_target) {
      return;
    }
    if (best == nullptr || st.next_step > best->next_step) {
      best = &st;
    }
  };
  consider(e.high, e.has_high);
  consider(e.recent, e.has_recent);
  consider(e.event, e.has_event);
  for (const IntegratorGridState& st : e.aligned) {
    consider(st, true);
    if (st.next_step > k_target) {
      break;
    }
  }
  return best;
}

void upsert_aligned(Entry& e, const IntegratorGridState& st, int stride) {
  if (stride < 1 || st.next_step % stride != 0) {
    return;
  }
  auto it = e.aligned.begin();
  for (; it != e.aligned.end(); ++it) {
    if (it->next_step == st.next_step) {
      *it = st;
      return;
    }
    if (it->next_step > st.next_step) {
      break;
    }
  }
  e.aligned.insert(it, st);
  while (static_cast<int>(e.aligned.size()) > kMaxAligned) {
    if (e.aligned.size() >= 2 && e.aligned.front().next_step == 0) {
      e.aligned.erase(e.aligned.begin() + 1);
    } else {
      e.aligned.erase(e.aligned.begin());
    }
  }
}

}  // namespace

bool split_eval_cache_enabled() noexcept {
  return cache().enabled;
}

int split_eval_cache_stride() noexcept {
  return cache().stride;
}

std::uint32_t split_eval_cache_epoch() noexcept {
  return cache().epoch.load(std::memory_order_acquire);
}

void split_eval_cache_clear() noexcept {
  Cache& c = cache();
  for (int i = 0; i < kShards; ++i) {
    Shard& sh = c.shards[i];
    std::lock_guard<std::shared_mutex> lock(sh.mu);
    for (FlatSlot& s : sh.map.slots) {
      if (s.hash != 0) {
        give_entry(sh, s.entry);
      }
    }
    sh.map.clear();
    sh.bytes.store(0, std::memory_order_relaxed);
    sh.particles.store(0, std::memory_order_relaxed);
    sh.checkpoints.store(0, std::memory_order_relaxed);
  }
}

void split_eval_cache_bump_epoch() noexcept {
  Cache& c = cache();
  c.epoch.fetch_add(1, std::memory_order_acq_rel);
  split_eval_cache_clear();
  split_eval_carry_bump_epoch();
  split_eval_persist_clear();
}

int split_eval_cache_shard_count() noexcept {
  return kShards;
}

bool split_eval_cache_precommit_one_shard(int shard, std::size_t slots_per_shard,
                                          std::size_t entries_per_shard) noexcept {
  if (shard < 0 || shard >= kShards) {
    return false;
  }
  Cache& c = cache();
  Shard& sh = c.shards[shard];
  std::lock_guard<std::shared_mutex> lock(sh.mu);
  const std::size_t slot_before = sh.map.slots.size();
  while (sh.map.slots.size() < slots_per_shard) {
    sh.map.grow();
  }
  if (sh.map.slots.size() > slot_before && !sh.map.slots.empty()) {
    wds::common::touch_pages(sh.map.slots.data(), sh.map.slots.size() * sizeof(FlatSlot));
  }
  while (sh.entry_free.size() < entries_per_shard) {
    Entry* e = new Entry();
    wds::common::touch_pages(e, sizeof(Entry));
    sh.entry_free.push_back(e);
  }
  return true;
}

void split_eval_cache_precommit(std::size_t slots_per_shard,
                                std::size_t entries_per_shard) noexcept {
  for (int i = 0; i < kShards; ++i) {
    split_eval_cache_precommit_one_shard(i, slots_per_shard, entries_per_shard);
  }
}

SplitEvalCacheStats split_eval_cache_stats() noexcept {
  const Cache& c = cache();
  SplitEvalCacheStats s;
  s.epoch = c.epoch.load(std::memory_order_relaxed);
  s.enabled = c.enabled;
  s.budget_bytes = c.budget;
  for (int i = 0; i < kShards; ++i) {
    const Shard& sh = c.shards[i];
    s.bytes_used += sh.bytes.load(std::memory_order_relaxed);
    s.particle_entries += sh.particles.load(std::memory_order_relaxed);
    s.checkpoints += sh.checkpoints.load(std::memory_order_relaxed);
    s.lookup_ops += sh.lookup_ops.load(std::memory_order_relaxed);
    s.lookup_hits += sh.lookup_hits.load(std::memory_order_relaxed);
    s.lookup_contend += sh.lookup_contend.load(std::memory_order_relaxed);
    s.store_ops += sh.store_ops.load(std::memory_order_relaxed);
    s.store_contend += sh.store_contend.load(std::memory_order_relaxed);
  }
  return s;
}

bool split_eval_cache_lookup(const ParticleCacheKey& key, int k_target,
                             IntegratorGridState& out) noexcept {
  if (key.valid == 0 || k_target < 0) {
    return false;
  }
  Cache& c = cache();
  if (!c.enabled) {
    return false;
  }
  SplitWallScope wall_lookup(SplitWallSec::CacheLookup);
  const std::uint64_t h = key_hash64(key);
  Shard& sh = c.shards[shard_of(h)];
  const std::shared_lock<std::shared_mutex> lock = lock_shard_shared(sh);
  FlatSlot* slot = sh.map.find(h, key);
  if (slot == nullptr) {
    return false;
  }
  slot->entry->used_gen.store(c.play_gen.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
  const IntegratorGridState* st = pick_leq(*slot->entry, k_target);
  if (st == nullptr) {
    return false;
  }
  out = *st;
  sh.lookup_hits.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void apply_store_locked(Cache& c, Shard& sh, std::uint64_t h, const ParticleCacheKey& key,
                        const IntegratorGridState& st) {
  FlatSlot* slot = sh.map.find(h, key);
  const bool inserted = slot == nullptr;
  if (inserted) {
    evict_until_fit(c, sh, kEntryOverhead + sizeof(IntegratorGridState));
    slot = sh.map.insert(h, key, take_entry(sh));
    sh.particles.fetch_add(1, std::memory_order_relaxed);
    sh.bytes.fetch_add(kEntryOverhead, std::memory_order_relaxed);
  }
  Entry& e = *slot->entry;
  const std::size_t old_b = entry_bytes(e);
  const int old_cps = count_cps(e);
  if (!e.has_high || st.next_step >= e.high.next_step) {
    e.high = st;
    e.has_high = true;
  } else if (!e.has_event || st.next_step >= e.event.next_step) {
    // pose_at / hydrate stores sit behind the now_t high. Keep the
    // newest of those so the next event can pick_leq instead of
    // walking from birth. hop, not 2022.3.
    e.event = st;
    e.has_event = true;
  }
  e.recent = st;
  e.has_recent = true;
  upsert_aligned(e, st, c.stride);
  e.tick = ++sh.tick;
  e.used_gen.store(c.play_gen.load(std::memory_order_relaxed), std::memory_order_relaxed);
  account_replace(sh, old_b, entry_bytes(e), old_cps, count_cps(e));
}

void split_eval_cache_store(const ParticleCacheKey& key, const IntegratorGridState& st) noexcept {
  if (key.valid == 0 || st.next_step < 0) {
    return;
  }
  Cache& c = cache();
  if (!c.enabled) {
    return;
  }
  SplitWallScope wall_store(SplitWallSec::CacheStore);
  const std::uint64_t h = key_hash64(key);
  Shard& sh = c.shards[shard_of(h)];
  const std::unique_lock<std::shared_mutex> lock = lock_shard(sh);
  apply_store_locked(c, sh, h, key, st);
}

void split_eval_cache_store_many(const ParticleCacheKey& key, const IntegratorGridState* sts,
                                 int n) noexcept {
  if (key.valid == 0 || sts == nullptr || n <= 0) {
    return;
  }
  Cache& c = cache();
  if (!c.enabled) {
    return;
  }
  SplitWallScope wall_store(SplitWallSec::CacheStore);
  const std::uint64_t h = key_hash64(key);
  Shard& sh = c.shards[shard_of(h)];
  const std::unique_lock<std::shared_mutex> lock = lock_shard(sh);
  for (int i = 0; i < n; ++i) {
    if (sts[i].next_step >= 0) {
      apply_store_locked(c, sh, h, key, sts[i]);
    }
  }
}

bool split_eval_cache_lookup_rol(const ParticleCacheKey& key, std::uint8_t axis, int k_target,
                                 int& k_have, float& acc) noexcept {
  if (key.valid == 0 || axis > 2 || k_target < 0) {
    return false;
  }
  Cache& c = cache();
  if (!c.enabled) {
    return false;
  }
  const std::uint64_t h = key_hash64(key);
  Shard& sh = c.shards[shard_of(h)];
  const std::shared_lock<std::shared_mutex> lock = lock_shard_shared(sh);
  FlatSlot* slot = sh.map.find(h, key);
  if (slot == nullptr) {
    return false;
  }
  slot->entry->used_gen.store(c.play_gen.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
  const int have = slot->entry->rol_k[axis];
  if (have < 0 || have > k_target) {
    return false;
  }
  k_have = have;
  acc = slot->entry->rol_acc[axis];
  sh.lookup_hits.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void split_eval_cache_store_rol(const ParticleCacheKey& key, std::uint8_t axis, int k,
                                float acc) noexcept {
  if (key.valid == 0 || axis > 2 || k < 0) {
    return;
  }
  Cache& c = cache();
  if (!c.enabled) {
    return;
  }
  const std::uint64_t h = key_hash64(key);
  Shard& sh = c.shards[shard_of(h)];
  const std::unique_lock<std::shared_mutex> lock = lock_shard(sh);
  FlatSlot* slot = sh.map.find(h, key);
  if (slot == nullptr) {
    evict_until_fit(c, sh, kEntryOverhead);
    slot = sh.map.insert(h, key, take_entry(sh));
    sh.particles.fetch_add(1, std::memory_order_relaxed);
    sh.bytes.fetch_add(kEntryOverhead, std::memory_order_relaxed);
  }
  Entry& e = *slot->entry;
  if (k >= e.rol_k[axis]) {
    e.rol_k[axis] = k;
    e.rol_acc[axis] = acc;
  }
  e.tick = ++sh.tick;
  e.used_gen.store(c.play_gen.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

std::atomic<double> g_prev_frame_ms{0.0};
int g_reap_cursor = 0;

void split_eval_cache_begin_play_frame() noexcept {
  Cache& c = cache();
  if (!c.enabled) {
    return;
  }
  c.play_gen.fetch_add(1, std::memory_order_acq_rel);
}

void split_eval_cache_note_frame_ms(double frame_ms) noexcept {
  g_prev_frame_ms.store(frame_ms, std::memory_order_relaxed);
}

double split_eval_cache_prev_frame_ms() noexcept {
  return g_prev_frame_ms.load(std::memory_order_relaxed);
}

void reap_shard_unused(Shard& sh, std::uint64_t gen) {
  std::size_t idx = 0;
  while (idx < sh.map.slots.size()) {
    FlatSlot& s = sh.map.slots[idx];
    if (s.hash == 0 || s.entry == nullptr ||
        s.entry->used_gen.load(std::memory_order_relaxed) == gen) {
      ++idx;
      continue;
    }
    drop_entry_account(sh, *s.entry);
    give_entry(sh, s.entry);
    sh.map.erase_at(idx);
  }
}

void split_eval_cache_reap_unused() noexcept {
  // Idle-sliced: reap only when the previous frame left vsync budget.
  // WDS_SPLIT_CACHE_REAP=0 off, =1 force every shard this frame.
  // Default: if prev work + 1 ms margin >= 16.7 ms, skip; else scan
  // shards from a persistent cursor until the leftover budget is gone.
  // Heavy load may never reap; byte-budget eviction still bounds memory.
  static const int mode = [] {
    const char* raw = std::getenv("WDS_SPLIT_CACHE_REAP");
    if (raw != nullptr && raw[0] == '0' && raw[1] == '\0') {
      return 0;
    }
    if (raw != nullptr && raw[0] == '1' && raw[1] == '\0') {
      return 1;
    }
    return -1;
  }();
  if (mode == 0) {
    return;
  }
  Cache& c = cache();
  if (!c.enabled) {
    return;
  }
  constexpr double kFrameMs = 16.7;
  constexpr double kMarginMs = 1.0;
  const double prev = g_prev_frame_ms.load(std::memory_order_relaxed);
  const bool force = mode == 1;
  if (!force) {
    if (!(prev > 0.0 && prev + kMarginMs < kFrameMs)) {
      return;
    }
  }
  const double remain_ms = force ? 1.0e9 : (kFrameMs - prev - kMarginMs);
  const auto t0 = std::chrono::steady_clock::now();
  const std::uint64_t gen = c.play_gen.load(std::memory_order_acquire);
  int scanned = 0;
  while (scanned < kShards) {
    if (!force) {
      const double used = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
      if (used >= remain_ms) {
        break;
      }
    }
    const int i = g_reap_cursor;
    g_reap_cursor = (g_reap_cursor + 1) % kShards;
    ++scanned;
    Shard& sh = c.shards[i];
    std::unique_lock<std::shared_mutex> lock(sh.mu);
    reap_shard_unused(sh, gen);
  }
}

}  // namespace wds::chart_render::shuriken

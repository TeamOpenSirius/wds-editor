#include "split_eval_carry.hpp"
#include "split_wall_profile.hpp"

#include <wds/common/page_touch.hpp>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace wds::chart_render::shuriken {
namespace {

constexpr int kShards = 64;
constexpr int kWalkShards = 256;
constexpr std::size_t kEntryOverhead = 128;

std::shared_ptr<std::vector<SubemitWalkPose>> take_walk_poses_shared(std::size_t n) {
  return std::make_shared<std::vector<SubemitWalkPose>>(n);
}

std::shared_ptr<std::vector<CarryParticle>> take_carry_parts_shared(
    std::vector<CarryParticle>&& parts) {
  return std::make_shared<std::vector<CarryParticle>>(std::move(parts));
}

struct KeyHash {
  std::size_t operator()(const SystemCarryKey& k) const noexcept {
    std::size_t h = k.epoch;
    h = h * 1315423911u + static_cast<std::size_t>(k.color_id);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_index);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_start_ms);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_start_ms >> 32);
    h = h * 1315423911u + static_cast<std::size_t>(k.catalog_index);
    h = h * 1315423911u + static_cast<std::size_t>(k.line_play_gen);
    h = h * 1315423911u + static_cast<std::size_t>(k.kind);
    h ^= static_cast<std::size_t>(k.input_hash) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }
};

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

float parse_gap() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_CARRY_GAP");
  if (raw == nullptr || raw[0] == '\0') {
    return 0.25f;
  }
  char* end = nullptr;
  const float v = std::strtof(raw, &end);
  if (end == raw || v <= 0.0f) {
    return 0.25f;
  }
  return v;
}

bool parse_enabled() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_CARRY");
  if (raw == nullptr || raw[0] == '\0') {
    return true;
  }
  const std::string_view v(raw);
  return v != "0" && v != "off" && v != "false" && v != "OFF";
}

std::size_t parse_budget() noexcept {
  const int mb = parse_int_env("WDS_SPLIT_CARRY_MB", 128);
  if (mb <= 0) {
    return 1;
  }
  return static_cast<std::size_t>(mb) * 1024ull * 1024ull;
}

std::size_t snapshot_bytes(const std::vector<CarryParticle>& parts) noexcept {
  return kEntryOverhead + parts.size() * sizeof(CarryParticle);
}

struct Entry {
  float now_t = -1.0e30f;
  int next_birth = 0;
  std::shared_ptr<const std::vector<CarryParticle>> particles;
  std::uint64_t tick = 0;
};

struct Shard {
  std::mutex mu;
  std::uint64_t tick = 1;
  std::unordered_map<SystemCarryKey, std::unique_ptr<Entry>, KeyHash> map;
  std::vector<std::unique_ptr<Entry>> entry_free;
};

std::unique_ptr<Entry> take_carry_entry(Shard& sh) {
  if (!sh.entry_free.empty()) {
    std::unique_ptr<Entry> e = std::move(sh.entry_free.back());
    sh.entry_free.pop_back();
    *e = Entry{};
    return e;
  }
  return std::make_unique<Entry>();
}

void give_carry_entry(Shard& sh, std::unique_ptr<Entry> e) {
  if (!e || sh.entry_free.size() >= 4096) {
    return;
  }
  e->particles.reset();
  sh.entry_free.push_back(std::move(e));
}

struct WalkKeyHash {
  std::size_t operator()(const SubemitWalkKey& k) const noexcept {
    std::size_t h = KeyHash{}(k.parent);
    h = h * 1315423911u + static_cast<std::size_t>(k.birth_index);
    h = h * 1315423911u + static_cast<std::size_t>(k.birth_extra);
    h = h * 1315423911u + static_cast<std::size_t>(k.birth_time_bits);
    h = h * 1315423911u + static_cast<std::size_t>(k.slot_i);
    return h;
  }
};

struct WalkEntry {
  float now_t = -1.0e30f;
  float t = 0.0f;
  float frac_t = 0.0f;
  float frac_d = 0.0f;
  float prev_x = 0.0f;
  float prev_y = 0.0f;
  float prev_z = 0.0f;
  float last_pose_age = -1.0f;
  int k = 0;
  int n_full_births = 0;
  std::size_t burst_count = 0;
  uint8_t have_prev = 0;
  uint8_t has_parent_grid = 0;
  IntegratorGridState parent_grid{};
  std::shared_ptr<std::vector<Birth>> births;
  std::shared_ptr<std::vector<SubemitWalkPose>> poses;
  std::uint64_t tick = 0;
};

struct WalkShard {
  std::mutex mu;
  std::uint64_t tick = 1;
  std::unordered_map<SubemitWalkKey, std::unique_ptr<WalkEntry>, WalkKeyHash> map;
  std::vector<std::unique_ptr<WalkEntry>> entry_free;
};

std::unique_ptr<WalkEntry> take_walk_entry(WalkShard& sh) {
  if (!sh.entry_free.empty()) {
    std::unique_ptr<WalkEntry> e = std::move(sh.entry_free.back());
    sh.entry_free.pop_back();
    *e = WalkEntry{};
    return e;
  }
  return std::make_unique<WalkEntry>();
}

void give_walk_entry(WalkShard& sh, std::unique_ptr<WalkEntry> e) {
  if (!e || sh.entry_free.size() >= 4096) {
    return;
  }
  e->births.reset();
  e->poses.reset();
  sh.entry_free.push_back(std::move(e));
}

std::size_t walk_payload_bytes(const std::shared_ptr<std::vector<Birth>>& births,
                               const std::shared_ptr<std::vector<SubemitWalkPose>>& poses) noexcept {
  const std::size_t nb = births ? births->size() * sizeof(Birth) : 0;
  const std::size_t np = poses ? poses->size() * sizeof(SubemitWalkPose) : 0;
  return nb + np;
}

std::size_t walk_bytes(const WalkEntry& e) noexcept {
  return kEntryOverhead + sizeof(WalkEntry) + walk_payload_bytes(e.births, e.poses);
}

std::size_t entry_bytes(const Entry& e) noexcept {
  return e.particles ? snapshot_bytes(*e.particles) : kEntryOverhead;
}

std::size_t entry_particles(const Entry& e) noexcept {
  return e.particles ? e.particles->size() : 0;
}

struct Store {
  Shard shards[kShards];
  WalkShard walks[kWalkShards];
  std::atomic<std::size_t> bytes{0};
  std::atomic<std::size_t> systems{0};
  std::atomic<std::size_t> particles{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> misses{0};
  std::atomic<std::uint64_t> fallbacks{0};
  std::atomic<std::uint64_t> commits{0};
  std::atomic<std::uint64_t> walk_hits{0};
  std::atomic<std::uint64_t> walk_misses{0};
  std::atomic<std::uint64_t> walk_commits{0};
  std::atomic<int> override_enabled{-1};
  std::atomic<uint8_t> inhibit_store{0};
  std::atomic<float> gap_override{-1.0f};
  const bool enabled = parse_enabled();
  const std::size_t budget = parse_budget();
  const float max_gap = parse_gap();
};

std::atomic<Store*> g_store{nullptr};
std::mutex g_init_mu;

#ifndef _WIN32
void after_fork_child() {
  g_store.store(nullptr, std::memory_order_relaxed);
}

void ensure_atfork() {
  static std::once_flag flag;
  std::call_once(flag, []() { pthread_atfork(nullptr, nullptr, after_fork_child); });
}
#endif

Store& store() {
  Store* p = g_store.load(std::memory_order_acquire);
  if (p != nullptr) {
    return *p;
  }
#ifndef _WIN32
  ensure_atfork();
#endif
  std::lock_guard<std::mutex> lock(g_init_mu);
  p = g_store.load(std::memory_order_relaxed);
  if (p == nullptr) {
    p = new Store();
    g_store.store(p, std::memory_order_release);
  }
  return *p;
}

int shard_of(const SystemCarryKey& key) noexcept {
  return static_cast<int>(KeyHash{}(key) & static_cast<std::size_t>(kShards - 1));
}

int walk_shard_of(const SubemitWalkKey& key) noexcept {
  return static_cast<int>(WalkKeyHash{}(key) & static_cast<std::size_t>(kWalkShards - 1));
}

// Saturating atomic subtract. Global counters are updated under different
// per-shard mutexes, so every update must be a single atomic RMW.
void sat_sub(std::atomic<std::size_t>& a, std::size_t d) noexcept {
  std::size_t cur = a.load(std::memory_order_relaxed);
  for (;;) {
    const std::size_t next = cur >= d ? cur - d : 0;
    if (a.compare_exchange_weak(cur, next, std::memory_order_relaxed)) {
      return;
    }
  }
}

// Apply (new_v - old_v) to a global counter as one atomic operation.
void apply_delta(std::atomic<std::size_t>& a, std::size_t old_v, std::size_t new_v) noexcept {
  if (new_v >= old_v) {
    a.fetch_add(new_v - old_v, std::memory_order_relaxed);
  } else {
    sat_sub(a, old_v - new_v);
  }
}

void note_drop_bytes(Store& s, std::size_t drop, std::size_t npart, bool drop_system) noexcept {
  sat_sub(s.bytes, drop);
  if (drop_system) {
    sat_sub(s.systems, 1);
  }
  sat_sub(s.particles, npart);
}

void evict_one(Store& s, Shard& sh) {
  if (sh.map.empty()) {
    return;
  }
  auto oldest = sh.map.begin();
  for (auto it = sh.map.begin(); it != sh.map.end(); ++it) {
    if (it->second->tick < oldest->second->tick) {
      oldest = it;
    }
  }
  const std::size_t drop = entry_bytes(*oldest->second);
  const std::size_t npart = entry_particles(*oldest->second);
  auto node = std::move(oldest->second);
  sh.map.erase(oldest);
  give_carry_entry(sh, std::move(node));
  note_drop_bytes(s, drop, npart, true);
}

void evict_walk(Store& s, WalkShard& sh) {
  if (sh.map.empty()) {
    return;
  }
  auto oldest = sh.map.begin();
  for (auto it = sh.map.begin(); it != sh.map.end(); ++it) {
    if (it->second->tick < oldest->second->tick) {
      oldest = it;
    }
  }
  const std::size_t drop = walk_bytes(*oldest->second);
  auto node = std::move(oldest->second);
  sh.map.erase(oldest);
  give_walk_entry(sh, std::move(node));
  sat_sub(s.bytes, drop);
}

}  // namespace

bool split_eval_carry_enabled() noexcept {
  Store& s = store();
  const int ov = s.override_enabled.load(std::memory_order_acquire);
  if (ov >= 0) {
    return ov != 0;
  }
  return s.enabled;
}

float split_eval_carry_max_gap() noexcept {
  const float ov = store().gap_override.load(std::memory_order_acquire);
  if (ov > 0.0f) {
    return ov;
  }
  return store().max_gap;
}

void split_eval_carry_set_enabled_for_tests(int on) noexcept {
  store().override_enabled.store(on, std::memory_order_release);
}

void split_eval_carry_set_max_gap_for_tests(float seconds) noexcept {
  store().gap_override.store(seconds > 0.0f ? seconds : -1.0f, std::memory_order_release);
}

void split_eval_carry_clear() noexcept {
  Store& s = store();
  for (int i = 0; i < kShards; ++i) {
    Shard& sh = s.shards[i];
    std::lock_guard<std::mutex> lock(sh.mu);
    for (auto& kv : sh.map) {
      give_carry_entry(sh, std::move(kv.second));
    }
    sh.map.clear();
  }
  for (int i = 0; i < kWalkShards; ++i) {
    WalkShard& sh = s.walks[i];
    std::lock_guard<std::mutex> lock(sh.mu);
    for (auto& kv : sh.map) {
      give_walk_entry(sh, std::move(kv.second));
    }
    sh.map.clear();
  }
  s.bytes.store(0, std::memory_order_relaxed);
  s.systems.store(0, std::memory_order_relaxed);
  s.particles.store(0, std::memory_order_relaxed);
}

void split_eval_carry_bump_epoch() noexcept {
  split_eval_carry_clear();
  store().inhibit_store.store(1, std::memory_order_relaxed);
}

int split_eval_carry_shard_count() noexcept {
  return kShards;
}

int split_eval_carry_walk_shard_count() noexcept {
  return kWalkShards;
}

bool split_eval_carry_precommit_one_shard(int shard, std::size_t entries) noexcept {
  if (shard < 0 || shard >= kShards) {
    return false;
  }
  Store& s = store();
  Shard& sh = s.shards[shard];
  std::lock_guard<std::mutex> lock(sh.mu);
  while (sh.entry_free.size() < entries) {
    auto e = std::make_unique<Entry>();
    wds::common::touch_pages(e.get(), sizeof(Entry));
    sh.entry_free.push_back(std::move(e));
  }
  return true;
}

bool split_eval_carry_precommit_one_walk(int shard, std::size_t entries) noexcept {
  if (shard < 0 || shard >= kWalkShards) {
    return false;
  }
  Store& s = store();
  WalkShard& sh = s.walks[shard];
  std::lock_guard<std::mutex> lock(sh.mu);
  while (sh.entry_free.size() < entries) {
    auto e = std::make_unique<WalkEntry>();
    wds::common::touch_pages(e.get(), sizeof(WalkEntry));
    sh.entry_free.push_back(std::move(e));
  }
  return true;
}

void split_eval_carry_precommit(std::size_t entries_per_shard,
                                std::size_t walk_per_shard) noexcept {
  for (int i = 0; i < kShards; ++i) {
    split_eval_carry_precommit_one_shard(i, entries_per_shard);
  }
  for (int i = 0; i < kWalkShards; ++i) {
    split_eval_carry_precommit_one_walk(i, walk_per_shard);
  }
}

void split_eval_carry_set_store_inhibited(bool inhibited) noexcept {
  store().inhibit_store.store(inhibited ? 1 : 0, std::memory_order_relaxed);
}

bool split_eval_carry_store_inhibited() noexcept {
  return store().inhibit_store.load(std::memory_order_relaxed) != 0;
}

SplitEvalCarryStats split_eval_carry_stats() noexcept {
  const Store& s = store();
  SplitEvalCarryStats out;
  out.bytes_used = s.bytes.load(std::memory_order_relaxed);
  out.systems = s.systems.load(std::memory_order_relaxed);
  out.particles = s.particles.load(std::memory_order_relaxed);
  out.budget_bytes = s.budget;
  out.enabled = split_eval_carry_enabled();
  out.max_gap = s.max_gap;
  out.hits = s.hits.load(std::memory_order_relaxed);
  out.misses = s.misses.load(std::memory_order_relaxed);
  out.fallbacks = s.fallbacks.load(std::memory_order_relaxed);
  out.commits = s.commits.load(std::memory_order_relaxed);
  out.walk_hits = s.walk_hits.load(std::memory_order_relaxed);
  out.walk_misses = s.walk_misses.load(std::memory_order_relaxed);
  out.walk_commits = s.walk_commits.load(std::memory_order_relaxed);
  return out;
}

bool split_eval_carry_can_resume(float prev_now, float now) noexcept {
  if (!(now + 1e-9f >= prev_now)) {
    return false;
  }
  const float gap = now - prev_now;
  return gap <= split_eval_carry_max_gap() + 1e-9f;
}

void fill_snap_from_entry(const Entry& e, SystemCarrySnapshot& out) noexcept {
  out.now_t = e.now_t;
  out.next_birth = e.next_birth;
  out.storage = e.particles;
}

bool split_eval_carry_lookup(const SystemCarryKey& key, SystemCarrySnapshot& out) noexcept {
  out.now_t = -1.0e30f;
  out.next_birth = 0;
  out.storage.reset();
  if (!split_eval_carry_enabled()) {
    return false;
  }
  SplitWallScope wall(SplitWallSec::CarryLookup);
  Store& s = store();
  Shard& sh = s.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    s.misses.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  fill_snap_from_entry(*it->second, out);
  it->second->tick = ++sh.tick;
  s.hits.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool split_eval_carry_lookup_for_now(const SystemCarryKey& key, float now,
                                     SystemCarrySnapshot& out) noexcept {
  out.now_t = -1.0e30f;
  out.next_birth = 0;
  out.storage.reset();
  if (!split_eval_carry_enabled()) {
    return false;
  }
  SplitWallScope wall(SplitWallSec::CarryLookup);
  Store& s = store();
  Shard& sh = s.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    s.misses.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  Entry& e = *it->second;
  e.tick = ++sh.tick;
  if (split_eval_carry_can_resume(e.now_t, now)) {
    fill_snap_from_entry(e, out);
    s.hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  s.misses.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void apply_entry_delta(Store& s, std::size_t old_bytes, std::size_t old_n, std::size_t add,
                       std::size_t npart) noexcept {
  apply_delta(s.bytes, old_bytes, add);
  apply_delta(s.particles, old_n, npart);
}

void trim_shard_for_key(Store& s, Shard& sh, const SystemCarryKey& key, Entry* self) {
  (void)self;
  while (s.bytes.load(std::memory_order_relaxed) > s.budget) {
    if (sh.map.size() <= 1) {
      break;
    }
    SystemCarryKey victim{};
    std::uint64_t oldest_tick = ~std::uint64_t{0};
    bool found = false;
    for (auto& kv : sh.map) {
      if (kv.first == key) {
        continue;
      }
      if (kv.second->tick < oldest_tick) {
        oldest_tick = kv.second->tick;
        victim = kv.first;
        found = true;
      }
    }
    if (!found) {
      break;
    }
    const auto it = sh.map.find(victim);
    if (it == sh.map.end()) {
      break;
    }
    const std::size_t drop = entry_bytes(*it->second);
    const std::size_t npart = entry_particles(*it->second);
    auto node = std::move(it->second);
    sh.map.erase(it);
    give_carry_entry(sh, std::move(node));
    note_drop_bytes(s, drop, npart, true);
  }
}

void store_held_locked(Store& s, Shard& sh, const SystemCarryKey& key, float now_t, int next_birth,
                       std::shared_ptr<const std::vector<CarryParticle>> held) {
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    const std::size_t add = snapshot_bytes(*held);
    const std::size_t npart = held->size();
    while (s.bytes.load(std::memory_order_relaxed) + add > s.budget && !sh.map.empty()) {
      evict_one(s, sh);
    }
    auto fresh = take_carry_entry(sh);
    fresh->now_t = now_t;
    fresh->next_birth = next_birth;
    fresh->particles = std::move(held);
    fresh->tick = ++sh.tick;
    sh.map.emplace(key, std::move(fresh));
    s.systems.fetch_add(1, std::memory_order_relaxed);
    s.bytes.fetch_add(add, std::memory_order_relaxed);
    s.particles.fetch_add(npart, std::memory_order_relaxed);
    s.commits.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Entry& e = *it->second;
  const std::size_t old_bytes = entry_bytes(e);
  const std::size_t old_n = entry_particles(e);
  e.now_t = now_t;
  e.next_birth = next_birth;
  e.particles = std::move(held);
  e.tick = ++sh.tick;
  apply_entry_delta(s, old_bytes, old_n, entry_bytes(e), entry_particles(e));
  trim_shard_for_key(s, sh, key, &e);
  s.commits.fetch_add(1, std::memory_order_relaxed);
}

void split_eval_carry_store(const SystemCarryKey& key, float now_t,
                            std::vector<CarryParticle>&& parts, int next_birth) noexcept {
  if (!split_eval_carry_enabled() || split_eval_carry_store_inhibited()) {
    return;
  }
  SplitWallScope wall(SplitWallSec::CarryStore);
  auto held = take_carry_parts_shared(std::move(parts));
  for (CarryParticle& cp : *held) {
    cp.st.p = nullptr;
  }
  Store& s = store();
  Shard& sh = s.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  store_held_locked(s, sh, key, now_t, next_birth, std::move(held));
}

void split_eval_carry_drop(const SystemCarryKey& key) noexcept {
  if (!split_eval_carry_enabled()) {
    return;
  }
  Store& s = store();
  Shard& sh = s.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    return;
  }
  const std::size_t drop = entry_bytes(*it->second);
  const std::size_t npart = entry_particles(*it->second);
  auto node = std::move(it->second);
  sh.map.erase(it);
  give_carry_entry(sh, std::move(node));
  note_drop_bytes(s, drop, npart, true);
}

void split_eval_carry_note_fallback() noexcept {
  store().fallbacks.fetch_add(1, std::memory_order_relaxed);
}

bool carry_birth_eq(const Birth& a, const Birth& b) noexcept {
  uint32_t ta = 0;
  uint32_t tb = 0;
  std::memcpy(&ta, &a.time, sizeof(ta));
  std::memcpy(&tb, &b.time, sizeof(tb));
  return a.index == b.index && a.extra == b.extra && ta == tb;
}

int carry_find_particle(const SystemCarrySnapshot& snap, const Birth& b, uint32_t seed) noexcept {
  const std::vector<CarryParticle>& particles = snap.parts();
  int lo = 0;
  int hi = static_cast<int>(particles.size()) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const Birth& mb = particles[static_cast<std::size_t>(mid)].b;
    if (mb.index < b.index || (mb.index == b.index && mb.extra < b.extra)) {
      lo = mid + 1;
    } else if (mb.index > b.index || (mb.index == b.index && mb.extra > b.extra)) {
      hi = mid - 1;
    } else {
      const CarryParticle& cp = particles[static_cast<std::size_t>(mid)];
      if (carry_birth_eq(cp.b, b) && cp.seed == seed) {
        return mid;
      }
      return -1;
    }
  }
  for (int i = std::max(0, lo - 2); i < static_cast<int>(particles.size()) && i <= lo + 2; ++i) {
    const CarryParticle& cp = particles[static_cast<std::size_t>(i)];
    if (carry_birth_eq(cp.b, b) && cp.seed == seed) {
      return i;
    }
  }
  return -1;
}

int child_key_cmp(const CarryParticle& cp, const Birth& parent, uint16_t slot_i,
                  const Birth& child) noexcept {
  if (cp.parent_b.index != parent.index) {
    return cp.parent_b.index < parent.index ? -1 : 1;
  }
  if (cp.parent_b.extra != parent.extra) {
    return cp.parent_b.extra < parent.extra ? -1 : 1;
  }
  uint32_t pt = 0;
  uint32_t ptp = 0;
  std::memcpy(&pt, &cp.parent_b.time, sizeof(pt));
  std::memcpy(&ptp, &parent.time, sizeof(ptp));
  if (pt != ptp) {
    return pt < ptp ? -1 : 1;
  }
  if (cp.slot_i != slot_i) {
    return cp.slot_i < slot_i ? -1 : 1;
  }
  if (cp.b.index != child.index) {
    return cp.b.index < child.index ? -1 : 1;
  }
  if (cp.b.extra != child.extra) {
    return cp.b.extra < child.extra ? -1 : 1;
  }
  uint32_t ct = 0;
  uint32_t ctc = 0;
  std::memcpy(&ct, &cp.b.time, sizeof(ct));
  std::memcpy(&ctc, &child.time, sizeof(ctc));
  if (ct != ctc) {
    return ct < ctc ? -1 : 1;
  }
  return 0;
}

int carry_find_child(const SystemCarrySnapshot& snap, const Birth& child, uint32_t seed,
                     const Birth& parent, uint16_t slot_i) noexcept {
  const std::vector<CarryParticle>& particles = snap.parts();
  int lo = 0;
  int hi = static_cast<int>(particles.size()) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const CarryParticle& cp = particles[static_cast<std::size_t>(mid)];
    const int c = child_key_cmp(cp, parent, slot_i, child);
    if (c < 0) {
      lo = mid + 1;
    } else if (c > 0) {
      hi = mid - 1;
    } else if (cp.seed == seed && carry_birth_eq(cp.b, child) &&
               carry_birth_eq(cp.parent_b, parent)) {
      return mid;
    } else {
      break;
    }
  }
  for (int i = std::max(0, lo - 4); i < static_cast<int>(particles.size()) && i <= lo + 4; ++i) {
    const CarryParticle& cp = particles[static_cast<std::size_t>(i)];
    if (cp.seed == seed && cp.slot_i == slot_i && carry_birth_eq(cp.b, child) &&
        carry_birth_eq(cp.parent_b, parent)) {
      return i;
    }
  }
  return -1;
}

void walk_entry_to_cursor(const WalkEntry& e, SubemitWalkCursor& out, bool want_grid) noexcept {
  out.now_t = e.now_t;
  out.t = e.t;
  out.frac_t = e.frac_t;
  out.frac_d = e.frac_d;
  out.prev_x = e.prev_x;
  out.prev_y = e.prev_y;
  out.prev_z = e.prev_z;
  out.last_pose_age = e.last_pose_age;
  out.k = e.k;
  out.n_full_births = e.n_full_births;
  out.burst_count = e.burst_count;
  out.have_prev = e.have_prev;
  if (want_grid) {
    out.has_parent_grid = e.has_parent_grid;
    out.parent_grid = e.parent_grid;
  } else {
    out.has_parent_grid = 0;
  }
  out.births = e.births;
  out.poses = e.poses;
}

void walk_cursor_to_entry(WalkEntry& e, float now_t, SubemitWalkCursor&& cur) noexcept {
  e.now_t = now_t;
  e.t = cur.t;
  e.frac_t = cur.frac_t;
  e.frac_d = cur.frac_d;
  e.prev_x = cur.prev_x;
  e.prev_y = cur.prev_y;
  e.prev_z = cur.prev_z;
  e.last_pose_age = cur.last_pose_age;
  e.k = cur.k;
  e.n_full_births = cur.n_full_births;
  e.burst_count = cur.burst_count;
  e.have_prev = cur.have_prev;
  e.has_parent_grid = cur.has_parent_grid;
  if (cur.has_parent_grid != 0) {
    e.parent_grid = cur.parent_grid;
  }
  if (cur.births) {
    e.births = std::move(cur.births);
  }
  if (cur.poses) {
    e.poses = std::move(cur.poses);
  }
}

bool split_eval_walk_lookup(const SubemitWalkKey& key, SubemitWalkCursor& out) noexcept {
  out = SubemitWalkCursor{};
  if (!split_eval_carry_enabled()) {
    return false;
  }
  SplitWallScope wall(SplitWallSec::CarryLookup);
  Store& s = store();
  WalkShard& sh = s.walks[walk_shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    s.walk_misses.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  walk_entry_to_cursor(*it->second, out, true);
  it->second->tick = ++sh.tick;
  s.walk_hits.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool split_eval_walk_lookup_for_now(const SubemitWalkKey& key, float now,
                                    SubemitWalkCursor& out, bool want_parent_grid) noexcept {
  out = SubemitWalkCursor{};
  if (!split_eval_carry_enabled()) {
    return false;
  }
  SplitWallScope wall(SplitWallSec::CarryLookup);
  Store& s = store();
  WalkShard& sh = s.walks[walk_shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    s.walk_misses.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  WalkEntry& e = *it->second;
  e.tick = ++sh.tick;
  if (split_eval_carry_can_resume(e.now_t, now)) {
    walk_entry_to_cursor(e, out, want_parent_grid);
    s.walk_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  s.walk_misses.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void trim_walk_shard_for_key(Store& s, WalkShard& sh, const SubemitWalkKey& key, WalkEntry* self) {
  (void)self;
  while (s.bytes.load(std::memory_order_relaxed) > s.budget) {
    if (sh.map.size() <= 1) {
      break;
    }
    evict_walk(s, sh);
    if (sh.map.find(key) == sh.map.end()) {
      break;
    }
  }
}

void split_eval_walk_store(const SubemitWalkKey& key, float now_t, SubemitWalkCursor&& cur) noexcept {
  if (!split_eval_carry_enabled() || split_eval_carry_store_inhibited()) {
    return;
  }
  SplitWallScope wall(SplitWallSec::CarryStore);
  Store& s = store();
  WalkShard& sh = s.walks[walk_shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    auto fresh = take_walk_entry(sh);
    walk_cursor_to_entry(*fresh, now_t, std::move(cur));
    const std::size_t add = walk_bytes(*fresh);
    while (s.bytes.load(std::memory_order_relaxed) + add > s.budget && !sh.map.empty()) {
      evict_walk(s, sh);
    }
    fresh->tick = ++sh.tick;
    sh.map.emplace(key, std::move(fresh));
    s.bytes.fetch_add(add, std::memory_order_relaxed);
    s.walk_commits.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  WalkEntry& e = *it->second;
  const std::size_t old_bytes = walk_bytes(e);
  walk_cursor_to_entry(e, now_t, std::move(cur));
  e.tick = ++sh.tick;
  const std::size_t add = walk_bytes(e);
  apply_delta(s.bytes, old_bytes, add);
  trim_walk_shard_for_key(s, sh, key, &e);
  s.walk_commits.fetch_add(1, std::memory_order_relaxed);
}

void split_eval_walk_touch(const SubemitWalkKey& key, float now_t,
                           const SubemitWalkCursor& cur) noexcept {
  if (!split_eval_carry_enabled() || split_eval_carry_store_inhibited()) {
    return;
  }
  SplitWallScope wall(SplitWallSec::CarryStore);
  Store& s = store();
  WalkShard& sh = s.walks[walk_shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    return;
  }
  WalkEntry& e = *it->second;
  e.now_t = now_t;
  e.t = cur.t;
  e.frac_t = cur.frac_t;
  e.frac_d = cur.frac_d;
  e.prev_x = cur.prev_x;
  e.prev_y = cur.prev_y;
  e.prev_z = cur.prev_z;
  e.last_pose_age = cur.last_pose_age;
  e.k = cur.k;
  e.n_full_births = cur.n_full_births;
  e.burst_count = cur.burst_count;
  e.have_prev = cur.have_prev;
  e.tick = ++sh.tick;
}

void split_eval_walk_write_poses(const SubemitWalkKey& key, const float* event_ts,
                                 const SubemitWalkPose* poses, int n) noexcept {
  if (event_ts == nullptr || poses == nullptr || n <= 0 || !split_eval_carry_enabled() ||
      split_eval_carry_store_inhibited()) {
    return;
  }
  SplitWallScope wall(SplitWallSec::CarryStore);
  Store& s = store();
  WalkShard& sh = s.walks[walk_shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end() || !it->second->births) {
    return;
  }
  WalkEntry& e = *it->second;
  if (!e.poses) {
    e.poses = take_walk_poses_shared(0);
    e.poses->clear();
  }
  if (e.poses.use_count() != 1) {
    auto copy = take_walk_poses_shared(e.poses->size());
    *copy = *e.poses;
    e.poses = std::move(copy);
  }
  const int nfull = static_cast<int>(e.births->size());
  if (static_cast<int>(e.poses->size()) < nfull) {
    e.poses->resize(static_cast<size_t>(nfull));
  }
  for (int i = 0; i < n; ++i) {
    for (int bi = 0; bi < nfull; ++bi) {
      if (std::fabs((*e.births)[bi].time - event_ts[i]) <= 1e-8f) {
        (*e.poses)[static_cast<size_t>(bi)] = poses[i];
        break;
      }
    }
  }
  e.tick = ++sh.tick;
}

}  // namespace wds::chart_render::shuriken

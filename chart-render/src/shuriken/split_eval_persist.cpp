#include "split_eval_persist.hpp"
#include "split_wall_profile.hpp"

#include <wds/common/page_touch.hpp>


#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wds::chart_render::shuriken {
namespace {

constexpr int kShards = 64;
constexpr std::size_t kEntryOverhead = 192;
constexpr int kMaxSlotsPerStore = 16384;
constexpr float kNowEps = 1.0e-5f;

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

bool parse_enabled() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_PERSIST");
  // Off by default: 2026-10-14 interleaved 10990 2xFull6 play, persist on
  // p95 23.6-25.2 vs off 17.7-17.9. Opt in with WDS_SPLIT_PERSIST=1.
  if (raw == nullptr || raw[0] == '\0') {
    return false;
  }
  const std::string_view v(raw);
  return v != "0" && v != "off" && v != "false" && v != "OFF";
}

std::size_t parse_budget() noexcept {
  const int mb = parse_int_env("WDS_SPLIT_PERSIST_MB", 128);
  if (mb <= 0) {
    return 1;
  }
  return static_cast<std::size_t>(mb) * 1024ull * 1024ull;
}

float parse_rebuild_gap() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_PERSIST_REBUILD_GAP");
  if (raw == nullptr || raw[0] == '\0') {
    // hop: 0.1→2.0 persist-ab was slightly slower than remake (child remake
    // still dominates; persist cull/merge is extra). Gaps > 1 s rebuild.
    return 1.0f;
  }
  char* end = nullptr;
  const float v = std::strtof(raw, &end);
  if (end == raw || v <= 0.0f) {
    return 0.0f;
  }
  return v;
}

struct PersistStore {
  SystemCarryKey key{};
  ChildPersistFp fp{};
  uint32_t gen = 1;
  float now_t = -1.0e30f;
  uint64_t tick = 0;
  uint8_t busy = 0;
  uint8_t live = 0;
  std::vector<IntegratorHot> hots;
  std::vector<PersistCold> colds;
  std::vector<PersistCand> cands;
};

std::size_t store_bytes(const PersistStore& st) noexcept {
  return kEntryOverhead + st.hots.capacity() * sizeof(IntegratorHot) +
         st.colds.capacity() * sizeof(PersistCold) + st.cands.capacity() * sizeof(PersistCand);
}

void assemble_slot(PersistSlot& o, const IntegratorHot& h, const PersistCold& c) noexcept {
  copy_hot_grid(h, o.grid);
  o.grid.trace = c.trace;
  o.b = c.b;
  o.parent_b = c.parent_b;
  o.seed = c.seed;
  o.emit_u = c.emit_u;
  o.st = c.st;
  o.slot_i = c.slot_i;
  o.has_grid = c.has_grid;
  o.is_child = c.is_child;
  o.gen = c.gen;
}

void write_cold_from_job(PersistCold& dst, const PrimaryEvalJob& job) noexcept {
  dst.b = job.b;
  dst.parent_b = job.parent_b;
  dst.seed = job.seed;
  dst.emit_u = job.emit_u;
  dst.st = job.st;
  dst.st.p = nullptr;
  dst.trace = job.grid.trace;
  dst.slot_i = job.slot_i;
  dst.has_grid = job.has_grid;
  dst.is_child = job.is_child;
  ++dst.gen;
  if (dst.gen == 0) {
    dst.gen = 1;
  }
}

void write_hot_from_job(IntegratorHot& h, const PrimaryEvalJob& job) noexcept {
  copy_grid_hot(job.grid, h);
}

struct Shard {
  std::mutex mu;
  std::uint64_t tick = 1;
  std::unordered_map<SystemCarryKey, uint32_t, KeyHash> map;
};

struct Registry {
  Shard shards[kShards];
  std::mutex table_mu;
  std::vector<std::unique_ptr<PersistStore>> table;
  std::vector<uint32_t> free_ids;
  std::atomic<std::size_t> bytes{0};
  std::atomic<std::size_t> systems{0};
  std::atomic<std::size_t> particles{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> misses{0};
  std::atomic<std::uint64_t> rebuilds{0};
  std::atomic<std::uint64_t> commits{0};
  std::atomic<int> override_enabled{-1};
  const bool enabled = parse_enabled();
  const std::size_t budget = parse_budget();
  const float rebuild_gap = parse_rebuild_gap();
};

std::atomic<Registry*> g_reg{nullptr};

Registry& registry() noexcept {
  Registry* p = g_reg.load(std::memory_order_acquire);
  if (p != nullptr) {
    return *p;
  }
  p = g_reg.load(std::memory_order_relaxed);
  if (p == nullptr) {
    p = new Registry();
    g_reg.store(p, std::memory_order_release);
  }
  return *p;
}

int shard_of(const SystemCarryKey& key) noexcept {
  return static_cast<int>(KeyHash{}(key) & static_cast<std::size_t>(kShards - 1));
}

void note_drop(Registry& r, std::size_t drop, std::size_t npart) noexcept {
  const std::size_t bytes = r.bytes.load(std::memory_order_relaxed);
  r.bytes.store(bytes > drop ? bytes - drop : 0, std::memory_order_relaxed);
  const std::size_t sys = r.systems.load(std::memory_order_relaxed);
  if (sys > 0) {
    r.systems.store(sys - 1, std::memory_order_relaxed);
  }
  const std::size_t parts = r.particles.load(std::memory_order_relaxed);
  r.particles.store(parts > npart ? parts - npart : 0, std::memory_order_relaxed);
}

uint32_t alloc_store(Registry& r, const SystemCarryKey& key) {
  std::lock_guard<std::mutex> lock(r.table_mu);
  uint32_t id = 0xffffffffu;
  if (!r.free_ids.empty()) {
    id = r.free_ids.back();
    r.free_ids.pop_back();
    PersistStore& st = *r.table[id];
    const uint32_t next_gen = st.gen + 1u == 0u ? 1u : st.gen + 1u;
    st.hots.clear();
    st.colds.clear();
    st.cands.clear();
    st.key = key;
    st.fp = {};
    st.now_t = -1.0e30f;
    st.tick = 0;
    st.busy = 0;
    st.live = 1;
    st.gen = next_gen;
    return id;
  }
  id = static_cast<uint32_t>(r.table.size());
  auto st = std::make_unique<PersistStore>();
  st->key = key;
  st->gen = 1;
  st->live = 1;
  r.table.push_back(std::move(st));
  return id;
}

void free_store_id(Registry& r, uint32_t id, PersistStore& st) {
  const std::size_t drop = store_bytes(st);
  const std::size_t npart = st.colds.size();
  ++st.gen;
  if (st.gen == 0) {
    st.gen = 1;
  }
  st.busy = 0;
  st.live = 0;
  st.hots.clear();
  st.colds.clear();
  st.cands.clear();
  st.now_t = -1.0e30f;
  {
    std::lock_guard<std::mutex> lock(r.table_mu);
    r.free_ids.push_back(id);
  }
  note_drop(r, drop, npart);
}

PersistStore* table_store(Registry& r, uint32_t id) noexcept {
  std::lock_guard<std::mutex> lock(r.table_mu);
  if (id >= r.table.size() || r.table[id] == nullptr) {
    return nullptr;
  }
  return r.table[id].get();
}

void evict_one(Registry& r, Shard& sh) {
  if (sh.map.empty()) {
    return;
  }
  auto oldest = sh.map.end();
  uint64_t oldest_tick = ~uint64_t{0};
  for (auto it = sh.map.begin(); it != sh.map.end(); ++it) {
    PersistStore* st = table_store(r, it->second);
    if (st == nullptr || st->busy != 0 || st->live == 0) {
      continue;
    }
    if (st->tick < oldest_tick) {
      oldest_tick = st->tick;
      oldest = it;
    }
  }
  if (oldest == sh.map.end()) {
    return;
  }
  const uint32_t id = oldest->second;
  PersistStore* st = table_store(r, id);
  sh.map.erase(oldest);
  if (st != nullptr) {
    free_store_id(r, id, *st);
  }
}

bool birth_lt(const Birth& a, const Birth& b) noexcept {
  if (a.index != b.index) {
    return a.index < b.index;
  }
  return a.extra < b.extra;
}

uint32_t f32_bits(float f) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

bool child_key_lt(const Birth& pa, uint16_t sa, const Birth& ca, const Birth& pb, uint16_t sb,
                  const Birth& cb) noexcept {
  if (pa.index != pb.index) {
    return pa.index < pb.index;
  }
  if (pa.extra != pb.extra) {
    return pa.extra < pb.extra;
  }
  const uint32_t pta = f32_bits(pa.time);
  const uint32_t ptb = f32_bits(pb.time);
  if (pta != ptb) {
    return pta < ptb;
  }
  if (sa != sb) {
    return sa < sb;
  }
  const uint32_t cta = f32_bits(ca.time);
  const uint32_t ctb = f32_bits(cb.time);
  if (cta != ctb) {
    return cta < ctb;
  }
  return ca.extra < cb.extra;
}

bool child_cold_lt(const PersistCold& a, const PersistCold& b) noexcept {
  return child_key_lt(a.parent_b, a.slot_i, a.b, b.parent_b, b.slot_i, b.b);
}

bool child_identity_eq(const PersistCold& s, const Birth& b, uint32_t seed, const Birth& parent_b,
                       uint16_t slot_i) noexcept {
  if (s.is_child == 0 || s.seed != seed || s.slot_i != slot_i) {
    return false;
  }
  if (!carry_birth_eq(s.parent_b, parent_b)) {
    return false;
  }
  uint32_t ta = 0;
  uint32_t tb = 0;
  std::memcpy(&ta, &s.b.time, sizeof(ta));
  std::memcpy(&tb, &b.time, sizeof(tb));
  return ta == tb && s.b.extra == b.extra;
}

int find_slot(const std::vector<PersistCold>& parts, const Birth& b,
              uint32_t seed) noexcept {
  int lo = 0;
  int hi = static_cast<int>(parts.size()) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const Birth& mb = parts[static_cast<std::size_t>(mid)].b;
    if (birth_lt(mb, b)) {
      lo = mid + 1;
    } else if (birth_lt(b, mb)) {
      hi = mid - 1;
    } else {
      const PersistCold& s = parts[static_cast<std::size_t>(mid)];
      if (carry_birth_eq(s.b, b) && s.seed == seed) {
        return mid;
      }
      return -1;
    }
  }
  for (int i = std::max(0, lo - 2); i < static_cast<int>(parts.size()) && i <= lo + 2; ++i) {
    const PersistCold& s = parts[static_cast<std::size_t>(i)];
    if (carry_birth_eq(s.b, b) && s.seed == seed) {
      return i;
    }
  }
  return -1;
}

int find_child_slot(const std::vector<PersistCold>& parts, const Birth& b,
                    uint32_t seed, const Birth& parent_b, uint16_t slot_i) noexcept {
  int lo = 0;
  int hi = static_cast<int>(parts.size()) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const PersistCold& ms = parts[static_cast<std::size_t>(mid)];
    if (child_key_lt(ms.parent_b, ms.slot_i, ms.b, parent_b, slot_i, b)) {
      lo = mid + 1;
    } else if (child_key_lt(parent_b, slot_i, b, ms.parent_b, ms.slot_i, ms.b)) {
      hi = mid - 1;
    } else {
      for (int i = mid; i >= 0; --i) {
        const PersistCold& s = parts[static_cast<std::size_t>(i)];
        if (child_key_lt(s.parent_b, s.slot_i, s.b, parent_b, slot_i, b) ||
            child_key_lt(parent_b, slot_i, b, s.parent_b, s.slot_i, s.b)) {
          break;
        }
        if (child_identity_eq(s, b, seed, parent_b, slot_i)) {
          return i;
        }
      }
      for (int i = mid + 1; i < static_cast<int>(parts.size()); ++i) {
        const PersistCold& s = parts[static_cast<std::size_t>(i)];
        if (child_key_lt(s.parent_b, s.slot_i, s.b, parent_b, slot_i, b) ||
            child_key_lt(parent_b, slot_i, b, s.parent_b, s.slot_i, s.b)) {
          break;
        }
        if (child_identity_eq(s, b, seed, parent_b, slot_i)) {
          return i;
        }
      }
      return -1;
    }
  }
  for (int i = std::max(0, lo - 2); i < static_cast<int>(parts.size()) && i <= lo + 2; ++i) {
    if (child_identity_eq(parts[static_cast<std::size_t>(i)], b, seed, parent_b, slot_i)) {
      return i;
    }
  }
  return -1;
}

bool should_rebuild(float prev, float now, float gap) noexcept {
  if (now + kNowEps < prev) {
    return true;
  }
  if (gap > 0.0f && now - prev > gap) {
    return true;
  }
  return false;
}

thread_local PersistStore* tls_claim_st = nullptr;
thread_local uint32_t tls_claim_id = 0xffffffffu;
thread_local uint32_t tls_claim_gen = 0;

void tls_bind_claim(PersistStore* st, uint32_t id, uint32_t gen) noexcept {
  tls_claim_st = st;
  tls_claim_id = id;
  tls_claim_gen = gen;
}

void tls_clear_claim() noexcept {
  tls_claim_st = nullptr;
  tls_claim_id = 0xffffffffu;
  tls_claim_gen = 0;
}

PersistStore* claimed_store(const PersistClaim& c) noexcept {
  if (c.kind != PersistClaimKind::Forward || c.store_id == 0xffffffffu) {
    return nullptr;
  }
  if (tls_claim_st != nullptr && tls_claim_id == c.store_id && tls_claim_gen == c.gen) {
    if (tls_claim_st->gen == c.gen && tls_claim_st->busy != 0 && tls_claim_st->live != 0) {
      return tls_claim_st;
    }
    tls_clear_claim();
  }
  Registry& r = registry();
  PersistStore* st = table_store(r, c.store_id);
  if (st == nullptr || st->gen != c.gen || st->busy == 0 || st->live == 0) {
    return nullptr;
  }
  tls_bind_claim(st, c.store_id, c.gen);
  return st;
}

bool primary_cold_lt(const PersistCold& a, const PersistCold& b) noexcept {
  if (a.b.index != b.b.index) {
    return a.b.index < b.b.index;
  }
  return a.b.extra < b.b.extra;
}

void sort_store(PersistStore& st, uint8_t child) noexcept {
  const std::size_t n = st.colds.size();
  if (n < 2) {
    return;
  }
  bool sorted = true;
  for (std::size_t i = 1; i < n; ++i) {
    if (child != 0) {
      if (child_cold_lt(st.colds[i], st.colds[i - 1])) {
        sorted = false;
        break;
      }
    } else if (primary_cold_lt(st.colds[i], st.colds[i - 1])) {
      sorted = false;
      break;
    }
  }
  if (sorted) {
    return;
  }
  std::vector<std::size_t> idx(n);
  for (std::size_t i = 0; i < n; ++i) {
    idx[i] = i;
  }
  if (child != 0) {
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
      return child_cold_lt(st.colds[a], st.colds[b]);
    });
  } else {
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
      return primary_cold_lt(st.colds[a], st.colds[b]);
    });
  }
  std::vector<IntegratorHot> th(n);
  std::vector<PersistCold> tc(n);
  for (std::size_t i = 0; i < n; ++i) {
    th[i] = st.hots[idx[i]];
    tc[i] = st.colds[idx[i]];
  }
  for (std::size_t i = 0; i < n; ++i) {
    st.hots[i] = th[i];
    st.colds[i] = tc[i];
  }
}

void reserve_store(PersistStore& st, std::size_t n) noexcept {
  st.hots.reserve(n);
  st.colds.reserve(n);
}

void trim_budget(Registry& r, Shard& sh, const SystemCarryKey& keep) {
  while (r.bytes.load(std::memory_order_relaxed) > r.budget && !sh.map.empty()) {
    if (sh.map.size() == 1 && sh.map.find(keep) != sh.map.end()) {
      break;
    }
    evict_one(r, sh);
    if (sh.map.find(keep) == sh.map.end()) {
      break;
    }
  }
}

}  // namespace

bool split_eval_persist_enabled() noexcept {
  Registry& r = registry();
  const int ov = r.override_enabled.load(std::memory_order_acquire);
  if (ov >= 0) {
    return ov != 0;
  }
  return r.enabled;
}

void split_eval_persist_set_enabled_for_tests(int on) noexcept {
  registry().override_enabled.store(on, std::memory_order_release);
}

bool split_eval_persist_precommit_one_store(std::size_t slots_each) noexcept {
  if (!split_eval_persist_enabled()) {
    return false;
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.table_mu);
  auto st = std::make_unique<PersistStore>();
  if (slots_each > 0) {
    st->hots.reserve(slots_each);
    st->colds.reserve(slots_each);
    st->cands.reserve(slots_each);
    st->hots.resize(slots_each);
    st->colds.resize(slots_each);
    st->cands.resize(slots_each);
    if (!st->hots.empty()) {
      wds::common::touch_pages(st->hots.data(), st->hots.size() * sizeof(IntegratorHot));
    }
    if (!st->colds.empty()) {
      wds::common::touch_pages(st->colds.data(), st->colds.size() * sizeof(PersistCold));
    }
    if (!st->cands.empty()) {
      wds::common::touch_pages(st->cands.data(), st->cands.size() * sizeof(PersistCand));
    }
    st->hots.clear();
    st->colds.clear();
    st->cands.clear();
  }
  r.free_ids.push_back(static_cast<uint32_t>(r.table.size()));
  r.table.push_back(std::move(st));
  return true;
}

void split_eval_persist_precommit(std::size_t stores, std::size_t slots_each) noexcept {
  if (!split_eval_persist_enabled() || stores == 0) {
    return;
  }
  Registry& r = registry();
  while (r.table.size() < stores) {
    if (!split_eval_persist_precommit_one_store(slots_each)) {
      break;
    }
  }
}

void split_eval_persist_clear() noexcept {
  Registry& r = registry();
  for (int i = 0; i < kShards; ++i) {
    std::lock_guard<std::mutex> lock(r.shards[i].mu);
    r.shards[i].map.clear();
  }
  {
    std::lock_guard<std::mutex> lock(r.table_mu);
    for (uint32_t id = 0; id < static_cast<uint32_t>(r.table.size()); ++id) {
      PersistStore* st = r.table[id].get();
      if (st == nullptr) {
        continue;
      }
      ++st->gen;
      if (st->gen == 0) {
        st->gen = 1;
      }
      st->busy = 0;
      st->live = 0;
      // Drop persist contents; keep the buffers so the next fill does not
      // remalloc / refault. Not a result cache.
      st->hots.clear();
      st->colds.clear();
      st->cands.clear();
    }
    r.free_ids.clear();
    r.free_ids.reserve(r.table.size());
    for (uint32_t id = 0; id < static_cast<uint32_t>(r.table.size()); ++id) {
      r.free_ids.push_back(id);
    }
  }
  r.bytes.store(0, std::memory_order_relaxed);
  r.systems.store(0, std::memory_order_relaxed);
  r.particles.store(0, std::memory_order_relaxed);
}

SplitEvalPersistStats split_eval_persist_stats() noexcept {
  const Registry& r = registry();
  SplitEvalPersistStats out;
  out.bytes_used = r.bytes.load(std::memory_order_relaxed);
  out.systems = r.systems.load(std::memory_order_relaxed);
  out.particles = r.particles.load(std::memory_order_relaxed);
  out.budget_bytes = r.budget;
  out.enabled = split_eval_persist_enabled();
  out.rebuild_gap = r.rebuild_gap;
  out.hits = r.hits.load(std::memory_order_relaxed);
  out.misses = r.misses.load(std::memory_order_relaxed);
  out.rebuilds = r.rebuilds.load(std::memory_order_relaxed);
  out.commits = r.commits.load(std::memory_order_relaxed);
  return out;
}

bool persist_primary_eligible(const SplitEffectParticle& p) noexcept {
  (void)p;
  if (!split_eval_persist_enabled()) {
    return false;
  }
  if (split_closed_form()) {
    return false;
  }
  if (split_integrator_is_legacy()) {
    return false;
  }
  return true;
}

bool persist_child_eligible(const SplitEffectParticle& p) noexcept {
  return persist_primary_eligible(p);
}

void split_eval_persist_claim(const SystemCarryKey& key, float now, PersistClaim& out) noexcept {
  ChildPersistFp none{};
  split_eval_persist_claim(key, now, none, out);
}

void split_eval_persist_claim(const SystemCarryKey& key, float now, const ChildPersistFp& fp,
                              PersistClaim& out) noexcept {
  out = PersistClaim{};
  if (!split_eval_persist_enabled()) {
    out.kind = PersistClaimKind::Miss;
    return;
  }
  Registry& r = registry();
  Shard& sh = r.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    r.misses.fetch_add(1, std::memory_order_relaxed);
    out.kind = PersistClaimKind::Miss;
    return;
  }
  const uint32_t id = it->second;
  PersistStore* st = table_store(r, id);
  if (st == nullptr || st->live == 0 || st->busy != 0) {
    if (st == nullptr || st->live == 0) {
      sh.map.erase(it);
    }
    r.misses.fetch_add(1, std::memory_order_relaxed);
    out.kind = PersistClaimKind::Miss;
    return;
  }
  if (should_rebuild(st->now_t, now, r.rebuild_gap) ||
      (fp.want != 0 && (st->fp.want == 0 || !child_persist_fp_eq(st->fp, fp)))) {
    sh.map.erase(it);
    free_store_id(r, id, *st);
    r.rebuilds.fetch_add(1, std::memory_order_relaxed);
    out.kind = PersistClaimKind::Rebuild;
    return;
  }
  st->busy = 1;
  st->tick = ++sh.tick;
  out.store_id = id;
  out.gen = st->gen;
  out.prev_now = st->now_t;
  out.kind = PersistClaimKind::Forward;
  tls_bind_claim(st, id, st->gen);
  r.hits.fetch_add(1, std::memory_order_relaxed);
}

const PersistSlot* persist_slot_at(const PersistClaim& c, int idx) noexcept {
  PersistStore* st = claimed_store(c);
  if (st == nullptr || idx < 0 || idx >= static_cast<int>(st->colds.size())) {
    return nullptr;
  }
  thread_local PersistSlot ring[4];
  thread_local int ring_i = 0;
  PersistSlot& v = ring[ring_i++ & 3];
  assemble_slot(v, st->hots[static_cast<std::size_t>(idx)], st->colds[static_cast<std::size_t>(idx)]);
  return &v;
}

IntegratorHot* persist_claimed_hot(int idx) noexcept {
  PersistStore* st = tls_claim_st;
  if (st == nullptr || idx < 0 || idx >= static_cast<int>(st->hots.size())) {
    return nullptr;
  }
  return &st->hots[static_cast<std::size_t>(idx)];
}

int persist_slot_count(const PersistClaim& c) noexcept {
  PersistStore* st = claimed_store(c);
  return st != nullptr ? static_cast<int>(st->colds.size()) : 0;
}

int persist_find_slot(const PersistClaim& c, const Birth& b, uint32_t seed) noexcept {
  PersistStore* st = claimed_store(c);
  if (st == nullptr) {
    return -1;
  }
  return find_slot(st->colds, b, seed);
}

int persist_find_child_slot(const PersistClaim& c, const Birth& b, uint32_t seed,
                            const Birth& parent_b, uint16_t slot_i) noexcept {
  PersistStore* st = claimed_store(c);
  if (st == nullptr) {
    return -1;
  }
  return find_child_slot(st->colds, b, seed, parent_b, slot_i);
}

void persist_cull_dead(PersistClaim& c, const SplitEffectParticle& p, float now) noexcept {
  SplitWallScope wall(SplitWallSec::PersistCull);
  PersistStore* st = claimed_store(c);
  if (st == nullptr) {
    return;
  }
  const float sim = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
  std::size_t w = 0;
  for (std::size_t i = 0; i < st->colds.size(); ++i) {
    PersistCold& s = st->colds[i];
    const float age = now * sim - s.b.time * sim;
    const float trail = p.trail_lifetime_count > 0
                            ? trail_lifetime_mul_hi(p)
                            : (s.st.trail_life_mul >= 0.0f ? s.st.trail_life_mul : p.trail_lifetime);
    if (age >= particle_keep_sim(p, s.st.life, trail)) {
      continue;
    }
    if (w != i) {
      st->colds[w] = s;
      st->hots[w] = st->hots[i];
    }
    ++w;
  }
  st->colds.resize(w);
  st->hots.resize(w);
}

void persist_release(PersistClaim& c) noexcept {
  if (c.kind != PersistClaimKind::Forward) {
    tls_clear_claim();
    c = PersistClaim{};
    return;
  }
  PersistStore* st = claimed_store(c);
  if (st != nullptr) {
    st->busy = 0;
  }
  tls_clear_claim();
  c = PersistClaim{};
}

void write_cands(PersistStore& st, const std::vector<PersistCand>* cands) noexcept {
  if (cands == nullptr) {
    return;
  }
  st.cands.clear();
  if (cands->empty()) {
    return;
  }
  st.cands.reserve(cands->size());
  st.cands.assign(cands->begin(), cands->end());
}

void persist_commit(PersistClaim& c, float now, const std::vector<PrimaryEvalJob>& jobs,
                    const ChildPersistFp* fp, const std::vector<PersistCand>* cands) noexcept {
  Registry& r = registry();
  PersistStore* st = nullptr;
  SystemCarryKey key{};
  if (c.kind == PersistClaimKind::Forward) {
    st = claimed_store(c);
    if (st != nullptr) {
      key = st->key;
    }
  }
  if (st == nullptr) {
    persist_release(c);
    return;
  }
  SplitWallScope wall(SplitWallSec::PersistCommit);
  const std::size_t old_bytes = store_bytes(*st);
  const std::size_t old_n = st->colds.size();
  const uint8_t child_store = key.kind == kPersistKindChild ? 1 : 0;
  for (const PrimaryEvalJob& job : jobs) {
    if (job.has_state == 0 || job.alive == 0) {
      continue;
    }
    int idx = -1;
    if (job.persist_i >= 0 && job.persist_i < static_cast<int>(st->colds.size())) {
      const PersistCold& s = st->colds[static_cast<std::size_t>(job.persist_i)];
      const bool match = child_store != 0
                             ? child_identity_eq(s, job.b, job.seed, job.parent_b, job.slot_i)
                             : (carry_birth_eq(s.b, job.b) && s.seed == job.seed);
      if (match) {
        idx = job.persist_i;
      }
    }
    if (idx < 0) {
      idx = child_store != 0
                ? find_child_slot(st->colds, job.b, job.seed, job.parent_b, job.slot_i)
                : find_slot(st->colds, job.b, job.seed);
    }
    if (idx >= 0) {
      write_hot_from_job(st->hots[static_cast<std::size_t>(idx)], job);
      st->colds[static_cast<std::size_t>(idx)].trace = job.grid.trace;
      st->colds[static_cast<std::size_t>(idx)].has_grid = job.has_grid;
    } else if (static_cast<int>(st->colds.size()) < kMaxSlotsPerStore) {
      IntegratorHot h{};
      PersistCold cold{};
      write_hot_from_job(h, job);
      write_cold_from_job(cold, job);
      st->hots.push_back(h);
      st->colds.push_back(cold);
    }
  }
  if (child_store == 0) {
    sort_store(*st, 0);
  }
  write_cands(*st, cands);
  st->now_t = now;
  if (fp != nullptr && fp->want != 0) {
    st->fp = *fp;
  }
  st->busy = 0;
  const std::size_t add = store_bytes(*st);
  const std::size_t bytes = r.bytes.load(std::memory_order_relaxed);
  r.bytes.store(bytes > old_bytes ? bytes - old_bytes + add : add, std::memory_order_relaxed);
  const std::size_t parts = r.particles.load(std::memory_order_relaxed);
  r.particles.store(parts > old_n ? parts - old_n + st->colds.size() : st->colds.size(),
                    std::memory_order_relaxed);
  r.commits.fetch_add(1, std::memory_order_relaxed);
  Shard& sh = r.shards[shard_of(key)];
  {
    std::lock_guard<std::mutex> lock(sh.mu);
    trim_budget(r, sh, key);
  }
  tls_clear_claim();
  c = PersistClaim{};
}

void persist_commit(PersistClaim& c, float now, const std::vector<PrimaryEvalJob>& jobs) noexcept {
  persist_commit(c, now, jobs, nullptr, nullptr);
}

void persist_commit_new(const SystemCarryKey& key, float now,
                        const std::vector<PrimaryEvalJob>& jobs, const ChildPersistFp* fp,
                        const std::vector<PersistCand>* cands) noexcept {
  SplitWallScope wall(SplitWallSec::PersistCommit);
  if (!split_eval_persist_enabled()) {
    return;
  }
  Registry& r = registry();
  uint32_t nstate = 0;
  for (const PrimaryEvalJob& job : jobs) {
    if (job.has_state != 0 && job.alive != 0) {
      ++nstate;
    }
  }
  if (nstate == 0 && (cands == nullptr || cands->empty())) {
    return;
  }
  Shard& sh = r.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it != sh.map.end()) {
    const uint32_t old_id = it->second;
    PersistStore* old = table_store(r, old_id);
    if (old != nullptr && old->busy != 0) {
      return;
    }
    sh.map.erase(it);
    if (old != nullptr) {
      free_store_id(r, old_id, *old);
    }
  }
  const uint32_t id = alloc_store(r, key);
  PersistStore* st = table_store(r, id);
  if (st == nullptr) {
    return;
  }
  st->hots.clear();
  st->colds.clear();
  reserve_store(*st, nstate);
  for (const PrimaryEvalJob& job : jobs) {
    if (job.has_state == 0 || job.alive == 0) {
      continue;
    }
    if (static_cast<int>(st->colds.size()) >= kMaxSlotsPerStore) {
      break;
    }
    IntegratorHot h{};
    PersistCold cold{};
    write_hot_from_job(h, job);
    write_cold_from_job(cold, job);
    st->hots.push_back(h);
    st->colds.push_back(cold);
  }
  if (key.kind == kPersistKindPrimary) {
    sort_store(*st, 0);
  }
  write_cands(*st, cands);
  st->now_t = now;
  if (fp != nullptr && fp->want != 0) {
    st->fp = *fp;
  }
  st->busy = 0;
  st->live = 1;
  st->tick = ++sh.tick;
  sh.map.emplace(key, id);
  r.bytes.fetch_add(store_bytes(*st), std::memory_order_relaxed);
  r.systems.fetch_add(1, std::memory_order_relaxed);
  r.particles.fetch_add(st->colds.size(), std::memory_order_relaxed);
  r.commits.fetch_add(1, std::memory_order_relaxed);
  trim_budget(r, sh, key);
}

void persist_finish(PersistClaim& c, const SystemCarryKey& key, float now, const ChildPersistFp& fp,
                    const std::vector<PrimaryEvalJob>& jobs,
                    const std::vector<PersistCand>& cands) noexcept {
  const ChildPersistFp* fp_ptr = fp.want != 0 ? &fp : nullptr;
  const std::vector<PersistCand>* cand_ptr = cands.empty() ? nullptr : &cands;
  if (c.kind == PersistClaimKind::Forward) {
    persist_commit(c, now, jobs, fp_ptr, cand_ptr);
    return;
  }
  persist_release(c);
  persist_commit_new(key, now, jobs, fp_ptr, cand_ptr);
}

void persist_finish(PersistClaim& c, const SystemCarryKey& key, float now, const ChildPersistFp& fp,
                    const std::vector<PrimaryEvalJob>& jobs) noexcept {
  persist_finish(c, key, now, fp, jobs, std::vector<PersistCand>{});
}

void persist_finish(PersistClaim& c, const SystemCarryKey& key, float now,
                    const std::vector<PrimaryEvalJob>& jobs) noexcept {
  persist_finish(c, key, now, ChildPersistFp{}, jobs);
}

void persist_touch_empty(const SystemCarryKey& key, float now) noexcept {
  if (!split_eval_persist_enabled()) {
    return;
  }
  Registry& r = registry();
  Shard& sh = r.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  if (sh.map.find(key) != sh.map.end()) {
    return;
  }
  const uint32_t id = alloc_store(r, key);
  PersistStore* st = table_store(r, id);
  if (st == nullptr) {
    return;
  }
  st->hots.clear();
  st->colds.clear();
  st->cands.clear();
  st->now_t = now;
  st->busy = 0;
  st->live = 1;
  st->tick = ++sh.tick;
  sh.map.emplace(key, id);
  r.bytes.fetch_add(store_bytes(*st), std::memory_order_relaxed);
  r.systems.fetch_add(1, std::memory_order_relaxed);
  r.commits.fetch_add(1, std::memory_order_relaxed);
}

bool persist_store_exists(const SystemCarryKey& key) noexcept {
  if (!split_eval_persist_enabled()) {
    return false;
  }
  Registry& r = registry();
  Shard& sh = r.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    return false;
  }
  PersistStore* st = table_store(r, it->second);
  return st != nullptr && st->live != 0;
}

bool persist_peek_cands(const SystemCarryKey& key, const ChildPersistFp& fp, float now,
                        std::vector<PersistCand>& out) noexcept {
  out.clear();
  if (!split_eval_persist_enabled() || fp.want == 0) {
    return false;
  }
  Registry& r = registry();
  Shard& sh = r.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    return false;
  }
  PersistStore* st = table_store(r, it->second);
  if (st == nullptr || st->live == 0 || st->cands.empty()) {
    return false;
  }
  if (should_rebuild(st->now_t, now, r.rebuild_gap) || st->fp.want == 0 ||
      !child_persist_fp_eq(st->fp, fp)) {
    return false;
  }
  out.resize(st->cands.size());
  for (std::size_t i = 0; i < st->cands.size(); ++i) {
    out[i] = st->cands[i];
  }
  return true;
}

bool split_eval_persist_lookup_particle(const SystemCarryKey& key, float now, const Birth& b,
                                        uint32_t seed, PersistParticleView& out) noexcept {
  if (!split_eval_persist_enabled()) {
    return false;
  }
  Registry& r = registry();
  Shard& sh = r.shards[shard_of(key)];
  std::lock_guard<std::mutex> lock(sh.mu);
  const auto it = sh.map.find(key);
  if (it == sh.map.end()) {
    return false;
  }
  PersistStore* st = table_store(r, it->second);
  if (st == nullptr || st->live == 0) {
    return false;
  }
  if (st->now_t > now + kNowEps) {
    return false;
  }
  const int idx = find_slot(st->colds, b, seed);
  if (idx < 0) {
    return false;
  }
  PersistSlot s;
  assemble_slot(s, st->hots[static_cast<std::size_t>(idx)], st->colds[static_cast<std::size_t>(idx)]);
  out.st = s.st;
  out.grid = s.grid;
  out.has_grid = s.has_grid;
  return true;
}

}  // namespace wds::chart_render::shuriken

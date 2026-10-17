#pragma once

#include "internal.hpp"

#include <cstddef>
#include <cstdint>

namespace wds::chart_render::shuriken {

struct SplitEvalCacheStats {
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
};

bool split_eval_cache_enabled() noexcept;
int split_eval_cache_stride() noexcept;
std::uint32_t split_eval_cache_epoch() noexcept;
void split_eval_cache_clear() noexcept;
void split_eval_cache_bump_epoch() noexcept;
// Grow buckets and free-list Entry pages. No keys, no results.
void split_eval_cache_precommit(std::size_t slots_per_shard,
                                std::size_t entries_per_shard) noexcept;
bool split_eval_cache_precommit_one_shard(int shard, std::size_t slots_per_shard,
                                          std::size_t entries_per_shard) noexcept;
int split_eval_cache_shard_count() noexcept;
void split_eval_precommit_tls() noexcept;
SplitEvalCacheStats split_eval_cache_stats() noexcept;

// Nearest checkpoint with next_step <= k_target. Returns false if none.
bool split_eval_cache_lookup(const ParticleCacheKey& key, int k_target,
                             IntegratorGridState& out) noexcept;
void split_eval_cache_store(const ParticleCacheKey& key, const IntegratorGridState& st) noexcept;
void split_eval_cache_store_many(const ParticleCacheKey& key, const IntegratorGridState* sts,
                                 int n) noexcept;

bool split_eval_cache_lookup_rol(const ParticleCacheKey& key, std::uint8_t axis, int k_target,
                                 int& k_have, float& acc) noexcept;
void split_eval_cache_store_rol(const ParticleCacheKey& key, std::uint8_t axis, int k,
                                float acc) noexcept;

// Play-only: bump the touch generation so reap can recycle entries that
// were not lookup/store'd this frame (dead particles). Seek / golden must
// not reap — leftover checkpoints across samples are part of the identity
// baseline. RecycleAlloc + entry_free take the nodes; not a 2022.3 formula.
void split_eval_cache_begin_play_frame() noexcept;
// Previous frame's work time (editor: tick+render; bench: collect). Idle
// reap uses 16.7 − this − margin. 0 = unknown, skip reap.
void split_eval_cache_note_frame_ms(double frame_ms) noexcept;
double split_eval_cache_prev_frame_ms() noexcept;
void split_eval_cache_reap_unused() noexcept;

}  // namespace wds::chart_render::shuriken

#pragma once

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace wds::chart_render::shuriken {

struct SplitEvalCarryStats {
  std::size_t bytes_used = 0;
  std::size_t systems = 0;
  std::size_t particles = 0;
  std::size_t budget_bytes = 0;
  bool enabled = true;
  float max_gap = 0.25f;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t fallbacks = 0;
  std::uint64_t commits = 0;
  std::uint64_t walk_hits = 0;
  std::uint64_t walk_misses = 0;
  std::uint64_t walk_commits = 0;
  // B1 rewind history (diagnostics; 0 until history is enabled).
  std::uint64_t rewind_hits = 0;
  std::uint64_t rewind_misses = 0;
  std::uint64_t rewind_bytes = 0;
};

struct SystemCarryKey {
  uint32_t epoch = 0;
  int32_t color_id = 0;
  int32_t line_index = 0;
  int64_t line_start_ms = 0;
  uint32_t catalog_index = 0;
  uint32_t line_play_gen = 0;
  uint64_t input_hash = 0;
  uint8_t kind = 0;
};

inline bool operator==(const SystemCarryKey& a, const SystemCarryKey& b) noexcept {
  return a.epoch == b.epoch && a.color_id == b.color_id && a.line_index == b.line_index &&
         a.line_start_ms == b.line_start_ms && a.catalog_index == b.catalog_index &&
         a.line_play_gen == b.line_play_gen && a.input_hash == b.input_hash && a.kind == b.kind;
}

struct CarryParticle {
  Birth b{};
  Birth parent_b{};
  ParticleState st{};
  IntegratorGridState grid{};
  uint32_t seed = 0;
  float emit_u = 0.0f;
  uint16_t slot_i = 0;
  uint8_t has_grid = 0;
  uint8_t on_grid = 1;
};

struct SystemCarrySnapshot {
  float now_t = -1.0e30f;
  int next_birth = 0;
  std::shared_ptr<const std::vector<CarryParticle>> storage;

  const std::vector<CarryParticle>& parts() const noexcept {
    static const std::vector<CarryParticle> kEmpty;
    return storage ? *storage : kEmpty;
  }
};

bool split_eval_carry_enabled() noexcept;
// B1: bounded rewind history is opt-in (WDS_SPLIT_REWIND=1).
bool split_eval_carry_rewind_enabled() noexcept;
float split_eval_carry_max_gap() noexcept;
void split_eval_carry_clear() noexcept;
// Seek path: drop the current forward snapshot but keep the bounded rewind
// history (B1). Doc/quality/epoch invalidation still uses the full clear.
void split_eval_carry_clear_current_keep_rewind() noexcept;
void split_eval_carry_bump_epoch() noexcept;
void split_eval_carry_precommit(std::size_t entries_per_shard,
                                std::size_t walk_per_shard) noexcept;
bool split_eval_carry_precommit_one_shard(int shard, std::size_t entries) noexcept;
bool split_eval_carry_precommit_one_walk(int shard, std::size_t entries) noexcept;
int split_eval_carry_shard_count() noexcept;
int split_eval_carry_walk_shard_count() noexcept;
void split_eval_carry_set_store_inhibited(bool inhibited) noexcept;
bool split_eval_carry_store_inhibited() noexcept;
void split_eval_carry_set_enabled_for_tests(int on) noexcept;
// seconds <= 0 restores the env/default gap (0.25 unless WDS_SPLIT_CARRY_GAP).
void split_eval_carry_set_max_gap_for_tests(float seconds) noexcept;
SplitEvalCarryStats split_eval_carry_stats() noexcept;

bool split_eval_carry_can_resume(float prev_now, float now) noexcept;
bool split_eval_carry_lookup(const SystemCarryKey& key, SystemCarrySnapshot& out) noexcept;
// Current snapshot only; resume if now is forward within max_gap.
bool split_eval_carry_lookup_for_now(const SystemCarryKey& key, float now,
                                     SystemCarrySnapshot& out) noexcept;
// Forward current snapshot first, then the newest rewind history <= now.
bool split_eval_carry_lookup_for_time(const SystemCarryKey& key, float now,
                                      SystemCarrySnapshot& out) noexcept;
void split_eval_carry_store(const SystemCarryKey& key, float now_t,
                            std::vector<CarryParticle>&& parts, int next_birth = 0) noexcept;
void split_eval_carry_drop(const SystemCarryKey& key) noexcept;
void split_eval_carry_note_fallback() noexcept;

bool carry_birth_eq(const Birth& a, const Birth& b) noexcept;
int carry_find_particle(const SystemCarrySnapshot& snap, const Birth& b, uint32_t seed) noexcept;
int carry_find_child(const SystemCarrySnapshot& snap, const Birth& child, uint32_t seed,
                     const Birth& parent, uint16_t slot_i) noexcept;

struct SubemitWalkKey {
  SystemCarryKey parent{};
  int birth_index = 0;
  uint32_t birth_extra = 0;
  uint32_t birth_time_bits = 0;
  uint16_t slot_i = 0;
};

inline bool operator==(const SubemitWalkKey& a, const SubemitWalkKey& b) noexcept {
  return a.parent == b.parent && a.birth_index == b.birth_index && a.birth_extra == b.birth_extra &&
         a.birth_time_bits == b.birth_time_bits && a.slot_i == b.slot_i;
}

struct SubemitWalkPose {
  float event_t = 0.0f;
  uint8_t type = 0;
  float wx = 0.0f;
  float wy = 0.0f;
  float wz = 0.0f;
  float size = 1.0f;
  float size_y = 1.0f;
  float size_z = 1.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
  float roll = 0.0f;
  float tilt_x = 0.0f;
  float tilt_y = 0.0f;
  float life = 1.0f;
  float age = 0.0f;
  float duration_u = 0.0f;
  uint32_t seed = 0;
  float birth_origin_y_delta = 0.0f;
  uint8_t written = 0;
};

struct SubemitWalkCursor {
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
};

bool split_eval_walk_lookup(const SubemitWalkKey& key, SubemitWalkCursor& out) noexcept;
bool split_eval_walk_lookup_for_now(const SubemitWalkKey& key, float now,
                                    SubemitWalkCursor& out, bool want_parent_grid = true) noexcept;
void split_eval_walk_store(const SubemitWalkKey& key, float now_t, SubemitWalkCursor&& cur) noexcept;
// Play resume with no new births / poses: bump now_t only (no vector copy). hop.
void split_eval_walk_touch(const SubemitWalkKey& key, float now_t,
                           const SubemitWalkCursor& cur) noexcept;
// Patch written poses in place (one lock, no birth/pose vector copy). hop.
void split_eval_walk_write_poses(const SubemitWalkKey& key, const float* event_ts,
                                 const SubemitWalkPose* poses, int n) noexcept;

}  // namespace wds::chart_render::shuriken

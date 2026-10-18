#include "wds/ui/regions/preview/playback_preview.hpp"
#include "wds/renderer/log.hpp"
#include "wds/ui/editor_ui_config.hpp"
#include "wds/ui/frame_diag.hpp"

#include <wds/chart_render/openwds_hud_layout.hpp>
#include <wds/chart_render/note_draw_order.hpp>
#include <wds/chart_render/note_strips.hpp>
#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/chart_render/shuriken/evaluate.hpp>
#include <split_collect_pool.hpp>
#include <wds/common/page_touch.hpp>
#include <wds/common/perf_stages.hpp>
#include <wds/chart_render/split_line_fade.hpp>
#include <wds/chart_render/split_line_official_colors.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/official_split_x.hpp>
#include <wds/core/split_effect_gate.hpp>
#include <wds/core/split_effect_syuriken.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/ui/note_skin_mapping.hpp>
#include <wds/ui/regions/edit/edit_gutters.hpp>

#include <wds/common/crash_handler.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wds::ui {

using wds::renderer::DrawBatch;
using wds::renderer::PreviewVisualConfig;
using wds::renderer::Quad;
using wds::renderer::SkinCatalog;
using wds::renderer::StageGeometry;
using wds::renderer::TextureCache;
using wds::renderer::TextureInfo;
using wds::renderer::TextureId;
using wds::renderer::Vec2;
using wds::renderer::VulkanRenderer;
using wds::renderer::add_sliced_note;

namespace {

using wds::chart_editor::NoteType;
using wds::chart_editor::PreviewNoteInstance;
using wds::chart_editor::PreviewNoteVisualState;
using wds::chart_editor::PreviewPlaybackState;
using wds::chart_editor::PreviewSnapshot;
using wds::chart_editor::PreviewSplitLaneInstance;
using wds::chart_editor::is_hold_body;
using wds::chart_editor::is_hold_mid_star;
using wds::chart_editor::is_hold_start;
using wds::chart_editor::is_hold_with_tail;
using wds::chart_editor::is_scratch_hold_body;
using wds::chart_editor::is_split_lane_gimmick;

// Prefer sub-ms clock so notes do not stair-step on floored timeline_ms.
int64_t preview_now_us(const PreviewSnapshot& snapshot, int64_t visual_lead_us = 0) noexcept {
  int64_t us = snapshot.timeline_us;
  if (us == 0 && snapshot.timeline_ms != 0) {
    us = wds::common::ms_to_us(snapshot.timeline_ms).count();
  }
  return us + visual_lead_us;
}

// Pool choose_participants: hint<512 → inline, <8000 → 2, else cap.
// Lane recolor of a few plates (4–8 × ~2k cells) is light: waking workers
// costs more than the 1–2 ms serial job. Particle emit (njobs × verts) is heavy.
constexpr uint64_t kUiPoolMinWork = 24000;
// Seek keeps lane_geom_pool_. Evict by last-hit seq (not per effect id).
constexpr size_t kLaneGeomPoolMaxEntries = 384;
constexpr uint32_t kLaneGeomPoolMaxAgeSeq = 240;
constexpr size_t kLaneGeomFreeCap = 256;

void ui_split_parallel_for(size_t n, void (*fn)(size_t, void*), void* ctx, uint64_t work_hint) {
  if (n == 0 || fn == nullptr) {
    return;
  }
  if (n == 1 || work_hint < kUiPoolMinWork) {
    for (size_t i = 0; i < n; ++i) {
      fn(i, ctx);
    }
    return;
  }
  // Per-call hint overload (chart-render). n-item form derives hint from n
  // (a handful of splits) and would inline a 1.1M-vert emit.
  wds::chart_render::split_collect_parallel_for(0, n, 0, fn, ctx, work_hint);
}

double preview_now_sec(const PreviewSnapshot& snapshot, int64_t visual_lead_us = 0) noexcept {
  return static_cast<double>(preview_now_us(snapshot, visual_lead_us)) / 1'000'000.0;
}

uint64_t mix_u64(uint64_t h, uint64_t v) noexcept {
  h ^= v;
  h *= 1099511628211ull;
  return h;
}

uint64_t mix_f32(uint64_t h, float f) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &f, sizeof(u));
  return mix_u64(h, u);
}

struct TrailDrawScratch {
  std::vector<float> xs;
  std::vector<float> ys;
  std::vector<float> zs;
  std::vector<float> widths;
  std::vector<float> v_along;
  std::vector<float> va;
  std::vector<float> vr;
  std::vector<float> vg;
  std::vector<float> vb;
  std::vector<uint16_t> verts;
  std::vector<float> lx;
  std::vector<float> ly;
  std::vector<float> lz;
  std::vector<float> rx;
  std::vector<float> ry;
  std::vector<float> rz;

  void resize_strip(int n) {
    const size_t nsz = static_cast<size_t>(n);
    xs.resize(nsz);
    ys.resize(nsz);
    zs.resize(nsz);
    widths.resize(nsz);
    v_along.resize(nsz);
    va.resize(nsz);
    vr.resize(nsz);
    vg.resize(nsz);
    vb.resize(nsz);
  }

  void resize_run(int m) {
    const size_t msz = static_cast<size_t>(m);
    lx.resize(msz);
    ly.resize(msz);
    lz.resize(msz);
    rx.resize(msz);
    ry.resize(msz);
    rz.resize(msz);
  }
};

TrailDrawScratch& trail_draw_scratch() {
  thread_local TrailDrawScratch scratch;
  return scratch;
}

// evaluate keeps remaining==0 only for PerParticle dieWith=false until
// trail verts expire. Skip the body quad; trail verts still draw. Ribbon
// dead particles are not kept (they leave the chain at death).
// https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysTrailsModule.html
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.TrailModule-dieWithParticles.html
bool split_live_skip_dead_trail_body(
    const wds::chart_render::SplitEffectLiveParticle& p) noexcept {
  if (p.remaining_lifetime > 1e-5f) {
    return false;
  }
  if ((p.flags & wds::chart_render::kSplitEffectFlagTrails) == 0) {
    return false;
  }
  return p.trail_count < 2 && p.trail_points >= 2;
}

// Unity 2022.3 TrailTextureMode.Stretch: "Map the texture once along the
// entire length of the trail". DistributePerSegment assumes even spacing
// (vertex index). Official catalog ribbons are all Stretch (textureMode=0;
// no Tile / RepeatPerSegment / DistributePerSegment). Native stores
// along-trail in V (API names this U); do not swap axes — BombEffectTrail
// is radial.
// hop (not 2022.3): u along 3D arc length, t=0 at the oldest living
// particle (Bake station 0 / near field). align2-n6 11230_1 / 10820 full
// chains: station 0 uv.u=0, last uv.u=1, Δu ≡ Δarc/total (mae=0).
// Pages do not name which end is u=0. Same 3D-arc t as stitch_ribbons
// widthOverTrail / colorOverTrail (ribbon-jitter-v1). Particle-mode UV
// stays vertex index.
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystemTrailTextureMode.html
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.TrailModule-textureMode.html
void fill_ribbon_stretch_along(float* v_along, const float* xs, const float* ys, const float* zs,
                               int n) noexcept {
  if (v_along == nullptr || n <= 0) {
    return;
  }
  if (n == 1) {
    v_along[0] = 0.0f;
    return;
  }
  float total = 0.0f;
  for (int k = 1; k < n; ++k) {
    const float dx = xs[k] - xs[k - 1];
    const float dy = ys[k] - ys[k - 1];
    const float dz = zs[k] - zs[k - 1];
    total += std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  // trail_vertex 0 is oldest; last is newest. t=0 = oldest (align2-n6).
  v_along[0] = 0.0f;
  float acc = 0.0f;
  for (int k = 1; k < n; ++k) {
    const float dx = xs[k] - xs[k - 1];
    const float dy = ys[k] - ys[k - 1];
    const float dz = zs[k] - zs[k - 1];
    acc += std::sqrt(dx * dx + dy * dy + dz * dz);
    v_along[static_cast<size_t>(k)] = total > 1e-6f ? acc / total : 1.0f;
  }
}

struct SplitEmitTargets {
  DrawBatch* notes = nullptr;
  DrawBatch* glow = nullptr;
  DrawBatch* late = nullptr;
  int* drawn = nullptr;
};

struct SplitPartScratch {
  DrawBatch notes;
  DrawBatch glow;
  DrawBatch late;
  int drawn = 0;
};

uint64_t hash_split_lanes_frame(uint64_t splits_hash, uint64_t view_hash, int64_t now_us,
                                float rgb_k, TextureId plate_tex) noexcept {
  uint64_t h = splits_hash;
  h = mix_u64(h, view_hash);
  h = mix_u64(h, static_cast<uint64_t>(now_us));
  h = mix_f32(h, rgb_k);
  h = mix_u64(h, static_cast<uint64_t>(plate_tex));
  return h;
}

uint64_t hash_one_lane_geom(const PreviewSplitLaneInstance& s, uint64_t view_hash,
                            TextureId plate_tex) noexcept {
  uint64_t h = 1469598103934665603ull;
  h = mix_u64(h, view_hash);
  h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.source_note_id)));
  h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.split_count)));
  h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.split_lane_type)));
  h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.scratch_length)));
  h = mix_u64(h, static_cast<uint64_t>(s.start_ms));
  h = mix_u64(h, static_cast<uint64_t>(s.end_ms));
  h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.effective_lane_count)));
  h = mix_f32(h, s.split_percent_start);
  h = mix_f32(h, s.split_percent_end);
  h = mix_f32(h, s.split_line_alpha);
  h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.split_anim_phase)));
  h = mix_u64(h, static_cast<uint64_t>(plate_tex));
  return h;
}

struct SplitLanePlateShared {
  StageGeometry* geometry = nullptr;
  const SkinCatalog* skin = nullptr;
  const TextureInfo* plate = nullptr;
  float time_sec = 0.0f;
  float rgb_k = 1.0f;
};

// Plates need the GPU Syuriken pipeline; textured_quad would read the
// world-Y-scale vertex alpha as opacity.
const TextureInfo* syuriken_plate_for(const SkinCatalog& skin,
                                      const wds::renderer::VulkanRenderer& vk) noexcept {
  if (!skin.syuriken_plate || !vk.syuriken_ready()) {
    return nullptr;
  }
  return &skin.syuriken_plate;
}

// OpenWDS SplitEffectSyuriken shades per fragment (renderer/shaders/syuriken.frag).
// One projective quad per Line: U spans the sprite textureRect, V is local_y /
// sprite at each end, rgb is LineColor premultiplied by its alpha and vertex
// alpha carries Unity world Y + kOfficialSyurikenPulseWorldYBias (batched
// SpriteRenderer ObjectToWorld is identity; pulse = sin(positionWS.y - 20 t)).
void emit_syuriken_line_quad(const SplitLanePlateShared& sh, const Quad& quad, float pa, float pb,
                             int32_t color_id, float fade_scale, float r, float g, float b,
                             float a, DrawBatch& out) {
  auto v_at = [&](float percent) {
    const float main_y = wds::chart_editor::official_percent_to_main_y(percent);
    return wds::chart_editor::official_split_line_local_y(color_id, main_y, fade_scale) /
           wds::chart_editor::kOfficialSplitLineSprite;
  };
  auto wy_at = [&](float percent) {
    return wds::chart_editor::official_syuriken_pulse_position_ws_y(
               wds::chart_editor::official_percent_to_main_y(percent)) +
           wds::chart_editor::kOfficialSyurikenPulseWorldYBias;
  };
  // The Syuriken pipeline culls back faces (material _Cull=2); fill_quad_corner_geom
  // winds lb→lt→rt clockwise, so hand it the mirrored quad with U swapped.
  const Quad mirrored{quad.rb, quad.rt, quad.lt, quad.lb};
  wds::renderer::DrawVertex geom[6];
  DrawBatch::fill_quad_corner_geom(mirrored, -0.6f, wds::chart_editor::kOfficialSplitLineMeshU1,
                                   v_at(pb), wds::chart_editor::kOfficialSplitLineMeshU0,
                                   v_at(pa), geom);
  // mirrored lb/rb = orig pb (near), lt/rt = orig pa (start).
  const float wy_b = wy_at(pb);
  const float wy_a = wy_at(pa);
  const float pr = r * a;
  const float pg = g * a;
  const float pbl = b * a;
  out.add_quad_from_geom(sh.plate->id, geom, wy_b, wy_b, wy_a, wy_a, pr, pg, pbl, pr, pg, pbl, pr,
                         pg, pbl, pr, pg, pbl);
}

Quad split_line_plate_quad(const SplitLanePlateShared& sh, float world_x, float pa, float pb) {
  return sh.geometry->split_line_quad_world_x(world_x, pa, pb,
                                              wds::chart_editor::kOfficialSplitLineMeshU0,
                                              wds::chart_editor::kOfficialSplitLineMeshU1);
}

void append_all_split_lanes_legacy_plates(const SplitLanePlateShared& sh,
                                          const std::vector<PreviewSplitLaneInstance>& splits,
                                          DrawBatch& out) {
  if (sh.plate == nullptr || sh.geometry == nullptr) {
    return;
  }
  out.bucket_for(sh.plate->id);
  for (const auto& split : splits) {
    if (!split.should_show || split.split_count <= 0 ||
        (split.split_percent_end - split.split_percent_start) < 1e-4f) {
      continue;
    }
    const int32_t color_id = split.scratch_length != 0 ? split.scratch_length : 1;
    const float p0 = split.split_percent_start;
    const float p1 = split.split_percent_end;
    const float play_sec = static_cast<float>(split.start_ms) / 1000.0f -
                           wds::chart_editor::kOfficialSplitFadeInSec;
    const float fade_scale =
        wds::chart_render::shuriken::fade_scale_at_play_t(sh.time_sec - play_sec);
    const float la = std::clamp(split.split_line_alpha, 0.0f, 1.0f);
    const int32_t n = wds::chart_editor::official_split_line_x_count(split.split_count);
    for (int32_t i = 0; i < n; ++i) {
      float world_x = 0.0f;
      if (!wds::chart_editor::official_split_line_world_x(split.split_count, i, world_x)) {
        continue;
      }
      const auto slot_c = split_slot_color(color_id, i, split.split_count, sh.skin);
      const float ca = slot_c.a * la;
      if (ca < 0.008f) {
        continue;
      }
      emit_syuriken_line_quad(sh, split_line_plate_quad(sh, world_x, p0, p1), p0, p1, color_id,
                              fade_scale, slot_c.r * sh.rgb_k, slot_c.g * sh.rgb_k,
                              slot_c.b * sh.rgb_k, ca, out);
    }
  }
}

struct LaneGeomBuildCtx {
  SplitLanePlateShared shared;
  const std::vector<PreviewSplitLaneInstance>* splits = nullptr;
  std::vector<PreviewLaneGeomSplit>* geoms = nullptr;
};

void build_lane_geom_split(size_t split_idx, void* raw) {
  auto* ctx = static_cast<LaneGeomBuildCtx*>(raw);
  if (ctx == nullptr || ctx->splits == nullptr || ctx->geoms == nullptr ||
      split_idx >= ctx->splits->size() || split_idx >= ctx->geoms->size()) {
    return;
  }
  const auto& split = (*ctx->splits)[split_idx];
  PreviewLaneGeomSplit& geom = (*ctx->geoms)[split_idx];
  geom.cells.clear();
  geom.play_sec = 0.0f;
  if (!split.should_show || split.split_count <= 0 ||
      (split.split_percent_end - split.split_percent_start) < 1e-4f) {
    return;
  }
  const SplitLanePlateShared& sh = ctx->shared;
  if (sh.plate == nullptr || sh.geometry == nullptr) {
    return;
  }
  const int32_t color_id = split.scratch_length != 0 ? split.scratch_length : 1;
  const float p0 = split.split_percent_start;
  const float p1 = split.split_percent_end;
  geom.play_sec = static_cast<float>(split.start_ms) / 1000.0f -
                  wds::chart_editor::kOfficialSplitFadeInSec;
  const float la = std::clamp(split.split_line_alpha, 0.0f, 1.0f);
  const int32_t n = wds::chart_editor::official_split_line_x_count(split.split_count);
  geom.cells.reserve(static_cast<size_t>(std::max(n, 0)));
  for (int32_t i = 0; i < n; ++i) {
    float world_x = 0.0f;
    if (!wds::chart_editor::official_split_line_world_x(split.split_count, i, world_x)) {
      continue;
    }
    const auto slot_c = split_slot_color(color_id, i, split.split_count, sh.skin);
    const float sa = slot_c.a * la;
    if (sa < 0.008f) {
      continue;
    }
    PreviewLaneGeomCell cell;
    cell.quad = split_line_plate_quad(sh, world_x, p0, p1);
    cell.pa = p0;
    cell.pb = p1;
    cell.sr = slot_c.r;
    cell.sg = slot_c.g;
    cell.sb = slot_c.b;
    cell.sa = sa;
    cell.color_id = color_id;
    geom.cells.push_back(cell);
  }
}

struct LaneGeomColorCtx {
  SplitLanePlateShared shared;
  const PreviewLaneGeomSplit* const* geoms = nullptr;
  size_t geom_n = 0;
  DrawBatch* outs = nullptr;
};

void color_lane_geom_one(const SplitLanePlateShared& sh, const PreviewLaneGeomSplit& geom,
                         DrawBatch& out) {
  if (geom.cells.empty() || sh.plate == nullptr) {
    return;
  }
  out.bucket_for(sh.plate->id);
  out.reserve_quads(geom.cells.size());
  const float fade_scale =
      wds::chart_render::shuriken::fade_scale_at_play_t(sh.time_sec - geom.play_sec);
  for (const auto& cell : geom.cells) {
    emit_syuriken_line_quad(sh, cell.quad, cell.pa, cell.pb, cell.color_id, fade_scale,
                            cell.sr * sh.rgb_k, cell.sg * sh.rgb_k, cell.sb * sh.rgb_k, cell.sa,
                            out);
  }
}

void color_lane_geom_split(size_t split_idx, void* raw) {
  auto* ctx = static_cast<LaneGeomColorCtx*>(raw);
  if (ctx == nullptr || ctx->geoms == nullptr || ctx->outs == nullptr ||
      split_idx >= ctx->geom_n || ctx->geoms[split_idx] == nullptr) {
    return;
  }
  color_lane_geom_one(ctx->shared, *ctx->geoms[split_idx], ctx->outs[split_idx]);
}

void emit_lane_geom_plates(const SplitLanePlateShared& sh,
                           const std::vector<const PreviewLaneGeomSplit*>& geoms, DrawBatch& out,
                           std::vector<DrawBatch>& parts) {
  const size_t split_n = geoms.size();
  if (split_n == 0 || sh.plate == nullptr) {
    return;
  }
  out.bucket_for(sh.plate->id);
  uint64_t work = 0;
  for (size_t i = 0; i < split_n; ++i) {
    if (geoms[i] != nullptr) {
      work += static_cast<uint64_t>(geoms[i]->cells.size());
    }
  }
  // Light recolor stays on the caller: no pool wake, no per-split batch copy.
  if (split_n <= 1 || work < kUiPoolMinWork) {
    for (size_t i = 0; i < split_n; ++i) {
      if (geoms[i] != nullptr) {
        color_lane_geom_one(sh, *geoms[i], out);
      }
    }
    return;
  }
  if (parts.size() < split_n) {
    parts.resize(split_n);
  }
  for (size_t i = 0; i < split_n; ++i) {
    parts[i].clear();
  }
  LaneGeomColorCtx ctx{};
  ctx.shared = sh;
  ctx.geoms = geoms.data();
  ctx.geom_n = split_n;
  ctx.outs = parts.data();
  ui_split_parallel_for(split_n, color_lane_geom_split, &ctx, work);
  for (size_t i = 0; i < split_n; ++i) {
    out.append_from(parts[i]);
  }
}

void recycle_lane_geom_entry(PreviewLaneGeomSplit&& geom,
                             std::vector<PreviewLaneGeomSplit>& recycled) {
  geom.cells.clear();
  geom.last_hit_seq = 0;
  if (recycled.size() < kLaneGeomFreeCap) {
    recycled.push_back(std::move(geom));
  }
}

void evict_lane_geom_pool(std::unordered_map<uint64_t, PreviewLaneGeomSplit>& pool,
                          std::vector<PreviewLaneGeomSplit>& recycled, uint32_t hit_seq) {
  const uint32_t min_keep = hit_seq > kLaneGeomPoolMaxAgeSeq ? hit_seq - kLaneGeomPoolMaxAgeSeq : 0;
  for (auto it = pool.begin(); it != pool.end();) {
    if (it->second.last_hit_seq < min_keep) {
      recycle_lane_geom_entry(std::move(it->second), recycled);
      it = pool.erase(it);
    } else {
      ++it;
    }
  }
  if (pool.size() <= kLaneGeomPoolMaxEntries) {
    return;
  }
  std::vector<std::pair<uint32_t, uint64_t>> aged;
  aged.reserve(pool.size());
  for (const auto& kv : pool) {
    if (kv.second.last_hit_seq != hit_seq) {
      aged.push_back({kv.second.last_hit_seq, kv.first});
    }
  }
  std::sort(aged.begin(), aged.end());
  size_t i = 0;
  while (pool.size() > kLaneGeomPoolMaxEntries && i < aged.size()) {
    auto it = pool.find(aged[i].second);
    ++i;
    if (it == pool.end()) {
      continue;
    }
    recycle_lane_geom_entry(std::move(it->second), recycled);
    pool.erase(it);
  }
}

size_t ensure_lane_geom_pool(std::unordered_map<uint64_t, PreviewLaneGeomSplit>& pool,
                             std::vector<PreviewLaneGeomSplit>& recycled,
                             const std::vector<PreviewSplitLaneInstance>& splits,
                             const SplitLanePlateShared& sh, uint64_t view_hash, uint32_t hit_seq,
                             std::vector<const PreviewLaneGeomSplit*>* frame_out) {
  const size_t split_n = splits.size();
  if (frame_out != nullptr) {
    frame_out->assign(split_n, nullptr);
  }
  if (split_n == 0 || sh.plate == nullptr || sh.geometry == nullptr) {
    return 0;
  }
  std::vector<uint64_t> keys(split_n, 0);
  std::vector<size_t> miss;
  miss.reserve(split_n);
  for (size_t i = 0; i < split_n; ++i) {
    keys[i] = hash_one_lane_geom(splits[i], view_hash, sh.plate->id);
    if (pool.find(keys[i]) == pool.end()) {
      miss.push_back(i);
    }
  }
  if (!miss.empty()) {
    std::vector<PreviewSplitLaneInstance> miss_splits(miss.size());
    std::vector<PreviewLaneGeomSplit> miss_geoms(miss.size());
    for (size_t j = 0; j < miss.size(); ++j) {
      miss_splits[j] = splits[miss[j]];
      if (!recycled.empty()) {
        miss_geoms[j] = std::move(recycled.back());
        recycled.pop_back();
        miss_geoms[j].cells.clear();
        miss_geoms[j].last_hit_seq = 0;
      }
    }
    LaneGeomBuildCtx build_ctx{};
    build_ctx.shared = sh;
    build_ctx.splits = &miss_splits;
    build_ctx.geoms = &miss_geoms;
    uint64_t build_work = 0;
    for (size_t j = 0; j < miss_splits.size(); ++j) {
      const int32_t line_n =
          wds::chart_editor::official_split_line_x_count(miss_splits[j].split_count);
      build_work += static_cast<uint64_t>(std::max(line_n, 0));
    }
    ui_split_parallel_for(miss.size(), build_lane_geom_split, &build_ctx, build_work);
    for (size_t j = 0; j < miss.size(); ++j) {
      pool[keys[miss[j]]] = std::move(miss_geoms[j]);
    }
  }
  for (size_t i = 0; i < split_n; ++i) {
    auto it = pool.find(keys[i]);
    if (it != pool.end()) {
      it->second.last_hit_seq = hit_seq;
    }
  }
  evict_lane_geom_pool(pool, recycled, hit_seq);
  if (frame_out == nullptr) {
    return miss.size();
  }
  for (size_t i = 0; i < split_n; ++i) {
    auto it = pool.find(keys[i]);
    (*frame_out)[i] = it == pool.end() ? nullptr : &it->second;
  }
  return miss.size();
}

void dump_draw_batch(FILE* out, const char* tag, const wds::renderer::DrawBatch& batch) {
  std::fwrite(tag, 1, 4, out);
  const uint32_t bucket_count = static_cast<uint32_t>(batch.buckets.size());
  std::fwrite(&bucket_count, sizeof(bucket_count), 1, out);
  for (const auto& bucket : batch.buckets) {
    std::vector<wds::renderer::DrawVertex> list;
    wds::renderer::DrawBatch::expand_bucket_list(bucket, list);
    const uint32_t tex = bucket.texture;
    const uint32_t clip = bucket.disable_alpha_clip ? 1u : 0u;
    const uint32_t vert_count = static_cast<uint32_t>(list.size());
    std::fwrite(&tex, sizeof(tex), 1, out);
    std::fwrite(&clip, sizeof(clip), 1, out);
    std::fwrite(&vert_count, sizeof(vert_count), 1, out);
    if (vert_count > 0) {
      std::fwrite(list.data(), sizeof(wds::renderer::DrawVertex), vert_count, out);
    }
  }
}

bool draw_batches_byte_equal(const wds::renderer::DrawBatch& a,
                             const wds::renderer::DrawBatch& b) {
  auto next_nonempty = [](const wds::renderer::DrawBatch& batch, size_t i) {
    while (i < batch.buckets.size() && batch.buckets[i].vertices.empty()) {
      ++i;
    }
    return i;
  };
  size_t ia = 0;
  size_t ib = 0;
  while (true) {
    ia = next_nonempty(a, ia);
    ib = next_nonempty(b, ib);
    if (ia == a.buckets.size() && ib == b.buckets.size()) {
      return true;
    }
    if (ia == a.buckets.size() || ib == b.buckets.size()) {
      return false;
    }
    const auto& ba = a.buckets[ia];
    const auto& bb = b.buckets[ib];
    if (ba.texture != bb.texture || ba.disable_alpha_clip != bb.disable_alpha_clip ||
        ba.overlay_pipe != bb.overlay_pipe) {
      return false;
    }
    std::vector<wds::renderer::DrawVertex> la;
    std::vector<wds::renderer::DrawVertex> lb;
    wds::renderer::DrawBatch::expand_bucket_list(ba, la);
    wds::renderer::DrawBatch::expand_bucket_list(bb, lb);
    if (la.size() != lb.size()) {
      return false;
    }
    if (!la.empty() &&
        std::memcmp(la.data(), lb.data(), la.size() * sizeof(wds::renderer::DrawVertex)) != 0) {
      return false;
    }
    ++ia;
    ++ib;
  }
}

uint32_t draw_batch_vertex_count(const wds::renderer::DrawBatch& batch) noexcept {
  uint32_t n = 0;
  for (const auto& bucket : batch.buckets) {
    n += bucket.indices.empty() ? static_cast<uint32_t>(bucket.vertices.size())
                                : static_cast<uint32_t>(bucket.indices.size());
  }
  return n;
}

uint64_t hash_draw_batch_bytes(const wds::renderer::DrawBatch& batch) noexcept {
  uint64_t h = 1469598103934665603ull;
  for (const auto& bucket : batch.buckets) {
    if (bucket.vertices.empty() && bucket.indices.empty()) {
      continue;
    }
    h = mix_u64(h, bucket.texture);
    h = mix_u64(h, bucket.disable_alpha_clip ? 1ull : 0ull);
    h = mix_u64(h, bucket.overlay_pipe);
    std::vector<wds::renderer::DrawVertex> list;
    wds::renderer::DrawBatch::expand_bucket_list(bucket, list);
    const size_t bytes = list.size() * sizeof(wds::renderer::DrawVertex);
    if (bytes > 0) {
      const auto* p = reinterpret_cast<const uint8_t*>(list.data());
      for (size_t i = 0; i < bytes; ++i) {
        h = mix_u64(h, p[i]);
      }
    }
  }
  return h;
}

std::string dump_verts_path_with_suffix(const char* path, const char* suffix) {
  std::string out(path);
  const auto slash = out.find_last_of("/\\");
  const auto dot = out.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    out.insert(dot, suffix);
    return out;
  }
  out += suffix;
  return out;
}

bool preview_dump_wanted(int64_t now_us, bool playback_playing, uint32_t total_n) {
  const char* path = std::getenv("WDS_DUMP_VERTS");
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  const char* at_us = std::getenv("WDS_DUMP_VERTS_AT_US");
  if (at_us != nullptr && at_us[0] != '\0') {
    const int64_t target = std::strtoll(at_us, nullptr, 10);
    const int64_t delta = now_us - target;
    if (delta < 0 || delta > 50'000) {
      return false;
    }
  }
  const bool when_paused =
      wds::ui::frame_diag_env_enabled(std::getenv("WDS_DUMP_VERTS_WHEN_PAUSED"));
  if (when_paused && playback_playing) {
    return false;
  }
  int min_verts = 4096;
  if (const char* min_env = std::getenv("WDS_DUMP_VERTS_MIN_VERTS");
      min_env != nullptr && min_env[0] != '\0') {
    min_verts = static_cast<int>(std::strtol(min_env, nullptr, 10));
    if (min_verts < 0) {
      min_verts = 0;
    }
  }
  return static_cast<int>(total_n) >= min_verts;
}

void write_texture_identity_table(FILE* out, const std::string& dump_path,
                                  const wds::renderer::TextureCache* textures) {
  std::vector<std::pair<wds::renderer::TextureId, std::string>> entries;
  if (textures != nullptr) {
    entries = textures->identity_entries();
  }
  const char magic[4] = {'T', 'E', 'X', 'K'};
  std::fwrite(magic, 1, 4, out);
  const uint32_t n = static_cast<uint32_t>(entries.size());
  std::fwrite(&n, sizeof(n), 1, out);
  for (const auto& e : entries) {
    const uint32_t id = e.first;
    const uint32_t len = static_cast<uint32_t>(e.second.size());
    std::fwrite(&id, sizeof(id), 1, out);
    std::fwrite(&len, sizeof(len), 1, out);
    if (len > 0) {
      std::fwrite(e.second.data(), 1, len, out);
    }
  }
  const std::string side = std::string(dump_path) + ".texmap";
  if (FILE* map = std::fopen(side.c_str(), "w")) {
    for (const auto& e : entries) {
      std::fprintf(map, "%u\t%s\n", e.first, e.second.c_str());
    }
    std::fclose(map);
  }
}

void dump_preview_draw_batches_if_requested(const wds::renderer::DrawBatch& scene,
                                            const wds::renderer::DrawBatch& glow,
                                            const wds::renderer::DrawBatch& notes,
                                            const wds::renderer::DrawBatch& late,
                                            const wds::renderer::DrawBatch& additive,
                                            int64_t now_us, bool playback_playing,
                                            bool used_pause_memo,
                                            const wds::renderer::TextureCache* textures) {
  const char* path = std::getenv("WDS_DUMP_VERTS");
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  const char* at_us = std::getenv("WDS_DUMP_VERTS_AT_US");
  if (at_us != nullptr && at_us[0] != '\0') {
    const int64_t target = std::strtoll(at_us, nullptr, 10);
    const int64_t delta = now_us - target;
    if (delta < 0 || delta > 50'000) {
      return;
    }
  }
  const bool when_paused =
      wds::ui::frame_diag_env_enabled(std::getenv("WDS_DUMP_VERTS_WHEN_PAUSED"));
  if (when_paused && playback_playing) {
    return;
  }
  const uint32_t scene_n = draw_batch_vertex_count(scene);
  const uint32_t glow_n = draw_batch_vertex_count(glow);
  const uint32_t notes_n = draw_batch_vertex_count(notes);
  const uint32_t late_n = draw_batch_vertex_count(late);
  const uint32_t add_n = draw_batch_vertex_count(additive);
  const uint32_t total_n = scene_n + glow_n + notes_n + late_n + add_n;
  int min_verts = 4096;
  if (const char* min_env = std::getenv("WDS_DUMP_VERTS_MIN_VERTS");
      min_env != nullptr && min_env[0] != '\0') {
    min_verts = static_cast<int>(std::strtol(min_env, nullptr, 10));
    if (min_verts < 0) {
      min_verts = 0;
    }
  }
  if (static_cast<int>(total_n) < min_verts) {
    return;
  }
  static bool dumped_fresh = false;
  static bool dumped_memo = false;
  if (when_paused) {
    if (used_pause_memo ? dumped_memo : dumped_fresh) {
      return;
    }
  } else if (dumped_fresh || dumped_memo) {
    return;
  }
  const std::string out_path =
      (when_paused && used_pause_memo) ? dump_verts_path_with_suffix(path, ".memo") : std::string(path);
  FILE* out = std::fopen(out_path.c_str(), "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "WDS_DUMP_VERTS: failed to open %s\n", out_path.c_str());
    return;
  }
  const char magic[4] = {'W', 'D', 'S', 'V'};
  const uint32_t version = 2;
  std::fwrite(magic, 1, 4, out);
  std::fwrite(&version, sizeof(version), 1, out);
  std::fwrite(&now_us, sizeof(now_us), 1, out);
  const uint32_t counts[5] = {scene_n, glow_n, notes_n, late_n, add_n};
  std::fwrite(counts, sizeof(counts[0]), 5, out);
  dump_draw_batch(out, "SCEN", scene);
  dump_draw_batch(out, "GLOW", glow);
  dump_draw_batch(out, "NOTE", notes);
  dump_draw_batch(out, "LATE", late);
  dump_draw_batch(out, "ADD_", additive);
  write_texture_identity_table(out, out_path, textures);
  std::fclose(out);
  if (used_pause_memo) {
    dumped_memo = true;
  } else {
    dumped_fresh = true;
  }

  const uint64_t h_scene = hash_draw_batch_bytes(scene);
  const uint64_t h_glow = hash_draw_batch_bytes(glow);
  const uint64_t h_notes = hash_draw_batch_bytes(notes);
  const uint64_t h_late = hash_draw_batch_bytes(late);
  const uint64_t h_add = hash_draw_batch_bytes(additive);
  long file_bytes = 0;
  if (FILE* fin = std::fopen(out_path.c_str(), "rb")) {
    if (std::fseek(fin, 0, SEEK_END) == 0) {
      file_bytes = std::ftell(fin);
    }
    std::fclose(fin);
  }
  std::fprintf(stderr,
               "WDS_DUMP_VERTS path=%s now_us=%lld bytes=%ld total_verts=%u "
               "(scene=%u glow=%u notes=%u late=%u add=%u) pause_memo=%d "
               "hash_scene=%016llx hash_glow=%016llx hash_notes=%016llx "
               "hash_late=%016llx hash_add=%016llx\n",
               out_path.c_str(), static_cast<long long>(now_us), file_bytes, total_n, scene_n,
               glow_n, notes_n, late_n, add_n, used_pause_memo ? 1 : 0,
               static_cast<unsigned long long>(h_scene), static_cast<unsigned long long>(h_glow),
               static_cast<unsigned long long>(h_notes), static_cast<unsigned long long>(h_late),
               static_cast<unsigned long long>(h_add));
  if (min_verts > 0 && static_cast<int>(total_n) < min_verts) {
    std::fprintf(stderr, "WDS_DUMP_VERTS: total_verts %u below min %d\n", total_n, min_verts);
    std::abort();
  }
  if (file_bytes < 256) {
    std::fprintf(stderr, "WDS_DUMP_VERTS: dump file implausibly small (%ld bytes)\n", file_bytes);
    std::abort();
  }
}

uint64_t hash_split_lanes(const std::vector<PreviewSplitLaneInstance>& splits) noexcept {
  uint64_t h = 1469598103934665603ull;
  h = mix_u64(h, splits.size());
  for (const auto& s : splits) {
    h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.source_note_id)));
    h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.split_count)));
    h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.split_lane_type)));
    h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.scratch_length)));
    h = mix_u64(h, s.should_show ? 1ull : 0ull);
    h = mix_u64(h, s.is_continued ? 1ull : 0ull);
    h = mix_u64(h, static_cast<uint64_t>(s.start_ms));
    h = mix_u64(h, static_cast<uint64_t>(s.end_ms));
    h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.effective_lane_count)));
    h = mix_f32(h, s.split_line_alpha);
    h = mix_f32(h, s.split_percent_start);
    h = mix_f32(h, s.split_percent_end);
    h = mix_u64(h, static_cast<uint64_t>(static_cast<uint32_t>(s.split_anim_phase)));
  }
  return h;
}

uint64_t hash_preview_view(const StageGeometry& geometry) noexcept {
  const auto& c = geometry.content();
  uint64_t h = 1469598103934665603ull;
  h = mix_f32(h, c.l);
  h = mix_f32(h, c.r);
  h = mix_f32(h, c.b);
  h = mix_f32(h, c.t);
  h = mix_f32(h, c.w);
  h = mix_f32(h, c.h);
  h = mix_u64(h, static_cast<uint64_t>(geometry.content_height_px()));
  return h;
}

using NoteSprites = wds::ui::NoteSprites;
using wds::ui::sprites_for;

[[maybe_unused]] int32_t scale_boundary(int32_t boundary_12, int32_t lane_count) {
  if (lane_count == 12) {
    return boundary_12;
  }
  return boundary_12 * lane_count / 12;
}

// Unity AnimationCurve cubic Hermite (SplitEffect_all baked curves).
float sample_hermite_curve(float t, const float* times, const float* values, const float* slopes,
                           int n) noexcept {
  if (n < 2) {
    return 0.0f;
  }
  if (t <= times[0]) {
    return values[0];
  }
  if (t >= times[n - 1]) {
    return values[n - 1];
  }
  int i = 0;
  while (i + 2 < n && t > times[i + 1]) {
    ++i;
  }
  const float t0 = times[i];
  const float t1 = times[i + 1];
  const float dt = t1 - t0;
  const float u = (t - t0) / dt;
  const float u2 = u * u;
  const float u3 = u2 * u;
  const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
  const float h10 = u3 - 2.0f * u2 + u;
  const float h01 = -2.0f * u3 + 3.0f * u2;
  const float h11 = u3 - u2;
  return h00 * values[i] + h10 * dt * slopes[i] + h01 * values[i + 1] + h11 * dt * slopes[i + 1];
}

// SplitEffect_all position-over-life (m_ExpressionIndex 40): ease-in 0→1.
[[maybe_unused]] float split_pulse_travel_t(float u) noexcept {
  constexpr float kTimes[] = {0.0f, 1.0f};
  constexpr float kValues[] = {-0.0023498535f, 1.0f};
  constexpr float kSlopes[] = {0.42439872f, 1.7960812f};
  return std::clamp(sample_hermite_curve(u, kTimes, kValues, kSlopes, 2), 0.0f, 1.0f);
}

// SplitEffect_all SplitLine alpha envelope (m_ExpressionIndex 68).
[[maybe_unused]] float split_pulse_envelope(float u) noexcept {
  return wds::chart_render::official_split_line_vfx_alpha(u);
}

// Tint hit VFX to match the on-screen note family (not Sonolus particle name colors).
struct EffectTint {
  float r, g, b;
};

EffectTint tint_for(NoteType type) {
  switch (type) {
    case NoteType::Critical:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      // CriticalHold heads use Critical VFX (not the pink/blue head sprite tint).
      return {1.0f, 0.82f, 0.25f};
    case NoteType::HoldStart:
    case NoteType::Hold:
    case NoteType::CriticalHold:  // body/end HoldBomb blue
    case NoteType::NontailHold:
    case NoteType::NontailCriticalHold:
    case NoteType::BlueTap:
      return {0.25f, 0.85f, 1.0f};  // blue hold / tap
    case NoteType::ScratchHoldStart:  // pink NormalNote head, but ScratchBomb purple
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::Flick:
    case NoteType::Scratch:
    case NoteType::ScratchSound:
      return {0.85f, 0.35f, 1.0f};  // purple scratch family
    case NoteType::Sound:
      return {0.35f, 0.95f, 0.55f};  // green tick
    case NoteType::HoldEighth:
      return {0.25f, 0.85f, 1.0f};
    case NoteType::Normal:
    default:
      return {1.0f, 0.35f, 0.4f};  // red normal
  }
}

float ease_in_sine(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return 1.0f - std::cos(x * 3.14159265f * 0.5f);
}

uint64_t note_span_key(int64_t start_ms, int32_t lane, int32_t width) noexcept {
  return (static_cast<uint64_t>(static_cast<uint32_t>(start_ms)) << 32) |
         (static_cast<uint64_t>(static_cast<uint16_t>(lane)) << 16) |
         static_cast<uint16_t>(width);
}

// Per-frame indexes so hit FX / SFX stay O(n) instead of O(n²).
struct PreviewLookup {
  std::vector<const PreviewNoteInstance*> mid_stars;
  std::vector<const PreviewNoteInstance*> duration_holds;
  std::unordered_set<uint64_t> duration_hold_heads;
};

PreviewLookup build_preview_lookup(const PreviewSnapshot& snapshot) {
  PreviewLookup out;
  out.mid_stars.reserve(8);
  out.duration_holds.reserve(8);
  for (const auto& note : snapshot.notes) {
    if (is_hold_mid_star(note.note_type)) {
      out.mid_stars.push_back(&note);
      continue;
    }
    if (is_hold_body(note.note_type) && note.end_ms > note.start_ms) {
      out.duration_holds.push_back(&note);
      out.duration_hold_heads.insert(note_span_key(note.start_ms, note.lane, note.width));
    }
  }
  return out;
}

}  // namespace

struct PlaybackPreviewView::SplitTexPreloadState {
  struct Decoded {
    std::string path;
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
  };
  std::future<std::vector<Decoded>> decode;
  bool started = false;
};

PlaybackPreviewView::PlaybackPreviewView() = default;

PlaybackPreviewView::~PlaybackPreviewView() { shutdown(); }

void PlaybackPreviewView::assign_split_effect_texture(const std::string& path,
                                                      const TextureInfo& info) {
  if (!info) {
    return;
  }
  const char* uv = wds::chart_render::official_split_effect_uv_atlas_relpath();
  const char* trail = wds::chart_render::official_split_effect_trail_tex_relpath();
  if (path.size() >= std::strlen(uv) && path.compare(path.size() - std::strlen(uv), std::strlen(uv),
                                                     uv) == 0) {
    skin_.split_effect_uvs = info;
  } else if (path.size() >= std::strlen(trail) &&
             path.compare(path.size() - std::strlen(trail), std::strlen(trail), trail) == 0) {
    skin_.split_effect_trail = info;
  }
}

void PlaybackPreviewView::finish_split_effect_preload_upload() {
  if (!split_tex_preload_ || !split_tex_preload_->decode.valid()) {
    return;
  }
  std::vector<SplitTexPreloadState::Decoded> imgs;
  try {
    imgs = split_tex_preload_->decode.get();
  } catch (...) {
    WDS_LOG("split effect preload decode failed\n");
    return;
  }
  for (const auto& img : imgs) {
    if (img.rgba.empty() || img.width <= 0 || img.height <= 0) {
      continue;
    }
    const TextureInfo info =
        textures_.load_standalone_rgba(img.path, img.rgba.data(), img.width, img.height);
    assign_split_effect_texture(img.path, info);
  }
}

void PlaybackPreviewView::request_split_effect_preload() {
  if (!ready_ || config_.skins_directory.empty() || wds::ui::preview_no_preload_from_env()) {
    return;
  }
  if (skin_.split_effect_uvs && skin_.split_effect_trail) {
    return;
  }
  if (!split_tex_preload_) {
    split_tex_preload_ = std::make_unique<SplitTexPreloadState>();
  }
  if (split_tex_preload_->started) {
    return;
  }
  split_tex_preload_->started = true;
  std::string dir = config_.skins_directory;
  if (dir.back() != '/' && dir.back() != '\\') {
    dir.push_back('/');
  }
  const std::string uv = dir + wds::chart_render::official_split_effect_uv_atlas_relpath();
  const std::string trail = dir + wds::chart_render::official_split_effect_trail_tex_relpath();
  split_tex_preload_->decode = std::async(std::launch::async, [uv, trail] {
    wds::common::install_thread_crash_stack();
    std::vector<SplitTexPreloadState::Decoded> out(2);
    out[0].path = uv;
    out[1].path = trail;
    (void)wds::renderer::load_png_rgba8(uv, out[0].rgba, out[0].width, out[0].height);
    (void)wds::renderer::load_png_rgba8(trail, out[1].rgba, out[1].width, out[1].height);
    return out;
  });
}

void PlaybackPreviewView::pump_split_effect_preload() {
  if (!split_tex_preload_ || !split_tex_preload_->decode.valid()) {
    return;
  }
  if (split_tex_preload_->decode.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready) {
    return;
  }
  finish_split_effect_preload_upload();
}

void PlaybackPreviewView::reset_lane_geom_time_state() {
  ++lane_geom_hit_seq_;
  if (lane_geom_hit_seq_ == 0) {
    lane_geom_hit_seq_ = 1;
  }
  lane_geom_frame_.clear();
  lane_color_warmup_valid_ = false;
  split_pause_lanes_valid_ = false;
  split_pause_lanes_hash_ = 0;
  split_pause_lanes_glow_.clear();
  // Seek/pause snapshots are remakes. Leftover pause memos or a live
  // playback boundary would make T depend on the previous pictured T.
  split_playback_state_.reset();
  split_playback_was_playing_ = false;
  split_pause_collect_valid_ = false;
  split_pause_verts_valid_ = false;
  split_pause_verts_pending_ = false;
}

void PlaybackPreviewView::request_buffer_precommit(
    const wds::chart_render::SplitEffectPrecommitPlan& plan, bool fill_now) {
  if (plan.draw_verts > draw_precommit_verts_) {
    draw_precommit_step_ = 0;
  }
  draw_precommit_verts_ = std::max(draw_precommit_verts_, plan.draw_verts);
  if (!fill_now) {
    // Edits only raise the high-water mark; idle slices touch the pages so
    // the edit frame never pays for them.
    wds::chart_render::split_effect_raise_precommit(plan);
    last_precommit_ = wds::chart_render::split_effect_precommit_stats();
    return;
  }
  // After the chart is loaded: one-shot reserve + page-touch. Startup hitch
  // is accepted; first seek should not take those faults. Not particle state.
  // WDS_PRECOMMIT=0 skips.
  last_precommit_ =
      wds::chart_render::split_effect_precommit_buffers(plan, &split_effect_collect_);
  pump_draw_precommit_slice(true);
}

void PlaybackPreviewView::pump_draw_precommit_slice(bool all) {
  if (draw_precommit_verts_ == 0) {
    return;
  }
  const std::size_t buckets = 8;
  const std::size_t verts_each = std::max<std::size_t>(draw_precommit_verts_ / buckets, 64);
  auto one = [&](DrawBatch& b) { b.precommit_capacity(buckets, verts_each); };
  DrawBatch* batches[] = {&notes_batch_,        &glow_batch_,      &split_late_batch_,
                          &split_emit_notes_,   &split_emit_glow_, &split_emit_late_};
  if (all) {
    for (DrawBatch* b : batches) {
      one(*b);
    }
    if (split_emit_parts_.size() < 8) {
      split_emit_parts_.resize(8);
    }
    for (auto& part : split_emit_parts_) {
      part.notes.precommit_capacity(4, verts_each);
      part.glow.precommit_capacity(4, verts_each);
      part.late.precommit_capacity(4, verts_each);
    }
    draw_precommit_step_ = 16;
  } else if (draw_precommit_step_ < 6) {
    one(*batches[draw_precommit_step_]);
    ++draw_precommit_step_;
  } else if (draw_precommit_step_ == 6) {
    if (split_emit_parts_.size() < 8) {
      split_emit_parts_.resize(8);
    }
    auto& part = split_emit_parts_[0];
    part.notes.precommit_capacity(4, verts_each);
    part.glow.precommit_capacity(4, verts_each);
    part.late.precommit_capacity(4, verts_each);
    ++draw_precommit_step_;
  } else if (draw_precommit_step_ == 7) {
    for (size_t i = 1; i < split_emit_parts_.size(); ++i) {
      split_emit_parts_[i].notes.precommit_capacity(4, verts_each);
      split_emit_parts_[i].glow.precommit_capacity(4, verts_each);
      split_emit_parts_[i].late.precommit_capacity(4, verts_each);
    }
    ++draw_precommit_step_;
  }
  if (lane_geom_free_.size() < 4) {
    PreviewLaneGeomSplit geom;
    geom.cells.reserve(32);
    geom.cells.resize(32);
    if (!geom.cells.empty()) {
      wds::common::touch_pages(geom.cells.data(),
                               geom.cells.size() * sizeof(PreviewLaneGeomCell));
    }
    geom.cells.clear();
    lane_geom_free_.push_back(std::move(geom));
  }
}

void PlaybackPreviewView::pump_buffer_precommit(int budget_us) {
  if (budget_us <= 0) {
    last_precommit_ = wds::chart_render::split_effect_precommit_stats();
    return;
  }
  const int used = wds::chart_render::split_effect_pump_precommit(&split_effect_collect_, budget_us);
  if (budget_us - used >= 200) {
    pump_draw_precommit_slice(false);
  }
  last_precommit_ = wds::chart_render::split_effect_precommit_stats();
}

void PlaybackPreviewView::invalidate_lane_geom_cache() {
  for (auto& kv : lane_geom_pool_) {
    recycle_lane_geom_entry(std::move(kv.second), lane_geom_free_);
  }
  lane_geom_pool_.clear();
  reset_lane_geom_time_state();
}

void PlaybackPreviewView::pump_lane_geom_preload(
    const std::vector<PreviewSplitLaneInstance>& splits, int64_t now_us,
    bool retain_colored_batch) {
  if (!ready_ || splits.empty() || wds::ui::preview_legacy_lanes_from_env()) {
    return;
  }
  const TextureInfo* plate = syuriken_plate_for(skin_, vulkan_);
  if (plate == nullptr) {
    return;
  }
  SplitLanePlateShared sh{};
  sh.geometry = &geometry_;
  sh.skin = &skin_;
  sh.plate = plate;
  sh.rgb_k = std::clamp(config_.split_line_opacity, 0.10f, 1.00f);
  std::vector<const PreviewLaneGeomSplit*> frame;
  // Chart time may be negative (delay, export −2s pad, official CSV start < 0).
  // Corners-only is retain_colored_batch=false, not now_us < 0.
  ensure_lane_geom_pool(lane_geom_pool_, lane_geom_free_, splits, sh,
                        hash_preview_view(geometry_), lane_geom_hit_seq_,
                        retain_colored_batch ? &frame : nullptr);
  size_t max_cells = 0;
  for (const auto& kv : lane_geom_pool_) {
    max_cells = std::max(max_cells, kv.second.cells.size());
  }
  if (max_cells == 0) {
    return;
  }
  if (lane_color_parts_.size() < 16) {
    lane_color_parts_.resize(16);
  }
  for (auto& part : lane_color_parts_) {
    part.reserve_quads(max_cells);
  }
  split_lanes_glow_.reserve_quads(max_cells * std::max<size_t>(splits.size(), 1));
  lane_color_warmup_.reserve_quads(max_cells * std::max<size_t>(splits.size(), 1));
  if (!retain_colored_batch || frame.empty()) {
    return;
  }
  // First playing frame would otherwise pay the full static Syuriken + first
  // DrawBatch fill inside lanes_us. Bake both on the idle/tick prebuild.
  sh.time_sec = static_cast<float>(now_us) / 1'000'000.0f;
  lane_color_warmup_.clear();
  emit_lane_geom_plates(sh, frame, lane_color_warmup_, lane_color_parts_);
  if (retain_colored_batch) {
    lane_color_warmup_hash_ =
        hash_split_lanes_frame(hash_split_lanes(splits), hash_preview_view(geometry_), now_us,
                               sh.rgb_k, plate->id);
    lane_color_warmup_valid_ = true;
  }
}

void PlaybackPreviewView::ensure_split_effect_textures() {
  const auto t0 = std::chrono::steady_clock::now();
  if (split_tex_preload_ && split_tex_preload_->decode.valid()) {
    finish_split_effect_preload_upload();
  }
  if (config_.skins_directory.empty()) {
    last_ensure_tex_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
    return;
  }
  std::string dir = config_.skins_directory;
  if (dir.back() != '/' && dir.back() != '\\') {
    dir.push_back('/');
  }
  if (!skin_.split_effect_uvs) {
    const std::string path = dir + wds::chart_render::official_split_effect_uv_atlas_relpath();
    skin_.split_effect_uvs = textures_.load_standalone_png(path);
  }
  if (!skin_.split_effect_trail) {
    const std::string path = dir + wds::chart_render::official_split_effect_trail_tex_relpath();
    skin_.split_effect_trail = textures_.load_standalone_png(path);
  }
  last_ensure_tex_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
}

bool PlaybackPreviewView::initialize(const wds::renderer::VulkanHostSurface& host, const PreviewVisualConfig& config) {
  shutdown();
  config_ = config;
  geometry_.configure(config_);
  vulkan_.set_preferred_msaa(config_.msaa_samples);
  const auto t0 = std::chrono::steady_clock::now();
  const auto ms_since = [t0]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };
  const bool vk_ok = vulkan_.create(host);
  if (!vk_ok) {
    WDS_LOG("vulkan_.create failed host_instance=%p\n",
            static_cast<void*>(host.external_instance));
    if (const char* dump = std::getenv("WDS_DUMP_VERTS"); dump != nullptr && dump[0] != '\0') {
      std::fprintf(stderr, "WDS_DUMP_VERTS: vulkan_.create failed\n");
    }
    return false;
  }
  WDS_LOG("preview vulkan_create_ms=%lld\n", static_cast<long long>(ms_since()));
  textures_.set_renderer(&vulkan_);
  const bool skin_ok = skin_.load(textures_, config_.skins_directory);
  if (!skin_ok) {
    WDS_LOG("skin_.load failed dir=%s\n", config_.skins_directory.c_str());
    if (const char* dump = std::getenv("WDS_DUMP_VERTS"); dump != nullptr && dump[0] != '\0') {
      std::fprintf(stderr, "WDS_DUMP_VERTS: skin_.load failed dir=%s\n",
                   config_.skins_directory.c_str());
    }
    shutdown();
    return false;
  }
  WDS_LOG("preview skins_load_ms=%lld\n", static_cast<long long>(ms_since()));
  {
    const int parts = std::max(wds::chart_render::split_collect_thread_count(), 1);
    if (split_emit_parts_.size() < static_cast<size_t>(parts)) {
      split_emit_parts_.resize(static_cast<size_t>(parts));
    }
    lane_color_parts_.reserve(16);
  }
  // SFX comes from Transport::audio via attach_audio().
  sfx_mono_us_ = -1;
  sfx_position_generation_ = std::numeric_limits<uint64_t>::max();
  hit_sfx_played_.clear();
  sfx_was_playing_ = false;

  geometry_.resize(vulkan_.framebuffer_width(), vulkan_.framebuffer_height());
  ready_ = true;
  request_split_effect_preload();
  {
    const std::uint32_t api = vulkan_.device_api_version();
    const std::uint32_t drv = vulkan_.device_driver_version();
    WDS_LOG("PlaybackPreviewView ready fb=%dx%d gpu='%s' api=%u.%u.%u drv=%u.%u.%u msaa=%d\n",
            vulkan_.framebuffer_width(), vulkan_.framebuffer_height(), vulkan_.device_name(),
            VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), VK_VERSION_PATCH(api),
            VK_VERSION_MAJOR(drv), VK_VERSION_MINOR(drv), VK_VERSION_PATCH(drv),
            vulkan_.active_msaa());
  }
  return true;
}

void PlaybackPreviewView::attach_audio(wds::audio::AudioEngine* audio) noexcept {
  hit_sfx_.attach(audio);
}

size_t PlaybackPreviewView::pending_sfx_sync_count() const noexcept {
  return hit_sfx_.pending_sfx_sync_count();
}

void PlaybackPreviewView::join_speculative_split_collect() {}

void PlaybackPreviewView::shutdown() {
  hit_sfx_.detach();
  sfx_mono_us_ = -1;
  sfx_position_generation_ = std::numeric_limits<uint64_t>::max();
  hit_sfx_played_.clear();
  sfx_was_playing_ = false;
  if (split_tex_preload_ && split_tex_preload_->decode.valid()) {
    try {
      (void)split_tex_preload_->decode.get();
    } catch (...) {
    }
  }
  split_tex_preload_.reset();
  invalidate_lane_geom_cache();
  split_pause_verts_pending_ = false;
  last_ensure_tex_us_ = 0;
  last_lanes_us_ = 0;
  last_lanes_geom_miss_ = 0;
  // In-flight frames still reference skin/atlas images — wait before free.
  if (vulkan_.ready()) {
    vulkan_.device_wait_idle();
  }
  textures_.clear();
  skin_ = SkinCatalog{};
  batch_.clear();
  notes_batch_.clear();
  glow_batch_.clear();
  split_lanes_glow_.clear();
  split_late_batch_.clear();
  additive_batch_.clear();
  hud_batch_.clear();
  notes_draw_indices_.clear();
  note_draw_order_.clear();
  vulkan_.destroy();
  ready_ = false;
}

void PlaybackPreviewView::set_config(const PreviewVisualConfig& config) {
  join_speculative_split_collect();
  config_ = config;
  geometry_.configure(config_);
  invalidate_lane_geom_cache();
}

void PlaybackPreviewView::apply_msaa(int samples) {
  join_speculative_split_collect();
  config_.msaa_samples = clamp_msaa_samples(samples);
  if (!ready_) {
    return;
  }
  vulkan_.apply_msaa(config_.msaa_samples);
}

void PlaybackPreviewView::resize(int framebuffer_width, int framebuffer_height) {
  if (!ready_) {
    return;
  }
  if (framebuffer_width != vulkan_.framebuffer_width() ||
      framebuffer_height != vulkan_.framebuffer_height() ||
      vulkan_.health() == wds::renderer::RendererHealth::SurfaceLost) {
    vulkan_.resize(framebuffer_width, framebuffer_height);
  }
  // Always sync stage pixels, even when the swapchain size did not change.
  // Video export temporarily resizes geometry to 1080p/2K/4K without touching
  // the live swapchain; a size-match early return would leave a tiny stage.
  geometry_.resize(vulkan_.framebuffer_width(), vulkan_.framebuffer_height());
  invalidate_lane_geom_cache();
}

void PlaybackPreviewView::render_editor_only(const wds::renderer::DrawBatch& editor_batch) {
  if (!ready_) return;
  batch_.clear();
  for (const auto& bucket : editor_batch.buckets) {
    if (bucket.vertices.empty()) continue;
    auto& dst = batch_.bucket_for(bucket.texture);
    dst.vertices.insert(dst.vertices.end(), bucket.vertices.begin(), bucket.vertices.end());
  }
  vulkan_.draw_frame(batch_, geometry_.screen(),
                     wds::chart_editor::kOfficialCameraBackgroundR,
                     wds::chart_editor::kOfficialCameraBackgroundG,
                     wds::chart_editor::kOfficialCameraBackgroundB,
                     nullptr, nullptr, nullptr, nullptr, nullptr);
}

void PlaybackPreviewView::sync_hit_sfx(const wds::chart_editor::PreviewSnapshot& snapshot) {
  update_hit_sfx(snapshot);
}

void PlaybackPreviewView::render(const PreviewSnapshot& snapshot, const DrawBatch* ui_overlay,
                                 TextureId ui_solid_texture, const DrawBatch* modal_overlay,
                                 const DrawBatch* modal_chrome, int64_t visual_lead_us,
                                 int64_t pulse_now_us) {
  if (!ready_) {
    return;
  }
  WDS_PERF_SCOPE("preview.render");
  const auto render_t0 = std::chrono::steady_clock::now();
  const auto path0 = vulkan_.path_diagnostics();
  last_ensure_tex_us_ = 0;
  last_lanes_us_ = 0;
  last_lanes_geom_miss_ = 0;
  visual_lead_us_ = visual_lead_us;
  pulse_now_us_ = pulse_now_us;
  if (snapshot.playback_state != PreviewPlaybackState::Playing) {
    join_speculative_split_collect();
  }

  // Hit SFX is armed in sync_hit_sfx() from ChartPreviewPanel::tick — before
  // Transport::start_pending_music() so POS syncs land on a still-paused stream.

  batch_.clear();
  notes_batch_.clear();
  glow_batch_.clear();
  split_late_batch_.overlay_ordered = true;
  split_late_batch_.clear();
  additive_batch_.clear();
  hud_batch_.clear();
  batch_.reserve_quads(64 + snapshot.split_lanes.size() * 8);
  notes_batch_.reserve_quads(64 + snapshot.notes.size() * 4);
  glow_batch_.reserve_quads(16384);
  split_late_batch_.reserve_quads(1024);
  additive_batch_.reserve_quads(16 + snapshot.notes.size() * 4 + 512);
  hud_batch_.reserve_quads(16);

  // Depth write is disabled; later draw calls / later draw_frame passes win.
  // DrawBatch::clear() keeps sticky per-texture bucket indices, so "merge later
  // in this function" does NOT move a texture later in the same batch — notes
  // and edit skins stay in mid_overlay (sort 10). Combo / AUTO are a later HUD
  // pass (TimingEffect 99, Combo ScreenSpaceOverlay).
  auto merge_overlay = [&](DrawBatch& dest, const DrawBatch* overlay, bool solid_pass) {
    if (overlay == nullptr) {
      return;
    }
    for (const auto& bucket : overlay->buckets) {
      if (bucket.vertices.empty()) {
        continue;
      }
      const bool is_solid = ui_solid_texture != wds::renderer::kInvalidTextureId &&
                            bucket.texture == ui_solid_texture;
      if (is_solid != solid_pass) {
        continue;
      }
      auto& dst = dest.bucket_for(bucket.texture);
      dst.vertices.insert(dst.vertices.end(), bucket.vertices.begin(), bucket.vertices.end());
    }
  };

  // Opaque ingame_bg must occupy an earlier DrawBatch bucket than the plate.
  // Depth write is off, so later buckets cover earlier ones. The plate shares
  // the UI 1×1 solid texture and must not be submitted before the background.
  draw_ingame_background(batch_);
  merge_overlay(batch_, ui_overlay, true);
  draw_stage(batch_, snapshot, ui_solid_texture);
  // Official SortingGroup 20 > Note sortingOrder 10: plate stays in glow.
  // All split-effect particles go to split_late_batch_ after notes + line
  // (OfflineRhythmPreview m_HDR=1, unclamped Custom1).
  {
    const auto lanes_t0 = std::chrono::steady_clock::now();
    draw_split_lanes(batch_, snapshot);
    last_lanes_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - lanes_t0)
                         .count();
  }
  if (!wds::ui::preview_legacy_lanes_from_env()) {
    glow_batch_.append_from(split_lanes_glow_);
  }
  draw_split_particles(snapshot, ui_solid_texture);
  draw_concurrent_lines(batch_, snapshot);

  // Pin the split plate under edit notes in this pass (same atlas as preview
  // SplitEffect). Otherwise sticky buckets can place edit split sprites last.
  if (skin_.soft_split_line) {
    notes_batch_.bucket_for(skin_.soft_split_line.id);
  } else if (skin_.split_line_1) {
    notes_batch_.bucket_for(skin_.split_line_1.id);
  } else if (skin_.split_line_2) {
    notes_batch_.bucket_for(skin_.split_line_2.id);
  }
  draw_notes(notes_batch_, snapshot);
  merge_overlay(notes_batch_, ui_overlay, false);
  // Bombs are a separate uncapped list (not kSplitEffectParticlePool). Bomb
  // SortingGroup 30 is after SplitEffect 20. The renderer stamps this batch
  // after scene-carve so dest+signed Add cannot wipe the judgeline.
  draw_hit_effects(additive_batch_, snapshot);
  // TimingEffect SpriteRenderer 99, then Combo on GameHudCanvas
  // ScreenSpaceOverlay. Both must sit after bombs and split particles.
  draw_timing_effect(hud_batch_, snapshot);
  draw_combo(hud_batch_, snapshot);

  const DrawBatch* post =
      (modal_overlay != nullptr && modal_overlay->vertex_count() > 0) ? modal_overlay : nullptr;
  const DrawBatch* post2 =
      (modal_chrome != nullptr && modal_chrome->vertex_count() > 0) ? modal_chrome : nullptr;
  // Clip additive hit FX (BomFlare etc.) to the preview panel so soft rings cannot
  // bleed into the settings/seek strip below.
  wds::renderer::VulkanRenderer::ScissorRect add_scissor{};
  const wds::renderer::VulkanRenderer::ScissorRect* add_scissor_ptr = nullptr;
  int px = 0, py = 0, pw = 0, ph = 0;
  if (geometry_.panel_framebuffer_rect(px, py, pw, ph)) {
    add_scissor = {px, py, pw, ph};
    add_scissor_ptr = &add_scissor;
  }
  size_t part_verts = 0;
  if (split_emit_direct_) {
    for (size_t i = 0; i < split_emit_direct_n_; ++i) {
      part_verts += split_emit_parts_[i].notes.vertex_count();
      part_verts += split_emit_parts_[i].glow.vertex_count();
      part_verts += split_emit_parts_[i].late.vertex_count();
    }
  }
  const size_t known_verts = batch_.vertex_count() + glow_batch_.vertex_count() +
                             notes_batch_.vertex_count() + split_late_batch_.vertex_count() +
                             additive_batch_.vertex_count() + hud_batch_.vertex_count() +
                             (post ? post->vertex_count() : 0) +
                             (post2 ? post2->vertex_count() : 0) + part_verts;
  const bool playing_now = snapshot.playback_state == PreviewPlaybackState::Playing;
  const int64_t now_us_dump = preview_now_us(snapshot, visual_lead_us_);
  const bool want_dump =
      preview_dump_wanted(now_us_dump, playing_now, static_cast<uint32_t>(known_verts));
  const bool ring_fits = known_verts * sizeof(wds::renderer::DrawVertex) <=
                         vulkan_.persistent_vertex_ring_bytes();
  if (split_emit_direct_ && !ring_fits) {
    split_late_batch_.overlay_ordered = true;
    for (size_t i = 0; i < split_emit_direct_n_; ++i) {
      split_late_batch_.append_from(split_emit_parts_[i].late);
    }
    split_emit_direct_ = false;
  }

  // Always pass the three overlay batches so GPU dump keeps sticky empty
  // buckets (WDSV NOTE/GLOW/LATE headers). Null-when-empty dropped the empty
  // plate slot on verts_memo frames and broke fresh==memo.
  const DrawBatch* mid = &notes_batch_;
  const DrawBatch* early = &glow_batch_;
  const DrawBatch* carve = &split_late_batch_;
  VulkanRenderer::DrawFrameExtras extras{};
  if (split_emit_direct_ && split_emit_direct_n_ > 0) {
    extras.carve_prefix = split_emit_carve_ptrs_.data();
    extras.carve_prefix_n = split_emit_carve_ptrs_.size();
    extras.extras_in_mapped_vb = split_emit_in_vb_;
    extras.mapped_extra_low = split_emit_vb_low_;
  }
  DrawBatch dump_scene;
  DrawBatch dump_glow;
  DrawBatch dump_notes;
  DrawBatch dump_late;
  DrawBatch dump_add;
  VulkanRenderer::DrawFrameDump dump{};
  const bool playing = playing_now;
  const int64_t now_us = now_us_dump;
  if (want_dump) {
    dump.scene = &dump_scene;
    dump.glow = &dump_glow;
    dump.notes = &dump_notes;
    dump.late = &dump_late;
    dump.additive = &dump_add;
  }
  {
    WDS_PERF_SCOPE("preview.draw_frame");
    vulkan_.draw_frame(batch_, geometry_.screen(),
                       wds::chart_editor::kOfficialCameraBackgroundR,
                       wds::chart_editor::kOfficialCameraBackgroundG,
                       wds::chart_editor::kOfficialCameraBackgroundB, &additive_batch_, post,
                       post2, add_scissor_ptr, mid, early, carve, &hud_batch_,
                       split_emit_direct_ ? &extras : nullptr, want_dump ? &dump : nullptr);
  }
  if (want_dump) {
    dump_preview_draw_batches_if_requested(dump_scene, dump_glow, dump_notes, dump_late, dump_add,
                                           now_us, playing, split_frame_diag_.verts_memo != 0,
                                           &textures_);
  }
  persist_split_pause_vert_memo();
  if (split_emit_in_vb_) {
    for (size_t i = 0; i < split_emit_direct_n_ && i < split_emit_parts_.size(); ++i) {
      split_emit_parts_[i].notes.unbind_external();
      split_emit_parts_[i].glow.unbind_external();
      split_emit_parts_[i].late.unbind_external();
    }
    split_emit_in_vb_ = false;
    split_emit_vb_low_ = 0;
  }
  const auto path1 = vulkan_.path_diagnostics();
  auto path_delta = [](int64_t now, int64_t then) {
    return now >= then ? now - then : 0;
  };
  split_frame_diag_.lanes_us = last_lanes_us_;
  split_frame_diag_.lanes_geom_miss = last_lanes_geom_miss_;
  split_frame_diag_.tex_us =
      last_ensure_tex_us_ +
      path_delta(path1.total.create_texture_rgba_us, path0.total.create_texture_rgba_us) +
      path_delta(path1.total.descriptor_allocate_us, path0.total.descriptor_allocate_us);
  split_frame_diag_.upload_us =
      path_delta(path1.total.upload_submit_us, path0.total.upload_submit_us) +
      path_delta(path1.total.upload_fence_wait_us, path0.total.upload_fence_wait_us) +
      path_delta(path1.total.upload_fence_reap_us, path0.total.upload_fence_reap_us);
  split_frame_diag_.submit_us = vulkan_.last_gpu_submit_us();
  split_frame_diag_.total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - render_t0)
                                  .count();
}

void PlaybackPreviewView::draw_ingame_background(DrawBatch& batch) {
  if (!skin_.ingame_background) {
    return;
  }
  // Official SpriteRendererScreenFitter cover-fits Texture2D/ingame_bg to the
  // 16:9 camera, not the possibly-taller preview dock. Content is that camera.
  const auto& p = geometry_.content();
  const Quad bg{{p.l, p.b}, {p.l, p.t}, {p.r, p.t}, {p.r, p.b}};
  const auto& tex = skin_.ingame_background;
  const float tw = std::max(1.0f, static_cast<float>(tex.width));
  const float th = std::max(1.0f, static_cast<float>(tex.height));
  const float tex_aspect = tw / th;
  const float panel_aspect = std::max(p.w, 1e-6f) / std::max(p.h, 1e-6f);
  float u0 = tex.u0;
  float u1 = tex.u1;
  float v0 = tex.v0;
  float v1 = tex.v1;
  if (tex_aspect > panel_aspect) {
    const float visible = panel_aspect / tex_aspect;
    const float mid = 0.5f * (tex.u0 + tex.u1);
    const float half = 0.5f * (tex.u1 - tex.u0) * visible;
    u0 = mid - half;
    u1 = mid + half;
  } else {
    const float visible = tex_aspect / panel_aspect;
    const float mid = 0.5f * (tex.v0 + tex.v1);
    const float half = 0.5f * (tex.v1 - tex.v0) * visible;
    v0 = mid - half;
    v1 = mid + half;
  }
  batch.add_quad(tex.id, bg, -0.98f, 1.0f, u0, v0, u1, v1);
}

void PlaybackPreviewView::draw_stage(DrawBatch& batch, const PreviewSnapshot& snapshot,
                                     TextureId solid_texture) {
  // BGShadow (−9) / BG_Lane (−8) / borders / judgeline. ingame_bg (−10) is
  // submitted earlier so its bucket is behind these (Vulkan later-bucket-wins).
  // Editor: default lane borders stay visible; active splits fade them out via
  // stage_cover_alpha (OpenWDS LaneGroup BG_LaneBorder starts at α=0 and fades
  // in only during splits — intentionally not mirrored here; user 2026-10-15).
  float border_cover = 1.0f;
  for (const auto& split : snapshot.split_lanes) {
    if (split.should_show && split.split_count > 0) {
      border_cover = std::min(border_cover, split.stage_cover_alpha);
    }
  }

  if (skin_.bg_shadow) {
    const float sx = wds::chart_editor::kOfficialBgShadowScaleX;
    const float sy = wds::chart_editor::kOfficialBgShadowScaleY;
    const float scales[4][2] = {{sx, sy}, {-sx, sy}, {sx, -sy}, {-sx, -sy}};
    for (const auto& s : scales) {
      batch.add_sprite(skin_.bg_shadow, geometry_.bg_shadow_quad(s[0], s[1]), -0.94f,
                       wds::chart_editor::kOfficialBgShadowColorA,
                       wds::chart_editor::kOfficialBgShadowColorR,
                       wds::chart_editor::kOfficialBgShadowColorG,
                       wds::chart_editor::kOfficialBgShadowColorB);
    }
  }

  TextureInfo plate{};
  if (solid_texture != wds::renderer::kInvalidTextureId) {
    plate = TextureInfo{solid_texture, 1, 1, 0.0f, 0.0f, 1.0f, 1.0f};
  } else if (skin_.soft_split_line) {
    plate = skin_.soft_split_line;
  }
  // Official BG_Lane: prefab a=0.8 × LaneAlphaValue/100 (default 80 → 0.64).
  if (plate) {
    batch.add_sprite(plate, geometry_.stage_quad(), -0.9f,
                     wds::chart_editor::kOfficialBgLaneAlpha, 0.0f, 0.0f, 0.0f);
  }
  const int split_opacity_pct = static_cast<int>(
      std::lround(static_cast<double>(config_.split_line_opacity) * 100.0));
  const float border_a =
      wds::chart_editor::official_lane_border_alpha(split_opacity_pct) * border_cover;
  if (border_a > 0.001f) {
    if (skin_.lane_border) {
      batch.add_sprite(skin_.lane_border, geometry_.stage_quad(), -0.88f, border_a);
    } else if (plate) {
      for (int32_t v = 0; v < wds::chart_editor::kOfficialVisualBorderEdgeCount; ++v) {
        const int32_t e =
            wds::chart_editor::official_visual_border_edge_index(v, config_.lane_count);
        batch.add_sprite(plate, geometry_.lane_border_quad(e), -0.88f, border_a);
      }
    }
  }

  const auto& j = geometry_.judgeline();
  Quad jq{{j.lb_x, j.lb_y}, {j.lt_x, j.lt_y}, {j.rt_x, j.rt_y}, {j.rb_x, j.rb_y}};
  batch.add_sprite(skin_.judgeline, jq, -0.7f, 1.0f);

  draw_hidden_line(batch);
}

void PlaybackPreviewView::draw_hidden_line(DrawBatch& batch) {
  if (!skin_.hidden_line) {
    return;
  }
  const float p = geometry_.hidden_line_center_percent();
  // Official StartLine SpriteRenderer.color.a = 1; alpha lives in the sprite
  // (core ~0.80, edges ~0.50). Do not pre-multiply a second 0.45.
  const float a = std::clamp(config_.hidden_line_alpha, 0.0f, 1.0f);
  batch.add_sprite(skin_.hidden_line, geometry_.hidden_line_quad(p), -0.55f, a);
}

float PlaybackPreviewView::spawn_clip_percent() const noexcept {
  // Official LaneMask bottom = GetNoteVisiblePositionY. Not the StartLine
  // sprite center (+0.4) and not the plate's judgeline-side half.
  return geometry_.lane_mask_bottom_percent();
}

void PlaybackPreviewView::draw_split_lanes(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  WDS_PERF_SCOPE("preview.split_lanes");
  // Split line X is Main / SetLineX, not Unity World. LiveParticle.world_x
  // is the same axis. Do not multiply LaneGroup or line height 27 again.
  (void)batch;
  const bool legacy_lanes = wds::ui::preview_legacy_lanes_from_env();
  if (!legacy_lanes) {
    split_lanes_glow_.clear();
  }
  if (snapshot.split_lanes.empty()) {
    split_pause_lanes_valid_ = false;
    lane_geom_frame_.clear();
    last_lanes_geom_miss_ = 0;
    return;
  }

  const TextureInfo* plate = syuriken_plate_for(skin_, vulkan_);
  if (plate == nullptr) {
    split_pause_lanes_valid_ = false;
    lane_geom_frame_.clear();
    last_lanes_geom_miss_ = 0;
    return;
  }

  auto split_visible = [](const PreviewSplitLaneInstance& s) {
    return s.should_show && s.split_count > 0 &&
           (s.split_percent_end - s.split_percent_start) >= 1e-4f;
  };

  const bool playing = snapshot.playback_state == PreviewPlaybackState::Playing;
  const int64_t now_us = preview_now_us(snapshot, visual_lead_us_);
  const float time_sec = static_cast<float>(now_us) / 1'000'000.0f;
  const float rgb_k = std::clamp(config_.split_line_opacity, 0.10f, 1.00f);
  // Pulse 43.06 Hz is already past Nyquist at 60 fps (−0.718 cyc/frame).
  // 1 ms of Δt ≈ 0.043 cycle and flips the nearest-neighbor alias; the note
  // visual lead's present-latency extra (0–16.7 ms) must not feed this clock.
  // hop / darkband-v2 2026-10-17: BASS position std 0.12 ms, 0 wrap flips;
  // extra-inclusive time_sec std 2.39 ms, 22/540 flips. Not 2022.3.
  // textured_quad.frag kSyuriken shares this uniform.
  vulkan_.set_syuriken_uniforms(static_cast<float>(pulse_now_us_) / 1'000'000.0f, plate->id);

  DrawBatch& lane_out = legacy_lanes ? glow_batch_ : split_lanes_glow_;

  if (legacy_lanes) {
    WDS_PERF_COUNT("preview.split_memo.lanes", 0);
    SplitLanePlateShared sh{};
    sh.geometry = &geometry_;
    sh.skin = &skin_;
    sh.plate = plate;
    sh.time_sec = time_sec;
    sh.rgb_k = rgb_k;
    append_all_split_lanes_legacy_plates(sh, snapshot.split_lanes, lane_out);
    return;
  }

  const uint64_t splits_hash = hash_split_lanes(snapshot.split_lanes);
  const uint64_t view_hash = hash_preview_view(geometry_);
  const uint64_t lanes_frame_hash =
      hash_split_lanes_frame(splits_hash, view_hash, now_us, rgb_k, plate->id);
  const bool lanes_memo = !playing && split_pause_lanes_valid_ &&
                          split_pause_revision_ == snapshot.revision &&
                          split_pause_lanes_hash_ == lanes_frame_hash;
  if (lanes_memo) {
    split_lanes_glow_.append_from(split_pause_lanes_glow_);
    last_lanes_geom_miss_ = 0;
    WDS_PERF_COUNT("preview.split_memo.lanes", 1);
    return;
  }
  WDS_PERF_COUNT("preview.split_memo.lanes", 0);

  size_t cells = 0;
  for (const auto& split : snapshot.split_lanes) {
    if (split_visible(split)) {
      const int32_t line_n = wds::chart_editor::official_split_line_x_count(split.split_count);
      cells += static_cast<size_t>(std::max(line_n, 0));
    }
  }
  lane_out.bucket_for(plate->id);
  lane_out.reserve_quads(cells);

  const size_t split_n = snapshot.split_lanes.size();
  SplitLanePlateShared sh{};
  sh.geometry = &geometry_;
  sh.skin = &skin_;
  sh.plate = plate;
  sh.time_sec = time_sec;
  sh.rgb_k = rgb_k;

  ++lane_geom_hit_seq_;
  if (lane_geom_hit_seq_ == 0) {
    lane_geom_hit_seq_ = 1;
  }
  last_lanes_geom_miss_ = static_cast<int>(ensure_lane_geom_pool(
      lane_geom_pool_, lane_geom_free_, snapshot.split_lanes, sh, view_hash, lane_geom_hit_seq_,
      &lane_geom_frame_));

  const bool warmup_hit =
      lane_color_warmup_valid_ && lane_color_warmup_hash_ == lanes_frame_hash;
  const bool lanes_selftest =
      wds::ui::frame_diag_env_enabled(std::getenv("WDS_PREVIEW_VERTS_SELFTEST"));
  static bool lanes_selftest_done = false;
  if (lanes_selftest && !lanes_selftest_done && !lanes_memo && split_n > 0) {
    lanes_selftest_done = true;
    DrawBatch leg_lanes;
    DrawBatch cache_lanes;
    append_all_split_lanes_legacy_plates(sh, snapshot.split_lanes, leg_lanes);
    if (warmup_hit) {
      cache_lanes.append_from(lane_color_warmup_);
    } else {
      emit_lane_geom_plates(sh, lane_geom_frame_, cache_lanes, lane_color_parts_);
    }
    const bool lanes_ok = draw_batches_byte_equal(leg_lanes, cache_lanes);
    std::fprintf(stderr, "WDS_PREVIEW_VERTS_SELFTEST lanes=%d (verts %u vs %u)\n", lanes_ok ? 1 : 0,
                 draw_batch_vertex_count(leg_lanes), draw_batch_vertex_count(cache_lanes));
  }

  if (warmup_hit) {
    lane_out.append_from(lane_color_warmup_);
    lane_color_warmup_valid_ = false;
    if (!playing) {
      split_pause_lanes_glow_ = split_lanes_glow_;
      split_pause_lanes_valid_ = true;
      split_pause_lanes_hash_ = lanes_frame_hash;
      split_pause_revision_ = snapshot.revision;
    } else {
      split_pause_lanes_valid_ = false;
    }
    return;
  }

  emit_lane_geom_plates(sh, lane_geom_frame_, lane_out, lane_color_parts_);

  if (!playing) {
    split_pause_lanes_glow_ = split_lanes_glow_;
    split_pause_lanes_valid_ = true;
    split_pause_lanes_hash_ = lanes_frame_hash;
    split_pause_revision_ = snapshot.revision;
  } else {
    split_pause_lanes_valid_ = false;
  }
}

void PlaybackPreviewView::persist_split_pause_vert_memo() {
  if (!split_pause_verts_pending_) {
    return;
  }
  split_pause_verts_pending_ = false;
  split_pause_notes_.clear();
  split_pause_glow_.clear();
  split_pause_late_.clear();
  split_pause_late_.overlay_ordered = true;
  if (split_emit_direct_n_ > 0) {
    for (size_t i = 0; i < split_emit_direct_n_; ++i) {
      split_pause_late_.append_from(split_emit_parts_[i].late);
    }
  } else {
    split_pause_late_.append_from(split_emit_late_);
  }
  split_pause_verts_valid_ = true;
}

void PlaybackPreviewView::draw_split_particles(const PreviewSnapshot& snapshot,
                                               TextureId solid_texture) {
  WDS_PERF_SCOPE("preview.split_particles");
  split_emit_direct_ = false;
  split_emit_direct_n_ = 0;
  split_emit_in_vb_ = false;
  split_emit_vb_low_ = 0;
  const int64_t now_us = preview_now_us(snapshot, visual_lead_us_);
  const double now = static_cast<double>(now_us) / 1'000'000.0;
  const bool playing = snapshot.playback_state == PreviewPlaybackState::Playing;
  int cache_cleared = 0;
  if (snapshot.revision != split_playback_revision_) {
    split_playback_state_.reset();
    split_playback_was_playing_ = false;
    split_playback_revision_ = snapshot.revision;
    split_pause_collect_valid_ = false;
    split_pause_verts_valid_ = false;
    split_pause_verts_pending_ = false;
    split_pause_lanes_valid_ = false;
    // Pose-cache keys already include color/line/start_ms/seed gen. Bumping
    // the epoch on every document revision (or a false revision churn) forces
    // a full integrate. Only drop checkpoints when the seed session resets.
  }
  if (!playing) {
    // A pause/seek snapshot is intentionally stateless.  The next Play starts
    // a fresh first-sample boundary in the caller-owned adapter.
    if (split_playback_was_playing_) {
      split_playback_state_.reset();
    }
    split_playback_was_playing_ = false;
  } else {
    split_pause_collect_valid_ = false;
    split_pause_verts_valid_ = false;
    split_pause_verts_pending_ = false;
    split_pause_lanes_valid_ = false;
    if (!split_playback_was_playing_) {
      split_playback_state_.reset();
      split_playback_was_playing_ = true;
    } else if (std::isfinite(split_playback_state_.last_now_sec) &&
               now + 1e-9 < split_playback_state_.last_now_sec) {
      // Explicit timeline seek while still marked Playing.
      split_playback_state_.reset();
    }
  }
  if (snapshot.split_lanes.empty()) {
    split_pause_collect_valid_ = false;
    split_pause_verts_valid_ = false;
    split_pause_verts_pending_ = false;
    split_frame_diag_ = {};
    split_frame_diag_.revision = snapshot.revision;
    split_frame_diag_.now_us = now_us;
    split_frame_diag_.playing = playing ? 1 : 0;
    return;
  }
  ensure_split_effect_textures();

  const int quality = config_.split_effect_quality;
  wds::chart_render::SplitEffectFrameCollect& collected = split_effect_collect_;
  const uint32_t split_verts0 = notes_batch_.vertex_count() + glow_batch_.vertex_count() +
                                split_late_batch_.vertex_count();
  int split_particles_drawn = 0;
  // Official SplitEffectSyurikenParticleAlpha/Add _MainTex is unbound
  // (materials.bundle pathId 0). OpenWDS shader default is "white".
  // TSA sprite null (catalog 11570_2 Cube) must sample that white, not
  // impersonate a circular soft_disk on each Primitive Cube face.
  TextureInfo unbound_main_tex{};
  if (solid_texture != wds::renderer::kInvalidTextureId) {
    unbound_main_tex = TextureInfo{solid_texture, 1, 1, 0.0f, 0.0f, 1.0f, 1.0f};
  }
  // Official SplitEffectSyurikenTrail _MainTex is BombEffectTrail. Do not
  // impersonate it with the homemade split-line plate or a procedural disk.
  const TextureInfo* trail_tex = skin_.split_effect_trail ? &skin_.split_effect_trail : nullptr;
  const float atlas_w = skin_.split_effect_uvs
                            ? std::max(1.0f, static_cast<float>(skin_.split_effect_uvs.width))
                            : 1.0f;
  const float atlas_h = skin_.split_effect_uvs
                            ? std::max(1.0f, static_cast<float>(skin_.split_effect_uvs.height))
                            : 1.0f;

  auto sprite_for = [&](const wds::chart_render::SplitEffectLiveParticle& p) -> TextureInfo {
    const bool cube = (p.flags & wds::chart_render::kSplitEffectFlagCubeMesh) != 0;
    bool present = false;
    int32_t x = 0;
    int32_t y = 0;
    if (!cube && wds::chart_render::official_split_effect_uv_gpu_px(p.uv_name_index, x, y,
                                                                   present)) {
      if (!present || !skin_.split_effect_uvs) {
        // Named sprite missing from the official atlas. Leave empty — do
        // not impersonate SenceFx_301 / 10250_* / TapEffect with soft_disk.
        return TextureInfo{};
      }
      const auto* cat = wds::chart_render::official_split_effect_uv_tile(p.uv_name_index);
      if (cat == nullptr || cat->w == 0 || cat->h == 0) {
        return TextureInfo{};
      }
      TextureInfo tile = skin_.split_effect_uvs;
      // Official TSA maps textureRect onto UV 0–1. A 0.5px inset from the
      // tile edge sampled the 1px gutter, so near-camera petals showed a
      // transparent ring instead of the authored halo.
      const float gutter = static_cast<float>(wds::chart_render::kSplitEffectUvGutterPx);
      const float tw = static_cast<float>(cat->w);
      const float th = static_cast<float>(cat->h);
      tile.u0 = (static_cast<float>(x) + gutter) / atlas_w;
      tile.v0 = (static_cast<float>(y) + gutter) / atlas_h;
      tile.u1 = (static_cast<float>(x) + tw - gutter) / atlas_w;
      tile.v1 = (static_cast<float>(y) + th - gutter) / atlas_h;
      return tile;
    }
    return unbound_main_tex;
  };

  // Unity Camera.nearClipPlane clips ParticleSystemRenderer triangles.
  // Preview is a 2D overlay, so clip the 3D polygon against the near plane
  // in view space, then project; the 2D GPU clips the viewport. Projecting a
  // behind-camera corner with max(z, 1e-4) explodes NDC across the stage.
  const float view_aspect =
      std::max(geometry_.content().w / std::max(geometry_.content().h, 1e-6f), 1e-4f);
  const auto& content_bounds = geometry_.content();
  const float view_zoom = std::max(geometry_.config().view_zoom, 1e-4f);
  const float proj_cx = (content_bounds.l + content_bounds.r) * 0.5f;
  const float proj_cy = (content_bounds.t + content_bounds.b) * 0.5f;
  const float proj_hx = content_bounds.w * 0.5f * view_zoom;
  const float proj_hy = content_bounds.h * 0.5f * view_zoom;
  const float tilt_rad = wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
  const float tilt_c = std::cos(tilt_rad);
  const float tilt_s = std::sin(tilt_rad);
  const float focal = wds::chart_editor::official_camera_focal();
  const float aspect_a = std::max(view_aspect, 1e-4f);
  const float focal_over_aspect = focal / aspect_a;
  const float cam_group_z = wds::chart_editor::kOfficialLaneGroupZ;
  const float near_clip = wds::chart_editor::kOfficialCameraNearClip;
  size_t dest_reserve_chunk = 0;
  auto camera_z_fast = [&](float y, float z) { return cam_group_z + y * tilt_s + z * tilt_c; };
  auto project_main_fast = [&](float x, float y, float z) {
    const float wy = y * tilt_c - z * tilt_s;
    const float wz = camera_z_fast(y, z);
    const float zz = std::max(wz, 1e-4f);
    const float ndc_x = (x / zz) * focal_over_aspect;
    const float ndc_y = (wy / zz) * focal;
    return wds::renderer::Vec2{proj_cx + ndc_x * proj_hx, proj_cy + ndc_y * proj_hy};
  };
  auto poly_in_front_of_near = [&](const float* qy, const float* qz, int n) {
    for (int i = 0; i < n; ++i) {
      if (camera_z_fast(qy[i], qz[i]) < near_clip) {
        return false;
      }
    }
    return true;
  };
  // View-clip a camera-facing strip quad and keep the authored corner UVs.
  // add_sprite remaps a UV rect onto the leftover polygon, so a near-plane
  // cut sampled the sprite's transparent edge (OpenWDS ParticleAlpha
  // clip(a-0.01) then discarded it — hollow + looks thinner than Start Size).
  // Unity 2022.3 interpolates vertex TEXCOORD across the triangle.
  auto emit_clipped_uv_poly = [&](DrawBatch& dest, const TextureInfo& tex, const float* qx,
                                 const float* qy, const float* qz, const float* qu, const float* qv,
                                 int n, float a, float r, float g, float b,
                                 wds::renderer::IndexedStripShare* share, bool allow_share) {
    if (!tex || n < 3) {
      if (share != nullptr) {
        share->valid = false;
      }
      return;
    }
    const float* sx = qx;
    const float* sy = qy;
    const float* sz = qz;
    const float* su = qu;
    const float* sv = qv;
    int cn = n;
    float cx[wds::chart_editor::kOfficialViewClipMaxVerts];
    float cy[wds::chart_editor::kOfficialViewClipMaxVerts];
    float cz[wds::chart_editor::kOfficialViewClipMaxVerts];
    float cu[wds::chart_editor::kOfficialViewClipMaxVerts];
    float cv[wds::chart_editor::kOfficialViewClipMaxVerts];
    const bool unclipped = poly_in_front_of_near(qy, qz, n);
    if (!unclipped) {
      // Near plane only (official_main_clip_poly_near_uv). Side planes of
      // five-plane SH ate the leftover of a tall Local Billboard that Unity
      // still rasterizes (ALIGN2-N6 10461 / 11332: bottom viewport.z<0).
      cn = wds::chart_editor::official_main_clip_poly_near_uv(qx, qy, qz, qu, qv, n, cx, cy, cz,
                                                             cu, cv);
      if (cn < 3) {
        if (share != nullptr) {
          share->valid = false;
        }
        return;
      }
      sx = cx;
      sy = cy;
      sz = cz;
      su = cu;
      sv = cv;
    }
    auto proj = [&](int i) { return project_main_fast(sx[i], sy[i], sz[i]); };
    auto persp_q = [&](int i) { return 1.0f / std::max(camera_z_fast(sy[i], sz[i]), 1e-4f); };
    auto write_vert = [&](wds::renderer::DrawVertex& vert, int i) {
      const wds::renderer::Vec2 p = proj(i);
      const float qq = std::max(persp_q(i), 1e-6f);
      vert.x = p.x;
      vert.y = p.y;
      vert.z = -0.54f;
      vert.u = su[i] * qq;
      vert.v = sv[i] * qq;
      vert.q = qq;
      vert.r = r;
      vert.g = g;
      vert.b = b;
      vert.a = a;
    };
    if (wds::renderer::split_geometry_indexed_from_env()) {
      const bool can_share =
          allow_share && unclipped && cn == 4 && share != nullptr && share->valid;
      const uint32_t vn = can_share ? 2u : static_cast<uint32_t>(cn);
      const uint32_t in = can_share ? 6u : 3u * (static_cast<uint32_t>(cn) - 2u);
      const wds::renderer::MeshWrite w = dest.append_mesh(tex.id, false, vn, in);
      if (w.v == nullptr || w.i == nullptr) {
        if (share != nullptr) {
          share->valid = false;
        }
        return;
      }
      if (can_share) {
        write_vert(w.v[0], 2);
        write_vert(w.v[1], 3);
        w.i[0] = share->ia;
        w.i[1] = share->ib;
        w.i[2] = w.vbase;
        w.i[3] = share->ia;
        w.i[4] = w.vbase;
        w.i[5] = w.vbase + 1;
        share->ia = w.vbase + 1;
        share->ib = w.vbase;
        share->valid = true;
        return;
      }
      uint32_t iw = 0;
      for (int i = 0; i < cn; ++i) {
        write_vert(w.v[i], i);
        if (i >= 2) {
          w.i[iw++] = w.vbase;
          w.i[iw++] = w.vbase + static_cast<uint32_t>(i - 1);
          w.i[iw++] = w.vbase + static_cast<uint32_t>(i);
        }
      }
      if (share != nullptr && unclipped && cn == 4) {
        share->ia = w.vbase + 3;
        share->ib = w.vbase + 2;
        share->valid = true;
      } else if (share != nullptr) {
        share->valid = false;
      }
      return;
    }
    wds::renderer::DrawVertex uniq[wds::chart_editor::kOfficialViewClipMaxVerts];
    for (int i = 0; i < cn; ++i) {
      write_vert(uniq[i], i);
    }
    dest.add_uv_fan(tex.id, false, uniq, static_cast<uint32_t>(cn), nullptr, false);
  };
  auto emit_clipped_textured_quad =
      [&](DrawBatch& dest, const TextureInfo& tex, const float* qx, const float* qy,
          const float* qz, float u0, float v0, float u1, float v1, float a, float r, float g,
          float b, wds::renderer::IndexedStripShare* share, bool allow_share) {
        const float qu[4] = {u0, u1, u1, u0};
        const float qv[4] = {v0, v0, v1, v1};
        emit_clipped_uv_poly(dest, tex, qx, qy, qz, qu, qv, 4, a, r, g, b, share, allow_share);
      };
  // live dump psr-flip-v1 hop (not 2022.3). Billboard + Stretch share this
  // atlas-corner swap. Mesh cube/ring and Trails keep authored UVs.
  auto emit_psr_flipped_quad =
      [&](DrawBatch& dest, const TextureInfo& tex,
          const wds::chart_render::SplitEffectLiveParticle& p, const float* qx, const float* qy,
          const float* qz) {
        float u0 = tex.u0;
        float v0 = tex.v0;
        float u1 = tex.u1;
        float v1 = tex.v1;
        wds::chart_render::official_split_effect_apply_psr_flip_uv(p.flip_u, p.flip_v, u0, v0, u1,
                                                                   v1);
        emit_clipped_textured_quad(dest, tex, qx, qy, qz, u0, v0, u1, v1, p.a, p.r, p.g, p.b,
                                  nullptr, false);
      };
  // Trail vertices are Main xyz (integrate_pose). Perp is official_main_trail_perp
  // (Trails View Cross). Ribbon uses the same inter-particle 3D strip as PTrail.
  // Do not write this Cross into Stretch.
  // in LaneGroup, then official_main_clip_poly_view_uv, then project_main.
  // Not Trails Cross on Stretch, and not Unity Screen pixels.
  const bool legacy_scratch =
      wds::ui::preview_legacy_lanes_from_env() || wds::ui::preview_legacy_emit_from_env();
  auto draw_clipped_trail_strip =
      [&](DrawBatch& dest, const TextureInfo& tex, const float* xs, const float* ys,
          const float* zs, const float* world_w, const float* v_along_src, const uint16_t* verts,
          int n, float a, float r, float g, float b, const float* va, const float* vr,
          const float* vg, const float* vb) {
        if (n < 2 || !tex) {
          return;
        }
        auto run_strip = [&](int i0, int i1) {
          const int m = i1 - i0;
          if (m < 2) {
            return;
          }
          std::vector<float> lx_legacy;
          std::vector<float> ly_legacy;
          std::vector<float> lz_legacy;
          std::vector<float> rx_legacy;
          std::vector<float> ry_legacy;
          std::vector<float> rz_legacy;
          if (legacy_scratch) {
            lx_legacy.resize(static_cast<size_t>(m));
            ly_legacy.resize(static_cast<size_t>(m));
            lz_legacy.resize(static_cast<size_t>(m));
            rx_legacy.resize(static_cast<size_t>(m));
            ry_legacy.resize(static_cast<size_t>(m));
            rz_legacy.resize(static_cast<size_t>(m));
          }
          TrailDrawScratch& run_ts = trail_draw_scratch();
          if (!legacy_scratch) {
            run_ts.resize_run(m);
          }
          const bool indexed = wds::renderer::split_geometry_indexed_from_env();
          size_t strip_reserve =
              static_cast<size_t>(std::max(m - 1, 1)) * (indexed ? 4u : 6u);
          if (dest.vertex_count() == 0 && dest_reserve_chunk > strip_reserve) {
            strip_reserve = dest_reserve_chunk;
          }
          dest.reserve_bucket_verts(tex.id, strip_reserve);
          if (indexed) {
            dest.reserve_bucket_indices(tex.id,
                                        static_cast<size_t>(std::max(m - 1, 1)) * 6u);
          }
          wds::renderer::IndexedStripShare share{};
          float prev_px = 0.0f, prev_py = 0.0f, prev_pz = 0.0f;
          // official_main_trail_perp: Cross(camera forward, tangent). Forward is
          // constant (LaneGroup Rx 60°), so hoist sin/cos out of the vertex loop.
          const float cam_fx = 0.0f;
          const float cam_fy = tilt_s;
          const float cam_fz = tilt_c;
          for (int k = 0; k < m; ++k) {
            const int i = i0 + k;
            float dx = 0.0f, dy = 1.0f, dz = 0.0f;
            if (k + 1 < m && k > 0) {
              dx = xs[i + 1] - xs[i - 1];
              dy = ys[i + 1] - ys[i - 1];
              dz = zs[i + 1] - zs[i - 1];
            } else if (k + 1 < m) {
              dx = xs[i + 1] - xs[i];
              dy = ys[i + 1] - ys[i];
              dz = zs[i + 1] - zs[i];
            } else {
              dx = xs[i] - xs[i - 1];
              dy = ys[i] - ys[i - 1];
              dz = zs[i] - zs[i - 1];
            }
            float px = cam_fy * dz - cam_fz * dy;
            float py = cam_fz * dx - cam_fx * dz;
            float pz = cam_fx * dy - cam_fy * dx;
            const float plen = std::sqrt(px * px + py * py + pz * pz);
            if (plen < 1e-8f) {
              px = 1.0f;
              py = 0.0f;
              pz = 0.0f;
            } else {
              px /= plen;
              py /= plen;
              pz /= plen;
            }
            // live dump v21: Unity BakeTrailsMesh perpFlips=0. Cross can flip
            // at a near-parallel tangent and pinch the strip (looks like
            // broken lightning). Keep the same side as the previous vertex.
            if (k > 0 && (prev_px * px + prev_py * py + prev_pz * pz) < 0.0f) {
              px = -px;
              py = -py;
              pz = -pz;
            }
            prev_px = px;
            prev_py = py;
            prev_pz = pz;
            const float h = world_w[i] * 0.5f;
            if (legacy_scratch) {
              lx_legacy[static_cast<size_t>(k)] = xs[i] + px * h;
              ly_legacy[static_cast<size_t>(k)] = ys[i] + py * h;
              lz_legacy[static_cast<size_t>(k)] = zs[i] + pz * h;
              rx_legacy[static_cast<size_t>(k)] = xs[i] - px * h;
              ry_legacy[static_cast<size_t>(k)] = ys[i] - py * h;
              rz_legacy[static_cast<size_t>(k)] = zs[i] - pz * h;
            } else {
              run_ts.lx[static_cast<size_t>(k)] = xs[i] + px * h;
              run_ts.ly[static_cast<size_t>(k)] = ys[i] + py * h;
              run_ts.lz[static_cast<size_t>(k)] = zs[i] + pz * h;
              run_ts.rx[static_cast<size_t>(k)] = xs[i] - px * h;
              run_ts.ry[static_cast<size_t>(k)] = ys[i] - py * h;
              run_ts.rz[static_cast<size_t>(k)] = zs[i] - pz * h;
            }
          }
          for (int k = 0; k + 1 < m; ++k) {
            const int i = i0 + k;
            if (world_w[i] < 1e-5f && world_w[i + 1] < 1e-5f) {
              continue;
            }
            float qx[4], qy[4], qz[4];
            if (legacy_scratch) {
              qx[0] = lx_legacy[static_cast<size_t>(k)];
              qx[1] = rx_legacy[static_cast<size_t>(k)];
              qx[2] = rx_legacy[static_cast<size_t>(k + 1)];
              qx[3] = lx_legacy[static_cast<size_t>(k + 1)];
              qy[0] = ly_legacy[static_cast<size_t>(k)];
              qy[1] = ry_legacy[static_cast<size_t>(k)];
              qy[2] = ry_legacy[static_cast<size_t>(k + 1)];
              qy[3] = ly_legacy[static_cast<size_t>(k + 1)];
              qz[0] = lz_legacy[static_cast<size_t>(k)];
              qz[1] = rz_legacy[static_cast<size_t>(k)];
              qz[2] = rz_legacy[static_cast<size_t>(k + 1)];
              qz[3] = lz_legacy[static_cast<size_t>(k + 1)];
            } else {
              qx[0] = run_ts.lx[static_cast<size_t>(k)];
              qx[1] = run_ts.rx[static_cast<size_t>(k)];
              qx[2] = run_ts.rx[static_cast<size_t>(k + 1)];
              qx[3] = run_ts.lx[static_cast<size_t>(k + 1)];
              qy[0] = run_ts.ly[static_cast<size_t>(k)];
              qy[1] = run_ts.ry[static_cast<size_t>(k)];
              qy[2] = run_ts.ry[static_cast<size_t>(k + 1)];
              qy[3] = run_ts.ly[static_cast<size_t>(k + 1)];
              qz[0] = run_ts.lz[static_cast<size_t>(k)];
              qz[1] = run_ts.rz[static_cast<size_t>(k)];
              qz[2] = run_ts.rz[static_cast<size_t>(k + 1)];
              qz[3] = run_ts.lz[static_cast<size_t>(k + 1)];
            }
            const float sa = va != nullptr ? 0.5f * (va[i] + va[i + 1]) : a;
            const float sr = vr != nullptr ? 0.5f * (vr[i] + vr[i + 1]) : r;
            const float sg = vg != nullptr ? 0.5f * (vg[i] + vg[i + 1]) : g;
            const float sb = vb != nullptr ? 0.5f * (vb[i] + vb[i + 1]) : b;
            // Joint L_k/R_k are the same source points as the previous far
            // edge. Segment colour is avg(endpoint_k, endpoint_k+1), so the
            // two quads share colour iff the outer endpoints match.
            const bool color_cont =
                (va == nullptr || (k >= 1 && va[i - 1] == va[i + 1])) &&
                (vr == nullptr || (k >= 1 && vr[i - 1] == vr[i + 1])) &&
                (vg == nullptr || (k >= 1 && vg[i - 1] == vg[i + 1])) &&
                (vb == nullptr || (k >= 1 && vb[i - 1] == vb[i + 1]));
            emit_clipped_textured_quad(dest, tex, qx, qy, qz, 0.0f, v_along_src[i], 1.0f,
                                       v_along_src[i + 1], sa, sr, sg, sb, &share, color_cont);
          }
        };
        int run0 = 0;
        for (int i = 0; i + 1 < n; ++i) {
          if (verts != nullptr &&
              verts[i + 1] != static_cast<uint16_t>(verts[static_cast<size_t>(i)] + 1u)) {
            run_strip(run0, i + 1);
            run0 = i + 1;
          }
        }
        run_strip(run0, n);
      };

  // OpenWDS Butterfly mesh + ButterflyEffect.shader, then Unity 2022.3
  // particle instancing: flap in mesh space, startSize scales the mesh,
  // Quaternion.Euler is Z then X then Y, renderAlignment World, then
  // inverse Rx(tilt) into LaneGroup for project_main.
  // hop: vertInstancingUVs samples Sprite.texture (tiles=1 → texcoord=uv).
  // Native atlas packs only textureRect; draw that span as a sub-quad
  // inside the flapped official wing (affine UV, no extra θ at mid-x).
  auto flap_wing = [&](const wds::chart_render::SplitEffectLiveParticle& p, float size_unity,
                      float x_spine, float x_tip) {
    const float hy = wds::chart_render::kSplitEffectButterflyMeshHalfY;
    const float tilt =
        wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
    const float cw = std::cos(tilt);
    const float sw = std::sin(tilt);
    const float cr = std::cos(p.roll);
    const float sr = std::sin(p.roll);
    const float cxr = std::cos(p.tilt_x);
    const float sxr = std::sin(p.tilt_x);
    const float cyr = std::cos(p.tilt_y);
    const float syr = std::sin(p.tilt_y);
    auto flap_mesh = [&](float mesh_x, float mesh_y, float& mx, float& my, float& mz) {
      const float theta = p.custom1_y * mesh_x * 7.5f;
      const float ct = std::cos(theta);
      const float st = std::sin(theta);
      mx = mesh_x * size_unity * ct;
      my = mesh_y * size_unity;
      mz = -mesh_x * size_unity * st;
    };
    auto project_flapped = [&](float mx, float my, float mz) {
      // Unity Euler(x,y,z): Z, X, Y.
      float x = mx * cr - my * sr;
      float y = mx * sr + my * cr;
      float z = mz;
      const float y2 = y * cxr - z * sxr;
      const float z2 = y * sxr + z * cxr;
      const float wx = x * cyr + z2 * syr;
      const float wy = y2;
      const float wz = -x * syr + z2 * cyr;
      const float lx = wx;
      const float ly = wy * cw + wz * sw;
      const float lz = -wy * sw + wz * cw;
      return geometry_.project_main(p.world_x + lx, p.main_y + ly, p.z + lz);
    };
    float sbx = 0.0f, sby = 0.0f, sbz = 0.0f;
    float stx = 0.0f, sty = 0.0f, stz = 0.0f;
    float tbx = 0.0f, tby = 0.0f, tbz = 0.0f;
    float ttx = 0.0f, tty = 0.0f, ttz = 0.0f;
    flap_mesh(x_spine, -hy, sbx, sby, sbz);
    flap_mesh(x_spine, hy, stx, sty, stz);
    flap_mesh(x_tip, -hy, tbx, tby, tbz);
    flap_mesh(x_tip, hy, ttx, tty, ttz);
    // Official mesh UV: right verts 0–3 (inset), left 4–7 (0..1).
    const bool right = x_tip > 0.0f;
    const float mesh_u0 = right ? wds::chart_render::kSplitEffectButterflyMeshUvU[3]
                                : wds::chart_render::kSplitEffectButterflyMeshUvU[4];
    const float mesh_u1 = right ? wds::chart_render::kSplitEffectButterflyMeshUvU[0]
                                : wds::chart_render::kSplitEffectButterflyMeshUvU[6];
    const float mesh_v0 = right ? wds::chart_render::kSplitEffectButterflyMeshUvV[0]
                                : wds::chart_render::kSplitEffectButterflyMeshUvV[4];
    const float mesh_v1 = right ? wds::chart_render::kSplitEffectButterflyMeshUvV[1]
                                : wds::chart_render::kSplitEffectButterflyMeshUvV[5];
    float ru0 = 0.0f, ru1 = 1.0f, rv0 = 0.0f, rv1 = 1.0f;
    wds::chart_render::official_split_effect_uv_tex_rect(p.uv_name_index, ru0, ru1, rv0, rv1);
    auto st_of = [](float a, float a0, float a1) {
      const float den = a1 - a0;
      if (std::fabs(den) <= 1e-8f) {
        return 0.0f;
      }
      return std::clamp((a - a0) / den, 0.0f, 1.0f);
    };
    const float s0 = st_of(ru0, mesh_u0, mesh_u1);
    const float s1 = st_of(ru1, mesh_u0, mesh_u1);
    const float t0 = st_of(rv0, mesh_v0, mesh_v1);
    const float t1 = st_of(rv1, mesh_v0, mesh_v1);
    auto at_st = [&](float s, float t, float& ox, float& oy, float& oz) {
      const float bx = sbx + (tbx - sbx) * s;
      const float by = sby + (tby - sby) * s;
      const float bz = sbz + (tbz - sbz) * s;
      const float tx = stx + (ttx - stx) * s;
      const float ty = sty + (tty - sty) * s;
      const float tz = stz + (ttz - stz) * s;
      ox = bx + (tx - bx) * t;
      oy = by + (ty - by) * t;
      oz = bz + (tz - bz) * t;
    };
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Quad q;
    at_st(s0, t0, x, y, z);
    q.lb = project_flapped(x, y, z);
    at_st(s1, t0, x, y, z);
    q.rb = project_flapped(x, y, z);
    at_st(s0, t1, x, y, z);
    q.lt = project_flapped(x, y, z);
    at_st(s1, t1, x, y, z);
    q.rt = project_flapped(x, y, z);
    return q;
  };

  // OpenWDS 11570_2: PSR Mesh = Unity Primitive Cube (1×1, UV per face),
  // renderAlignment View, startRotation 3D. Manual/PrimitiveObjects.html.
  // Main startSizeZ is Mesh depth (Manual/PartSysMainModule.html). Official
  // Cube is size3D false so X=Y=Z=startSize; still scale each axis.
  // Material _MainTex is unbound → shader "white"; vertex color is
  // startColor RandomColor × COL. View + Euler Z,X,Y. Raster cull is none.
  auto draw_cube = [&](DrawBatch& dest, const TextureInfo& tex,
                      const wds::chart_render::SplitEffectLiveParticle& p, float size_x,
                      float size_y, float size_z) {
    const float hx = wds::chart_render::kSplitEffectCubeMeshHalf * size_x;
    const float hy = wds::chart_render::kSplitEffectCubeMeshHalf * size_y;
    const float hz = wds::chart_render::kSplitEffectCubeMeshHalf * size_z;
    const float tilt =
        wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
    const float cam_up_y = std::cos(tilt);
    const float cam_up_z = -std::sin(tilt);
    const float cam_fwd_y = std::sin(tilt);
    const float cam_fwd_z = std::cos(tilt);
    const float cr = std::cos(p.roll);
    const float sr = std::sin(p.roll);
    const float cxr = std::cos(p.tilt_x);
    const float sxr = std::sin(p.tilt_x);
    const float cyr = std::cos(p.tilt_y);
    const float syr = std::sin(p.tilt_y);
    auto corner_xyz = [&](float mx, float my, float mz, float& ox, float& oy, float& oz) {
      // Unity Euler(x,y,z): Z, X, Y.
      float x = mx * cr - my * sr;
      float y = mx * sr + my * cr;
      float z = mz;
      const float y2 = y * cxr - z * sxr;
      const float z2 = y * sxr + z * cxr;
      const float wx = x * cyr + z2 * syr;
      const float wy = y2;
      const float wz = -x * syr + z2 * cyr;
      ox = p.world_x + wx;
      oy = p.main_y + wy * cam_up_y + wz * cam_fwd_y;
      oz = p.z + wy * cam_up_z + wz * cam_fwd_z;
    };
    auto face = [&](float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2,
                    float z2, float x3, float y3, float z3) {
      float qx[4], qy[4], qz[4];
      corner_xyz(x0, y0, z0, qx[0], qy[0], qz[0]);
      corner_xyz(x1, y1, z1, qx[1], qy[1], qz[1]);
      corner_xyz(x2, y2, z2, qx[2], qy[2], qz[2]);
      corner_xyz(x3, y3, z3, qx[3], qy[3], qz[3]);
      emit_clipped_textured_quad(dest, tex, qx, qy, qz, tex.u0, tex.v0, tex.u1, tex.v1, p.a, p.r,
                                 p.g, p.b, nullptr, false);
    };
    face(-hx, -hy, hz, hx, -hy, hz, hx, hy, hz, -hx, hy, hz);
    face(hx, -hy, -hz, -hx, -hy, -hz, -hx, hy, -hz, hx, hy, -hz);
    face(-hx, hy, -hz, -hx, hy, hz, hx, hy, hz, hx, hy, -hz);
    face(-hx, -hy, hz, -hx, -hy, -hz, hx, -hy, -hz, hx, -hy, hz);
    face(hx, -hy, -hz, hx, hy, -hz, hx, hy, hz, hx, -hy, hz);
    face(-hx, -hy, hz, -hx, hy, hz, -hx, hy, -hz, -hx, -hy, -hz);
  };

  // OpenWDS models.bundle Mesh "Ring": 32-seg cylinder, AABB ±1 × ±1 × ±0.1.
  // PSR Mesh startSize scales that mesh. Local aligns to the PS Transform
  // (identity) — Manual/PartSysRendererModule.html — so Euler stays in
  // LaneGroup axes (oriented_quad Local), not Cube's View cam-up remap.
  // SOL Z is Mesh-only depth (Manual/PartSysSizeOverLifeModule.html).
  auto draw_ring = [&](DrawBatch& dest, const TextureInfo& tex,
                      const wds::chart_render::SplitEffectLiveParticle& p, float size_x,
                      float size_y, float size_z) {
    const float cr = std::cos(p.roll);
    const float sr = std::sin(p.roll);
    const float cxr = std::cos(p.tilt_x);
    const float sxr = std::sin(p.tilt_x);
    const float cyr = std::cos(p.tilt_y);
    const float syr = std::sin(p.tilt_y);
    auto corner_xyz = [&](float mx, float my, float mz, float& ox, float& oy, float& oz) {
      mx *= size_x;
      my *= size_y;
      mz *= size_z;
      // Unity Euler(x,y,z): Z, X, Y.
      float x = mx * cr - my * sr;
      float y = mx * sr + my * cr;
      float z = mz;
      const float y2 = y * cxr - z * sxr;
      const float z2 = y * sxr + z * cxr;
      const float wx = x * cyr + z2 * syr;
      const float wy = y2;
      const float wz = -x * syr + z2 * cyr;
      ox = p.world_x + wx;
      oy = p.main_y + wy;
      oz = p.z + wz;
    };
    const float du = tex.u1 - tex.u0;
    const float dv = tex.v1 - tex.v0;
    for (int t = 0; t < wds::chart_render::kSplitEffectRingMeshTriCount; ++t) {
      const uint16_t i0 = wds::chart_render::kSplitEffectRingMeshIdx[t * 3];
      const uint16_t i1 = wds::chart_render::kSplitEffectRingMeshIdx[t * 3 + 1];
      const uint16_t i2 = wds::chart_render::kSplitEffectRingMeshIdx[t * 3 + 2];
      const uint16_t ix[3] = {i0, i1, i2};
      float qx[3], qy[3], qz[3], qu[3], qv[3];
      for (int k = 0; k < 3; ++k) {
        const int vi = static_cast<int>(ix[static_cast<size_t>(k)]);
        corner_xyz(wds::chart_render::kSplitEffectRingMeshPos[vi * 3],
                   wds::chart_render::kSplitEffectRingMeshPos[vi * 3 + 1],
                   wds::chart_render::kSplitEffectRingMeshPos[vi * 3 + 2], qx[k], qy[k], qz[k]);
        qu[k] = tex.u0 + wds::chart_render::kSplitEffectRingMeshUv[vi * 2] * du;
        qv[k] = tex.v0 + wds::chart_render::kSplitEffectRingMeshUv[vi * 2 + 1] * dv;
      }
      emit_clipped_uv_poly(dest, tex, qx, qy, qz, qu, qv, 3, p.a, p.r, p.g, p.b, nullptr, false);
    }
  };

  auto draw_stretch = [&](DrawBatch& dest, const TextureInfo& tex,
                         const wds::chart_render::SplitEffectLiveParticle& p, float size_unity) {
    const float along = wds::chart_render::official_split_effect_stretch_height(
        size_unity, p.size > 1e-8f ? p.size_y * (size_unity / p.size) : p.size_y, p.size_3d);
    // kettlewell along = current size.y (SOL Y), including !startSize3D.
    // 3D and non-3D Stretch share one helper. Freeform != 0 → pcwalton
    // particle-space basis; else kettlewell strip. Do not blend.
    // Official playfield / preview camera is static → cam vel 0,0,0; the
    // camera_velocity_scale multiply still runs on the helper.
    // live dump v22: kettlewell direction is totalVelocity (2022.3 Stretch
    // uses velocity + animatedVelocity). Applied LS = authored ×
    // Sprite.textureRect h/w (2026-10-08 hop; isolate square sprites stay
    // ×1). v21 0.05 cut withdrawn. 11320 ID branch withdrawn.
    float svx = p.total_vx;
    float svy = p.total_vy;
    float svz = p.total_vz;
    if (svx * svx + svy * svy + svz * svz <= 1e-20f) {
      svx = p.vel_x;
      svy = p.vel_y;
      svz = p.vel_z;
    }
    const float applied_ls = wds::chart_render::official_split_effect_stretch_applied_length_scale(
        p.stored_vx, p.stored_vy, p.stored_vz, p.total_vx, p.total_vy, p.total_vz, p.length_scale,
        p.effect_id, p.render_mode, p.uv_name_index);
    float qx[4], qy[4], qz[4];
    if (geometry_.split_particle_stretch_world_corners(
            p.world_x, p.main_y, p.z, svx, svy, svz, size_unity, along, p.pivot_y,
            p.roll, p.pivot_x, qx, qy, qz, applied_ls, p.velocity_scale,
            p.camera_velocity_scale, 0.0f, 0.0f, 0.0f, p.freeform_stretching,
            p.rotate_with_stretch)) {
      // Same near-plane path as Trails: project after clip and keep
      // authored corner UVs. add_sprite remaps a UV rect onto the leftover
      // polygon, so a cut sampled Split_404_*'s transparent edge
      // (ParticleAlpha clip(a-0.01)) and the rail looked thinner than
      // Start Size X.
      emit_psr_flipped_quad(dest, tex, p, qx, qy, qz);
      return;
    }
    float bqx[4], bqy[4], bqz[4];
    if (geometry_.split_particle_oriented_world_corners(
            p.world_x, p.main_y, std::max(size_unity, 1e-4f),
            along / std::max(size_unity, 1e-4f), p.pivot_y, p.roll, 0.0f, 0.0f, p.z, 0, p.pivot_x,
            0.0f, bqx, bqy, bqz)) {
      emit_psr_flipped_quad(dest, tex, p, bqx, bqy, bqz);
    }
  };

  notes_batch_.reserve_quads(4096);
  glow_batch_.reserve_quads(16384);
  split_late_batch_.reserve_quads(1024);
  additive_batch_.reserve_quads(1024);

  const uint64_t splits_hash = hash_split_lanes(snapshot.split_lanes);
  const uint64_t view_hash = hash_preview_view(geometry_);
  const auto cache_stats = wds::chart_render::split_effect_eval_cache_stats();
  const bool collect_memo =
      !playing && split_pause_collect_valid_ &&
      split_pause_revision_ == snapshot.revision && split_pause_now_us_ == now_us &&
      split_pause_quality_ == quality && split_pause_splits_hash_ == splits_hash &&
      split_pause_seed_epoch_ == cache_stats.epoch;
  const bool verts_memo =
      collect_memo && split_pause_verts_valid_ && split_pause_view_hash_ == view_hash;
  if (verts_memo) {
    split_late_batch_.overlay_ordered = true;
    split_late_batch_.append_from(split_pause_late_);
    split_particles_drawn = split_pause_drawn_;
    WDS_PERF_COUNT("preview.split_particles.count", split_particles_drawn);
    WDS_PERF_COUNT("preview.verts.count", split_pause_vert_count_);
    WDS_PERF_COUNT("preview.split_memo.collect", 1);
    WDS_PERF_COUNT("preview.split_memo.verts", 1);
    split_frame_diag_.revision = snapshot.revision;
    split_frame_diag_.now_us = now_us;
    split_frame_diag_.quality = quality;
    split_frame_diag_.splits = static_cast<int>(snapshot.split_lanes.size());
    split_frame_diag_.particles = collected.total;
    split_frame_diag_.cache_epoch = cache_stats.epoch;
    split_frame_diag_.pool_cap = cache_stats.pool_cap;
    split_frame_diag_.pool_last = cache_stats.pool_participants;
    split_frame_diag_.pool_participants = 0;
    split_frame_diag_.pool_p_cores = cache_stats.pool_p_cores;
    split_frame_diag_.pool_e_cores = cache_stats.pool_e_cores;
    split_frame_diag_.simd_path = cache_stats.simd_path != nullptr ? cache_stats.simd_path : "";
    split_frame_diag_.cache_entries = static_cast<uint32_t>(cache_stats.particle_entries);
    split_frame_diag_.cache_lookup_ops = cache_stats.lookup_ops;
    split_frame_diag_.cache_lookup_hits = cache_stats.lookup_hits;
    split_frame_diag_.cache_lookup_contend = cache_stats.lookup_contend;
    split_frame_diag_.cache_store_ops = cache_stats.store_ops;
    split_frame_diag_.cache_store_contend = cache_stats.store_contend;
    split_frame_diag_.cleared = cache_cleared;
    split_frame_diag_.collect_memo = 1;
    split_frame_diag_.verts_memo = 1;
    split_frame_diag_.playing = 0;
    split_frame_diag_.collect_us = 0;
    split_frame_diag_.verts_us = 0;
    split_pause_verts_pending_ = false;
    return;
  }

  int64_t collect_us = 0;
  {
    WDS_PERF_SCOPE("preview.split_particles.collect");
    const auto collect_t0 = std::chrono::steady_clock::now();
    if (!collect_memo) {
      if (playing && std::isfinite(split_playback_state_.last_now_sec) &&
          now + 1e-9 < split_playback_state_.last_now_sec) {
        split_playback_state_.reset();
      }
      wds::chart_render::collect_split_effect_frame(snapshot.split_lanes, quality, now, playing,
                                                    split_playback_state_, collected);
    }
    collect_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - collect_t0)
                     .count();
  }
  WDS_PERF_COUNT("preview.split_memo.collect", collect_memo ? 1 : 0);
  if (collected.total <= 0) {
    // Lines still on screen, no live particles: skip emit job walk / mapped-VB
    // / full-pool trail estimate. Collect already ran (births / dead latch).
    split_emit_notes_.clear();
    split_emit_glow_.clear();
    split_emit_late_.clear();
    split_emit_direct_ = false;
    split_emit_direct_n_ = 0;
    const auto cache_after = wds::chart_render::split_effect_eval_cache_stats();
    if (!playing) {
      split_pause_collect_valid_ = true;
      split_pause_verts_valid_ = false;
      split_pause_verts_pending_ = true;
      split_pause_revision_ = snapshot.revision;
      split_pause_now_us_ = now_us;
      split_pause_quality_ = quality;
      split_pause_splits_hash_ = splits_hash;
      split_pause_view_hash_ = view_hash;
      split_pause_seed_epoch_ = cache_after.epoch;
      split_pause_drawn_ = 0;
      split_pause_vert_count_ = 0;
    } else {
      split_pause_verts_pending_ = false;
    }
    split_frame_diag_.revision = snapshot.revision;
    split_frame_diag_.now_us = now_us;
    split_frame_diag_.quality = quality;
    split_frame_diag_.splits = static_cast<int>(snapshot.split_lanes.size());
    split_frame_diag_.particles = 0;
    split_frame_diag_.cache_epoch = cache_after.epoch;
    split_frame_diag_.pool_cap = cache_after.pool_cap;
    split_frame_diag_.pool_last = cache_after.pool_participants;
    split_frame_diag_.pool_participants =
        (!collect_memo && collect_us >= 50) ? cache_after.pool_participants : 0;
    split_frame_diag_.pool_p_cores = cache_after.pool_p_cores;
    split_frame_diag_.pool_e_cores = cache_after.pool_e_cores;
    split_frame_diag_.simd_path = cache_after.simd_path != nullptr ? cache_after.simd_path : "";
    split_frame_diag_.cache_entries = static_cast<uint32_t>(cache_after.particle_entries);
    split_frame_diag_.cache_lookup_ops = cache_after.lookup_ops;
    split_frame_diag_.cache_lookup_hits = cache_after.lookup_hits;
    split_frame_diag_.cache_lookup_contend = cache_after.lookup_contend;
    split_frame_diag_.cache_store_ops = cache_after.store_ops;
    split_frame_diag_.cache_store_contend = cache_after.store_contend;
    split_frame_diag_.cleared = cache_cleared;
    split_frame_diag_.collect_memo = collect_memo ? 1 : 0;
    split_frame_diag_.verts_memo = 0;
    split_frame_diag_.playing = playing ? 1 : 0;
    split_frame_diag_.collect_us = collect_us;
    split_frame_diag_.verts_us = 0;
    return;
  }
  const auto verts_t0 = std::chrono::steady_clock::now();
  const auto& pool = collected.pool;
  const bool legacy_emit = wds::ui::preview_legacy_emit_from_env();
  const size_t split_n = snapshot.split_lanes.size();
  // Particle-only dests. glow_batch_ already holds split-lane plates (appended
  // before this function). Saving glow_batch_ as pause memo would replay those
  // plates on the next paused frame (+80640 glow on these charts).
  split_emit_notes_.clear();
  split_emit_glow_.clear();
  split_emit_late_.clear();
  split_emit_late_.overlay_ordered = true;
  split_late_batch_.overlay_ordered = true;
  size_t trail_vert_est = split_emit_reserve_hint_;
  if (trail_vert_est == 0) {
    const bool indexed_est = wds::renderer::split_geometry_indexed_from_env();
    for (const auto& p : pool) {
      if (p.trail_points >= 2) {
        trail_vert_est +=
            static_cast<size_t>(p.trail_points - 1) * (indexed_est ? 4u : 6u);
      }
    }
  }
  dest_reserve_chunk = trail_vert_est;
  if (trail_vert_est > 0) {
    split_emit_late_.reserve_verts(trail_vert_est);
    split_late_batch_.reserve_verts(trail_vert_est);
  }
  DrawBatch& emit_notes = split_emit_notes_;
  DrawBatch& emit_glow = split_emit_glow_;
  DrawBatch& emit_late = split_emit_late_;
  split_emit_direct_ = false;
  split_emit_direct_n_ = 0;
  split_emit_in_vb_ = false;
  split_emit_vb_low_ = 0;
  SplitEmitTargets emit_targets{&emit_notes, &emit_glow, &emit_late, &split_particles_drawn};

  auto emit_one = [&](const wds::chart_render::SplitEffectLiveParticle& p, SplitEmitTargets tg) {
      const bool unnamed = p.uv_name_index == wds::chart_render::kSplitEffectNoUvName;
      const bool cube = (p.flags & wds::chart_render::kSplitEffectFlagCubeMesh) != 0;
      const bool ring = (p.flags & wds::chart_render::kSplitEffectFlagRingMesh) != 0;
      // live dump dump-pending-v2 hop (not 2022.3): only the 30 systems
      // whose materials[0] is SplitEffectSyurikenTrail submit a body
      // (BombEffectTrail, UV all 0). Unnamed ParticleAdd/Alpha + Trails
      // stay trail-only (42). startColor a=0 is common on both classes —
      // skip the body, never return before the strip (v2 10770/10660/11220).
      const bool unnamed_add_alpha_trails =
          unnamed && !cube && !ring &&
          (p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0 &&
          p.blend != static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Other);
      bool skip_body =
          split_live_skip_dead_trail_body(p) || unnamed_add_alpha_trails;
      const bool in_front =
          wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z);
      // World-space RoD can birth at the SplitEffect origin (percent>1).
      // OpenWDS still draws those billboards — large fog quads reach the
      // visible stage and overlap the judge line (no homemade floor clip).
      TextureInfo tex = sprite_for(p);
      // materials[0] == SplitEffectSyurikenTrail: body _MainTex is
      // BombEffectTrail (same standalone as the strip). BakeMesh UV all 0;
      // native sampler is CLAMP_TO_EDGE, so UV 0 is the corner texel.
      const bool body_trail_mat_tex =
          p.blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Other);
      if (unnamed && !cube && !ring && body_trail_mat_tex && trail_tex != nullptr && *trail_tex) {
        tex = *trail_tex;
        tex.u0 = 0.0f;
        tex.v0 = 0.0f;
        tex.u1 = 0.0f;
        tex.v1 = 0.0f;
      }
      if (!tex || p.a < 0.001f) {
        skip_body = true;
      }
      const auto trail_mat =
          wds::chart_render::official_split_effect_trail_material(p.catalog_index);
      const bool trail_bomb =
          wds::chart_render::official_split_effect_trail_uses_bomb_tex(trail_mat);
      const TextureInfo* strip_tex = trail_bomb ? trail_tex : &unbound_main_tex;
      const bool can_trail = strip_tex != nullptr && *strip_tex && p.trail_count < 2 &&
                             p.trail_points >= 2;
      if (skip_body && !can_trail) {
        return;
      }
      ++(*tg.drawn);
      const bool butterfly = (p.flags & wds::chart_render::kSplitEffectFlagButterfly) != 0;
      // OpenWDS materials.bundle SplitEffectSyurikenTrail: color *= _AddtiveIntencity
      // on fixed4 (RGBA), then clip(a-0.01). Catalog Other is materials[0]=Trail.
      const bool body_trail_mat =
          p.blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Other);
      wds::chart_render::SplitEffectLiveParticle body = p;
      if (body_trail_mat) {
        const float body_k = wds::chart_render::kOfficialSplitEffectTrailAdditiveIntensity;
        body.r *= body_k;
        body.g *= body_k;
        body.b *= body_k;
        body.a *= body_k;
      }
      // SortingGroup 20 after notes (10) and line (0). Fold early/carve into
      // one ordered HDR sequence; pipe is blend/clip, not a sort bucket.
      uint8_t body_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::Additive);
      if (p.blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Alpha)) {
        body_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::Alpha);
      } else if (body_trail_mat) {
        body_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::AdditiveClip01);
      }
      tg.late->overlay_ordered = true;
      tg.late->overlay_pipe = body_pipe;
      DrawBatch& dest = *tg.late;
      float size_unity = p.size;
      // live dump n7 (align2-n7-unity-v1 11520_1 Hamming-1). Not 2022.3.
      // Billboard: size' = min(GetCurrentSize, maxP * W(viewZ)),
      // W = 2 * viewZ * tan(fov/2) * aspect. viewZ<=0: no clamp.
      // Measure current size vs viewport width (not height / Start Size
      // projection / AABB / camDist). Then U1-6 sprite width-norm (tr_w(i)/
      // tr_w(0)) on the clamped width. Stretch / Mesh: do not write a clamp.
      // Trails keep unclamped GetCurrentSize (evaluate applies the ratio
      // after fill_trails).
      const bool mesh_render = p.render_mode == wds::chart_render::kSplitEffectRenderMesh;
      const bool stretch_render =
          p.render_mode == wds::chart_render::kSplitEffectRenderStretch;
      // Center-behind skip hid straddling Local Billboards (ALIGN2-N6 10461 /
      // 11332: GetCurrentSize.y 117–153, bottom viewport.z<0, Unity still
      // rasterizes the leftover). Mesh / Stretch / Billboard go through
      // emit_clipped (near-plane clip then project). Butterfly is a 2D sprite
      // without that clip — keep the centre test so behind-camera wings do
      // not explode NDC.
      if (!skip_body) {
        if (!mesh_render && !stretch_render) {
          const float view_z = wds::chart_editor::official_main_camera_z(p.main_y, p.z);
          const float tan_half = 1.0f / std::max(focal, 1e-6f);
          size_unity = wds::chart_render::split_effect_billboard_draw_width(
              p.current_size, p.size, p.max_particle_size, view_z, tan_half, view_aspect,
              p.render_mode);
        }
        const bool draw_body = mesh_render || size_unity > 1e-6f;
        if (!draw_body) {
          // maxP=0: skip Billboard/Stretch draw; trails still run below.
        } else if (butterfly) {
          if (in_front) {
            WDS_PERF_COUNT("preview.verts.butterfly", 1);
            const float wing = p.butterfly_wing_x > 1e-6f ? p.butterfly_wing_x
                                                          : wds::chart_render::kSplitEffectButterflyMeshHalfX;
            dest.add_sprite_no_alpha_clip(tex, flap_wing(p, size_unity, 0.0f, wing), -0.55f, p.a,
                                          p.r, p.g, p.b);
            dest.add_sprite_no_alpha_clip(tex, flap_wing(p, size_unity, 0.0f, -wing), -0.55f, p.a,
                                          p.r, p.g, p.b);
          }
        } else if (ring && mesh_render) {
          WDS_PERF_COUNT("preview.verts.ring", 1);
          draw_ring(dest, tex, body, p.size, p.size_y, p.size_z);
        } else if (cube && mesh_render) {
          WDS_PERF_COUNT("preview.verts.cube", 1);
          draw_cube(dest, tex, body, p.size, p.size_y, p.size_z);
        } else if (p.render_mode == wds::chart_render::kSplitEffectRenderStretch) {
          WDS_PERF_COUNT("preview.verts.stretch", 1);
          draw_stretch(dest, tex, body, size_unity);
        } else {
          WDS_PERF_COUNT("preview.verts.billboard", 1);
          // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Camera-nearClipPlane.html
          float qx[4], qy[4], qz[4];
          if (geometry_.split_particle_oriented_world_corners(
                  p.world_x, p.main_y, size_unity, p.sol_y, p.pivot_y, p.roll, p.tilt_x,
                  p.tilt_y, p.z, p.render_alignment, p.pivot_x, p.pivot_z, qx, qy, qz)) {
            emit_psr_flipped_quad(dest, tex, body, qx, qy, qz);
          }
        }
      }
      if (p.trail_count >= 2 || p.trail_points < 2 || strip_tex == nullptr || !(*strip_tex)) {
        return;
      }
      bool any_trail_width = p.trail_width >= 1e-4f;
      if (p.trail_widths.size() == static_cast<size_t>(p.trail_points)) {
        any_trail_width = false;
        for (float w : p.trail_widths) {
          any_trail_width = any_trail_width || w >= 1e-4f;
        }
      }
      if (!any_trail_width) {
        return;
      }
      // OpenWDS trailMaterial: ParticleTrailAdditive × BombEffectTrail × 2.
      // ParticleAdd / ParticleAlpha default tex is white; no _AddtiveIntencity.
      const float trail_k = wds::chart_render::official_split_effect_trail_intensity(trail_mat);
      // Unity 2022.3 ParticleSystemRenderer.trailMaterial draws the trail.
      // Same ordered HDR sequence as the body; pipe follows trailMaterial.
      const uint8_t trail_blend = wds::chart_render::official_split_effect_trail_blend(trail_mat);
      uint8_t trail_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::Additive);
      if (trail_blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Alpha)) {
        trail_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::Alpha);
      } else if (trail_blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Other)) {
        trail_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::AdditiveClip01);
      }
      tg.late->overlay_ordered = true;
      tg.late->overlay_pipe = trail_pipe;
      DrawBatch& trail_dest = *tg.late;
      const int pn = static_cast<int>(p.trail_points);
      TrailDrawScratch& ts = trail_draw_scratch();
      ts.resize_strip(pn);
      const float denom = std::max(static_cast<float>(pn - 1), 1.0f);
      for (int t = 0; t < pn; ++t) {
        ts.xs[static_cast<size_t>(t)] = p.trail_x[t];
        ts.ys[static_cast<size_t>(t)] = p.trail_y[t];
        ts.zs[static_cast<size_t>(t)] = p.trail_z[t];
        ts.widths[static_cast<size_t>(t)] =
            p.trail_widths.size() == static_cast<size_t>(pn) ? p.trail_widths[t] : p.trail_width;
        // Particle-mode: index 0 is newest (head). Color over Trail t=0 head.
        // Stretch UV stays vertex index (C7 P2: equal-spacing hop). Do not
        // borrow Ribbon arc-length Stretch (C7-03) onto this path.
        const float t_head = static_cast<float>(t) / denom;
        ts.v_along[static_cast<size_t>(t)] = t_head;
        if (p.trail_color_r.size() == static_cast<size_t>(pn) &&
            p.trail_color_g.size() == static_cast<size_t>(pn) &&
            p.trail_color_b.size() == static_cast<size_t>(pn) &&
            p.trail_color_a.size() == static_cast<size_t>(pn)) {
          ts.vr[static_cast<size_t>(t)] = p.trail_color_r[static_cast<size_t>(t)] * trail_k;
          ts.vg[static_cast<size_t>(t)] = p.trail_color_g[static_cast<size_t>(t)] * trail_k;
          ts.vb[static_cast<size_t>(t)] = p.trail_color_b[static_cast<size_t>(t)] * trail_k;
          ts.va[static_cast<size_t>(t)] = p.trail_color_a[static_cast<size_t>(t)] * trail_k;
        } else {
          ts.vr[static_cast<size_t>(t)] = p.trail_r * trail_k;
          ts.vg[static_cast<size_t>(t)] = p.trail_g * trail_k;
          ts.vb[static_cast<size_t>(t)] = p.trail_b * trail_k;
          ts.va[static_cast<size_t>(t)] = p.trail_a * trail_k;
        }
      }
      WDS_PERF_COUNT("preview.verts.trail", 1);
      draw_clipped_trail_strip(trail_dest, *strip_tex, ts.xs.data(), ts.ys.data(), ts.zs.data(),
                               ts.widths.data(), ts.v_along.data(), nullptr, pn, 0.0f, 0.0f, 0.0f,
                               0.0f, ts.va.data(), ts.vr.data(), ts.vg.data(), ts.vb.data());
  };

  auto process_chain = [&](std::vector<int>& idxs, SplitEmitTargets tg) {
    std::sort(idxs.begin(), idxs.end(),
              [&](int a, int b) { return pool[a].trail_vertex < pool[b].trail_vertex; });
    if (idxs.size() < 2) {
      return;
    }
    const auto& marker = pool[idxs.back()];
    const auto trail_mat =
        wds::chart_render::official_split_effect_trail_material(marker.catalog_index);
    const bool trail_bomb = wds::chart_render::official_split_effect_trail_uses_bomb_tex(trail_mat);
    const TextureInfo* strip_tex = trail_bomb ? trail_tex : &unbound_main_tex;
    if (strip_tex == nullptr || !(*strip_tex)) {
      return;
    }
    const uint8_t trail_blend = wds::chart_render::official_split_effect_trail_blend(trail_mat);
    uint8_t trail_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::Additive);
    if (trail_blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Alpha)) {
      trail_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::Alpha);
    } else if (trail_blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Other)) {
      trail_pipe = static_cast<uint8_t>(wds::renderer::DrawOverlayPipe::AdditiveClip01);
    }
    tg.late->overlay_ordered = true;
    tg.late->overlay_pipe = trail_pipe;
    DrawBatch& dest = *tg.late;
    const float trail_k = wds::chart_render::official_split_effect_trail_intensity(trail_mat);
    const int cn = static_cast<int>(idxs.size());
    TrailDrawScratch& ts = trail_draw_scratch();
    ts.resize_strip(cn);
    ts.verts.resize(static_cast<size_t>(cn));
    for (int k = 0; k < cn; ++k) {
      const auto& v = pool[idxs[static_cast<size_t>(k)]];
      ts.xs[static_cast<size_t>(k)] = v.world_x;
      ts.ys[static_cast<size_t>(k)] = v.main_y;
      ts.zs[static_cast<size_t>(k)] = v.z;
      ts.widths[static_cast<size_t>(k)] = v.trail_width;
      ts.va[static_cast<size_t>(k)] = v.trail_a * trail_k;
      ts.vr[static_cast<size_t>(k)] = v.trail_r * trail_k;
      ts.vg[static_cast<size_t>(k)] = v.trail_g * trail_k;
      ts.vb[static_cast<size_t>(k)] = v.trail_b * trail_k;
      ts.verts[static_cast<size_t>(k)] = v.trail_vertex;
    }
    fill_ribbon_stretch_along(ts.v_along.data(), ts.xs.data(), ts.ys.data(), ts.zs.data(), cn);
    draw_clipped_trail_strip(dest, *strip_tex, ts.xs.data(), ts.ys.data(), ts.zs.data(),
                             ts.widths.data(), ts.v_along.data(), ts.verts.data(), cn, 0.0f, 0.0f,
                             0.0f, 0.0f, ts.va.data(), ts.vr.data(), ts.vg.data(), ts.vb.data());
  };

  struct EmitJob {
    enum Kind : uint8_t { Particle, Ribbon } kind = Particle;
    int32_t index = 0;
  };
  std::vector<EmitJob> emit_jobs;
  std::vector<std::vector<int>> ribbon_job_idxs;
  // live dump align2-u1-unity-v2 hop (not 2022.3): within a line, draw
  // catalog (prefab) index — primaries and subemitters together. Do not
  // use Subemit task.layer=pcount (that only stays on pool / over_cap so
  // collect golden is unchanged). Same catalog_index: birth_t, then seed.
  auto live_emit_lt = [&](int ia, int ib) -> bool {
    const auto& a = pool[static_cast<size_t>(ia)];
    const auto& b = pool[static_cast<size_t>(ib)];
    if (a.sorting_order != b.sorting_order) {
      return a.sorting_order < b.sorting_order;
    }
    if (a.catalog_index != b.catalog_index) {
      return a.catalog_index < b.catalog_index;
    }
    if (a.birth_t != b.birth_t) {
      return a.birth_t < b.birth_t;
    }
    if (a.seed != b.seed) {
      return a.seed < b.seed;
    }
    return ia < ib;
  };
  // Same hop for Ribbon: a chain draws in its own system's catalog slot,
  // right after that system's bodies. Hash-map order put 11230_1 (wide pink
  // Alpha) over 11230_2/_3 (gold) on some lines only; Unity draws all seven
  // lines alike. PerParticle trails already follow the body in emit_one.
  auto for_each_line_job = [&](const std::vector<int>& particle_idxs, auto&& on_particle,
                               auto&& on_ribbon) {
    std::unordered_map<uint32_t, std::vector<int>> chains;
    for (int idx : particle_idxs) {
      const auto& rp = pool[static_cast<size_t>(idx)];
      if (rp.trail_chain != 0 && rp.trail_count >= 2) {
        chains[rp.trail_chain].push_back(idx);
      }
    }
    struct ChainRef {
      uint8_t sorting_order;
      uint32_t catalog_index;
      uint32_t chain;
      std::vector<int>* idxs;
    };
    std::vector<ChainRef> order;
    order.reserve(chains.size());
    for (auto& entry : chains) {
      const auto& head = pool[static_cast<size_t>(entry.second.front())];
      order.push_back({head.sorting_order, head.catalog_index, entry.first, &entry.second});
    }
    std::sort(order.begin(), order.end(), [](const ChainRef& a, const ChainRef& b) {
      if (a.sorting_order != b.sorting_order) {
        return a.sorting_order < b.sorting_order;
      }
      if (a.catalog_index != b.catalog_index) {
        return a.catalog_index < b.catalog_index;
      }
      return a.chain < b.chain;
    });
    size_t ci = 0;
    for (int idx : particle_idxs) {
      const auto& p = pool[static_cast<size_t>(idx)];
      while (ci < order.size() &&
             (order[ci].sorting_order < p.sorting_order ||
              (order[ci].sorting_order == p.sorting_order &&
               order[ci].catalog_index < p.catalog_index))) {
        on_ribbon(*order[ci].idxs);
        ++ci;
      }
      on_particle(idx);
    }
    for (; ci < order.size(); ++ci) {
      on_ribbon(*order[ci].idxs);
    }
  };
  auto collect_emit_jobs = [&]() {
    emit_jobs.clear();
    ribbon_job_idxs.clear();
    emit_jobs.reserve(static_cast<size_t>(std::max(collected.total, 0)) + split_n);
    for (size_t split_i = 0; split_i < split_n; ++split_i) {
      const int begin =
          split_i < collected.split_begin.size() ? collected.split_begin[split_i] : 0;
      const int n = split_i < collected.split_count.size() ? collected.split_count[split_i] : 0;
      std::vector<int> particle_idxs;
      particle_idxs.reserve(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        particle_idxs.push_back(begin + i);
      }
      std::sort(particle_idxs.begin(), particle_idxs.end(), live_emit_lt);
      WDS_PERF_COUNT("preview.verts.ribbon", 1);
      for_each_line_job(
          particle_idxs, [&](int idx) { emit_jobs.push_back({EmitJob::Particle, idx}); },
          [&](std::vector<int>& idxs) {
            ribbon_job_idxs.push_back(std::move(idxs));
            emit_jobs.push_back(
                {EmitJob::Ribbon, static_cast<int32_t>(ribbon_job_idxs.size() - 1)});
          });
    }
  };
  auto run_emit_job = [&](const EmitJob& job, SplitEmitTargets tg) {
    if (job.kind == EmitJob::Particle) {
      if (job.index >= 0 && static_cast<size_t>(job.index) < pool.size()) {
        emit_one(pool[static_cast<size_t>(job.index)], tg);
      }
      return;
    }
    if (job.index >= 0 && static_cast<size_t>(job.index) < ribbon_job_idxs.size()) {
      process_chain(ribbon_job_idxs[static_cast<size_t>(job.index)], tg);
    }
  };

  std::function<void(size_t, SplitEmitTargets)> build_one_split;
  build_one_split = [&](size_t split_i, SplitEmitTargets tg) {
    const int begin =
        split_i < collected.split_begin.size() ? collected.split_begin[split_i] : 0;
    const int n = split_i < collected.split_count.size() ? collected.split_count[split_i] : 0;
    std::vector<int> particle_idxs;
    particle_idxs.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      particle_idxs.push_back(begin + i);
    }
    std::sort(particle_idxs.begin(), particle_idxs.end(), live_emit_lt);
    WDS_PERF_COUNT("preview.verts.ribbon", 1);
    for_each_line_job(
        particle_idxs, [&](int idx) { emit_one(pool[static_cast<size_t>(idx)], tg); },
        [&](std::vector<int>& idxs) { process_chain(idxs, tg); });
  };

  auto body_layer = [&](const wds::chart_render::SplitEffectLiveParticle&) -> int {
    return 2;
  };
  auto add_layer = [](int layer, uint32_t n, uint32_t& notes, uint32_t& glow, uint32_t& late) {
    if (layer == 2) {
      late += n;
    } else if (layer == 1) {
      glow += n;
    } else {
      notes += n;
    }
  };
  auto estimate_particle = [&](const wds::chart_render::SplitEffectLiveParticle& p, uint32_t& notes,
                               uint32_t& glow, uint32_t& late) {
    const bool unnamed = p.uv_name_index == wds::chart_render::kSplitEffectNoUvName;
    const bool cube = (p.flags & wds::chart_render::kSplitEffectFlagCubeMesh) != 0;
    const bool ring = (p.flags & wds::chart_render::kSplitEffectFlagRingMesh) != 0;
    const bool unnamed_add_alpha_trails =
        unnamed && !cube && !ring &&
        (p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0 &&
        p.blend != static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Other);
    bool skip_body = split_live_skip_dead_trail_body(p) || unnamed_add_alpha_trails;
    if (p.a < 0.001f) {
      skip_body = true;
    }
    const bool in_front = wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z);
    const bool butterfly = (p.flags & wds::chart_render::kSplitEffectFlagButterfly) != 0;
    if (!skip_body && (!butterfly || in_front)) {
      // Near-plane clip of a quad adds at most one vertex (fan 3 tris).
      const bool indexed = wds::renderer::split_geometry_indexed_from_env();
      uint32_t body = indexed ? 5u : 9u;
      if (butterfly) {
        body = indexed ? 8u : 12u;
      } else if (ring) {
        body = static_cast<uint32_t>(wds::chart_render::kSplitEffectRingMeshTriCount) * 3u;
      } else if (cube) {
        body = indexed ? 24u : 36u;
      }
      add_layer(body_layer(p), body, notes, glow, late);
    }
    if (p.trail_count >= 2 || p.trail_points < 2) {
      return;
    }
    const int pn = std::max(static_cast<int>(p.trail_points), 2);
    int tlayer = 2;
    add_layer(tlayer,
              static_cast<uint32_t>(pn - 1) *
                  (wds::renderer::split_geometry_indexed_from_env() ? 4u : 6u),
              notes, glow, late);
  };
  auto estimate_job = [&](const EmitJob& job, uint32_t& notes, uint32_t& glow, uint32_t& late) {
    if (job.kind == EmitJob::Particle) {
      if (job.index >= 0 && static_cast<size_t>(job.index) < pool.size()) {
        estimate_particle(pool[static_cast<size_t>(job.index)], notes, glow, late);
      }
      return;
    }
    if (job.index >= 0 && static_cast<size_t>(job.index) < ribbon_job_idxs.size()) {
      const auto& idxs = ribbon_job_idxs[static_cast<size_t>(job.index)];
      if (idxs.size() < 2) {
        return;
      }
      int tlayer = 2;
      add_layer(tlayer,
                static_cast<uint32_t>(idxs.size() - 1) *
                    (wds::renderer::split_geometry_indexed_from_env() ? 4u : 6u),
                notes, glow, late);
    }
  };

  auto emit_jobs_into = [&](DrawBatch& notes, DrawBatch& glow, DrawBatch& late, int& drawn) {
    late.overlay_ordered = true;
    collect_emit_jobs();
    auto job_sort_key = [&](const EmitJob& job) -> uint8_t {
      if (job.kind == EmitJob::Particle && job.index >= 0 &&
          static_cast<size_t>(job.index) < pool.size()) {
        return pool[static_cast<size_t>(job.index)].sorting_order;
      }
      if (job.kind == EmitJob::Ribbon && job.index >= 0 &&
          static_cast<size_t>(job.index) < ribbon_job_idxs.size()) {
        const auto& idxs = ribbon_job_idxs[static_cast<size_t>(job.index)];
        if (!idxs.empty() && idxs.back() >= 0 &&
            static_cast<size_t>(idxs.back()) < pool.size()) {
          return pool[static_cast<size_t>(idxs.back())].sorting_order;
        }
      }
      return 1;
    };
    std::stable_sort(emit_jobs.begin(), emit_jobs.end(),
                     [&](const EmitJob& a, const EmitJob& b) {
                       return job_sort_key(a) < job_sort_key(b);
                     });
    const size_t njobs = emit_jobs.size();
    if (njobs == 0) {
      return;
    }
    const int threads = std::max(wds::chart_render::split_collect_thread_count(), 1);
    uint32_t est_notes = 0;
    uint32_t est_glow = 0;
    uint32_t est_late = 0;
    for (const auto& job : emit_jobs) {
      estimate_job(job, est_notes, est_glow, est_late);
    }
    const uint32_t est_verts = est_notes + est_glow + est_late;
    // Small tessellation stays on this thread; the shared collect pool
    // scales with topology (not a hardcoded 4).
    const bool use_pool = threads > 1 && njobs >= 2 && est_verts >= 2048u;
    const size_t nparts =
        use_pool ? std::max<size_t>(1, std::min(static_cast<size_t>(threads), njobs))
                 : size_t{1};
    if (nparts <= 1 && !(&notes == &emit_notes && &glow == &emit_glow && &late == &emit_late)) {
      SplitEmitTargets tg{&notes, &glow, &late, &drawn};
      for (const auto& job : emit_jobs) {
        run_emit_job(job, tg);
      }
      return;
    }
    if (split_emit_parts_.size() < nparts) {
      split_emit_parts_.resize(nparts);
    }
    const size_t saved_chunk = dest_reserve_chunk;
    if (dest_reserve_chunk > 0) {
      dest_reserve_chunk = dest_reserve_chunk / nparts + 64;
    }
    for (size_t i = 0; i < nparts; ++i) {
      SplitEmitPart& part = split_emit_parts_[i];
      part.notes.unbind_external();
      part.glow.unbind_external();
      part.late.unbind_external();
      part.notes.clear();
      part.glow.clear();
      part.late.clear();
      part.late.overlay_ordered = true;
      part.drawn = 0;
      if (split_emit_reserve_hint_ > 0) {
        const size_t hint = split_emit_reserve_hint_ / nparts + 64;
        part.late.reserve_verts(hint);
        part.glow.reserve_verts(hint);
        part.notes.reserve_verts(hint);
      }
    }
    const bool dest_is_emit =
        &notes == &emit_notes && &glow == &emit_glow && &late == &emit_late;
    // Mapped-VB is a write destination, not a play-state. Paused/seek first
    // frames used to skip this and then memcpy 10660-class late (~1.1M verts)
    // through CPU batches + pause memo (~4–7× verts_us vs play).
    if (dest_is_emit) {
      emit_notes_cap_.assign(nparts, 64);
      emit_glow_cap_.assign(nparts, 64);
      emit_late_cap_.assign(nparts, 64);
      std::vector<uint32_t>& notes_cap = emit_notes_cap_;
      std::vector<uint32_t>& glow_cap = emit_glow_cap_;
      std::vector<uint32_t>& late_cap = emit_late_cap_;
      for (size_t part_i = 0; part_i < nparts; ++part_i) {
        const size_t lo = njobs * part_i / nparts;
        const size_t hi = njobs * (part_i + 1) / nparts;
        for (size_t j = lo; j < hi; ++j) {
          estimate_job(emit_jobs[j], notes_cap[part_i], glow_cap[part_i], late_cap[part_i]);
        }
      }
      uint64_t extra_verts = 0;
      for (size_t i = 0; i < nparts; ++i) {
        notes_cap[i] += notes_cap[i] / 16u;
        glow_cap[i] += glow_cap[i] / 16u;
        late_cap[i] += late_cap[i] / 16u;
        extra_verts += static_cast<uint64_t>(late_cap[i]);
      }
      const size_t extra_bytes =
          static_cast<size_t>(extra_verts) * sizeof(wds::renderer::DrawVertex);
      wds::renderer::VulkanRenderer::PersistentVbLock lock{};
      // Heads write from vertex 0 (scene + lane glow + notes + uncapped hit
      // VFX). Place split extras after that reserve so they cannot overlap
      // heads. Packing from the tail made extra_hi≈ring_n (used≈64MB / ~8ms).
      // 65536 verts (2.5MB) is below glow (80640) and overlaps.
      constexpr uint32_t kHeadReserveVerts = 262144u;
      constexpr uint32_t kHeadReserveIdx = 524288u;
      const size_t head_bytes =
          static_cast<size_t>(kHeadReserveVerts) * sizeof(wds::renderer::DrawVertex);
      const size_t lock_need = extra_bytes + head_bytes;
      const size_t extra_idx_bytes = static_cast<size_t>(extra_verts) * sizeof(uint32_t) * 2u;
      const size_t head_idx_bytes = static_cast<size_t>(kHeadReserveIdx) * sizeof(uint32_t);
      const size_t lock_need_ib = extra_idx_bytes + head_idx_bytes;
      const bool need_ib = wds::renderer::split_geometry_indexed_from_env();
      if (extra_bytes > 0 &&
          vulkan_.lock_persistent_vb(std::max(lock_need, size_t{4096}), &lock, lock_need_ib) &&
          lock.mapped != nullptr && lock.capacity_bytes >= lock_need &&
          (!need_ib || (lock.idx_mapped != nullptr && lock.idx_capacity_bytes >= lock_need_ib))) {
        const uint32_t ring_n = static_cast<uint32_t>(lock.capacity_bytes /
                                                      sizeof(wds::renderer::DrawVertex));
        const uint32_t idx_ring_n = lock.idx_mapped == nullptr
                                        ? 0u
                                        : static_cast<uint32_t>(lock.idx_capacity_bytes /
                                                                sizeof(uint32_t));
        uint32_t cur = kHeadReserveVerts;
        uint32_t icur = kHeadReserveIdx;
        bool placed = kHeadReserveVerts <= ring_n;
        auto place = [&](wds::renderer::DrawBatch& batch, uint32_t cap) {
          if (!placed || cap == 0) {
            return;
          }
          if (cap > ring_n || cur > ring_n - cap) {
            placed = false;
            return;
          }
          const uint32_t icap = cap * 2u;
          uint32_t* idx = nullptr;
          uint32_t idx_cap = 0;
          if (lock.idx_mapped != nullptr && icap > 0) {
            if (icap > idx_ring_n || icur > idx_ring_n - icap) {
              placed = false;
              return;
            }
            idx = lock.idx_mapped + icur;
            idx_cap = icap;
          }
          batch.bind_external(lock.mapped + cur, cap, idx, idx_cap, cur);
          cur += cap;
          icur += idx_cap;
        };
        for (size_t i = 0; i < nparts; ++i) {
          place(split_emit_parts_[i].late, late_cap[i]);
        }
        if (placed && cur > kHeadReserveVerts) {
          split_emit_in_vb_ = true;
          split_emit_vb_low_ = kHeadReserveVerts;
        } else {
          for (size_t i = 0; i < nparts; ++i) {
            split_emit_parts_[i].notes.unbind_external();
            split_emit_parts_[i].glow.unbind_external();
            split_emit_parts_[i].late.unbind_external();
          }
          split_emit_in_vb_ = false;
          split_emit_vb_low_ = 0;
        }
      }
    }
    struct JobEmitParCtx {
      std::vector<EmitJob>* jobs = nullptr;
      std::vector<SplitEmitPart>* parts = nullptr;
      size_t nparts = 0;
      size_t njobs = 0;
      decltype(run_emit_job)* run = nullptr;
    } par_ctx;
    par_ctx.jobs = &emit_jobs;
    par_ctx.parts = &split_emit_parts_;
    par_ctx.nparts = nparts;
    par_ctx.njobs = njobs;
    par_ctx.run = &run_emit_job;
    ui_split_parallel_for(
        nparts,
        [](size_t part_i, void* raw) {
          auto* ctx = static_cast<JobEmitParCtx*>(raw);
          if (ctx == nullptr || ctx->jobs == nullptr || ctx->parts == nullptr ||
              ctx->run == nullptr || part_i >= ctx->nparts || part_i >= ctx->parts->size()) {
            return;
          }
          const size_t lo = ctx->njobs * part_i / ctx->nparts;
          const size_t hi = ctx->njobs * (part_i + 1) / ctx->nparts;
          SplitEmitPart& part = (*ctx->parts)[part_i];
          SplitEmitTargets tg{&part.notes, &part.glow, &part.late, &part.drawn};
          for (size_t j = lo; j < hi; ++j) {
            (*ctx->run)((*ctx->jobs)[j], tg);
          }
        },
        &par_ctx, static_cast<uint64_t>(est_verts));
    dest_reserve_chunk = saved_chunk;
    if (split_emit_in_vb_) {
      bool overflow = false;
      for (size_t i = 0; i < nparts; ++i) {
        overflow = overflow || split_emit_parts_[i].notes.external_overflow() ||
                   split_emit_parts_[i].glow.external_overflow() ||
                   split_emit_parts_[i].late.external_overflow();
      }
      if (overflow) {
        for (size_t i = 0; i < nparts; ++i) {
          SplitEmitPart& part = split_emit_parts_[i];
          part.notes.unbind_external();
          part.glow.unbind_external();
          part.late.unbind_external();
          part.notes.clear();
          part.glow.clear();
          part.late.clear();
          part.late.overlay_ordered = true;
          part.drawn = 0;
        }
        split_emit_in_vb_ = false;
        split_emit_vb_low_ = 0;
        ui_split_parallel_for(
            nparts,
            [](size_t part_i, void* raw) {
              auto* ctx = static_cast<JobEmitParCtx*>(raw);
              if (ctx == nullptr || ctx->jobs == nullptr || ctx->parts == nullptr ||
                  ctx->run == nullptr || part_i >= ctx->nparts || part_i >= ctx->parts->size()) {
                return;
              }
              const size_t lo = ctx->njobs * part_i / ctx->nparts;
              const size_t hi = ctx->njobs * (part_i + 1) / ctx->nparts;
              SplitEmitPart& part = (*ctx->parts)[part_i];
              SplitEmitTargets tg{&part.notes, &part.glow, &part.late, &part.drawn};
              for (size_t j = lo; j < hi; ++j) {
                (*ctx->run)((*ctx->jobs)[j], tg);
              }
            },
            &par_ctx, static_cast<uint64_t>(est_verts));
      }
    }
    for (size_t i = 0; i < nparts; ++i) {
      drawn += split_emit_parts_[i].drawn;
    }
    split_emit_direct_n_ = nparts;
    if (&notes == &emit_notes && &glow == &emit_glow && &late == &emit_late) {
      split_emit_direct_ = true;
    } else {
      for (size_t i = 0; i < nparts; ++i) {
        SplitEmitPart& part = split_emit_parts_[i];
        late.append_from(part.late);
      }
    }
  };

  auto emit_splits_serial = [&]() {
    for (size_t split_i = 0; split_i < split_n; ++split_i) {
      build_one_split(split_i, emit_targets);
    }
  };

  const bool verts_selftest =
      wds::ui::frame_diag_env_enabled(std::getenv("WDS_PREVIEW_VERTS_SELFTEST"));
  static bool verts_selftest_done = false;
  if (verts_selftest && !verts_selftest_done && !collect_memo && !verts_memo &&
      collected.total > 0) {
    verts_selftest_done = true;
    DrawBatch leg_notes;
    DrawBatch leg_glow;
    DrawBatch leg_late;
    int leg_drawn = 0;
    SplitEmitTargets leg_targets{&leg_notes, &leg_glow, &leg_late, &leg_drawn};
    for (size_t split_i = 0; split_i < split_n; ++split_i) {
      build_one_split(split_i, leg_targets);
    }
    DrawBatch par_notes;
    DrawBatch par_glow;
    DrawBatch par_late;
    int par_drawn = 0;
    emit_jobs_into(par_notes, par_glow, par_late, par_drawn);
    const bool notes_ok = draw_batches_byte_equal(leg_notes, par_notes);
    const bool glow_ok = draw_batches_byte_equal(leg_glow, par_glow);
    const bool late_ok = draw_batches_byte_equal(leg_late, par_late);
    const bool drawn_ok = leg_drawn == par_drawn;
    std::fprintf(stderr,
                 "WDS_PREVIEW_VERTS_SELFTEST emit notes=%d glow=%d late=%d drawn=%d (%d vs %d)\n",
                 notes_ok ? 1 : 0, glow_ok ? 1 : 0, late_ok ? 1 : 0, drawn_ok ? 1 : 0, leg_drawn,
                 par_drawn);
    if (notes_ok && glow_ok && late_ok && drawn_ok) {
      // Keep CPU parallel as the byte-identity reference for the mapped-VB path.
    }
  }

  if (legacy_emit) {
    emit_splits_serial();
    split_late_batch_.overlay_ordered = true;
    split_late_batch_.append_from(emit_late);
  } else {
    emit_jobs_into(emit_notes, emit_glow, emit_late, split_particles_drawn);
    static bool vb_selftest_done = false;
    if (verts_selftest && !vb_selftest_done && split_emit_in_vb_ && split_emit_direct_n_ > 0) {
      vb_selftest_done = true;
      DrawBatch rec_notes;
      DrawBatch rec_glow;
      DrawBatch rec_late;
      for (size_t i = 0; i < split_emit_direct_n_; ++i) {
        rec_notes.append_from(split_emit_parts_[i].notes);
        rec_glow.append_from(split_emit_parts_[i].glow);
        rec_late.append_from(split_emit_parts_[i].late);
      }
      DrawBatch leg_notes;
      DrawBatch leg_glow;
      DrawBatch leg_late;
      int leg_drawn = 0;
      SplitEmitTargets leg_targets{&leg_notes, &leg_glow, &leg_late, &leg_drawn};
      for (size_t split_i = 0; split_i < split_n; ++split_i) {
        build_one_split(split_i, leg_targets);
      }
      const bool vb_notes = draw_batches_byte_equal(leg_notes, rec_notes);
      const bool vb_glow = draw_batches_byte_equal(leg_glow, rec_glow);
      const bool vb_late = draw_batches_byte_equal(leg_late, rec_late);
      std::fprintf(stderr,
                   "WDS_PREVIEW_VERTS_SELFTEST vb notes=%d glow=%d late=%d (verts %u/%u/%u)\n",
                   vb_notes ? 1 : 0, vb_glow ? 1 : 0, vb_late ? 1 : 0,
                   draw_batch_vertex_count(rec_notes), draw_batch_vertex_count(rec_glow),
                   draw_batch_vertex_count(rec_late));
    }
    if (split_emit_direct_ && split_emit_direct_n_ > 0) {
      split_emit_early_ptrs_.clear();
      split_emit_mid_ptrs_.clear();
      split_emit_carve_ptrs_.resize(split_emit_direct_n_);
      for (size_t i = 0; i < split_emit_direct_n_; ++i) {
        split_emit_carve_ptrs_[i] = &split_emit_parts_[i].late;
      }
    } else {
      split_late_batch_.overlay_ordered = true;
      split_late_batch_.append_from(emit_late);
      split_emit_direct_ = false;
    }
  }
  WDS_PERF_COUNT("preview.split_particles.count", split_particles_drawn);
  uint32_t split_verts1 = notes_batch_.vertex_count() + glow_batch_.vertex_count() +
                          split_late_batch_.vertex_count();
  if (split_emit_direct_) {
    for (size_t i = 0; i < split_emit_direct_n_; ++i) {
      split_verts1 += static_cast<uint32_t>(split_emit_parts_[i].notes.vertex_count() +
                                            split_emit_parts_[i].glow.vertex_count() +
                                            split_emit_parts_[i].late.vertex_count());
    }
  }
  const int64_t vert_delta =
      static_cast<int64_t>(split_verts1) - static_cast<int64_t>(split_verts0);
  WDS_PERF_COUNT("preview.verts.count", vert_delta);
  const auto cache_after = wds::chart_render::split_effect_eval_cache_stats();
  if (!playing) {
    split_pause_collect_valid_ = true;
    split_pause_verts_valid_ = false;
    split_pause_verts_pending_ = true;
    split_pause_revision_ = snapshot.revision;
    split_pause_now_us_ = now_us;
    split_pause_quality_ = quality;
    split_pause_splits_hash_ = splits_hash;
    split_pause_view_hash_ = view_hash;
    split_pause_seed_epoch_ = cache_after.epoch;
    split_pause_drawn_ = split_particles_drawn;
    split_pause_vert_count_ = vert_delta;
  } else {
    split_pause_verts_pending_ = false;
  }
  if (vert_delta > 0) {
    split_emit_reserve_hint_ = std::max(split_emit_reserve_hint_, static_cast<size_t>(vert_delta));
  }
  split_frame_diag_.revision = snapshot.revision;
  split_frame_diag_.now_us = now_us;
  split_frame_diag_.quality = quality;
  split_frame_diag_.splits = static_cast<int>(snapshot.split_lanes.size());
  split_frame_diag_.particles = collected.total;
  split_frame_diag_.cache_epoch = cache_after.epoch;
  split_frame_diag_.pool_cap = cache_after.pool_cap;
  split_frame_diag_.pool_last = cache_after.pool_participants;
  // last_participants is only stored inside parallel_for. Empty collect
  // does not run it, so the raw value sticks at the last heavy/lane-color
  // wave. Per-frame collect participants are 0 when this frame had no
  // collect work (memo or collect_us < 50µs).
  split_frame_diag_.pool_participants =
      (!collect_memo && collect_us >= 50) ? cache_after.pool_participants : 0;
  split_frame_diag_.pool_p_cores = cache_after.pool_p_cores;
  split_frame_diag_.pool_e_cores = cache_after.pool_e_cores;
  split_frame_diag_.simd_path = cache_after.simd_path != nullptr ? cache_after.simd_path : "";
  split_frame_diag_.cache_entries = static_cast<uint32_t>(cache_after.particle_entries);
  split_frame_diag_.cache_lookup_ops = cache_after.lookup_ops;
  split_frame_diag_.cache_lookup_hits = cache_after.lookup_hits;
  split_frame_diag_.cache_lookup_contend = cache_after.lookup_contend;
  split_frame_diag_.cache_store_ops = cache_after.store_ops;
  split_frame_diag_.cache_store_contend = cache_after.store_contend;
  split_frame_diag_.cleared = cache_cleared;
  split_frame_diag_.collect_memo = collect_memo ? 1 : 0;
  split_frame_diag_.verts_memo = 0;
  split_frame_diag_.playing = playing ? 1 : 0;
  split_frame_diag_.collect_us = collect_us;
  split_frame_diag_.verts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - verts_t0)
                                  .count();
}

void PlaybackPreviewView::draw_concurrent_lines(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  if (!skin_.sync_line) {
    return;
  }
  const double now = preview_now_sec(snapshot, visual_lead_us_);
  for (const auto& line : snapshot.concurrent_lines) {
    const double beat = static_cast<double>(line.milliseconds) / 1000.0;
    const float p = geometry_.note_percent(beat, now);
    if (p <= 0.0f || p > geometry_.judgeline_percent() + 0.02f) {
      continue;
    }
    if (p < spawn_clip_percent()) {
      continue;
    }
    const int32_t end_lane = line.start_lane + std::max(1, line.width) - 1;
    const float world_w = wds::chart_editor::official_concurrent_line_visual_width(
        wds::chart_editor::official_span_width(line.start_lane, end_lane));
    // Official ConcurrentLineNote: Sliced 12×8, m_Border L/R=4, color a=1.
    add_sliced_note(batch, skin_.sync_line, geometry_.sync_line_quad(line.start_lane, end_lane, p),
                    skin_.sync_slice_border_l, skin_.sync_slice_border_r, -0.5f, 1.0f, 1.0f, -1.0f,
                    1.0f, 1.0f, 1.0f, world_w);
  }
}

void PlaybackPreviewView::prepare_note_draw_order(const PreviewSnapshot& snapshot) {
  // Visible set is small (on-screen notes). IncrementalPatch swap-remove can
  // reorder start_ms while size+revision stay put — always rebuild.
  wds::chart_render::build_draw_order_indices(
      snapshot.notes.size(), notes_draw_indices_,
      [&](size_t i) { return snapshot.notes[i].start_ms; },
      [&](size_t i) { return static_cast<int32_t>(snapshot.notes[i].note_type); });
  note_draw_order_.clear();
  note_draw_order_.reserve(snapshot.notes.size());
  for (size_t idx : notes_draw_indices_) {
    if (idx >= snapshot.notes.size()) {
      continue;
    }
    const auto& note = snapshot.notes[idx];
    // Sirius: note despawns at judgment; AutoHit only drives VFX / judge text.
    if (note.visual_state == PreviewNoteVisualState::Approaching ||
        note.visual_state == PreviewNoteVisualState::Holding) {
      note_draw_order_.push_back(&note);
    }
  }
}

void PlaybackPreviewView::draw_notes(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  prepare_note_draw_order(snapshot);
  const auto& order = note_draw_order_;

  const double now = preview_now_sec(snapshot, visual_lead_us_);

  // Official sandwich (depthWrite off → draw order = visual order):
  // hold → all bottoms → all tops → mid-stars → arrows.
  for (const PreviewNoteInstance* note : order) {
    if (is_hold_body(note->note_type) && !is_hold_mid_star(note->note_type)) {
      draw_hold_body(batch, *note, now);
    }
  }
  for (const PreviewNoteInstance* note : order) {
    draw_note_flat_layer(batch, *note, snapshot, /*bottom_layer=*/true);
  }
  for (const PreviewNoteInstance* note : order) {
    draw_note_flat_layer(batch, *note, snapshot, /*bottom_layer=*/false);
  }
  for (const PreviewNoteInstance* note : order) {
    if (note->visual_state == PreviewNoteVisualState::Holding) {
      continue;
    }
    const NoteSprites sprites = sprites_for(skin_, note->note_type);
    if (sprites.is_tick) {
      draw_tick_note(batch, *note, now);
    }
  }
  for (const PreviewNoteInstance* note : order) {
    // Scratch-hold end flick (Sirius ScratchHoldEnd) — arrows at end_ms.
    if (is_scratch_hold_body(note->note_type) && is_hold_with_tail(note->note_type) &&
        (note->visual_state == PreviewNoteVisualState::Approaching ||
         note->visual_state == PreviewNoteVisualState::Holding)) {
      draw_arrows_at(batch, *note, static_cast<double>(note->end_ms) / 1000.0, now, now);
      continue;
    }
    // Only an actual Flick draws arrows at its start. ScratchHold arrows are
    // handled at a judged tail above; nontail bodies have no arrow at either end.
    if (wds::chart_render::preview_draws_start_scratch_arrows(note->note_type) &&
        note->visual_state != PreviewNoteVisualState::Holding) {
      draw_arrows(batch, *note, now, now);
    }
  }
}

void PlaybackPreviewView::draw_note_flat_layer(DrawBatch& batch, const PreviewNoteInstance& note,
                                               const PreviewSnapshot& snapshot,
                                               bool bottom_layer) {
  const double now = preview_now_sec(snapshot, visual_lead_us_);
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  if (sprites.is_tick) {
    return;
  }

  // Hold bodies never invent a start head — only a real HoldStart* note draws one.
  // Tailed holds still draw the approaching end cap (HoldEnd / ScratchHoldEnd).
  if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
    const double end = static_cast<double>(note.end_ms) / 1000.0;
    if (is_hold_with_tail(note.note_type) &&
        (note.visual_state == PreviewNoteVisualState::Approaching ||
         note.visual_state == PreviewNoteVisualState::Holding)) {
      draw_flat_note_at(batch, note, end, now, 0.05f, /*use_jump_lanes=*/true, bottom_layer);
    }
    return;
  }

  // HoldStart* / taps: only while Approaching; Holding means already judged → gone.
  if (note.visual_state == PreviewNoteVisualState::Holding) {
    return;
  }

  if (is_hold_start(note.note_type) || !is_hold_body(note.note_type)) {
    draw_flat_note(batch, note, now, 0.0f, bottom_layer);
  }
}

void PlaybackPreviewView::draw_note(DrawBatch& batch, const PreviewNoteInstance& note,
                                    const PreviewSnapshot& snapshot) {
  // Kept for callers that want a single-note composite (top layer only + tick).
  draw_note_flat_layer(batch, note, snapshot, /*bottom_layer=*/false);
  const double now = preview_now_sec(snapshot, visual_lead_us_);
  if (note.visual_state == PreviewNoteVisualState::Holding) {
    return;
  }
  if (sprites_for(skin_, note.note_type).is_tick) {
    draw_tick_note(batch, note, now);
  }
}

void PlaybackPreviewView::draw_hold_body(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double now_sec) {
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  if (!sprites.connection) {
    return;
  }

  const double start = static_cast<double>(note.start_ms) / 1000.0;
  const double end = static_cast<double>(note.end_ms) / 1000.0;
  const float judge_p = geometry_.judgeline_percent();
  float p_near = std::clamp(geometry_.note_percent(start, now_sec), 0.0f, judge_p);
  float p_far = std::clamp(geometry_.note_percent(end, now_sec), 0.0f, judge_p);
  if (p_near < p_far) {
    std::swap(p_near, p_far);
  }
  if (p_near <= p_far) {
    return;
  }

  // Sirius drawHoldEighth: 0.8 idle / approaching, 0.85 on odd 0.1s while holding.
  float base_alpha = config_.hold_body_alpha;
  if (note.visual_state == PreviewNoteVisualState::Holding) {
    const int phase = static_cast<int>(std::floor(now_sec / 0.1)) % 2;
    base_alpha = phase == 1 ? config_.hold_body_holding_alpha : config_.hold_body_alpha;
  }

  // Fade the ribbon tip inside the judgment-line band.
  const float band =
      std::max(0.001f, geometry_.judgeline_half_percent() * 2.0f * 0.65f);
  const float fade_lo = judge_p - band;

  // Hard-clip tip side at the official LaneMask bottom (VisibleOutsideMask).
  const float clip_p = spawn_clip_percent();
  if (p_near <= clip_p) {
    return;
  }
  if (p_far < clip_p) {
    p_far = clip_p;
  }

  auto alpha_at = [&](float p) {
    if (p <= fade_lo) return base_alpha;
    return base_alpha * std::clamp(1.0f - (p - fade_lo) / band, 0.0f, 1.0f);
  };

  const float hold_world_w = wds::chart_editor::official_hold_line_visual_width(
      wds::chart_editor::official_span_width(note.lane, note.end_lane));

  // Official HoldLongNotes: SpriteRenderer Sliced, m_Border L/R = 10 on 157-wide art.
  // Cap world = 10/100 via dest_world_width (no dest_h / tex_h).
  // Temporary approximation: sliced_cap_layout shrinks caps if they cannot fit.
  auto emit = [&](float lo, float hi) {
    if (hi <= lo) return;
    add_sliced_note(batch, sprites.connection,
                    geometry_.hold_line_quad(note.lane, note.end_lane, hi, lo),
                    skin_.hold_slice_border_l, skin_.hold_slice_border_r, -0.25f, alpha_at(hi),
                    alpha_at(lo), -1.0f, sprites.connection_r, sprites.connection_g,
                    sprites.connection_b, hold_world_w);
  };

  if (p_near <= fade_lo) {
    emit(p_far, p_near);
  } else if (p_far >= fade_lo) {
    emit(p_far, p_near);
  } else {
    emit(p_far, fade_lo);
    emit(fade_lo, p_near);
  }
}

void PlaybackPreviewView::draw_flat_note(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double now_sec, float z_bias, bool bottom_layer) {
  draw_flat_note_at(batch, note, static_cast<double>(note.start_ms) / 1000.0, now_sec, z_bias,
                    /*use_jump_lanes=*/false, bottom_layer);
}

void PlaybackPreviewView::draw_flat_note_at(DrawBatch& batch, const PreviewNoteInstance& note,
                                            double beat_sec, double now_sec, float z_bias,
                                            bool use_jump_lanes, bool bottom_layer) {
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  NoteSprites head = sprites;
  // Hold body end caps: shared apply_hold_tail_sprites (scratch purple / hold blue).
  if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
    const bool scratch_tail = is_scratch_hold_body(note.note_type) && use_jump_lanes;
    apply_hold_tail_sprites(head, skin_, scratch_tail);
  }

  TextureInfo layer = bottom_layer ? head.bottom : head.top;
  if (!layer) {
    return;
  }

  const float p = geometry_.note_percent(beat_sec, now_sec);
  if (p < -0.05f || p >= geometry_.judgeline_percent()) {
    return;
  }

  int32_t lane = note.lane;
  int32_t end_lane = note.end_lane;
  if (use_jump_lanes && note.uses_jump_scratch_position) {
    lane = note.jump_scratch_lane_from;
    end_lane = note.jump_scratch_lane_to;
  }

  // Bottom/Top share plane footprint; Unity local Z = height, pinhole-projected.
  const float unity_z = bottom_layer ? config_.note_unity_local_z_bottom
                                     : config_.note_unity_local_z_top;
  const float pc = std::clamp(p, 0.0f, geometry_.judgeline_percent());
  const float half = geometry_.note_half_height_percent(lane, pc);
  const float p_tip = pc - half;
  const float p_near = pc + half;
  // Plane-side reject: if the near edge is still inside the mask on z=0,
  // both layers are hidden (top is pushed farther into the mask).
  if (p_near <= spawn_clip_percent()) {
    return;
  }
  // Project the full sprite (including layer Z), then stencil like official
  // SpriteMask: a horizontal screen line at GetNoteVisiblePositionY.
  Quad q = geometry_.note_span_quad(lane, end_lane, p_near, p_tip, unity_z);
  float far_t = 1.0f;
  if (!geometry_.clip_quad_outside_lane_mask(q, far_t)) {
    return;
  }
  const float z = z_bias + unity_z - static_cast<float>(beat_sec) * 1e-4f;
  const float alpha = note.is_grayed_out ? 0.55f : 1.0f;
  const float tap_world_w = wds::chart_editor::official_tap_visual_width(
      wds::chart_editor::official_span_width(lane, end_lane));
  const float v_near = layer.v0;
  const float v_far = layer.v0 + (layer.v1 - layer.v0) * far_t;
  wds::renderer::add_sliced_note_v(batch, layer, q, skin_.note_slice_border_l,
                                   skin_.note_slice_border_r, z, alpha, -1.0f, v_near, v_far,
                                   1.0f, 1.0f, 1.0f, tap_world_w);
}

void PlaybackPreviewView::draw_tick_note(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double now_sec) {
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  if (!sprites.tick) {
    return;
  }
  const double beat = static_cast<double>(note.start_ms) / 1000.0;
  const float p = geometry_.note_percent(beat, now_sec);
  if (p < 0.0f || p >= geometry_.judgeline_percent()) {
    return;
  }
  if (p < spawn_clip_percent()) {
    return;
  }
  // Sirius drawTick: Draw(..., 200000 - beat, 0.5) — above flats, alpha 0.5.
  const float alpha = note.is_grayed_out ? config_.tick_alpha * 0.55f : config_.tick_alpha;
  const Quad q = geometry_.star_quad(note.lane, note.end_lane, p);
  batch.add_sprite(sprites.tick, q, 0.15f, alpha);
}

void PlaybackPreviewView::draw_arrows(DrawBatch& batch, const PreviewNoteInstance& note,
                                      double now_sec, double anim_time_sec) {
  draw_arrows_at(batch, note, static_cast<double>(note.start_ms) / 1000.0, now_sec, anim_time_sec);
}

void PlaybackPreviewView::draw_arrows_at(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double beat_sec, double now_sec, double anim_time_sec) {
  if (!skin_.scratch_arrow) {
    return;
  }
  const float p = geometry_.note_percent(beat_sec, now_sec);
  if (p <= 0.0f || p >= geometry_.judgeline_percent()) {
    return;
  }
  if (p < spawn_clip_percent()) {
    return;
  }

  int32_t lane = note.lane;
  int32_t end_lane = note.end_lane;
  if (note.uses_jump_scratch_position) {
    lane = note.jump_scratch_lane_from;
    end_lane = note.jump_scratch_lane_to;
  }
  if (end_lane < lane) {
    std::swap(lane, end_lane);
  }

  const float unit = geometry_.content_unit();
  const float w = geometry_.authored_lane_width(lane, p);
  const float w_ref =
      std::max(geometry_.authored_lane_width(lane, geometry_.judgeline_percent()), 1e-6f);
  const float multiplier = w / w_ref;
  const Vec2 c1 = geometry_.authored_lane_position(lane, p);
  const Vec2 c2 = geometry_.authored_lane_position(end_lane, p);
  const float H = config_.arrow_height * unit * multiplier;

  // Match note_quad horizontal inset (note_move_length) so arrows sit inside the note.
  const float move = config_.note_move_length * unit * multiplier;
  const float L = c1.x - w * 0.5f + move;
  const float R = c2.x + w * 0.5f - move;
  if (R <= L) {
    return;
  }

  // Official FlickNoteEntity / NotesArrowsObject. [L,R] is the visible note
  // (width - margin); world positions use full GetNoteWidth. JumpScratch is
  // the gimmick flag — hold-end cover lanes are not the jump arrow table.
  const int32_t body_lanes = end_lane - lane + 1;
  const float note_world = wds::chart_editor::official_note_width(body_lanes);
  const float visual_world = wds::chart_editor::official_tap_visual_width(note_world);
  const float world_to_dest = (R - L) / visual_world;
  const float W = wds::chart_editor::kOfficialArrowSpriteWidth *
                  wds::chart_editor::kOfficialArrowGroupScale * world_to_dest;
  if (W <= 1e-5f) {
    return;
  }

  const bool jump = wds::chart_editor::is_jump_scratch(note.gimmick_type);
  // Hold-chain preview already placed cover lanes. Standalone JumpScratch
  // (official Flick) uses abs(GimmickValue), not body Width.
  const int32_t arrow_lanes =
      (!note.uses_jump_scratch_position && jump) ? std::max(1, std::abs(note.scratch_length))
                                                : body_lanes;
  const int count = wds::chart_editor::official_scratch_arrow_count(arrow_lanes, jump);
  const float interval = wds::chart_editor::official_scratch_arrow_interval(arrow_lanes);
  const float step_world = interval * wds::chart_editor::kOfficialArrowGroupScale;
  const float offset_world =
      jump ? (step_world * static_cast<float>(count) * 0.5f)
           : (note_world * 0.5f - wds::chart_editor::kOfficialArrowGroupInset);

  wds::chart_render::AnimatedArrowLayoutParams params;
  params.span_left = L;
  params.span_right = R;
  params.arrow_w = W;
  params.arrow_step = step_world * world_to_dest;
  params.group_offset = offset_world * world_to_dest;
  params.arrow_count = count;
  params.fill_to_far_edge = !jump && note.scratch_length != 0;
  params.scratch_length = note.scratch_length;
  params.official_one_direction =
      wds::chart_editor::is_flick_like(note.note_type) &&
      wds::chart_editor::is_one_direction(note.gimmick_type);
  params.sonolus_num = static_cast<float>(count);
  params.anim_time_sec = static_cast<float>(anim_time_sec);
  params.arrow_speed = config_.arrow_speed;
  for (const auto& inst : wds::chart_render::layout_animated_scratch_arrows(params)) {
    const float y = inst.flip_x ? c2.y : c1.y;
    batch.add_sprite(skin_.scratch_arrow,
                     Quad{{inst.x0, y}, {inst.x0, y + H * 0.5f}, {inst.x1, y + H * 0.5f}, {inst.x1, y}},
                     0.2f, inst.alpha);
  }
}

// Note bombs are a separate draw list from split-line particles. They do not
// share kSplitEffectParticlePool / over_cap, and they are not the carve dest.
void PlaybackPreviewView::draw_hit_effects(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  const double now = preview_now_sec(snapshot, visual_lead_us_);
  const double duration_d = static_cast<double>(config_.effect_duration);
  // BombControllerBase.SimulationHighSpeed — LaneEffectController.OnBomb speeds up every
  // tracked bomb whose CurrentStartMilliseconds differs from the new hit (not lane-based).
  constexpr float kSimHighSpeed = 3.0f;

  struct BombEvt {
    double t0 = 0.0;
    int64_t key_ms = 0;  // CurrentStartMilliseconds
    int32_t lane = 0;
    int32_t end_lane = 0;
    NoteType type = NoteType::Normal;
    int hit_fx_role = 0;
    bool jump_flare = false;
    float z = 0.95f;
  };

  std::vector<BombEvt> evts;
  evts.reserve(snapshot.notes.size() * 2);

  for (const auto& note : snapshot.notes) {
    const bool hold_body = is_hold_body(note.note_type);
    const bool mid_star = is_hold_mid_star(note.note_type);
    const bool with_tail = is_hold_with_tail(note.note_type);

    // Hold body / mid-stars: SFX only (no visual bomb). Heads + tails only below.
    if (!hold_body && !mid_star && !is_split_lane_gimmick(note.gimmick_type)) {
      if (is_hold_start(note.note_type) || note.note_type == NoteType::Normal ||
          note.note_type == NoteType::Critical ||
          is_flick_like(note.note_type) || note.note_type == NoteType::BlueTap) {
        const double t0 = static_cast<double>(note.start_ms) / 1000.0;
        if (t0 <= now) {
          evts.push_back(BombEvt{t0, note.start_ms, note.lane, note.end_lane, note.note_type, 0,
                                 false, 0.95f});
        }
      }
    }

    // Tail: HoldBomb; JumpScratch ends also get ScratchBomb flare (same as Flick).
    if (with_tail && note.end_ms > note.start_ms) {
      const double t0 = static_cast<double>(note.end_ms) / 1000.0;
      if (t0 <= now) {
        const bool use_jump_span =
            note.uses_jump_scratch_position || is_jump_scratch(note.gimmick_type);
        const bool jump_flare =
            use_jump_span &&
            (is_scratch_hold_body(note.note_type) || is_flick_like(note.note_type));
        int32_t fx_lane = note.lane;
        int32_t fx_end = note.end_lane;
        if (use_jump_span) {
          fx_lane = note.jump_scratch_lane_from;
          fx_end = note.jump_scratch_lane_to;
        }
        evts.push_back(BombEvt{t0, note.end_ms, fx_lane, fx_end, note.note_type, 1, jump_flare,
                               0.96f});
      }
    }
  }

  for (const auto& evt : evts) {
    const double wall_age = now - evt.t0;
    // Retail keeps the pooled instance until FireTime (wall-clock _animationTime).
    if (wall_age < 0.0 || wall_age >= duration_d) {
      continue;
    }

    // Earliest later bomb with a different startMs → ChangeSpeed(3) (TrySpeedUp).
    double speedup_at = std::numeric_limits<double>::infinity();
    for (const auto& other : evts) {
      if (other.key_ms == evt.key_ms) {
        continue;  // same-ms chord: stay 1x
      }
      if (other.t0 > evt.t0 && other.t0 <= now && other.t0 < speedup_at) {
        speedup_at = other.t0;
      }
    }

    double effective_age = wall_age;
    if (speedup_at <= now) {
      effective_age = (speedup_at - evt.t0) + (now - speedup_at) * static_cast<double>(kSimHighSpeed);
    }

    draw_hit_effect_at(batch, evt.lane, evt.end_lane, evt.type, static_cast<float>(effective_age),
                       evt.z, 1.0f, evt.hit_fx_role, evt.jump_flare);
  }
}

namespace {

// Hit FX role: head/tap (0), hold tail (1). Body/stars: no visual FX.
// Prefab map (Default Light):
//   Critical / CriticalHoldStart / ScratchCriticalHoldStart → CriticalBomb
//   Flick / JumpScratch end → ScratchBomb (+Flare)
//   ScratchHoldStart → ScratchBomb (purple; head sprite is pink NormalNote)
//   HoldStart / plain hold tail → HoldBomb
//   Normal → NormalBomb
struct BombFxSpec {
  const char* dir;
  bool flare;
};

// Flare/Square share OnBomb lane-span midpoint on the judgeline (Y=0,Z=0 in
// retail). flare.png is recentered so Unity Sprite BombEffectDefault_10
// m_Pivot≈(0.514,0.483) sits on UV mid (skins/effects README). Do NOT map
// ParticleSystemRenderer.pivot (±0.267×size) into stage Y.

BombFxSpec bomb_fx_spec_for(NoteType type, int hit_fx_role, bool jump_scratch_flare) noexcept {
  // JumpScratch ScratchHoldEnd → ScratchBomb with flare (retail keeps BomFlare in Light).
  if (hit_fx_role == 1 && jump_scratch_flare) {
    return {"scratch", true};
  }
  // Plain hold tails: HoldBomb / scratch-colored HoldBomb Square only.
  if (hit_fx_role == 1) {
    if (is_scratch_hold_body(type) || is_flick_like(type) ||
        type == NoteType::ScratchSound) {
      return {"scratch", false};
    }
    return {"hold", false};
  }

  switch (type) {
    case NoteType::Critical:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return {"critical", true};
    case NoteType::Flick:
    case NoteType::Scratch:
      return {"scratch", true};
    case NoteType::ScratchHoldStart:  // pink NormalNote head → purple ScratchBomb (no flare)
      return {"scratch", false};
    case NoteType::HoldStart:
      return {"hold", false};
    case NoteType::Sound:
    case NoteType::BlueTap:
      return {"sound", false};
    case NoteType::ScratchSound:
      return {"scratch", false};
    case NoteType::Normal:
    default:
      return {"normal", false};
  }
}

// BombController / *BombEffect.prefab (Light: Square [+ Flare]).
// Square: lengthInSec 0.1, rate 80, startLifetime ∈ [0.3, 0.5] (RandomBetweenTwoConstants).
constexpr float kSquareEmitSec = 0.1f;
constexpr float kSquareEmitRate = 80.0f;
constexpr float kSquareLifeMin = 0.3f;
constexpr float kSquareLifeMax = 0.5f;
constexpr float kFlareLife = 0.6f;
// ColorModule atime1 = 26214/65535 ≈ 0.4 (hold), then → 0 at life end.
constexpr float kColorFadeStart = 26214.0f / 65535.0f;

float lerp01(float a, float b, float t) noexcept {
  return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

// SizeModule TwoCurves×scalar=2 — use mid of min/max (not max alone):
//   X: (1.0→1.3 + 1.0→1.1)/2 → 1.0→1.2
//   Y: (1.0→2.0 + 1.0→1.3)/2 → 1.0→1.65
// Max Y×2 over-expands on screen (Local Y is foreshortened in Unity).
float square_size_x_mul(float u) noexcept { return lerp01(1.0f, 1.2f, u); }
float square_size_y_mul(float u) noexcept { return lerp01(1.0f, 1.65f, u); }

// Official ColorModule alpha keys (atime 0 / 26214 / 65535): hold until ~0.4, then linear → 0.
float color_module_alpha(float u) noexcept {
  if (u <= kColorFadeStart) {
    return 1.0f;
  }
  const float t = (u - kColorFadeStart) / (1.0f - kColorFadeStart);
  return 1.0f - std::clamp(t, 0.0f, 1.0f);
}

struct FlareGradRgb {
  float r, g, b;
};

// BomFlare ColorModule maxGradient color keys (ctime/65535). Alpha is separate (above).
// CriticalBombEffect / ScratchBombEffect — RGB darkens hard after mid-life; using a flat
// note tint instead left the additive ring bright far longer than retail.
FlareGradRgb flare_color_module_rgb(float u, bool scratch) noexcept {
  u = std::clamp(u, 0.0f, 1.0f);
  // times: 0, 0.2, 0.4, 1.0
  struct Key {
    float t, r, g, b;
  };
  const Key* keys = nullptr;
  constexpr Key kCritical[] = {
      {0.0f, 1.0f, 0.603905f, 0.3176471f},
      {0.2f, 1.0f, 0.9862867f, 0.7783019f},
      {0.4f, 1.0f, 0.6039216f, 0.31764707f},
      {1.0f, 1.0f, 0.22532359f, 0.0f},
  };
  constexpr Key kScratch[] = {
      {0.0f, 0.6862745f, 0.25882354f, 1.0f},
      {0.2f, 0.9934902f, 0.7877358f, 1.0f},
      {0.4f, 0.68543196f, 0.25943398f, 1.0f},
      {1.0f, 0.43529412f, 0.0f, 1.0f},
  };
  keys = scratch ? kScratch : kCritical;
  if (u <= keys[0].t) {
    return {keys[0].r, keys[0].g, keys[0].b};
  }
  for (int i = 0; i < 3; ++i) {
    if (u <= keys[i + 1].t) {
      const float s = (u - keys[i].t) / (keys[i + 1].t - keys[i].t);
      return {lerp01(keys[i].r, keys[i + 1].r, s), lerp01(keys[i].g, keys[i + 1].g, s),
              lerp01(keys[i].b, keys[i + 1].b, s)};
    }
  }
  return {keys[3].r, keys[3].g, keys[3].b};
}

// Unity AnimationCurve Hermite between two keys (slopes are dValue/dTime).
float curve_hermite(float t, float t0, float t1, float p0, float p1, float out_slope0,
                    float in_slope1) noexcept {
  const float dt = t1 - t0;
  if (dt <= 1e-8f) {
    return p1;
  }
  const float u = (t - t0) / dt;
  const float u2 = u * u;
  const float u3 = u2 * u;
  const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
  const float h10 = u3 - 2.0f * u2 + u;
  const float h01 = -2.0f * u3 + 3.0f * u2;
  const float h11 = u3 - u2;
  return h00 * p0 + h10 * (out_slope0 * dt) + h01 * p1 + h11 * (in_slope1 * dt);
}

// BomFlare SizeModule X curve (Critical/Scratch prefab identical; separateAxes=0).
// Keys: (time, value, inSlope, outSlope) — linear lerp was ~150–200ms slower mid-growth.
float flare_size_mul(float u) noexcept {
  u = std::clamp(u, 0.0f, 1.0f);
  constexpr float kT0 = 0.0f;
  constexpr float kV0 = 0.0f;
  constexpr float kOut0 = 13.959289f;
  constexpr float kT1 = 0.13296969f;
  constexpr float kV1 = 0.6850656f;
  constexpr float kIn1 = 1.0710392f;
  constexpr float kOut1 = 1.0710392f;
  constexpr float kT2 = 1.0f;
  constexpr float kV2 = 1.0f;
  constexpr float kIn2 = 0.0f;
  if (u <= kT0) {
    return kV0;
  }
  if (u >= kT2) {
    return kV2;
  }
  if (u < kT1) {
    return curve_hermite(u, kT0, kT1, kV0, kV1, kOut0, kIn1);
  }
  return curve_hermite(u, kT1, kT2, kV1, kV2, kOut1, kIn2);
}

}  // namespace

void PlaybackPreviewView::draw_hit_effect_at(DrawBatch& batch, int32_t lane, int32_t end_lane,
                                             NoteType type, float age_sec, float z,
                                             float alpha_scale, int hit_fx_role,
                                             bool jump_scratch_flare) {
  if (age_sec < 0.0f || alpha_scale < 0.02f) {
    return;
  }
  // Particles are gone well before BombController._animationTime (0.7 / 1.0).
  constexpr float kMaxParticleWindow = kSquareEmitSec + kSquareLifeMax;
  if (age_sec >= std::max(kMaxParticleWindow, kFlareLife)) {
    return;
  }

  const EffectTint tint = tint_for(type);
  const BombFxSpec fx = bomb_fx_spec_for(type, hit_fx_role, jump_scratch_flare);

  TextureInfo square{};
  TextureInfo flare{};
  const bool has_bomb = skin_.bomb_light_for(fx.dir, square, flare);

  if (has_bomb) {
    if (square && age_sec < kMaxParticleWindow) {
      // BomSquare: lengthInSec 0.1, rateOverTime 80 → ~8 stacked frames.
      // InitializeSquare: startSizeX ∈ [laneCount*0.725, laneCount*0.925].
      // Prefab startSizeY ∈ [0.5, 0.68]. RenderAlignment=Local on JudgeArea.
      constexpr int kLayers = 8;
      for (int i = 0; i < kLayers; ++i) {
        const float spawn = (static_cast<float>(i) + 0.5f) / kSquareEmitRate;
        if (spawn > kSquareEmitSec) {
          break;
        }
        const float life = lerp01(kSquareLifeMin, kSquareLifeMax, static_cast<float>(i % 3) / 2.0f);
        const float pa = age_sec - spawn;
        if (pa < 0.0f || pa >= life) {
          continue;
        }
        const float u = pa / life;
        const float start_x_frac =
            lerp01(0.725f / 0.925f, 1.0f, static_cast<float>((i * 3) % 5) / 4.0f);
        const float start_y_unity = lerp01(0.5f, 0.68f, static_cast<float>((i * 2) % 5) / 4.0f);
        const float width_scale = start_x_frac * square_size_x_mul(u);
        const float height_unity = start_y_unity * square_size_y_mul(u);
        // Official ColorModule linear × startColor.a=1 (~8 additive layers).
        const float a = color_module_alpha(u) * alpha_scale;
        if (a < 0.02f) {
          continue;
        }
        batch.add_sprite(square,
                         geometry_.bomb_frame_quad(lane, end_lane, width_scale, height_unity),
                         z + static_cast<float>(i) * 0.0001f, a, tint.r, tint.g, tint.b);
      }
    }

    if (fx.flare && flare && age_sec < kFlareLife) {
      // Critical/Scratch BomFlare: RenderAlignment=View, startSize=8 constant.
      // Initialize never scales _bombFlare by laneCount / note width.
      const bool scratch_flare = jump_scratch_flare || is_flick_like(type);
      const int flare_burst = scratch_flare ? 2 : 4;
      const float flare_start_a = scratch_flare ? (150.0f / 255.0f) : (50.0f / 255.0f);
      for (int i = 0; i < flare_burst; ++i) {
        const float u = age_sec / kFlareLife;
        const float size_mul = flare_size_mul(u);
        const FlareGradRgb grad = flare_color_module_rgb(u, scratch_flare);
        const float a = flare_start_a * color_module_alpha(u) * alpha_scale;
        if (a < 0.01f) {
          continue;
        }
        batch.add_sprite(flare,
                         geometry_.bomb_flare_billboard_quad(lane, end_lane, 8.0f * size_mul),
                         z + 0.002f + static_cast<float>(i) * 0.0001f, a, grad.r, grad.g,
                         grad.b);
      }
    }
    return;
  }

  // Fallback: legacy Sonolus linear + circular.
  const float duration = std::max(config_.effect_duration, 1e-4f);
  const float t = std::clamp(age_sec / duration, 0.0f, 1.0f);
  const float alpha = (1.0f - t) * (1.0f - t) * alpha_scale;
  if (alpha < 0.02f) {
    return;
  }
  const Quad q = geometry_.effect_quad(lane, end_lane);
  if (skin_.effect_linear_bg) {
    batch.add_sprite(skin_.effect_linear_bg, q, z, alpha * 0.85f, tint.r, tint.g, tint.b);
  }
  if (skin_.effect_linear_star) {
    batch.add_sprite(skin_.effect_linear_star, q, z + 0.002f, alpha, tint.r, tint.g, tint.b);
  }
  if (skin_.effect_circular) {
    batch.add_sprite(skin_.effect_circular, q, z + 0.003f, alpha, tint.r, tint.g, tint.b);
  }
}

void PlaybackPreviewView::draw_timing_effect(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  if (!show_judgment_text_ || !skin_.judge_auto || snapshot.combo_count <= 0) {
    return;
  }
  const float age =
      static_cast<float>(preview_now_sec(snapshot, visual_lead_us_) -
                         static_cast<double>(snapshot.last_judge_ms) / 1000.0);
  const float duration = std::max(config_.timing_effect_duration, 1e-4f);
  if (age < 0.0f || age >= duration) {
    return;
  }

  // TimingEffect_anime: scale 0.4→1 @0.083s; alpha 1 until 0.333 then →0 @0.417.
  constexpr float kPopEndSec = 0.083333336f;
  constexpr float kFadeStartSec = 0.33333334f;
  constexpr float kFadeEndSec = 0.41666666f;
  float scale_mul = 1.0f;
  if (age < kPopEndSec) {
    const float u = age / kPopEndSec;
    scale_mul = 0.4f + 0.6f * ease_in_sine(u);
  }
  float alpha = 1.0f;
  if (age >= kFadeEndSec) {
    return;
  }
  if (age > kFadeStartSec) {
    const float u = (age - kFadeStartSec) / (kFadeEndSec - kFadeStartSec);
    const float lin = 1.0f - std::clamp(u, 0.0f, 1.0f);
    alpha = lin * lin * lin;
  }
  if (alpha < 0.02f) {
    return;
  }

  using wds::chart_render::kOpenWdsEffectsWorldX;
  using wds::chart_render::kOpenWdsEffectsWorldZ;
  using wds::chart_render::kOpenWdsTimingAutoPivotX;
  using wds::chart_render::kOpenWdsTimingAutoPivotY;
  using wds::chart_render::kOpenWdsTimingAutoSpriteH;
  using wds::chart_render::kOpenWdsTimingAutoSpriteW;
  using wds::chart_render::openwds_sprite_world_quad;
  using wds::chart_render::openwds_timing_effect_scale;
  using wds::chart_render::openwds_timing_position_y;

  const float world_y = openwds_timing_position_y();
  const float root = openwds_timing_effect_scale() * scale_mul;
  const auto local =
      openwds_sprite_world_quad(kOpenWdsEffectsWorldX, world_y, kOpenWdsTimingAutoSpriteW * root,
                               kOpenWdsTimingAutoSpriteH * root, kOpenWdsTimingAutoPivotX,
                               kOpenWdsTimingAutoPivotY);
  const Vec2 lb = geometry_.project_world(local.x0, local.y0, kOpenWdsEffectsWorldZ);
  const Vec2 lt = geometry_.project_world(local.x0, local.y1, kOpenWdsEffectsWorldZ);
  const Vec2 rt = geometry_.project_world(local.x1, local.y1, kOpenWdsEffectsWorldZ);
  const Vec2 rb = geometry_.project_world(local.x1, local.y0, kOpenWdsEffectsWorldZ);
  Quad q{lb, lt, rt, rb};
  // TimingEffect.prefab SpriteRenderer m_SortingOrder 99. Pass order, not z,
  // puts this above Bomb 30 / SplitEffect 20 (depth write is off).
  batch.add_sprite(skin_.judge_auto, q, 0.97f, alpha);
}

void PlaybackPreviewView::draw_combo(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  if (snapshot.combo_count <= 0 || !skin_.combo_ap_text) {
    return;
  }
  for (int i = 0; i < 10; ++i) {
    if (!skin_.combo_ap_digit[i]) {
      return;
    }
  }

  using wds::chart_render::kOpenWdsComboCountAnimDuration;
  using wds::chart_render::kOpenWdsComboCountPivotX;
  using wds::chart_render::kOpenWdsComboCountPivotY;
  using wds::chart_render::kOpenWdsComboDigitH;
  using wds::chart_render::kOpenWdsComboDigitW;
  using wds::chart_render::kOpenWdsComboFireAnimDuration;
  using wds::chart_render::kOpenWdsComboHeight;
  using wds::chart_render::kOpenWdsComboLabelH;
  using wds::chart_render::kOpenWdsComboLabelW;
  using wds::chart_render::kOpenWdsComboPosX;
  using wds::chart_render::kOpenWdsComboPosY;
  using wds::chart_render::kOpenWdsComboWidth;
  using wds::chart_render::kOpenWdsHudRefHeight;
  using wds::chart_render::kOpenWdsHudRefWidth;
  using wds::chart_render::openwds_canvas_map;
  using wds::chart_render::openwds_combo_count_scale;
  using wds::chart_render::openwds_combo_digit_xs;
  using wds::chart_render::openwds_combo_fire_alpha;
  using wds::chart_render::openwds_combo_fire_scale;
  using wds::chart_render::openwds_scale_about;

  const auto& content = geometry_.content();
  const auto canvas = openwds_canvas_map(content.l, content.r, content.b, content.t);
  const float combo_right = canvas.x(kOpenWdsHudRefWidth + kOpenWdsComboPosX);
  const float combo_bottom = canvas.y(0.5f * kOpenWdsHudRefHeight + kOpenWdsComboPosY);
  const float combo_h = canvas.scale * kOpenWdsComboHeight;
  const float combo_w = canvas.scale * kOpenWdsComboWidth;
  const float combo_top = combo_bottom + combo_h;
  const float combo_left = combo_right - combo_w;
  const float digit_w = canvas.scale * kOpenWdsComboDigitW;
  const float digit_h = canvas.scale * kOpenWdsComboDigitH;
  const float label_w = canvas.scale * kOpenWdsComboLabelW;
  const float label_h = canvas.scale * kOpenWdsComboLabelH;
  // ComboCount pivot (0.5, 0.5) of the Combo root rect. Combo_anim scales this
  // transform; ComboLabel is a sibling and stays unscaled.
  const float pivot_x = combo_left + kOpenWdsComboCountPivotX * combo_w;
  const float pivot_y = combo_bottom + kOpenWdsComboCountPivotY * combo_h;

  auto draw_digits = [&](int32_t number, float sx, float sy, float a, float z) {
    int digits = 1;
    for (int32_t n = number; n >= 10 && digits < 4; n /= 10) {
      ++digits;
    }
    int xs_n = 0;
    const float* xs = openwds_combo_digit_xs(digits, xs_n);
    int32_t remaining = number;
    for (int i = 0; i < xs_n; ++i) {
      const int digit = remaining % 10;
      remaining /= 10;
      const float right = combo_right + canvas.scale * xs[i];
      const float left = right - digit_w;
      float x0 = left;
      float y0 = combo_bottom;
      float x1 = left;
      float y1 = combo_bottom + digit_h;
      float x2 = right;
      float y2 = combo_bottom + digit_h;
      float x3 = right;
      float y3 = combo_bottom;
      openwds_scale_about(pivot_x, pivot_y, sx, sy, x0, y0);
      openwds_scale_about(pivot_x, pivot_y, sx, sy, x1, y1);
      openwds_scale_about(pivot_x, pivot_y, sx, sy, x2, y2);
      openwds_scale_about(pivot_x, pivot_y, sx, sy, x3, y3);
      Quad q{{x0, y0}, {x1, y1}, {x2, y2}, {x3, y3}};
      batch.add_sprite(skin_.combo_ap_digit[digit], q, z, a);
    }
  };

  const float alpha = config_.combo_alpha;
  constexpr float kZ = 0.98f;
  const double now_sec = preview_now_sec(snapshot);

  // ComboCountPer100 is drawn under ComboCount (Combo.prefab sibling order).
  if (snapshot.combo_count >= 100) {
    const float fire_age =
        static_cast<float>(now_sec - static_cast<double>(snapshot.last_combo_fire_ms) / 1000.0);
    if (fire_age >= 0.0f && fire_age < kOpenWdsComboFireAnimDuration) {
      const auto fire_s = openwds_combo_fire_scale(fire_age);
      const float fire_a = openwds_combo_fire_alpha(fire_age) * alpha;
      if (fire_a > 0.001f) {
        draw_digits((snapshot.combo_count / 100) * 100, fire_s.x, fire_s.y, fire_a, kZ - 0.001f);
      }
    }
  }

  float count_age = kOpenWdsComboCountAnimDuration;
  if (snapshot.combo_count > 0) {
    count_age =
        static_cast<float>(now_sec - static_cast<double>(snapshot.last_judge_ms) / 1000.0);
  }
  const auto count_s = openwds_combo_count_scale(count_age);
  draw_digits(snapshot.combo_count, count_s.x, count_s.y, alpha, kZ);

  const float label_cx = 0.5f * (combo_left + combo_right);
  const float label_left = label_cx - 0.5f * label_w;
  const float label_top = combo_top;
  Quad text_q{{label_left, label_top - label_h},
              {label_left, label_top},
              {label_left + label_w, label_top},
              {label_left + label_w, label_top - label_h}};
  batch.add_sprite(skin_.combo_ap_text, text_q, kZ, alpha);
}

bool PlaybackPreviewView::mark_hit_sfx_event(uint64_t key) {
  return hit_sfx_played_.insert(key).second;
}

void PlaybackPreviewView::collect_due_hit_sfx(const PreviewSnapshot& snapshot, bool arm,
                                              int64_t clock_us) {
  // Clock: PreviewSnapshot::timeline_us ← Transport committed Timeline (µs).
  // With BGM, arm future hits via AudioEngine BASS_SYNC_POS → 1× ChannelPlay.
  // Without BGM, play on the tick where Timeline first reaches the hit time.
  const bool music_clock = hit_sfx_.has_music();
  const PreviewLookup lookup = build_preview_lookup(snapshot);

  auto transport_hit_ms = [&](int64_t chart_hit_ms) -> int64_t {
    return wds::chart_editor::EditLeadIn::transport_ms_for_chart_ms(
        chart_hit_ms, preview_lead_in_visible_ms_);
  };

  auto emit = [&](int32_t note_id, uint32_t kind, wds::audio::HitSfxClip clip, int64_t hit_ms,
                  bool bypass_history) {
    if (clip == wds::audio::HitSfxClip::Count) {
      return;
    }
    const uint64_t key = (static_cast<uint64_t>(snapshot.revision) << 32) |
                         (static_cast<uint64_t>(static_cast<uint32_t>(note_id)) << 2) | kind;
    const int64_t when_ms = music_clock ? transport_hit_ms(hit_ms) : hit_ms;
    const int64_t when_us = wds::common::ms_to_us(when_ms).count();

    if (!arm) {
      // Seek/pause resync: remember hits already at/behind the playhead so resume
      // does not replay them. Leave future hits unmarked so they can be armed later.
      if (when_us <= clock_us) {
        mark_hit_sfx_event(key);
      }
      return;
    }

    auto reject_permanently = [&]() {
      mark_hit_sfx_event(key);
    };

    if (music_clock) {
      if (when_us <= clock_us) {
        // Already behind the music playhead — skip (marked on seek/pause).
        mark_hit_sfx_event(key);
        return;
      }
      if (!sfx_history_.can_admit(when_ms, clip, bypass_history) ||
          !sfx_budget_.can_admit(when_ms)) {
        reject_permanently();
        return;
      }
      // Music byte sync (not display-frame quantized). Engine plays immediately if
      // decode already passed the target so SetSync cannot miss silently.
      // TooFar / AtCapacity / SetSyncFailure return false — drop the mark so a
      // later tick retries instead of permanently skipping the hit. Pass a factory
      // so already-marked keys do not re-evaluate schedule_at every frame.
      const bool accepted = commit_hit_sfx_schedule(hit_sfx_played_, key, [&] {
        return hit_sfx_.schedule_at(clip, wds::common::ms_to_us(std::max<int64_t>(0, when_ms)));
      });
      if (accepted) {
        sfx_history_.record(when_ms, clip, bypass_history);
        sfx_budget_.record(when_ms);
      }
      return;
    }

    // No BGM: fire once when the Timeline reaches the hit (edge on each tick).
    // Do not play future notes — the snapshot window includes spawn-lead notes
    // that are still approaching the judgeline.
    if (when_us > clock_us) {
      return;
    }
    if (!sfx_history_.can_admit(when_ms, clip, bypass_history) ||
        !sfx_budget_.can_admit(when_ms)) {
      reject_permanently();
      return;
    }
    if (!mark_hit_sfx_event(key)) {
      return;
    }
    if (!hit_sfx_.play(clip)) {
      hit_sfx_played_.erase(key);
      return;
    }
    sfx_history_.record(when_ms, clip, bypass_history);
    sfx_budget_.record(when_ms);
  };

  constexpr uint32_t kHead = 0;
  constexpr uint32_t kTail = 1;
  constexpr uint32_t kStar = 2;

  for (const auto& note : snapshot.notes) {
    const bool hold_body = is_hold_body(note.note_type);
    const bool mid_star = is_hold_mid_star(note.note_type);
    const bool with_tail = is_hold_with_tail(note.note_type);
    const bool duration_hold = hold_body && !mid_star && note.end_ms > note.start_ms;

    // Head / tap / flick — arm once when the note is in the snapshot window.
    // Official HoldStart* paired with a duration body: body emits the start hit.
    if (!hold_body && !mid_star && !is_split_lane_gimmick(note.gimmick_type)) {
      if (is_hold_start(note.note_type) || note.note_type == NoteType::Normal ||
          note.note_type == NoteType::Critical ||
          is_flick_like(note.note_type) || note.note_type == NoteType::BlueTap) {
        const bool paired = is_hold_start(note.note_type) &&
                            lookup.duration_hold_heads.count(note_span_key(
                                note.start_ms, note.lane, note.width)) != 0;
        if (!paired) {
          emit(note.note_id, kHead, hit_sfx_clip_for_head(note.note_type), note.start_ms,
               hit_sfx_bypasses_history(note.note_type));
        }
      }
    }

    // Hold body start — Perfect/Critical one-shot; Hold loop is handled separately.
    if (duration_hold) {
      emit(note.note_id, kHead, hit_sfx_clip_for_hold_body_start(note.note_type), note.start_ms,
           false);
    }

    if (with_tail && note.end_ms > note.start_ms) {
      emit(note.note_id, kTail, hit_sfx_clip_for_hold_tail(note.note_type), note.end_ms, false);
    }

    if (mid_star) {
      emit(note.note_id, kStar, hit_sfx_clip_for_mid_star(note.note_type), note.start_ms,
           hit_sfx_bypasses_history(note.note_type));
    }
  }
}

void PlaybackPreviewView::collect_hold_intervals(const PreviewSnapshot& snapshot,
                                                 std::vector<wds::audio::HoldInterval>& out) const {
  out.clear();
  const bool music_clock = hit_sfx_.has_music();
  auto transport_hit_ms = [&](int64_t chart_hit_ms) -> int64_t {
    return wds::chart_editor::EditLeadIn::transport_ms_for_chart_ms(chart_hit_ms,
                                                                   preview_lead_in_visible_ms_);
  };
  for (const auto& note : snapshot.notes) {
    if (!is_hold_body(note.note_type) || is_hold_mid_star(note.note_type) ||
        note.end_ms <= note.start_ms) {
      continue;
    }
    const int64_t start_ms = music_clock ? transport_hit_ms(note.start_ms) : note.start_ms;
    const int64_t end_ms = music_clock ? transport_hit_ms(note.end_ms) : note.end_ms;
    if (end_ms <= start_ms) {
      continue;
    }
    out.push_back(wds::audio::HoldInterval{start_ms, end_ms});
  }
  wds::audio::merge_hold_intervals(out);
}

void PlaybackPreviewView::sync_hold_loop(const PreviewSnapshot& snapshot, int64_t clock_us,
                                         bool arm) {
  if (mute_hold_body_sfx_) {
    hit_sfx_.set_hold_looping(false);
    hit_sfx_.clear_hold_gates();
    hold_gates_generation_ = hit_sfx_.position_generation();
    hold_gates_revision_ = snapshot.revision;
    return;
  }

  std::vector<wds::audio::HoldInterval> intervals;
  collect_hold_intervals(snapshot, intervals);
  const int64_t clock_ms = clock_us / 1000;
  // Not an unconditional set: a MIXTIME hold gate fires at mix time, ahead of this
  // heard clock, and reconcile defers to it until the clock agrees.
  hit_sfx_.reconcile_hold_looping(wds::audio::hold_covers_ms(intervals, clock_ms));

  if (!arm || !hit_sfx_.has_music()) {
    if (!arm) {
      hit_sfx_.clear_hold_gates();
      hold_gates_generation_ = std::numeric_limits<uint64_t>::max();
    }
    return;
  }

  const uint64_t pos_gen = hit_sfx_.position_generation();
  if (hold_gates_generation_ == pos_gen && hold_gates_revision_ == snapshot.revision) {
    return;
  }
  hit_sfx_.clear_hold_gates();
  for (const auto& interval : intervals) {
    const auto start_us = wds::common::ms_to_us(std::max<int64_t>(0, interval.start_ms));
    const auto end_us = wds::common::ms_to_us(std::max<int64_t>(0, interval.end_ms));
    if (start_us.count() > clock_us) {
      (void)hit_sfx_.schedule_hold_gate(true, start_us);
    }
    if (end_us.count() > clock_us) {
      (void)hit_sfx_.schedule_hold_gate(false, end_us);
    }
  }
  hold_gates_generation_ = pos_gen;
  hold_gates_revision_ = snapshot.revision;
}

void PlaybackPreviewView::release_sfx_clock_control(const PreviewSnapshot& snapshot,
                                                    int64_t raw_us, bool playing) {
  // User/transport took the clock (pause, seek, scrub, play resync): mute, drop
  // armed keys, and release the monotonic filter so it re-latches at raw_us.
  hit_sfx_.stop_all();
  hit_sfx_played_.clear();
  sfx_history_.clear();
  sfx_budget_.clear();
  hold_gates_generation_ = std::numeric_limits<uint64_t>::max();
  sfx_mono_us_ = std::max<int64_t>(0, raw_us);
  sfx_position_generation_ = hit_sfx_.position_generation();
  collect_due_hit_sfx(snapshot, /*arm=*/false, sfx_mono_us_);
  if (playing) {
    collect_due_hit_sfx(snapshot, /*arm=*/true, sfx_mono_us_);
  }
  sfx_was_playing_ = playing;
  sync_hold_loop(snapshot, sfx_mono_us_, playing);
}

void PlaybackPreviewView::update_hit_sfx(const PreviewSnapshot& snapshot) {
  if (!hit_sfx_.ready()) {
    return;
  }

  using wds::chart_editor::PreviewPlaybackState;
  const bool playing = snapshot.playback_state == PreviewPlaybackState::Playing;
  const bool music_clock = hit_sfx_.has_music();
  // Prefer BASS playtime when BGM is loaded; otherwise Transport Timeline µs.
  const int64_t timeline_us =
      (snapshot.timeline_us != 0 || snapshot.timeline_ms == 0)
          ? snapshot.timeline_us
          : wds::common::ms_to_us(snapshot.timeline_ms).count();
  const int64_t raw_us =
      music_clock ? hit_sfx_.music_position().count() : timeline_us;

  const uint64_t pos_gen = hit_sfx_.position_generation();
  const bool control_event = pos_gen != sfx_position_generation_;
  const bool pause_edge = !playing && sfx_was_playing_;
  const bool revision_changed = snapshot.revision != sfx_document_revision_;
  if (revision_changed) {
    hit_sfx_played_.clear();
    sfx_history_.clear();
    sfx_budget_.clear();
    hold_gates_generation_ = std::numeric_limits<uint64_t>::max();
    sfx_document_revision_ = snapshot.revision;
  }

  if (control_event || pause_edge) {
    release_sfx_clock_control(snapshot, raw_us, playing);
  } else if (!playing) {
    // Stay paused: keep mono latched; do not arm. Scrubs bump pos_gen above.
    if (sfx_mono_us_ < 0) {
      sfx_mono_us_ = std::max<int64_t>(0, raw_us);
    }
    collect_due_hit_sfx(snapshot, /*arm=*/false, sfx_mono_us_);
    sfx_was_playing_ = false;
    hit_sfx_.set_hold_looping(false);
    hit_sfx_.clear_hold_gates();
    hold_gates_generation_ = std::numeric_limits<uint64_t>::max();
  } else {
    // Playing: advance mono clock; filter occasional BASS/Timeline regressions so
    // they cannot un-mark hits and cause duplicate one-shots.
    constexpr int64_t kBassGlitchBackUs = 50000;  // 50ms — ignore as glitch
    if (sfx_mono_us_ < 0 || raw_us >= sfx_mono_us_) {
      sfx_mono_us_ = std::max<int64_t>(0, raw_us);
    } else if (sfx_mono_us_ - raw_us > kBassGlitchBackUs) {
      // Large backwards jump without control_event — treat as scrub fallback.
      release_sfx_clock_control(snapshot, raw_us, playing);
      // Hold / arm already handled inside release when playing.
      return;
    }
    // else: small backwards glitch — keep sfx_mono_us_, do not re-fire.

    collect_due_hit_sfx(snapshot, /*arm=*/true, sfx_mono_us_);
    sfx_was_playing_ = true;
    sync_hold_loop(snapshot, sfx_mono_us_, /*arm=*/true);
  }
}

}  // namespace wds::ui

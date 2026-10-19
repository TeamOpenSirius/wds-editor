#pragma once

#include "draw_types.hpp"

#include <wds/common/page_touch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace wds::renderer {

using TextureId = uint32_t;
inline constexpr TextureId kInvalidTextureId = 0;

// Exact "0" keeps the old triangle-list emit. Unset / other values index
// unique verts (renderer storage only — not a 2022.3 claim).
inline bool split_geometry_indexed_from_env() noexcept {
  static const bool on = []() {
    const char* v = std::getenv("WDS_SPLIT_TRAIL_INDEXED");
    if (v == nullptr || v[0] == '\0') {
      return true;
    }
    return !(v[0] == '0' && v[1] == '\0');
  }();
  return on;
}

struct DrawVertex {
  float x = 0, y = 0, z = 0;
  // Perspective-correct UVs for trapezoids: store (u*q, v*q, q); fragment does uv = (u,v)/q.
  float u = 0, v = 0, q = 1;
  float r = 1, g = 1, b = 1, a = 1;
};

// Joint indices only. Sharing is decided at build time (same source point +
// same colour path) — no per-segment memcmp of vertex bytes.
struct IndexedStripShare {
  uint32_t ia = 0;
  uint32_t ib = 0;
  bool valid = false;
};

struct MeshWrite {
  DrawVertex* v = nullptr;
  uint32_t* i = nullptr;
  uint32_t vbase = 0;
};

// Sprite handle: shared atlas texture id + UV rect in that atlas.
struct TextureInfo {
  TextureId id = kInvalidTextureId;
  int width = 0;
  int height = 0;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 1.0f;
  float v1 = 1.0f;

  explicit operator bool() const noexcept { return id != kInvalidTextureId; }
};

// Vertices are appended into per-texture buckets (no per-frame sort needed).
// Depth buffer resolves cross-texture ordering via vertex z.
// Particle overlay (C6-03): mix Alpha / Additive / Trail-clip in one ordered
// batch. Inherit keeps the pass default (notes mid, line early, hits late).
enum class DrawOverlayPipe : uint8_t {
  Inherit = 0,
  Alpha = 1,
  Additive = 2,
  AdditiveClip01 = 3,
};

struct DrawBucket {
  TextureId texture = kInvalidTextureId;
  // OpenWDS ButterflyEffect.shader has no alpha clip.  Keep this policy on
  // the bucket so the generic particle pipelines retain their documented
  // thresholds while Butterfly can use the exact no-clip variant.
  bool disable_alpha_clip = false;
  std::vector<DrawVertex> vertices;
  std::vector<uint32_t> indices;
  uint8_t overlay_pipe = 0;
};

// Per-texture vertex lists. All writers (add_quad / append_from / UiPainter::flush_to)
// append; callers must clear() (or reset()) at the start of each frame rebuild.
struct DrawBatch {
  std::vector<DrawBucket> buckets;
  // The high bit of the key carries the bucket's alpha-clip policy. TextureId
  // is a 32-bit value, so this cannot collide with the texture-only key.
  std::unordered_map<uint64_t, size_t> bucket_lookup;

  // Optional persistent-mapped VB arena. Tessellation writes here so draw_frame
  // does not memcpy the same verts again. Overflow leaves the CPU path.
  DrawVertex* ext_dst = nullptr;
  uint32_t ext_cap = 0;
  uint32_t ext_used = 0;
  uint32_t* ext_idx = nullptr;
  uint32_t ext_idx_cap = 0;
  uint32_t ext_idx_used = 0;
  uint32_t ext_vert_base = 0;
  bool ext_overflow = false;
  struct ExtRun {
    TextureId texture = kInvalidTextureId;
    bool disable_alpha_clip = false;
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t index_first = 0;
    uint32_t index_count = 0;
    uint8_t overlay_pipe = 0;
  };
  std::vector<ExtRun> ext_runs;
  // Current pipe for the next append. Particle emit sets this per system.
  uint8_t overlay_pipe = 0;
  // Only extend the last live bucket when texture+clip+pipe match. Preserves
  // sortingOrder across blend changes (C6-03). Default lookup still merges.
  bool overlay_ordered = false;
  // How many buckets are live this frame when overlay_ordered. clear() keeps
  // leftover slots for capacity; append reuses from the front.
  size_t overlay_used = 0;

  static uint64_t bucket_lookup_key(TextureId texture, bool disable_alpha_clip,
                                   uint8_t pipe) noexcept {
    return static_cast<uint64_t>(texture) | (disable_alpha_clip ? (uint64_t{1} << 32) : 0u) |
           (static_cast<uint64_t>(pipe) << 40);
  }

  void bind_external(DrawVertex* dst, uint32_t cap, uint32_t* idx = nullptr, uint32_t idx_cap = 0,
                     uint32_t vert_base = 0) {
    ext_dst = dst;
    ext_cap = cap;
    ext_used = 0;
    ext_idx = idx;
    ext_idx_cap = idx_cap;
    ext_idx_used = 0;
    ext_vert_base = vert_base;
    ext_overflow = false;
    ext_runs.clear();
    for (auto& b : buckets) {
      b.vertices.clear();
      b.indices.clear();
    }
  }

  void unbind_external() {
    ext_dst = nullptr;
    ext_cap = 0;
    ext_used = 0;
    ext_idx = nullptr;
    ext_idx_cap = 0;
    ext_idx_used = 0;
    ext_vert_base = 0;
    ext_overflow = false;
    ext_runs.clear();
  }

  bool using_external() const noexcept { return ext_dst != nullptr; }
  bool external_overflow() const noexcept { return ext_overflow; }

  // Drop vertices but keep sticky bucket slots/capacity for reused texture ids.
  // This is the per-frame reset — not optional when the batch is member-owned.
  void clear() {
    for (auto& b : buckets) {
      b.vertices.clear();
      b.indices.clear();
    }
    overlay_used = 0;
    ext_used = 0;
    ext_idx_used = 0;
    ext_overflow = false;
    ext_runs.clear();
  }

  void reset() {
    buckets.clear();
    bucket_lookup.clear();
    overlay_used = 0;
    unbind_external();
  }

  void reserve_quads(size_t n) {
    if (ext_dst != nullptr || buckets.empty()) {
      return;
    }
    // Soft hint: spread across existing buckets if any.
    const size_t per = (n * 6) / buckets.size() + 6;
    for (auto& b : buckets) {
      b.vertices.reserve(std::max(b.vertices.capacity(), per));
    }
  }

  void reserve_verts(size_t n) {
    if (ext_dst != nullptr || buckets.empty()) {
      return;
    }
    const size_t per = n / buckets.size() + 8;
    for (auto& b : buckets) {
      if (b.vertices.capacity() < per) {
        b.vertices.reserve(per);
      }
    }
  }

  // Commit vertex/index pages. overlay_ordered reuses leading slots.
  void precommit_capacity(size_t bucket_n, size_t verts_each) {
    if (ext_dst != nullptr || bucket_n == 0 || verts_each == 0) {
      return;
    }
    if (buckets.size() < bucket_n) {
      buckets.resize(bucket_n);
    }
    const size_t idx_each = verts_each + verts_each / 2 + 8;
    for (auto& b : buckets) {
      if (b.vertices.capacity() < verts_each) {
        b.vertices.reserve(verts_each);
      }
      if (b.indices.capacity() < idx_each) {
        b.indices.reserve(idx_each);
      }
      const size_t vs = b.vertices.size();
      const size_t is = b.indices.size();
      b.vertices.resize(b.vertices.capacity());
      b.indices.resize(b.indices.capacity());
      if (!b.vertices.empty()) {
        wds::common::touch_pages(b.vertices.data(), b.vertices.size() * sizeof(DrawVertex));
      }
      if (!b.indices.empty()) {
        wds::common::touch_pages(b.indices.data(), b.indices.size() * sizeof(uint32_t));
      }
      b.vertices.resize(vs);
      b.indices.resize(is);
    }
  }

  void reserve_bucket_verts(TextureId texture, size_t extra, bool disable_alpha_clip = false) {
    if (ext_dst != nullptr || texture == kInvalidTextureId || extra == 0) {
      return;
    }
    auto& bucket = bucket_for(texture, disable_alpha_clip);
    const size_t need = bucket.vertices.size() + extra;
    if (bucket.vertices.capacity() < need) {
      bucket.vertices.reserve(std::max(need, bucket.vertices.capacity() * 2 + 64));
    }
  }

  void reserve_bucket_indices(TextureId texture, size_t extra, bool disable_alpha_clip = false) {
    if (ext_dst != nullptr || texture == kInvalidTextureId || extra == 0) {
      return;
    }
    auto& bucket = bucket_for(texture, disable_alpha_clip);
    const size_t need = bucket.indices.size() + extra;
    if (bucket.indices.capacity() < need) {
      bucket.indices.reserve(std::max(need, bucket.indices.capacity() * 2 + 64));
    }
  }

  DrawVertex* append_verts(TextureId texture, bool disable_alpha_clip, uint32_t n) {
    if (n == 0 || texture == kInvalidTextureId) {
      return nullptr;
    }
    bucket_for(texture, disable_alpha_clip);
    if (ext_dst != nullptr) {
      if (ext_used + n > ext_cap) {
        ext_overflow = true;
        return nullptr;
      }
      DrawVertex* out = ext_dst + ext_used;
      if (!ext_runs.empty() && ext_runs.back().texture == texture &&
          ext_runs.back().disable_alpha_clip == disable_alpha_clip &&
          ext_runs.back().overlay_pipe == overlay_pipe) {
        ext_runs.back().count += n;
      } else {
        ext_runs.push_back(
            {texture, disable_alpha_clip, ext_used, n, ext_idx_used, 0, overlay_pipe});
      }
      ext_used += n;
      return out;
    }
    auto& bucket = bucket_for(texture, disable_alpha_clip);
    const size_t i = bucket.vertices.size();
    bucket.vertices.resize(i + static_cast<size_t>(n));
    return bucket.vertices.data() + i;
  }

  uint32_t* append_indices(TextureId texture, bool disable_alpha_clip, uint32_t n) {
    if (n == 0 || texture == kInvalidTextureId) {
      return nullptr;
    }
    bucket_for(texture, disable_alpha_clip);
    if (ext_dst != nullptr) {
      if (ext_idx == nullptr || ext_idx_used + n > ext_idx_cap) {
        ext_overflow = true;
        return nullptr;
      }
      uint32_t* out = ext_idx + ext_idx_used;
      if (!ext_runs.empty() && ext_runs.back().texture == texture &&
          ext_runs.back().disable_alpha_clip == disable_alpha_clip &&
          ext_runs.back().overlay_pipe == overlay_pipe) {
        ext_runs.back().index_count += n;
      } else {
        ext_runs.push_back(
            {texture, disable_alpha_clip, ext_used, 0, ext_idx_used, n, overlay_pipe});
      }
      ext_idx_used += n;
      return out;
    }
    auto& bucket = bucket_for(texture, disable_alpha_clip);
    const size_t i = bucket.indices.size();
    bucket.indices.resize(i + static_cast<size_t>(n));
    return bucket.indices.data() + i;
  }

  // One overflow check + one ext_run update. Caller writes vn verts and in
  // indices into the pre-sized spans (resize, no push_back growth).
  MeshWrite append_mesh(TextureId texture, bool disable_alpha_clip, uint32_t vn, uint32_t in) {
    MeshWrite out{};
    if (vn == 0 || texture == kInvalidTextureId) {
      return out;
    }
    out.vbase = current_vertex_base(texture, disable_alpha_clip);
    if (ext_dst != nullptr) {
      if (ext_used + vn > ext_cap ||
          (in > 0 && (ext_idx == nullptr || ext_idx_used + in > ext_idx_cap))) {
        ext_overflow = true;
        return {};
      }
      out.v = ext_dst + ext_used;
      out.i = in > 0 ? ext_idx + ext_idx_used : nullptr;
      if (!ext_runs.empty() && ext_runs.back().texture == texture &&
          ext_runs.back().disable_alpha_clip == disable_alpha_clip &&
          ext_runs.back().overlay_pipe == overlay_pipe) {
        ext_runs.back().count += vn;
        ext_runs.back().index_count += in;
      } else {
        ext_runs.push_back(
            {texture, disable_alpha_clip, ext_used, vn, ext_idx_used, in, overlay_pipe});
      }
      ext_used += vn;
      ext_idx_used += in;
      return out;
    }
    auto& bucket = bucket_for(texture, disable_alpha_clip);
    const size_t vs = bucket.vertices.size();
    const size_t is = bucket.indices.size();
    bucket.vertices.resize(vs + vn);
    if (in > 0) {
      bucket.indices.resize(is + in);
    }
    out.v = bucket.vertices.data() + vs;
    out.i = in > 0 ? bucket.indices.data() + is : nullptr;
    return out;
  }

  uint32_t current_vertex_base(TextureId texture, bool disable_alpha_clip) const {
    if (ext_dst != nullptr) {
      return ext_vert_base + ext_used;
    }
    if (overlay_ordered) {
      if (overlay_used > 0) {
        const auto& last = buckets[overlay_used - 1];
        if (last.texture == texture && last.disable_alpha_clip == disable_alpha_clip &&
            last.overlay_pipe == overlay_pipe) {
          return static_cast<uint32_t>(last.vertices.size());
        }
      }
      return 0;
    }
    for (const auto& b : buckets) {
      if (b.texture == texture && b.disable_alpha_clip == disable_alpha_clip &&
          b.overlay_pipe == overlay_pipe) {
        return static_cast<uint32_t>(b.vertices.size());
      }
    }
    return 0;
  }

  DrawBucket& bucket_for(TextureId texture, bool disable_alpha_clip = false) {
    if (overlay_ordered) {
      if (overlay_used > 0) {
        DrawBucket& last = buckets[overlay_used - 1];
        if (last.texture == texture && last.disable_alpha_clip == disable_alpha_clip &&
            last.overlay_pipe == overlay_pipe) {
          return last;
        }
      }
      if (overlay_used < buckets.size()) {
        DrawBucket& slot = buckets[overlay_used++];
        slot.texture = texture;
        slot.disable_alpha_clip = disable_alpha_clip;
        slot.overlay_pipe = overlay_pipe;
        slot.vertices.clear();
        slot.indices.clear();
        return slot;
      }
      buckets.push_back(DrawBucket{texture, disable_alpha_clip, {}, {}, overlay_pipe});
      ++overlay_used;
      return buckets.back();
    }
    const uint64_t key = bucket_lookup_key(texture, disable_alpha_clip, overlay_pipe);
    const auto it = bucket_lookup.find(key);
    if (it != bucket_lookup.end()) {
      return buckets[it->second];
    }
    bucket_lookup.emplace(key, buckets.size());
    buckets.push_back(DrawBucket{texture, disable_alpha_clip, {}, {}, overlay_pipe});
    return buckets.back();
  }

  // Sticky slots append on first use. A texture imported after the first
  // frame would otherwise draw last (depth write off → later buckets win).
  // Jacket sortingOrder −11 must stay in front of ingame_bg −10 / lanes.
  void pin_bucket_front(TextureId texture, bool disable_alpha_clip = false) {
    if (overlay_ordered) {
      bucket_for(texture, disable_alpha_clip);
      return;
    }
    bucket_for(texture, disable_alpha_clip);
    const uint64_t key = bucket_lookup_key(texture, disable_alpha_clip, overlay_pipe);
    const auto it = bucket_lookup.find(key);
    if (it == bucket_lookup.end() || it->second == 0) {
      return;
    }
    const size_t idx = it->second;
    DrawBucket pinned = std::move(buckets[idx]);
    for (size_t i = idx; i > 0; --i) {
      buckets[i] = std::move(buckets[i - 1]);
    }
    buckets[0] = std::move(pinned);
    bucket_lookup.clear();
    for (size_t i = 0; i < buckets.size(); ++i) {
      const auto& b = buckets[i];
      bucket_lookup.emplace(bucket_lookup_key(b.texture, b.disable_alpha_clip, b.overlay_pipe), i);
    }
  }

  // Appends non-empty buckets from `src`. Does not clear this batch first.
  void append_from(const DrawBatch& src) {
    if (src.ext_dst != nullptr) {
      for (const auto& run : src.ext_runs) {
        if (run.count == 0 || src.ext_dst == nullptr) {
          continue;
        }
        overlay_pipe = run.overlay_pipe;
        const uint32_t vbase = current_vertex_base(run.texture, run.disable_alpha_clip);
        DrawVertex* dst = append_verts(run.texture, run.disable_alpha_clip, run.count);
        if (dst == nullptr) {
          return;
        }
        std::memcpy(dst, src.ext_dst + run.first, static_cast<size_t>(run.count) * sizeof(DrawVertex));
        if (run.index_count > 0 && src.ext_idx != nullptr) {
          uint32_t* di = append_indices(run.texture, run.disable_alpha_clip, run.index_count);
          if (di == nullptr) {
            return;
          }
          for (uint32_t i = 0; i < run.index_count; ++i) {
            const uint32_t src_i = src.ext_idx[run.index_first + i];
            di[i] = vbase + (src_i - (src.ext_vert_base + run.first));
          }
        }
      }
      return;
    }
    for (const auto& bucket : src.buckets) {
      if (bucket.vertices.empty()) {
        continue;
      }
      overlay_pipe = bucket.overlay_pipe;
      const uint32_t vbase = current_vertex_base(bucket.texture, bucket.disable_alpha_clip);
      DrawVertex* dst = append_verts(bucket.texture, bucket.disable_alpha_clip,
                                    static_cast<uint32_t>(bucket.vertices.size()));
      if (dst == nullptr) {
        return;
      }
      std::memcpy(dst, bucket.vertices.data(), bucket.vertices.size() * sizeof(DrawVertex));
      if (!bucket.indices.empty()) {
        uint32_t* di = append_indices(bucket.texture, bucket.disable_alpha_clip,
                                      static_cast<uint32_t>(bucket.indices.size()));
        if (di == nullptr) {
          return;
        }
        for (size_t i = 0; i < bucket.indices.size(); ++i) {
          di[i] = vbase + bucket.indices[i];
        }
      }
    }
  }

  void add_quad(TextureId texture, const Quad& quad, float z, float alpha, float u0 = 0.0f,
                float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f, float r = 1.0f,
                float g = 1.0f, float b = 1.0f) {
    add_quad_corners(texture, quad, z, alpha, alpha, alpha, alpha, u0, v0, u1, v1, r, g, b);
  }

  void add_quad_corners(TextureId texture, const Quad& quad, float z, float a_lb, float a_rb,
                        float a_lt, float a_rt, float u0 = 0.0f, float v0 = 0.0f,
                        float u1 = 1.0f, float v1 = 1.0f, float r = 1.0f, float g = 1.0f,
                        float b = 1.0f, bool disable_alpha_clip = false) {
    add_quad_corners(texture, quad, z, a_lb, a_rb, a_lt, a_rt, u0, v0, u1, v1, r, g, b, r, g, b,
                     r, g, b, r, g, b, disable_alpha_clip);
  }

  // Per-corner RGB + alpha (GPU interpolates along the strip — soft tip/pulse gradients).
  void add_quad_corners(TextureId texture, const Quad& quad, float z, float a_lb, float a_rb,
                        float a_lt, float a_rt, float u0, float v0, float u1, float v1,
                        float r_lb, float g_lb, float b_lb, float r_rb, float g_rb, float b_rb,
                        float r_lt, float g_lt, float b_lt, float r_rt, float g_rt, float b_rt,
                        bool disable_alpha_clip = false) {
    if (texture == kInvalidTextureId) {
      return;
    }
    if (a_lb <= 0.0f && a_rb <= 0.0f && a_lt <= 0.0f && a_rt <= 0.0f) {
      return;
    }
    DrawVertex geom[6];
    fill_quad_corner_geom(quad, z, u0, v0, u1, v1, geom);
    add_quad_from_geom(texture, geom, a_lb, a_rb, a_lt, a_rt, r_lb, g_lb, b_lb, r_rb, g_rb, b_rb,
                       r_lt, g_lt, b_lt, r_rt, g_rt, b_rt, disable_alpha_clip);
  }

  // Same 6-vert order as add_quad_corners (lb, lt, rt, lb, rt, rb). `geom` already
  // stores projected (u*q, v*q, q).
  void add_quad_from_geom(TextureId texture, const DrawVertex geom[6], float a_lb, float a_rb,
                         float a_lt, float a_rt, float r_lb, float g_lb, float b_lb, float r_rb,
                         float g_rb, float b_rb, float r_lt, float g_lt, float b_lt, float r_rt,
                         float g_rt, float b_rt, bool disable_alpha_clip = false) {
    if (texture == kInvalidTextureId) {
      return;
    }
    if (a_lb <= 0.0f && a_rb <= 0.0f && a_lt <= 0.0f && a_rt <= 0.0f) {
      return;
    }
    const float as[6] = {a_lb, a_lt, a_rt, a_lb, a_rt, a_rb};
    const float rs[6] = {r_lb, r_lt, r_rt, r_lb, r_rt, r_rb};
    const float gs[6] = {g_lb, g_lt, g_rt, g_lb, g_rt, g_rb};
    const float bs[6] = {b_lb, b_lt, b_rt, b_lb, b_rt, b_rb};
    auto paint = [&](DrawVertex& vert, int k) {
      vert = geom[k];
      vert.r = rs[k];
      vert.g = gs[k];
      vert.b = bs[k];
      vert.a = as[k];
    };
    if (split_geometry_indexed_from_env()) {
      const MeshWrite w = append_mesh(texture, disable_alpha_clip, 4, 6);
      if (w.v == nullptr || w.i == nullptr) {
        return;
      }
      paint(w.v[0], 0);
      paint(w.v[1], 1);
      paint(w.v[2], 2);
      paint(w.v[3], 5);
      w.i[0] = w.vbase;
      w.i[1] = w.vbase + 1;
      w.i[2] = w.vbase + 2;
      w.i[3] = w.vbase;
      w.i[4] = w.vbase + 2;
      w.i[5] = w.vbase + 3;
      return;
    }
    DrawVertex* verts = append_verts(texture, disable_alpha_clip, 6);
    if (verts == nullptr) {
      return;
    }
    for (int k = 0; k < 6; ++k) {
      paint(verts[k], k);
    }
  }

  // Corner positions + projective UVs only (lb, lt, rt, lb, rt, rb). Time-independent.
  static void fill_quad_corner_geom(const Quad& quad, float z, float u0, float v0, float u1,
                                    float v1, DrawVertex out[6]) {
    auto push = [&](int i, float x, float y, float u, float v, float q) {
      const float qq = std::max(q, 1e-6f);
      out[i].x = x;
      out[i].y = y;
      out[i].z = z;
      out[i].u = u * qq;
      out[i].v = v * qq;
      out[i].q = qq;
    };

    auto emit = [&](float q_lb, float q_rb, float q_lt, float q_rt) {
      push(0, quad.lb.x, quad.lb.y, u0, v0, q_lb);
      push(1, quad.lt.x, quad.lt.y, u0, v1, q_lt);
      push(2, quad.rt.x, quad.rt.y, u1, v1, q_rt);
      push(3, quad.lb.x, quad.lb.y, u0, v0, q_lb);
      push(4, quad.rt.x, quad.rt.y, u1, v1, q_rt);
      push(5, quad.rb.x, quad.rb.y, u1, v0, q_rb);
    };

    // Affine fast path: axis-aligned rects (UI / edit grid / most note caps).
    const bool axis_aligned = std::abs(quad.lb.y - quad.rb.y) < 1e-5f &&
                              std::abs(quad.lt.y - quad.rt.y) < 1e-5f &&
                              std::abs(quad.lb.x - quad.lt.x) < 1e-5f &&
                              std::abs(quad.rb.x - quad.rt.x) < 1e-5f;
    if (axis_aligned) {
      emit(1.0f, 1.0f, 1.0f, 1.0f);
      return;
    }

    auto len = [](Vec2 a, Vec2 b) {
      const float dx = b.x - a.x;
      const float dy = b.y - a.y;
      return std::sqrt(dx * dx + dy * dy);
    };

    // Projective texture mapping for trapezoids (e.g. #STAGE_COVER lane lines).
    // Strip+affine made constant-U lines into staircases; store (u*q,v*q,q) so the
    // fragment `uv = uvq.xy/uvq.z` recovers the homography even with flat 2D positions.
    // q from diagonal-intersection ratios (Heckbert / Reedbeta).
    const float bottom = std::max(len(quad.lb, quad.rb), 1e-8f);
    const float top = std::max(len(quad.lt, quad.rt), 1e-8f);
    const float taper = std::max(bottom, top) / std::min(bottom, top);

    float q_lb = 1.0f, q_rb = 1.0f, q_lt = 1.0f, q_rt = 1.0f;
    if (taper > 1.01f) {
      // Prefer parallel-edge ratio when top/bottom are roughly horizontal (stage).
      const float dy_l = std::abs(quad.lt.y - quad.lb.y);
      const float dy_r = std::abs(quad.rt.y - quad.rb.y);
      const bool parallel_trap =
          dy_l > 1e-5f && dy_r > 1e-5f &&
          std::abs(quad.lb.y - quad.rb.y) < 0.02f * dy_l &&
          std::abs(quad.lt.y - quad.rt.y) < 0.02f * dy_l;

      if (parallel_trap) {
        // Bottom (near/wide) q=1, top (far/narrow) q = top/bottom.
        q_lb = q_rb = 1.0f;
        q_lt = q_rt = top / bottom;
      } else {
        // General convex quad: diagonal intersection.
        const Vec2 a = quad.lb, b = quad.rb, c = quad.rt, d = quad.lt;
        const float den = (a.x - c.x) * (b.y - d.y) - (a.y - c.y) * (b.x - d.x);
        if (std::abs(den) > 1e-12f) {
          const float t =
              ((a.x - b.x) * (b.y - d.y) - (a.y - b.y) * (b.x - d.x)) / den;
          const Vec2 o{a.x + t * (c.x - a.x), a.y + t * (c.y - a.y)};
          q_lb = std::max(len(o, c), 1e-6f);
          q_rt = std::max(len(o, a), 1e-6f);
          q_rb = std::max(len(o, d), 1e-6f);
          q_lt = std::max(len(o, b), 1e-6f);
          // Normalize so average q ≈ 1 (keeps u*q in a stable range).
          const float inv =
              4.0f / std::max(q_lb + q_rb + q_lt + q_rt, 1e-6f);
          q_lb *= inv;
          q_rb *= inv;
          q_lt *= inv;
          q_rt *= inv;
        }
      }
    }

    emit(q_lb, q_rb, q_lt, q_rt);
  }

  void add_sprite(const TextureInfo& sprite, const Quad& quad, float z, float alpha,
                  float r = 1.0f, float g = 1.0f, float b = 1.0f) {
    if (!sprite) {
      return;
    }
    add_quad(sprite.id, quad, z, alpha, sprite.u0, sprite.v0, sprite.u1, sprite.v1, r, g, b);
  }

  // ButterflyEffect.shader returns its sampled color without clip().  This
  // preserves that local contract without changing generic additive sprites.
  void add_sprite_no_alpha_clip(const TextureInfo& sprite, const Quad& quad, float z, float alpha,
                                float r = 1.0f, float g = 1.0f, float b = 1.0f) {
    if (!sprite) {
      return;
    }
    add_quad_corners(sprite.id, quad, z, alpha, alpha, alpha, alpha, sprite.u0, sprite.v0,
                     sprite.u1, sprite.v1, r, g, b, r, g, b, r, g, b, r, g, b, true);
  }

  // One triangle with caller UVs. add_sprite remaps a UV *rect* onto four
  // corners — after view clip that puts the sprite edge on the cut. Unity
  // 2022.3 interpolates vertex TEXCOORD across the triangle
  // (docs.unity3d.com/2022.3/Documentation/Manual/SL-ShaderSemantics.html).
  // q is the perspective weight (1/camera_z); fragment does uv = (u,v)/q.
  void add_tri_uv(TextureId texture, Vec2 p0, Vec2 p1, Vec2 p2, float z, float ua, float va,
                  float qa, float ub, float vb, float qb, float uc, float vc, float qc,
                  float alpha, float red = 1.0f, float green = 1.0f, float blue = 1.0f) {
    if (texture == kInvalidTextureId || alpha <= 0.0f) {
      return;
    }
    auto write = [&](DrawVertex& vert, Vec2 p, float u, float v, float q) {
      const float qq = std::max(q, 1e-6f);
      vert.x = p.x;
      vert.y = p.y;
      vert.z = z;
      vert.u = u * qq;
      vert.v = v * qq;
      vert.q = qq;
      vert.r = red;
      vert.g = green;
      vert.b = blue;
      vert.a = alpha;
    };
    if (split_geometry_indexed_from_env()) {
      const MeshWrite w = append_mesh(texture, false, 3, 3);
      if (w.v == nullptr || w.i == nullptr) {
        return;
      }
      write(w.v[0], p0, ua, va, qa);
      write(w.v[1], p1, ub, vb, qb);
      write(w.v[2], p2, uc, vc, qc);
      w.i[0] = w.vbase;
      w.i[1] = w.vbase + 1;
      w.i[2] = w.vbase + 2;
      return;
    }
    DrawVertex* verts = append_verts(texture, false, 3);
    if (verts == nullptr) {
      return;
    }
    write(verts[0], p0, ua, va, qa);
    write(verts[1], p1, ub, vb, qb);
    write(verts[2], p2, uc, vc, qc);
  }

  // Fan 0,i,i+1. Indexed path writes unique verts + the same triangle order.
  // Adjacent unclipped quads share the last two unique verts when the caller
  // already knows the joint is the same source + colour (allow_share).
  void add_uv_fan(TextureId texture, bool disable_alpha_clip, const DrawVertex* unique, uint32_t n,
                  IndexedStripShare* share, bool allow_share = false) {
    if (texture == kInvalidTextureId || unique == nullptr || n < 3) {
      if (share != nullptr) {
        share->valid = false;
      }
      return;
    }
    if (!split_geometry_indexed_from_env()) {
      for (uint32_t i = 1; i + 1 < n; ++i) {
        DrawVertex* verts = append_verts(texture, disable_alpha_clip, 3);
        if (verts == nullptr) {
          return;
        }
        verts[0] = unique[0];
        verts[1] = unique[i];
        verts[2] = unique[i + 1];
      }
      if (share != nullptr) {
        share->valid = false;
      }
      return;
    }
    if (n == 4 && allow_share && share != nullptr && share->valid) {
      const MeshWrite w = append_mesh(texture, disable_alpha_clip, 2, 6);
      if (w.v == nullptr || w.i == nullptr) {
        share->valid = false;
        return;
      }
      w.v[0] = unique[2];
      w.v[1] = unique[3];
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
    const uint32_t tri_n = 3u * (n - 2u);
    const MeshWrite w = append_mesh(texture, disable_alpha_clip, n, tri_n);
    if (w.v == nullptr || w.i == nullptr) {
      if (share != nullptr) {
        share->valid = false;
      }
      return;
    }
    uint32_t iw = 0;
    for (uint32_t i = 0; i < n; ++i) {
      w.v[i] = unique[i];
      if (i >= 2) {
        w.i[iw++] = w.vbase;
        w.i[iw++] = w.vbase + (i - 1);
        w.i[iw++] = w.vbase + i;
      }
    }
    if (share != nullptr && n == 4) {
      share->ia = w.vbase + 3;
      share->ib = w.vbase + 2;
      share->valid = true;
    } else if (share != nullptr) {
      share->valid = false;
    }
  }

  // Vertical alpha gradient: lb/rb use alpha_near, lt/rt use alpha_far (hold past-fade).
  void add_sprite_vfade(const TextureInfo& sprite, const Quad& quad, float z, float alpha_near,
                        float alpha_far, float r = 1.0f, float g = 1.0f, float b = 1.0f) {
    if (!sprite) {
      return;
    }
    add_quad_corners(sprite.id, quad, z, alpha_near, alpha_near, alpha_far, alpha_far, sprite.u0,
                     sprite.v0, sprite.u1, sprite.v1, r, g, b);
  }

  size_t vertex_count() const {
    if (ext_dst != nullptr) {
      return static_cast<size_t>(ext_used);
    }
    size_t n = 0;
    for (const auto& b : buckets) {
      n += b.vertices.size();
    }
    return n;
  }

  size_t index_count() const {
    if (ext_idx != nullptr) {
      return static_cast<size_t>(ext_idx_used);
    }
    size_t n = 0;
    for (const auto& b : buckets) {
      n += b.indices.size();
    }
    return n;
  }

  size_t expanded_vertex_count() const {
    const size_t idx = index_count();
    return idx > 0 ? idx : vertex_count();
  }

  static void expand_bucket_list(const DrawBucket& bucket, std::vector<DrawVertex>& out) {
    out.clear();
    if (!bucket.indices.empty()) {
      out.resize(bucket.indices.size());
      for (size_t i = 0; i < bucket.indices.size(); ++i) {
        const uint32_t vi = bucket.indices[i];
        if (vi < bucket.vertices.size()) {
          out[i] = bucket.vertices[vi];
        }
      }
      return;
    }
    out = bucket.vertices;
  }

  void expand_list(std::vector<DrawVertex>& out) const {
    out.clear();
    out.reserve(expanded_vertex_count());
    if (ext_dst != nullptr) {
      if (ext_idx != nullptr && ext_idx_used > 0) {
        for (uint32_t i = 0; i < ext_idx_used; ++i) {
          const uint32_t vi = ext_idx[i] - ext_vert_base;
          if (vi < ext_used) {
            out.push_back(ext_dst[vi]);
          }
        }
        return;
      }
      out.insert(out.end(), ext_dst, ext_dst + ext_used);
      return;
    }
    for (const auto& bucket : buckets) {
      std::vector<DrawVertex> part;
      expand_bucket_list(bucket, part);
      out.insert(out.end(), part.begin(), part.end());
    }
  }
};

}  // namespace wds::renderer

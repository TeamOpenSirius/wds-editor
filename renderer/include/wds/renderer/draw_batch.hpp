#pragma once

#include "draw_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wds::renderer {

using TextureId = uint32_t;
inline constexpr TextureId kInvalidTextureId = 0;

struct DrawVertex {
  float x = 0, y = 0, z = 0;
  // Perspective-correct UVs for trapezoids: store (u*q, v*q, q); fragment does uv = (u,v)/q.
  float u = 0, v = 0, q = 1;
  float r = 1, g = 1, b = 1, a = 1;
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
struct DrawBucket {
  TextureId texture = kInvalidTextureId;
  std::vector<DrawVertex> vertices;
};

// Per-texture vertex lists. All writers (add_quad / append_from / UiPainter::flush_to)
// append; callers must clear() (or reset()) at the start of each frame rebuild.
struct DrawBatch {
  std::vector<DrawBucket> buckets;
  std::unordered_map<TextureId, size_t> bucket_lookup;

  // Drop vertices but keep sticky bucket slots/capacity for reused texture ids.
  // This is the per-frame reset — not optional when the batch is member-owned.
  void clear() {
    for (auto& b : buckets) {
      b.vertices.clear();
    }
  }

  void reset() {
    buckets.clear();
    bucket_lookup.clear();
  }

  void reserve_quads(size_t n) {
    if (buckets.empty()) {
      return;
    }
    // Soft hint: spread across existing buckets if any.
    const size_t per = (n * 6) / buckets.size() + 6;
    for (auto& b : buckets) {
      b.vertices.reserve(std::max(b.vertices.capacity(), per));
    }
  }

  DrawBucket& bucket_for(TextureId texture) {
    const auto it = bucket_lookup.find(texture);
    if (it != bucket_lookup.end()) {
      return buckets[it->second];
    }
    bucket_lookup.emplace(texture, buckets.size());
    buckets.push_back(DrawBucket{texture, {}});
    return buckets.back();
  }

  // Appends non-empty buckets from `src`. Does not clear this batch first.
  void append_from(const DrawBatch& src) {
    for (const auto& bucket : src.buckets) {
      if (bucket.vertices.empty()) {
        continue;
      }
      auto& dst = bucket_for(bucket.texture);
      dst.vertices.insert(dst.vertices.end(), bucket.vertices.begin(), bucket.vertices.end());
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
                        float b = 1.0f) {
    add_quad_corners(texture, quad, z, a_lb, a_rb, a_lt, a_rt, u0, v0, u1, v1, r, g, b, r, g, b,
                     r, g, b, r, g, b);
  }

  // Per-corner RGB + alpha (GPU interpolates along the strip — soft tip/pulse gradients).
  void add_quad_corners(TextureId texture, const Quad& quad, float z, float a_lb, float a_rb,
                        float a_lt, float a_rt, float u0, float v0, float u1, float v1,
                        float r_lb, float g_lb, float b_lb, float r_rb, float g_rb, float b_rb,
                        float r_lt, float g_lt, float b_lt, float r_rt, float g_rt, float b_rt) {
    if (texture == kInvalidTextureId) {
      return;
    }
    if (a_lb <= 0.0f && a_rb <= 0.0f && a_lt <= 0.0f && a_rt <= 0.0f) {
      return;
    }
    auto& bucket = bucket_for(texture);

    auto push = [&](float x, float y, float u, float v, float q, float a, float r, float g,
                    float b) {
      const float qq = std::max(q, 1e-6f);
      DrawVertex vert;
      vert.x = x;
      vert.y = y;
      vert.z = z;
      vert.u = u * qq;
      vert.v = v * qq;
      vert.q = qq;
      vert.r = r;
      vert.g = g;
      vert.b = b;
      vert.a = a;
      bucket.vertices.push_back(vert);
    };

    auto emit = [&](float q_lb, float q_rb, float q_lt, float q_rt) {
      push(quad.lb.x, quad.lb.y, u0, v0, q_lb, a_lb, r_lb, g_lb, b_lb);
      push(quad.lt.x, quad.lt.y, u0, v1, q_lt, a_lt, r_lt, g_lt, b_lt);
      push(quad.rt.x, quad.rt.y, u1, v1, q_rt, a_rt, r_rt, g_rt, b_rt);
      push(quad.lb.x, quad.lb.y, u0, v0, q_lb, a_lb, r_lb, g_lb, b_lb);
      push(quad.rt.x, quad.rt.y, u1, v1, q_rt, a_rt, r_rt, g_rt, b_rt);
      push(quad.rb.x, quad.rb.y, u1, v0, q_rb, a_rb, r_rb, g_rb, b_rb);
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
    size_t n = 0;
    for (const auto& b : buckets) {
      n += b.vertices.size();
    }
    return n;
  }
};

}  // namespace wds::renderer

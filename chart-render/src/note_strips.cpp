#include <wds/chart_render/note_strips.hpp>
#include <wds/chart_render/note_visual_policy.hpp>

#include <algorithm>
#include <cmath>

namespace wds::renderer {
namespace {

float dist(Vec2 a, Vec2 b) noexcept {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

void add_sliced_note_impl(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                          float border_l_px, float border_r_px, float z, float alpha_near,
                          float alpha_far, float border_scale_px, float r, float g, float b,
                          float v0, float v1, float dest_world_width) {
  if (!sprite || (alpha_near <= 0.0f && alpha_far <= 0.0f)) {
    return;
  }
  const float tex_w = static_cast<float>(std::max(1, sprite.width));
  const float tex_h = static_cast<float>(std::max(1, sprite.height));

  // UV caps stay fixed fractions of the source sprite (Unity sliced mesh UVs).
  float u_bl = std::clamp(border_l_px / tex_w, 0.0f, 0.49f);
  float u_br = std::clamp(border_r_px / tex_w, 0.0f, 0.49f);
  if (u_bl + u_br > 0.98f) {
    const float s = 0.98f / (u_bl + u_br);
    u_bl *= s;
    u_br *= s;
  }

  const Vec2 lb = quad.lb;
  const Vec2 lt = quad.lt;
  const Vec2 rb = quad.rb;
  const Vec2 rt = quad.rt;

  const float dest_w = 0.5f * (dist(lb, rb) + dist(lt, rt));
  const float dest_h = 0.5f * (dist(lb, lt) + dist(rb, rt));
  if (dest_w <= 1e-4f || dest_h <= 1e-4f) {
    return;
  }

  float bl = 0.0f;
  float br = 0.0f;
  bool emit_middle = true;
  if (dest_world_width > 1e-6f) {
    const auto layout = wds::chart_render::sliced_cap_layout(border_l_px, border_r_px,
                                                            dest_world_width);
    bl = layout.bl;
    br = layout.br;
    emit_middle = layout.emit_middle;
  } else {
    // Edit timeline: height-relative caps, shrink if they cannot fit.
    float scale = border_scale_px;
    if (scale < 0.0f) {
      scale = dest_h / tex_h;
    }
    float left_cap = std::max(0.0f, border_l_px) * scale;
    float right_cap = std::max(0.0f, border_r_px) * scale;
    if (left_cap + right_cap > dest_w && left_cap + right_cap > 1e-4f) {
      const float s = dest_w / (left_cap + right_cap);
      left_cap *= s;
      right_cap *= s;
    }
    bl = left_cap / dest_w;
    br = right_cap / dest_w;
    emit_middle = (bl + br) < 1.0f - 1e-5f;
  }

  auto lerp2 = [](Vec2 a, Vec2 b, float t) {
    return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
  };
  // Alpha along each vertical edge (near = lb/rb, far = lt/rt).
  auto lerp1 = [](float a, float b, float t) { return a + (b - a) * t; };

  const float u0 = sprite.u0;
  const float u1 = sprite.u1;
  const float du = u1 - u0;
  const float u_l = u0 + du * u_bl;
  const float u_r = u1 - du * u_br;

  const float a_lb = alpha_near;
  const float a_rb = alpha_near;
  const float a_lt = alpha_far;
  const float a_rt = alpha_far;

  // Left cap
  {
    const Vec2 lb_m = lerp2(lb, rb, bl);
    const Vec2 lt_m = lerp2(lt, rt, bl);
    const float a_rb_m = lerp1(a_lb, a_rb, bl);
    const float a_rt_m = lerp1(a_lt, a_rt, bl);
    batch.add_quad_corners(sprite.id, Quad{lb, lt, lt_m, lb_m}, z, a_lb, a_rb_m, a_lt, a_rt_m, u0,
                           v0, u_l, v1, r, g, b);
  }
  // Middle stretch (skipped when Unity-style caps overlap).
  if (emit_middle) {
    const Vec2 lb_m = lerp2(lb, rb, bl);
    const Vec2 lt_m = lerp2(lt, rt, bl);
    const Vec2 rb_m = lerp2(rb, lb, br);
    const Vec2 rt_m = lerp2(rt, lt, br);
    const float a_lb_m = lerp1(a_lb, a_rb, bl);
    const float a_lt_m = lerp1(a_lt, a_rt, bl);
    const float a_rb_m = lerp1(a_rb, a_lb, br);
    const float a_rt_m = lerp1(a_rt, a_lt, br);
    batch.add_quad_corners(sprite.id, Quad{lb_m, lt_m, rt_m, rb_m}, z, a_lb_m, a_rb_m, a_lt_m,
                           a_rt_m, u_l, v0, u_r, v1, r, g, b);
  }
  // Right cap
  {
    const Vec2 rb_m = lerp2(rb, lb, br);
    const Vec2 rt_m = lerp2(rt, lt, br);
    const float a_lb_m = lerp1(a_rb, a_lb, br);
    const float a_lt_m = lerp1(a_rt, a_lt, br);
    batch.add_quad_corners(sprite.id, Quad{rb_m, rt_m, rt, rb}, z, a_lb_m, a_rb, a_lt_m, a_rt, u_r,
                           v0, u1, v1, r, g, b);
  }
}

}  // namespace

void add_sliced_note(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                     float border_l_px, float border_r_px, float z, float alpha_near,
                     float alpha_far, float border_scale_px, float r, float g, float b,
                     float dest_world_width) {
  add_sliced_note_impl(batch, sprite, quad, border_l_px, border_r_px, z, alpha_near, alpha_far,
                       border_scale_px, r, g, b, sprite.v0, sprite.v1, dest_world_width);
}

void add_sliced_note_v(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                       float border_l_px, float border_r_px, float z, float alpha,
                       float border_scale_px, float v_near_atlas, float v_far_atlas, float r,
                       float g, float b, float dest_world_width) {
  add_sliced_note_impl(batch, sprite, quad, border_l_px, border_r_px, z, alpha, alpha,
                       border_scale_px, r, g, b, v_near_atlas, v_far_atlas, dest_world_width);
}

}  // namespace wds::renderer

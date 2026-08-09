#include <wds/chart_render/note_strips.hpp>

#include <algorithm>

namespace wds::renderer {

void add_sliced_note(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                     float border_l_px, float border_r_px, float z, float alpha, float r,
                     float g, float b) {
  if (!sprite || alpha <= 0.0f) {
    return;
  }
  const float tex_w = static_cast<float>(std::max(1, sprite.width));
  float bl = std::clamp(border_l_px / tex_w, 0.0f, 0.49f);
  float br = std::clamp(border_r_px / tex_w, 0.0f, 0.49f);
  if (bl + br > 0.98f) {
    const float s = 0.98f / (bl + br);
    bl *= s;
    br *= s;
  }

  auto lerp2 = [](Vec2 a, Vec2 b, float t) {
    return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
  };

  const Vec2 lb = quad.lb;
  const Vec2 lt = quad.lt;
  const Vec2 rb = quad.rb;
  const Vec2 rt = quad.rt;

  const float u0 = sprite.u0;
  const float u1 = sprite.u1;
  const float v0 = sprite.v0;
  const float v1 = sprite.v1;
  const float du = u1 - u0;
  const float u_l = u0 + du * bl;
  const float u_r = u1 - du * br;

  // Left cap
  {
    const Vec2 lb_m = lerp2(lb, rb, bl);
    const Vec2 lt_m = lerp2(lt, rt, bl);
    batch.add_quad(sprite.id, Quad{lb, lt, lt_m, lb_m}, z, alpha, u0, v0, u_l, v1, r, g, b);
  }
  // Middle stretch
  {
    const Vec2 lb_m = lerp2(lb, rb, bl);
    const Vec2 lt_m = lerp2(lt, rt, bl);
    const Vec2 rb_m = lerp2(rb, lb, br);
    const Vec2 rt_m = lerp2(rt, lt, br);
    batch.add_quad(sprite.id, Quad{lb_m, lt_m, rt_m, rb_m}, z, alpha, u_l, v0, u_r, v1, r, g, b);
  }
  // Right cap
  {
    const Vec2 rb_m = lerp2(rb, lb, br);
    const Vec2 rt_m = lerp2(rt, lt, br);
    batch.add_quad(sprite.id, Quad{rb_m, rt_m, rt, rb}, z, alpha, u_r, v0, u1, v1, r, g, b);
  }
}

}  // namespace wds::renderer

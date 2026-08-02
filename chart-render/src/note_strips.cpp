#include <wds/chart_render/note_strips.hpp>

#include <algorithm>

namespace wds::renderer {

void add_note_strips(DrawBatch& batch, const TextureInfo& left, const TextureInfo& middle,
                     const TextureInfo& right, const Quad& quad, float border_percent,
                     int32_t lane_span, float z, float alpha) {
  const float span = static_cast<float>(std::max(1, lane_span));
  const float border_frac = std::clamp(border_percent * 12.0f / span, 0.02f, 0.45f);

  auto lerp2 = [](Vec2 a, Vec2 b, float t) {
    return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
  };

  const Vec2 lb = quad.lb;
  const Vec2 lt = quad.lt;
  const Vec2 rb = quad.rb;
  const Vec2 rt = quad.rt;

  const Vec2 lb_m = lerp2(lb, rb, border_frac);
  const Vec2 lt_m = lerp2(lt, rt, border_frac);
  const Vec2 rb_m = lerp2(rb, lb, border_frac);
  const Vec2 rt_m = lerp2(rt, lt, border_frac);

  if (left) {
    batch.add_sprite(left, Quad{lb, lt, lt_m, lb_m}, z, alpha);
  }
  if (middle) {
    batch.add_sprite(middle, Quad{lb_m, lt_m, rt_m, rb_m}, z, alpha);
  }
  if (right) {
    batch.add_sprite(right, Quad{rb_m, rt_m, rt, rb}, z, alpha);
  }
}

}  // namespace wds::renderer

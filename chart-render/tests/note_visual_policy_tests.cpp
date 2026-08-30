#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/chart_render/skin_catalog.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

using wds::chart_render::AnimatedArrowLayoutParams;
using wds::chart_render::StaticArrowLayoutParams;
using wds::chart_render::border_scale_from_ppu;
using wds::chart_render::hold_tail_layers;
using wds::chart_render::layout_animated_scratch_arrows;
using wds::chart_render::layout_static_scratch_arrows;
using wds::chart_render::scratch_arrow_sides;
using wds::renderer::SkinCatalog;
using wds::renderer::TextureId;

void test_border_scale_from_ppu_matches_unity_corners() {
  // dest units per source pixel = dest_w / (world_w * PPU).
  // 65px @ 100 ppu on a 0.765-wide note → each cap is 0.65/0.765 of dest_w.
  const float dest_w = 0.8639f;
  const float world_w = 0.765f;
  const float scale = border_scale_from_ppu(dest_w, world_w, 100.0f);
  const float cap = 65.0f * scale;
  assert(std::abs(cap - dest_w * (0.65f / 0.765f)) < 1e-5f);
  assert(cap * 2.0f > dest_w);
}

void test_sliced_caps_overlap_instead_of_shrinking() {
  using wds::chart_render::sliced_cap_layout;
  const auto layout = sliced_cap_layout(65.0f, 65.0f, 0.765f, 100.0f);
  assert(layout.bl > 0.84f);
  assert(layout.br > 0.84f);
  assert(layout.bl + layout.br > 1.0f);
  assert(!layout.emit_middle);
}

void test_scratch_arrow_sides() {
  assert(scratch_arrow_sides(-3).draw_left && !scratch_arrow_sides(-3).draw_right);
  assert(!scratch_arrow_sides(4).draw_left && scratch_arrow_sides(4).draw_right);
  assert(scratch_arrow_sides(0).draw_left && scratch_arrow_sides(0).draw_right);
}

void test_static_arrows_respect_sides() {
  StaticArrowLayoutParams p;
  p.span_left = 0.0f;
  p.span_right = 100.0f;
  p.arrow_w = 10.0f;
  p.scratch_length = -2;
  auto left_only = layout_static_scratch_arrows(p);
  assert(!left_only.empty());
  for (const auto& a : left_only) {
    assert(!a.flip_x);
  }
  p.scratch_length = 2;
  auto right_only = layout_static_scratch_arrows(p);
  assert(!right_only.empty());
  for (const auto& a : right_only) {
    assert(a.flip_x);
  }
}

void test_animated_arrows_bidirectional_half_density() {
  AnimatedArrowLayoutParams p;
  p.span_left = 0.0f;
  p.span_right = 100.0f;
  p.arrow_w = 10.0f;
  p.scratch_length = 0;
  p.sonolus_num = 8.0f;
  p.anim_time_sec = 0.0f;
  p.arrow_speed = 1.0f;
  auto both = layout_animated_scratch_arrows(p);
  p.scratch_length = 1;
  auto right = layout_animated_scratch_arrows(p);
  // Bidirectional uses half density per side → fewer arrows than full directional.
  assert(both.size() < right.size() * 2);
  assert(!both.empty());
}

void test_hold_tail_layers() {
  SkinCatalog skin;
  skin.note_bottom.id = static_cast<TextureId>(1);
  skin.note_blue_top.id = static_cast<TextureId>(2);
  skin.note_purple_top.id = static_cast<TextureId>(3);
  auto hold = hold_tail_layers(skin, false);
  assert(hold.bottom.id == static_cast<TextureId>(1));
  assert(hold.top.id == static_cast<TextureId>(2));
  assert(!hold.is_scratch_family);
  auto scratch = hold_tail_layers(skin, true);
  assert(scratch.top.id == static_cast<TextureId>(3));
  assert(scratch.is_scratch_family);
}

}  // namespace

int main() {
  test_border_scale_from_ppu_matches_unity_corners();
  test_sliced_caps_overlap_instead_of_shrinking();
  test_scratch_arrow_sides();
  test_static_arrows_respect_sides();
  test_animated_arrows_bidirectional_half_density();
  test_hold_tail_layers();
  std::puts("note_visual_policy_tests OK");
  return 0;
}

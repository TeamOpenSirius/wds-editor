#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/chart_render/skin_catalog.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

using wds::chart_render::AnimatedArrowLayoutParams;
using wds::chart_render::StaticArrowLayoutParams;
using wds::chart_render::border_scale_from_flat_height;
using wds::chart_render::hold_tail_layers;
using wds::chart_render::layout_animated_scratch_arrows;
using wds::chart_render::layout_static_scratch_arrows;
using wds::chart_render::scratch_arrow_sides;
using wds::renderer::SkinCatalog;
using wds::renderer::TextureId;

void test_border_scale_uses_flat_height_not_pixels_mixed() {
  // NDC-sized flat height (typical note quad ~0.05–0.2); must not use raw px.
  const float scale = border_scale_from_flat_height(0.108f, 108.0f);
  assert(std::abs(scale - 0.001f) < 1e-6f);
  const float wide = border_scale_from_flat_height(0.216f, 108.0f);
  assert(std::abs(wide - 0.002f) < 1e-6f);
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
  test_border_scale_uses_flat_height_not_pixels_mixed();
  test_scratch_arrow_sides();
  test_static_arrows_respect_sides();
  test_animated_arrows_bidirectional_half_density();
  test_hold_tail_layers();
  std::puts("note_visual_policy_tests OK");
  return 0;
}

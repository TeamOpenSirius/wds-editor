#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/chart_render/skin_catalog.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                                          \
  do {                                                                                       \
    if (!(cond)) {                                                                           \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);         \
      std::abort();                                                                          \
    }                                                                                        \
  } while (0)

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

void test_sliced_caps_shrink_when_they_cannot_fit() {
  using wds::chart_render::sliced_cap_layout;
  // Temporary approximation (not Unity Sliced): 1-wide tap is 0.765, each
  // border_px/PPU cap is 0.65, so 1.30 > 0.765. Scale both caps by
  // 0.765/1.30 so they sit side-by-side instead of overlapping.
  const auto narrow = sliced_cap_layout(65.0f, 65.0f, 0.765f, 100.0f);
  CHECK(std::abs(narrow.bl - 0.5f) < 1e-5f);
  CHECK(std::abs(narrow.br - 0.5f) < 1e-5f);
  CHECK(narrow.bl + narrow.br <= 1.0f + 1e-5f);
  CHECK(!narrow.emit_middle);

  const float four = 4.0f * 0.915f + 3.0f * 0.01f - 0.15f;
  const auto wide = sliced_cap_layout(65.0f, 65.0f, four, 100.0f);
  const float raw = 0.65f / four;
  CHECK(std::abs(wide.bl - raw) < 1e-5f);
  CHECK(std::abs(wide.br - raw) < 1e-5f);
  CHECK(wide.emit_middle);
}

void test_concurrent_line_sliced_caps_stay_four_pixels() {
  using wds::chart_render::sliced_cap_layout;
  // NoteConcurrentLine m_Border L/R=4 @ 100 ppu → 0.04wu each. A 4-lane
  // notation span (~3.69) must keep those caps and still emit a solid middle.
  const float four = 4.0f * 0.915f + 3.0f * 0.01f;
  const auto layout = sliced_cap_layout(4.0f, 4.0f, four, 100.0f);
  const float raw = 0.04f / four;
  CHECK(std::abs(layout.bl - raw) < 1e-5f);
  CHECK(std::abs(layout.br - raw) < 1e-5f);
  CHECK(layout.emit_middle);
  CHECK(layout.bl + layout.br < 0.05f);
}

void test_scratch_arrow_sides() {
  assert(scratch_arrow_sides(-3).draw_left && !scratch_arrow_sides(-3).draw_right);
  assert(!scratch_arrow_sides(4).draw_left && scratch_arrow_sides(4).draw_right);
  assert(scratch_arrow_sides(0).draw_left && scratch_arrow_sides(0).draw_right);
  assert(scratch_arrow_sides_compat(false, -3).draw_left &&
         !scratch_arrow_sides_compat(false, -3).draw_right);
  assert(scratch_arrow_sides_compat(true, 0).draw_left &&
         !scratch_arrow_sides_compat(true, 0).draw_right);
  assert(!scratch_arrow_sides_compat(true, 1).draw_left &&
         scratch_arrow_sides_compat(true, 1).draw_right);
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

void test_animated_arrows_bidirectional_uses_same_count_per_side() {
  AnimatedArrowLayoutParams p;
  p.span_left = 0.0f;
  p.span_right = 100.0f;
  p.arrow_w = 10.0f;
  p.arrow_step = 5.0f;
  p.arrow_count = 5;
  p.group_offset = 40.0f;
  p.scratch_length = 0;
  p.anim_time_sec = 0.0f;
  p.arrow_speed = 1.0f;
  auto both = layout_animated_scratch_arrows(p);
  p.scratch_length = 1;
  auto right = layout_animated_scratch_arrows(p);
  // Official: same ActivateArrowSpriteRenderer count; OneDirection hides one GO.
  CHECK(right.size() == 5);
  CHECK(both.size() == 10);
}

void test_official_bidirectional_does_not_cross_center() {
  // FlickNoteEntity width=4, flick: count 5, interval 0.36, scale 0.7.
  // Visual note = width - margin = 3.54 → ±1.77. No UV clip.
  AnimatedArrowLayoutParams p;
  p.span_left = -1.77f;
  p.span_right = 1.77f;
  p.arrow_w = 0.68f * 0.7f;
  p.arrow_step = 0.36f * 0.7f;
  p.group_offset = 3.69f * 0.5f - 0.145f;
  p.arrow_count = 5;
  p.scratch_length = 0;
  p.anim_time_sec = 0.0f;
  p.arrow_speed = 1.0f;
  const auto both = layout_animated_scratch_arrows(p);
  CHECK(both.size() == 10);
  int left_n = 0;
  int right_n = 0;
  float left_last = -1e9f;
  float right_first = 1e9f;
  for (const auto& a : both) {
    CHECK(std::abs(a.u0) < 1e-5f);
    CHECK(std::abs(a.u1 - 1.0f) < 1e-5f);
    if (a.flip_x) {
      ++right_n;
      right_first = std::min(right_first, std::min(a.x0, a.x1));
    } else {
      ++left_n;
      left_last = std::max(left_last, std::max(a.x0, a.x1));
    }
  }
  CHECK(left_n == 5);
  CHECK(right_n == 5);
  // Each side stays on its half — the previous fill+clip path stacked both
  // directions across the whole note (the diamond / X overlap).
  CHECK(left_last < 0.0f);
  CHECK(right_first > 0.0f);
}

void test_one_way_fills_without_tail_or_uv_clip() {
  AnimatedArrowLayoutParams p;
  p.span_left = -1.77f;
  p.span_right = 1.77f;
  p.arrow_w = 0.68f * 0.7f;
  p.arrow_step = 0.36f * 0.7f;
  p.group_offset = 3.69f * 0.5f - 0.145f;
  p.arrow_count = 5;
  p.fill_to_far_edge = true;
  p.scratch_length = -1;
  p.anim_time_sec = 0.0f;
  p.arrow_speed = 1.0f;
  const auto left = layout_animated_scratch_arrows(p);
  CHECK(left.size() > 5);
  for (const auto& a : left) {
    CHECK(!a.flip_x);
    CHECK(std::abs(a.u0) < 1e-5f);
    CHECK(std::abs(a.u1 - 1.0f) < 1e-5f);
    CHECK(std::max(a.x0, a.x1) <= p.span_right + 1e-4f);
  }
  // Official first head sits past the near edge (no UV clip).
  CHECK(std::min(left.front().x0, left.front().x1) < p.span_left);
  // Last tail stays inside; the chain must reach the far half.
  CHECK(std::max(left.back().x0, left.back().x1) > 0.0f);
  CHECK(std::max(left.back().x0, left.back().x1) > p.span_right * 0.5f);
}

void test_official_scratch_arrow_table() {
  using wds::chart_editor::official_scratch_arrow_count;
  using wds::chart_editor::official_scratch_arrow_interval;
  // FlickNoteEntity.SetActive (dump.cs / libil2cpp): width pairs 1-2..11-12.
  CHECK(official_scratch_arrow_count(1, false) == 3);
  CHECK(official_scratch_arrow_count(2, false) == 3);
  CHECK(official_scratch_arrow_count(4, false) == 5);
  CHECK(official_scratch_arrow_count(4, true) == 11);
  CHECK(official_scratch_arrow_count(12, false) == 20);
  CHECK(official_scratch_arrow_count(12, true) == 42);
  CHECK(std::abs(official_scratch_arrow_interval(1) - 0.35f) < 1e-6f);
  CHECK(std::abs(official_scratch_arrow_interval(3) - 0.35f) < 1e-6f);
  CHECK(std::abs(official_scratch_arrow_interval(4) - 0.36f) < 1e-6f);
  CHECK(std::abs(official_scratch_arrow_interval(12) - 0.36f) < 1e-6f);
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
  test_sliced_caps_shrink_when_they_cannot_fit();
  test_concurrent_line_sliced_caps_stay_four_pixels();
  test_scratch_arrow_sides();
  test_static_arrows_respect_sides();
  test_animated_arrows_bidirectional_uses_same_count_per_side();
  test_official_bidirectional_does_not_cross_center();
  test_one_way_fills_without_tail_or_uv_clip();
  test_official_scratch_arrow_table();
  test_hold_tail_layers();
  std::puts("note_visual_policy_tests OK");
  return 0;
}

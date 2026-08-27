#include <wds/chart_render/preview_visual_config.hpp>
#include <wds/chart_render/split_line_fade.hpp>
#include <wds/chart_render/split_line_official_colors.hpp>
#include <wds/chart_render/split_soft_profile.hpp>
#include <wds/chart_render/stage_geometry.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/renderer/texture.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using wds::chart_render::official_split_color_ids;
using wds::chart_render::official_split_line_color;
using wds::chart_render::split_line_pulse_peak;
using wds::chart_render::split_line_whiten_t;

int g_fails = 0;

void check(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_fails;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

bool near4(int32_t id, int32_t slot, float r, float g, float b, float a) {
  float cr = 0, cg = 0, cb = 0, ca = -1;
  if (!official_split_line_color(id, slot, cr, cg, cb, ca)) {
    return false;
  }
  return std::fabs(cr - r) < 0.02f && std::fabs(cg - g) < 0.02f && std::fabs(cb - b) < 0.02f &&
         std::fabs(ca - a) < 0.02f;
}

void test_4txt_official_element_slots() {
  // Official prefab Line index (before world X flip).
  CHECK(near4(11611, 0, 1.0f, 0.25f, 0.25f, 1.0f));
  CHECK(near4(11611, 1, 0.0f, 0.0f, 0.0f, 0.0f));
  CHECK(near4(11612, 5, 0.2123f, 0.3401f, 1.0f, 1.0f));
  CHECK(near4(11612, 1, 0.0f, 0.0f, 0.0f, 0.0f));
  CHECK(near4(11616, 1, 1.0f, 0.6332f, 0.3915f, 1.0f));
  CHECK(near4(11613, 2, 1.0f, 0.9373f, 0.3066f, 1.0f));
  CHECK(near4(11614, 3, 0.3066f, 1.0f, 0.3847f, 1.0f));
  CHECK(near4(11615, 4, 0.8239f, 0.3632f, 1.0f, 1.0f));
  CHECK(near4(11617, 6, 1.0f, 1.0f, 1.0f, 1.0f));
  CHECK(near4(10390, 0, 0.0f, 0.7373f, 0.1765f, 1.0f));
  CHECK(near4(10390, 3, 0.0f, 0.7373f, 0.1765f, 1.0f));  // broadcast

  // 10518 official controller order (10518_5 … 10518_1), not a second mirror.
  CHECK(near4(10518, 0, 0.9098f, 0.3333f, 0.4941f, 1.0f));
  CHECK(near4(10518, 4, 0.3059f, 0.3569f, 0.6588f, 1.0f));

  float r, g, b, a;
  CHECK(!official_split_line_color(0, 0, r, g, b, a));

  const auto ids = official_split_color_ids();
  CHECK(ids.size() == 318);
  CHECK(ids.front() == 1);
}

void test_official_default_split_line_opacity_is_100() {
  // GameSettings ctor / ResetGameSettings: SplitEffectLineOpacity = 100.
  CHECK(std::fabs(wds::renderer::PreviewVisualConfig{}.split_line_opacity - 1.0f) < 1e-5f);
}

void test_official_split_fade_out_window_is_300ms() {
  // SplitEffect_fadeOut_anim alpha keys: t=0 → 1, t=0.3 → 0.
  CHECK(std::fabs(wds::renderer::PreviewVisualConfig{}.split_line_animation_end - 0.3f) < 1e-5f);
  CHECK(std::fabs(wds::renderer::PreviewVisualConfig{}.split_line_animation_start - 1.0f) <
        1e-5f);
}

void test_apply_split_line_opacity_keeps_alpha() {
  // Official Initialize: RGB *= settings/100; SpriteRenderer.a stays _lineColor.a.
  float r = 1.0f, g = 0.25f, b = 0.25f, a = 1.0f;
  wds::chart_render::apply_split_line_opacity(r, g, b, a, 0.40f, 1.0f);
  CHECK(std::fabs(r - 0.40f) < 1e-5f);
  CHECK(std::fabs(g - 0.10f) < 1e-5f);
  CHECK(std::fabs(b - 0.10f) < 1e-5f);
  CHECK(std::fabs(a - 1.0f) < 1e-5f);

  r = 1.0f;
  g = 0.25f;
  b = 0.25f;
  a = 1.0f;
  wds::chart_render::apply_split_line_opacity(r, g, b, a, 0.40f, 0.5f);
  CHECK(std::fabs(r - 0.40f) < 1e-5f);
  CHECK(std::fabs(a - 0.5f) < 1e-5f);

  r = 1.0f;
  g = 1.0f;
  b = 1.0f;
  a = 0.0f;
  wds::chart_render::apply_split_line_opacity(r, g, b, a, 0.40f, 1.0f);
  CHECK(std::fabs(a - 0.0f) < 1e-5f);
}

void test_write_white_soft_texel() {
  unsigned char px[4] = {12, 34, 56, 78};
  wds::renderer::write_white_soft_texel(px, 0.5f);
  CHECK(px[0] == 255);
  CHECK(px[1] == 255);
  CHECK(px[2] == 255);
  CHECK(px[3] == 128);
}

void test_whiten_and_pulse_anchor() {
  using wds::chart_editor::official_judgeline_percent;
  using wds::chart_editor::official_split_visible_tip_span;
  const float p0 = 0.0f;
  const float p1 = 1.0f;
  const float span = official_split_visible_tip_span(p0, p1, false);
  CHECK(std::fabs(span - (45.0f / 256.0f)) < 1e-5f);
  // Identity: VFX Height/256 cap at the far/upward tip (p0).
  CHECK(split_line_whiten_t(p0, p0, p1, span, false) > 0.85f);
  CHECK(split_line_whiten_t(p0 + span * 0.5f, p0, p1, span, false) > 0.70f);
  CHECK(split_line_whiten_t(p0 + span, p0, p1, span, false) < 0.15f);
  CHECK(split_line_whiten_t(0.5f, p0, p1, span, false) < 0.05f);
  // z=180 steady: VFX Height=45 (12.15 wu) from the mesh tip intersects
  // the visible ribbon — a large near-end cap, not a hidden 5% trim.
  const float full_z180 = official_split_visible_tip_span(p0, p1, true);
  CHECK(full_z180 > 0.40f);
  CHECK(full_z180 < 0.70f);
  CHECK(split_line_whiten_t(p1, p0, p1, full_z180, true) > 0.85f);
  CHECK(split_line_whiten_t(official_judgeline_percent(), p0, p1, full_z180, true) > 0.40f);
  const float mid_end = 0.45f;
  const float grow = official_split_visible_tip_span(p0, mid_end, true);
  CHECK(grow > 0.15f);
  CHECK(grow < 0.40f);
  CHECK(split_line_whiten_t(mid_end, p0, mid_end, grow, true) > 0.85f);

  CHECK(std::fabs(split_line_pulse_peak(0.2f, false) - 0.8f) < 1e-5f);
  CHECK(std::fabs(split_line_pulse_peak(0.2f, true) - 0.2f) < 1e-5f);
}

void test_split_line_quad_follows_official_projection() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const auto& stage = geometry.stage();
  CHECK(std::fabs(stage.w / std::max(stage.h, 1e-6f) -
                  wds::chart_editor::kOfficialPreviewAspect) < 0.02f);

  // Percent is screen-space; world Y along the lane is the perspective curve.
  using wds::chart_editor::official_percent_to_main_y;
  const float y0 = official_percent_to_main_y(0.25f);
  const float y1 = official_percent_to_main_y(0.50f);
  const float y2 = official_percent_to_main_y(0.75f);
  CHECK(std::fabs((y1 - y0) - (y2 - y1)) > 1.0f);

  const float p0 = geometry.note_percent(0.0, 0.0);
  CHECK(std::fabs(p0 - geometry.judgeline_percent()) < 0.02f);
  CHECK(p0 > 0.70f);
  CHECK(p0 < 0.80f);
}

void test_note_top_bottom_official_spacing() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const float p_far = geometry.note_percent(2.0, 0.0);
  const float p_near = geometry.note_percent(0.25, 0.0);
  CHECK(p_far < p_near);
  CHECK(p_near < geometry.judgeline_percent());

  const auto bot_far = geometry.note_quad(5, 5, p_far, -0.01f);
  const auto top_far = geometry.note_quad(5, 5, p_far, -0.1f);
  const auto bot_near = geometry.note_quad(5, 5, p_near, -0.01f);
  const auto top_near = geometry.note_quad(5, 5, p_near, -0.1f);
  auto cy = [](const wds::renderer::Quad& q) {
    return 0.25f * (q.lb.y + q.lt.y + q.rb.y + q.rt.y);
  };
  CHECK(cy(top_far) > cy(bot_far));
  CHECK(cy(top_near) > cy(bot_near));
  CHECK(std::fabs(cy(top_near) - cy(bot_near)) > std::fabs(cy(top_far) - cy(bot_far)));

  // Fake pinhole (cam_height=12, vanish at stage.t) is not the official spacing.
  const auto& stage = geometry.stage();
  auto fake = [&](const wds::renderer::Quad& q, float z) {
    const float h = -0.01f - z;
    const float factor = (12.0f - h) / 12.0f;
    return stage.t + (cy(q) - stage.t) * factor;
  };
  CHECK(std::fabs(cy(top_near) - fake(bot_near, -0.1f)) > 0.002f);
}

void test_stage_covers_frustum_and_extends_past_judge() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const auto q = geometry.stage_quad();
  const auto& content = geometry.content();
  const float top = 0.5f * (q.lt.y + q.rt.y);
  const float bot = 0.5f * (q.lb.y + q.rb.y);
  // Camera frustum only — no percent overscan past the 16:9 content.
  CHECK(std::fabs(top - content.t) < 0.02f);
  CHECK(std::fabs(bot - content.b) < 0.02f);
  CHECK(bot >= content.b - 0.005f);
  CHECK(top <= content.t + 0.005f);
  const auto& j = geometry.judgeline();
  const float jy = 0.5f * (j.lb_y + j.lt_y);
  CHECK(bot < jy - 0.02f);
}

void test_hidden_line_uses_official_lane_mask_height() {
  CHECK(std::fabs(wds::renderer::PreviewVisualConfig{}.hidden_line_height -
                  wds::chart_editor::kOfficialStartLineSpriteHeight) < 1e-5f);
  CHECK(wds::renderer::PreviewVisualConfig{}.hidden_line_height > 4.9f);
}

void test_lane_mask_clips_projected_note_not_plate_near_edge() {
  using wds::chart_editor::kOfficialNoteLocalZTop;
  using wds::chart_editor::official_lane_mask_bottom_percent;
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);

  CHECK(std::fabs(geometry.lane_mask_bottom_percent() - official_lane_mask_bottom_percent(0)) <
        1e-6f);
  // Mask bottom is closer to the judgeline than the StartLine sprite center.
  CHECK(geometry.lane_mask_bottom_percent() > geometry.hidden_line_center_percent());

  const float mask_y = geometry.lane_mask_bottom_content_y();
  const float p_mask = geometry.lane_mask_bottom_percent();
  const float half = std::max(geometry.note_half_height_percent(5, p_mask), 1e-4f);
  const float p_near = p_mask + half;
  const float p_tip = p_mask - half;
  auto q = geometry.note_span_quad(5, 5, p_near, p_tip, kOfficialNoteLocalZTop);
  const float tip_before = 0.5f * (q.lt.y + q.rt.y);
  CHECK(tip_before > mask_y);

  float far_t = 1.0f;
  CHECK(geometry.clip_quad_outside_lane_mask(q, far_t));
  CHECK(far_t > 0.0f);
  CHECK(far_t < 1.0f);
  const float tip_after = 0.5f * (q.lt.y + q.rt.y);
  CHECK(std::fabs(tip_after - mask_y) < 1e-4f);

  auto q_bot = geometry.note_span_quad(5, 5, p_near, p_tip,
                                      wds::chart_editor::kOfficialNoteLocalZBottom);
  float far_bot = 1.0f;
  CHECK(geometry.clip_quad_outside_lane_mask(q_bot, far_bot));
  // Top z is farther into the mask, so more of it is clipped.
  CHECK(far_t < far_bot);

  // Must not use the old plate near-edge (center + half-height).
  const float old_clip_p =
      geometry.hidden_line_center_percent() + geometry.hidden_line_half_percent();
  CHECK(old_clip_p > geometry.lane_mask_bottom_percent() + 0.001f);
}

void test_judgeline_uses_official_sprite_height() {
  CHECK(std::fabs(wds::renderer::PreviewVisualConfig{}.judgeline_height -
                  wds::chart_editor::kOfficialJudgeSpriteHeight) < 1e-5f);
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const auto& j = geometry.judgeline();
  const float h = std::fabs(0.5f * (j.lt_y + j.rt_y) - 0.5f * (j.lb_y + j.rb_y));
  CHECK(h > 0.04f);
}

void test_visual_lane_borders_are_every_two_columns() {
  using wds::chart_editor::kOfficialVisualBorderEdgeCount;
  using wds::chart_editor::official_visual_border_edge_index;
  CHECK(kOfficialVisualBorderEdgeCount == 7);
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const auto left = geometry.lane_border_quad(official_visual_border_edge_index(0));
  const auto mid = geometry.lane_border_quad(official_visual_border_edge_index(3));
  const auto right = geometry.lane_border_quad(official_visual_border_edge_index(6));
  const auto odd = geometry.lane_border_quad(1);
  auto cx = [](const wds::renderer::Quad& q) {
    return 0.25f * (q.lb.x + q.lt.x + q.rb.x + q.rt.x);
  };
  CHECK(cx(left) < cx(mid));
  CHECK(cx(mid) < cx(right));
  // A 12-column inner edge must not sit on a visual-track border.
  CHECK(std::fabs(cx(odd) - cx(left)) > 0.01f);
  CHECK(std::fabs(cx(odd) - cx(mid)) > 0.01f);
}

fs::path find_skins_png(const char* name) {
  for (fs::path dir = fs::current_path(); !dir.empty() && dir != dir.parent_path();
       dir = dir.parent_path()) {
    const fs::path cand = dir / "skins" / name;
    if (fs::is_regular_file(cand)) {
      return cand;
    }
  }
  return {};
}

void test_official_lane_border_png_is_six_track_sprite() {
  const auto png = find_skins_png("img_ingame_lane_border2.png");
  CHECK(!png.empty());
  if (png.empty()) {
    return;
  }
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
  CHECK(wds::renderer::load_png_rgba8(png.string(), rgba, w, h));
  CHECK(w == wds::chart_editor::kOfficialLaneBorderSpriteWidth);
  CHECK(h == wds::chart_editor::kOfficialLaneBorderSpriteHeight);
}

void test_official_start_line_png_is_vertical_plate() {
  const auto png = find_skins_png("img_game_common_start_line_500.png");
  CHECK(!png.empty());
  if (png.empty()) {
    return;
  }
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
  CHECK(wds::renderer::load_png_rgba8(png.string(), rgba, w, h));
  CHECK(w == 2);
  CHECK(h == 498);
  const size_t mid = (static_cast<size_t>(h / 2) * static_cast<size_t>(w)) * 4;
  CHECK(rgba[mid + 0] < 50);
  CHECK(rgba[mid + 3] > 180);
}

void test_official_judgment_png_is_twelve_cells() {
  CHECK(wds::chart_editor::kOfficialJudgeSpritePixelWidth == 1119);
  CHECK(wds::chart_editor::kOfficialJudgeSpritePixelHeight == 72);
  const auto png = find_skins_png("img_ingame_judgment_area3.png");
  CHECK(!png.empty());
  if (png.empty()) {
    return;
  }
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
  CHECK(wds::renderer::load_png_rgba8(png.string(), rgba, w, h));
  CHECK(w == wds::chart_editor::kOfficialJudgeSpritePixelWidth);
  CHECK(h == wds::chart_editor::kOfficialJudgeSpritePixelHeight);
}

void test_bomb_quads_follow_judge_plane_perspective() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const auto square = geometry.bomb_frame_quad(4, 7, 1.0f, 0.68f);
  const float near_w = std::fabs(square.rb.x - square.lb.x);
  const float far_w = std::fabs(square.rt.x - square.lt.x);
  CHECK(near_w > far_w + 0.005f);
  CHECK(square.lt.y > square.lb.y);
  // Not an axis-aligned screen rect (old bug).
  CHECK(std::fabs(square.lt.x - square.lb.x) > 0.002f);

  const auto flare = geometry.bomb_flare_billboard_quad(4, 7, 8.0f);
  const auto flare_wide = geometry.bomb_flare_billboard_quad(0, 11, 8.0f);
  CHECK(std::fabs(flare.lt.x - flare.lb.x) < 1e-5f);
  CHECK(std::fabs(flare.lt.y - flare.rt.y) < 1e-5f);
  CHECK(std::fabs((flare.rb.x - flare.lb.x) - (flare_wide.rb.x - flare_wide.lb.x)) < 1e-5f);
  CHECK(std::fabs((flare.lt.y - flare.lb.y) - (flare.rb.x - flare.lb.x)) < 1e-5f);
}

void test_star_quad_is_centered_official_size_not_span_width() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const float p = 0.42f;
  const auto tick = geometry.tick_quad(2, 8, p);
  const auto star = geometry.star_quad(2, 8, p);
  const auto star_narrow = geometry.star_quad(5, 5, p);
  const auto one_lane = geometry.note_quad(5, 5, p);
  auto cx = [](const wds::renderer::Quad& q) {
    return 0.25f * (q.lb.x + q.lt.x + q.rb.x + q.rt.x);
  };
  auto width = [](const wds::renderer::Quad& q) { return std::fabs(q.rb.x - q.lb.x); };
  CHECK(std::fabs(cx(star) - cx(tick)) < 0.03f);
  CHECK(width(star) < width(tick) * 0.40f);
  CHECK(std::fabs(width(star) - width(star_narrow)) < 0.01f);
  // Official SoundNote 1.12 vs 1-lane note 0.915.
  CHECK(width(star) > width(one_lane) * 1.05f);
  CHECK(width(star) < width(one_lane) * 1.40f);
}

void test_content_aspect_is_official_16_9() {
  CHECK(std::fabs(wds::renderer::PreviewVisualConfig{}.target_aspect_ratio -
                  wds::chart_editor::kOfficialPreviewAspect) < 1e-6f);
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const auto& stage = geometry.stage();
  CHECK(std::fabs(stage.w / std::max(stage.h, 1e-6f) -
                  wds::chart_editor::kOfficialPreviewAspect) < 0.03f);

  const auto& j = geometry.judgeline();
  const float half_w = 0.5f * std::fabs(j.rb_x - j.lb_x);
  const float content_half = 0.5f * geometry.content().w;
  CHECK(half_w < content_half);
  CHECK(half_w > content_half * 0.70f);
}

}  // namespace

int main() {
  test_4txt_official_element_slots();
  test_official_default_split_line_opacity_is_100();
  test_official_split_fade_out_window_is_300ms();
  test_apply_split_line_opacity_keeps_alpha();
  test_write_white_soft_texel();
  test_whiten_and_pulse_anchor();
  test_split_line_quad_follows_official_projection();
  test_note_top_bottom_official_spacing();
  test_stage_covers_frustum_and_extends_past_judge();
  test_hidden_line_uses_official_lane_mask_height();
  test_lane_mask_clips_projected_note_not_plate_near_edge();
  test_judgeline_uses_official_sprite_height();
  test_visual_lane_borders_are_every_two_columns();
  test_official_lane_border_png_is_six_track_sprite();
  test_official_start_line_png_is_vertical_plate();
  test_official_judgment_png_is_twelve_cells();
  test_bomb_quads_follow_judge_plane_perspective();
  test_star_quad_is_centered_official_size_not_span_width();
  test_content_aspect_is_official_16_9();
  if (g_fails == 0) {
    std::printf("All split_line official color tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", g_fails);
  return 1;
}

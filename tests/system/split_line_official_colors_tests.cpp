#include <wds/chart_render/preview_visual_config.hpp>
#include <wds/chart_render/shuriken/evaluate.hpp>
#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/chart_render/split_line_fade.hpp>
#include <wds/chart_render/split_line_official_colors.hpp>
#include <wds/chart_render/split_soft_profile.hpp>
#include <wds/chart_render/stage_geometry.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/official_split_x.hpp>
#include <wds/core/preview_snapshot.hpp>
#include <wds/core/split_effect_gate.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/renderer/texture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <utility>
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

void test_official_split_fade_out_window_is_0_3s() {
  // Runtime animators.bundle SplitEffect_fadeOut_anim alpha keys: t=0 → 1, t=0.3 → 0.
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

void test_split_line_tip_follows_opacity() {
  // Body RGB *= k; tip whitening must land on (k,k,k), not leftover (1,1,1).
  float r = 0, g = 0, b = 0;
  wds::chart_render::split_line_tinted_rgb(1.0f, 0.25f, 0.25f, 0.0f, 0.40f, r, g, b);
  CHECK(std::fabs(r - 0.40f) < 1e-5f);
  CHECK(std::fabs(g - 0.10f) < 1e-5f);
  CHECK(std::fabs(b - 0.10f) < 1e-5f);

  wds::chart_render::split_line_tinted_rgb(1.0f, 0.25f, 0.25f, 1.0f, 0.40f, r, g, b);
  CHECK(std::fabs(r - 0.40f) < 1e-5f);
  CHECK(std::fabs(g - 0.40f) < 1e-5f);
  CHECK(std::fabs(b - 0.40f) < 1e-5f);

  wds::chart_render::split_line_tinted_rgb(1.0f, 0.25f, 0.25f, 1.0f, 1.0f, r, g, b);
  CHECK(std::fabs(r - 1.0f) < 1e-5f);
  CHECK(std::fabs(g - 1.0f) < 1e-5f);
  CHECK(std::fabs(b - 1.0f) < 1e-5f);
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
  // Identity at official 720p: 45/256 ribbon floor. Scales with stage height.
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

#ifndef WDS_TEST_REPO_ROOT
#error "WDS_TEST_REPO_ROOT must be injected by CMake (repo root containing skins/)"
#endif

fs::path find_skins_png(const char* name) {
  const fs::path cand = fs::path(WDS_TEST_REPO_ROOT) / "skins" / name;
  if (fs::is_regular_file(cand)) {
    return cand;
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

void test_adjacent_tap_notes_leave_official_margin_gap() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const float p = geometry.judgeline_percent() - 0.02f;
  const auto left = geometry.note_quad(5, 5, p);
  const auto right = geometry.note_quad(6, 6, p);
  auto width = [](const wds::renderer::Quad& q) {
    return 0.5f * (std::fabs(q.rb.x - q.lb.x) + std::fabs(q.rt.x - q.lt.x));
  };
  const float note_w = width(left);
  const float gap = 0.5f * ((right.lb.x - left.rb.x) + (right.lt.x - left.rt.x));
  // Official: visual 0.765, lane pitch 0.925 → gap 0.16 → gap/width ≈ 0.209.
  // Notation-only path: gap 0.01 → gap/width ≈ 0.011.
  CHECK(gap / std::max(note_w, 1e-6f) > 0.12f);
  CHECK(gap > 0.0f);
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
  // Official SoundNote 1.12 vs 1-lane tap visual 0.765 (notation 0.915 − 0.15).
  CHECK(width(star) > width(one_lane) * 1.30f);
  CHECK(width(star) < width(one_lane) * 1.70f);
}

void test_star_quad_ignores_note_height_level() {
  wds::renderer::PreviewVisualConfig thin;
  thin.note_height_level = 1;
  wds::renderer::PreviewVisualConfig thick;
  thick.note_height_level = 10;
  wds::renderer::StageGeometry a;
  wds::renderer::StageGeometry b;
  a.configure(thin);
  b.configure(thick);
  a.resize(1280, 720);
  b.resize(1280, 720);
  const float p = 0.42f;
  const auto sa = a.star_quad(5, 5, p);
  const auto sb = b.star_quad(5, 5, p);
  const auto na = a.note_quad(5, 5, p);
  const auto nb = b.note_quad(5, 5, p);
  auto height = [](const wds::renderer::Quad& q) {
    return 0.5f * (std::fabs(q.lt.y - q.lb.y) + std::fabs(q.rt.y - q.rb.y));
  };
  CHECK(std::fabs(height(sa) - height(sb)) < 1e-5f);
  CHECK(height(nb) > height(na) + 1e-5f);
}

void test_jacket_quad_ndc_width_is_half_screen_at_16_9() {
  // OpenWDS SpriteRendererScreenFitter cover-fit × Jacket localScale 0.6 × 8 world
  // (GameBackground.prefab + ingame_bg 19.2×11.8 @ 100 ppu) → NDC width 0.50 at 16:9.
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const auto q = geometry.jacket_quad();
  const float ndc_w = (q.rb.x - q.lb.x) / (geometry.content().w * 0.5f);
  CHECK(std::fabs(ndc_w - 0.50f) < 1e-4f);
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

void test_split_view_billboard_uses_z_roll_and_pivot() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const float x = 0.0f;
  const float y = 10.0f;
  const float size = 1.0f;
  const auto upright = geometry.split_particle_oriented_quad(x, y, size, 2.0f, 0.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 0);
  auto extents = [](const wds::renderer::Quad& q, float& w, float& h) {
    const float xs[] = {q.lb.x, q.rb.x, q.lt.x, q.rt.x};
    const float ys[] = {q.lb.y, q.rb.y, q.lt.y, q.rt.y};
    float xmin = xs[0], xmax = xs[0], ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < 4; ++i) {
      xmin = std::min(xmin, xs[i]);
      xmax = std::max(xmax, xs[i]);
      ymin = std::min(ymin, ys[i]);
      ymax = std::max(ymax, ys[i]);
    }
    w = xmax - xmin;
    h = ymax - ymin;
  };
  float w0 = 0.0f, h0 = 0.0f;
  extents(upright, w0, h0);
  CHECK(h0 > w0 * 1.5f);

  const float kHalfPi = 1.57079632679f;
  const auto side = geometry.split_particle_oriented_quad(x, y, size, 2.0f, 0.0f, kHalfPi, 0.0f,
                                                         0.0f, 0.0f, 0);
  float w1 = 0.0f, h1 = 0.0f;
  extents(side, w1, h1);
  CHECK(w1 > h1 * 1.5f);

  const auto hung = geometry.split_particle_oriented_quad(x, y, size, 2.0f, 0.5f, 0.0f, 0.0f,
                                                         0.0f, 0.0f, 0);
  const auto c = geometry.project_main(x, y, 0.0f);
  const float top = 0.5f * (hung.lt.y + hung.rt.y);
  CHECK(std::fabs(top - c.y) < 0.02f);

  // View + 3D startRotation tumbles the quad (Unity rotation3D on billboards).
  const auto tilted = geometry.split_particle_oriented_quad(x, y, size, 2.0f, 0.0f, 0.0f, 0.7f,
                                                           0.7f, 0.0f, 0);
  CHECK(std::fabs(tilted.lb.x - upright.lb.x) > 1e-4f ||
        std::fabs(tilted.rt.y - upright.rt.y) > 1e-4f);
}

void test_2020_catalog_four_layers() {
  const auto* entry = wds::chart_render::official_split_effect(2020);
  CHECK(entry != nullptr);
  CHECK(entry->particle_count == 4);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  CHECK(ps != nullptr);
  CHECK(n == 4);
  const auto& fog = ps[0];
  CHECK(std::fabs(fog.size - 3.0f) < 1e-4f);
  CHECK(fog.shape_type == 0);
  CHECK(std::fabs(fog.shape_radius - 0.2f) < 1e-4f);
  CHECK(std::fabs(fog.shape_scale_y - 8.0f) < 1e-4f);
  CHECK(fog.scaling_mode == 1);
  CHECK((fog.flags & wds::chart_render::kSplitEffectFlagLooping) == 0);
  CHECK(fog.vol_in_world == 0);
  CHECK(fog.sol_count >= 2);
  CHECK(fog.start_color_state == 4);
  CHECK(fog.start_col_color_count == 3);
  CHECK(fog.start_col_mode == 0);
  CHECK(fog.sorting_order == 1);
  {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    wds::chart_render::official_split_effect_eval_start_color(fog, 0.497063f, r, g, b, a);
    CHECK(std::fabs(r - 1.0f) < 1e-3f);
    CHECK(std::fabs(g - 0.0f) < 1e-3f);
    CHECK(std::fabs(b - 0.059916f) < 1e-3f);
    wds::chart_render::official_split_effect_eval_start_color(fog, 0.0f, r, g, b, a);
    CHECK(std::fabs(g - 0.039801f) < 1e-3f);
    wds::chart_render::official_split_effect_eval_start_color(fog, 1.0f, r, g, b, a);
    CHECK(std::fabs(b - 0.321741f) < 1e-3f);
  }
  const auto& petals = ps[1];
  CHECK(std::fabs(petals.size - 0.6f) < 1e-4f);
  CHECK(petals.rotation_3d != 0);
  CHECK(petals.uv_frame_count == 5);
  CHECK(petals.start_color_state == 4);
  CHECK(petals.start_col_color_count == 2);
  CHECK(petals.sorting_order == 2);
  const auto& thorn_a = ps[2];
  const auto& thorn_b = ps[3];
  CHECK(thorn_a.sorting_order == 1);
  CHECK(thorn_b.sorting_order == 1);
  CHECK(std::fabs(thorn_a.size - 0.2f) < 1e-4f);
  CHECK(std::fabs(thorn_a.sol_y - 2.0f) < 1e-4f);
  CHECK(thorn_a.sol_y_count >= 1);
  CHECK(std::fabs(thorn_a.pivot_y - 0.5f) < 1e-4f);
  std::printf("2020 align fog/petals/thorn=%u/%u/%u size3d=%u/%u/%u sol_y=%.3f\n",
              static_cast<unsigned>(fog.render_alignment),
              static_cast<unsigned>(petals.render_alignment),
              static_cast<unsigned>(thorn_a.render_alignment),
              static_cast<unsigned>(fog.size_3d), static_cast<unsigned>(petals.size_3d),
              static_cast<unsigned>(thorn_a.size_3d), thorn_a.sol_y);
  std::printf(
      "2020 thorn size=%.3f size_y=%.3f curve=%u len=%.3f mode=%u scale=%.3f align=%u rot3d=%u "
      "rot=%.3f/%.3f pivot=%.2f sol_y=%.3f sol_y_n=%u\n",
      thorn_a.size, thorn_a.start_size_y, static_cast<unsigned>(thorn_a.size_curve_count),
      thorn_a.length_in_sec, static_cast<unsigned>(thorn_a.render_mode), thorn_a.length_scale,
      static_cast<unsigned>(thorn_a.render_alignment), static_cast<unsigned>(thorn_a.rotation_3d),
      thorn_a.rot_z, thorn_a.rot_z_min, thorn_a.pivot_y, thorn_a.sol_y,
      static_cast<unsigned>(thorn_a.sol_y_count));
  CHECK(std::fabs(thorn_a.rot_z - 2.094395f) < 1e-3f);
  CHECK(std::fabs(thorn_a.rot_z_min - 1.047198f) < 1e-3f);
  CHECK(std::fabs(thorn_b.rot_z - 5.235988f) < 1e-3f);
  CHECK(std::fabs(thorn_b.rot_z_min - 4.188790f) < 1e-3f);
  CHECK(std::fabs(fog.gravity_modifier) < 1e-6f);
  CHECK(std::fabs(petals.gravity_modifier) < 1e-6f);
  CHECK(std::fabs(thorn_a.gravity_modifier) < 1e-6f);
  CHECK(std::fabs(thorn_b.gravity_modifier) < 1e-6f);
  CHECK(fog.noise_damping != 0);
  CHECK(petals.noise_damping != 0);
  CHECK(thorn_a.noise_damping == 0);
  CHECK(fog.auto_random_seed == 0);
  CHECK(petals.auto_random_seed == 0);
  CHECK(fog.force_enabled != 0);
  CHECK(petals.force_enabled != 0);
  CHECK(thorn_a.force_enabled == 0);
  CHECK(thorn_b.force_enabled == 0);
  CHECK(std::fabs(fog.force_z - 2.0f) < 1e-4f);
  CHECK(std::fabs(petals.force_z - 2.0f) < 1e-4f);
  CHECK(fog.force_in_world == 0);
  CHECK(fog.clamp_enabled != 0);
  CHECK(petals.clamp_enabled != 0);
  CHECK(thorn_a.clamp_enabled == 0);
  CHECK(std::fabs(fog.clamp_speed - 1.0f) < 1e-4f);
  CHECK(std::fabs(fog.clamp_drag - 1.0f) < 1e-4f);
  CHECK(fog.clamp_mul_vel != 0);
  CHECK(fog.rbs_enabled == 0);
  CHECK(petals.rbs_enabled != 0);
  CHECK(petals.rbs_separate != 0);
  CHECK(std::fabs(petals.rbs_x - 1.745329f) < 1e-4f);
  CHECK(std::fabs(petals.rbs_x_min + 1.745329f) < 1e-4f);
  CHECK(thorn_a.rbs_enabled == 0);
  CHECK(fog.inherit_enabled == 0);
  CHECK(petals.inherit_enabled == 0);
}

void test_shared_force_family_not_2020_only() {
  auto layer = [](int32_t id, uint16_t i) -> const wds::chart_render::SplitEffectParticle* {
    const auto* e = wds::chart_render::official_split_effect(id);
    if (e == nullptr) {
      return nullptr;
    }
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*e, &n);
    if (ps == nullptr || i >= n) {
      return nullptr;
    }
    return ps + i;
  };
  const auto* p2031 = layer(2030, 1);
  CHECK(p2031 != nullptr);
  CHECK(p2031->force_enabled != 0);
  CHECK(p2031->rbs_enabled != 0);
  CHECK(std::fabs(p2031->force_z - 2.0f) < 1e-4f);
  const auto* p10100 = layer(10100, 1);
  CHECK(p10100 != nullptr);
  CHECK(p10100->force_enabled != 0);
  CHECK(p10100->rbs_enabled != 0);
  const auto* p1 = layer(1, 0);
  CHECK(p1 != nullptr);
  CHECK(p1->shape_type == 0);
  CHECK(std::fabs(p1->shape_scale_y - 20.0f) < 1e-4f);
  const auto* p10030 = layer(10030, 0);
  CHECK(p10030 != nullptr);
  CHECK(p10030->force_enabled != 0);
  CHECK(std::fabs(p10030->force_y) > 1e-4f);
  const auto* p11370 = layer(11370, 2);
  CHECK(p11370 != nullptr);
  CHECK(p11370->rod_random != 0);
}

void test_split_line_plate_uses_tight_sprite_mesh() {
  using wds::chart_editor::kOfficialSplitLineMeshU0;
  using wds::chart_editor::kOfficialSplitLineMeshU1;
  using wds::chart_editor::official_split_line_local_x;
  using wds::chart_editor::official_split_line_mesh_u;
  CHECK(kOfficialSplitLineMeshU0 > 0.03f);
  CHECK(kOfficialSplitLineMeshU1 < 0.97f);
  CHECK(std::fabs(official_split_line_mesh_u(0.0f) - kOfficialSplitLineMeshU0) < 1e-6f);
  CHECK(std::fabs(official_split_line_mesh_u(1.0f) - kOfficialSplitLineMeshU1) < 1e-6f);
  CHECK(std::fabs(official_split_line_local_x(0.5f)) < 1e-6f);
  // u=0–1 on the 0.256 plate is left of textureRect (empty) and right of it
  // (shader tail). Tight mesh sits inside that span.
  CHECK(official_split_line_local_x(0.0f) < official_split_line_local_x(kOfficialSplitLineMeshU0));
  CHECK(official_split_line_local_x(1.0f) > official_split_line_local_x(kOfficialSplitLineMeshU1));
}

void test_11770_and_10170_catalog_particle_counts() {
  const auto* e11770 = wds::chart_render::official_split_effect(11770);
  CHECK(e11770 != nullptr);
  CHECK(e11770->particle_count == 15);
  uint16_t n11770 = 0;
  const auto* p11770 = wds::chart_render::official_split_effect_particles(*e11770, &n11770);
  CHECK(p11770 != nullptr);
  CHECK(n11770 == 15);

  // Side rails: Stretch + 3D Start Size Y=11.53, Circle offset beside the line.
  const float rail_x[] = {0.2f, 0.2f, -0.2f, -0.2f, -0.4f, -0.4f, 0.4f, 0.4f};
  for (uint16_t i = 0; i < 8; ++i) {
    const auto& p = p11770[i];
    CHECK(p.size_3d != 0);
    CHECK(p.render_mode == wds::chart_render::kSplitEffectRenderStretch);
    CHECK(std::fabs(p.start_size_y - 11.53f) < 1e-3f);
    CHECK(std::fabs(p.shape_pos_x - rail_x[i]) < 1e-4f);
    CHECK(std::fabs(p.shape_rot_z - 90.0f) < 1e-3f);
    CHECK(p.shape_arc_mode == 1);
    CHECK(std::fabs(p.shape_arc_speed) < 1e-5f);
    CHECK(p.blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Alpha));
    {
      const uint16_t uv = wds::chart_render::official_split_effect_uv_frame(p, 0.0f, 0.0f);
      const char* name = wds::chart_render::official_split_effect_uv_name(uv);
      CHECK(name != nullptr);
      CHECK(std::strcmp(name, "Split_404_6") == 0);
    }
    const float along = wds::chart_render::official_split_effect_stretch_along(p.size, p.start_size_y,
                                                                              p.size_3d);
    CHECK(std::fabs(along - p.size) < 1e-3f);
    CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_height(
                        p.size, p.start_size_y, p.size_3d) -
                    p.start_size_y) < 1e-3f);
    const float length = wds::chart_render::official_split_effect_stretch_length(
        p.size, p.start_size_y, p.size_3d, p.length_scale, 0.0f, 0.0f);
    CHECK(std::fabs(length - p.start_size_y * p.length_scale) < 1e-3f);
  }

  {
    const float tilt = 0.0f;
    CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_view_roll(0.0f, 1.0f, 0.0f,
                                                                              tilt) -
                    1.570796f) < 1e-4f);
    CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_view_roll(1.0f, 0.0f, 0.0f,
                                                                              tilt)) < 1e-5f);
    CHECK(std::fabs(std::fabs(wds::chart_render::official_split_effect_stretch_view_roll(
                                  -1.0f, 0.0f, 0.0f, tilt)) -
                    3.141593f) < 1e-4f);
  }

  // Local + startRotation X=90° + rotOL Z only: spin around the split line.
  const uint16_t spin[] = {8, 10, 12, 14};
  for (uint16_t i : spin) {
    const auto& p = p11770[i];
    CHECK(p.rotation_3d != 0);
    CHECK(p.render_alignment == 2);
    CHECK(p.rot_ol_enabled != 0);
    CHECK(p.rot_ol_separate == 0);
    CHECK(std::fabs(p.rot_x - 1.570796f) < 1e-3f);
    CHECK(std::fabs(p.rot_x_min - 1.570796f) < 1e-3f);
  }

  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 11770;
  ctx.line_index = 0;
  ctx.line_x = 0.0f;
  ctx.fade_scale = 1.0f;
  ctx.now_t = 1.2f;
  ctx.want_trail = false;
  std::vector<wds::chart_render::SplitEffectLiveParticle> live(256);
  const int nspin = wds::chart_render::shuriken::evaluate(p11770[8], ctx, live.data(), 256);
  CHECK(nspin > 0);
  for (int i = 0; i < nspin; ++i) {
    CHECK(std::fabs(live[static_cast<size_t>(i)].tilt_x - 1.570796f) < 0.05f);
    CHECK(std::fabs(live[static_cast<size_t>(i)].tilt_y) < 0.05f);
    CHECK(std::fabs(live[static_cast<size_t>(i)].roll) > 0.2f);
  }

  const int nrail = wds::chart_render::shuriken::evaluate(p11770[0], ctx, live.data(), 256);
  CHECK(nrail > 0);
  for (int i = 0; i < nrail; ++i) {
    const auto& lp = live[static_cast<size_t>(i)];
    // Stretch uses stored/total (live dump v21), not the 1ms pose finite
    // difference. TransformPoint localPosition.y × 27×fade puts World rails
    // farther in Main Y, where that 1ms delta can round to 0.
    CHECK(std::fabs(lp.total_vy) > std::fabs(lp.total_vx) * 2.0f + 1e-4f);
  }

  const auto* e10170 = wds::chart_render::official_split_effect(10170);
  CHECK(e10170 != nullptr);
  CHECK(e10170->particle_count == 5);
  uint16_t n10170 = 0;
  CHECK(wds::chart_render::official_split_effect_particles(*e10170, &n10170) != nullptr);
  CHECK(n10170 == 5);
}

void test_1070_wrap_is_world_stretch_rod() {
  // User-confirmed isolate: 1070_5/6 World Stretch+RoD are the streak fog.
  // 1070_1/3 Local Billboard follow the tip and are not the wrap. OpenWDS
  // catalog: 5/6 simulationSpace World, renderMode Stretch, RoD=2, SOL Y
  // scalar 50, startSpeed=1, lifetime TwoConstants 0.5–1. Do not ID-branch
  // 1070; the wrap class is World + Stretch + RoD + SOL Y.
  using wds::chart_render::kSplitEffectRenderBillboard;
  using wds::chart_render::kSplitEffectRenderStretch;
  using wds::chart_render::SplitEffectSimulationSpace;
  const auto* entry = wds::chart_render::official_split_effect(1070);
  CHECK(entry != nullptr);
  uint16_t catalog_n = 0;
  const auto* catalog = wds::chart_render::official_split_effect_particles(*entry, &catalog_n);
  CHECK(catalog != nullptr);
  CHECK(catalog_n == 6);
  CHECK(catalog[0].render_mode == kSplitEffectRenderBillboard);
  CHECK(catalog[0].simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Local));
  CHECK(catalog[0].render_alignment == 2);
  CHECK(std::fabs(catalog[0].sol_y - 100.0f) < 1e-3f);
  CHECK(catalog[0].burst_count > 0);
  CHECK(catalog[2].render_mode == kSplitEffectRenderBillboard);
  CHECK(catalog[2].simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Local));
  CHECK(catalog[2].render_alignment == 2);
  CHECK(std::fabs(catalog[2].sol_y - 100.0f) < 1e-3f);
  CHECK(catalog[3].render_mode == kSplitEffectRenderStretch);
  CHECK(catalog[3].simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::World));
  CHECK(std::fabs(catalog[3].sol_y - 50.0f) < 1e-3f);
  CHECK(catalog[3].blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Alpha));
  CHECK(catalog[4].render_mode == kSplitEffectRenderStretch);
  CHECK(catalog[4].simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::World));
  CHECK(catalog[4].blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Additive));
  CHECK(wds::chart_render::official_split_effect_late_hdr(catalog[4].blend, catalog[4].sorting_order));
  CHECK(!wds::chart_render::official_split_effect_late_hdr(catalog[3].blend, catalog[3].sorting_order));

  CHECK(catalog[3].rate_over_distance > 1.0f);
  CHECK(catalog[4].rate_over_distance > 1.0f);
  CHECK(std::fabs(catalog[3].start_speed - 1.0f) < 1e-3f);
  const auto ids = wds::chart_render::official_split_effect_ids();
  int wrap_class = 0;
  for (int32_t id : ids) {
    const auto* e = wds::chart_render::official_split_effect(id);
    uint16_t pn = 0;
    const auto* ps = e ? wds::chart_render::official_split_effect_particles(*e, &pn) : nullptr;
    for (uint16_t i = 0; ps != nullptr && i < pn; ++i) {
      const auto& p = ps[i];
      if (p.render_mode != kSplitEffectRenderStretch ||
          p.simulation_space != static_cast<uint8_t>(SplitEffectSimulationSpace::World) ||
          p.rate_over_distance < 1e-6f || p.sol_y < 10.0f) {
        continue;
      }
      ++wrap_class;
    }
  }
  CHECK(wrap_class >= 2);
  std::printf("World Stretch+RoD+SOL-Y wrap class n=%d\n", wrap_class);

  const uint32_t begin = entry->particle_begin;
  wds::chart_editor::PreviewSplitLaneInstance split;
  split.should_show = true;
  split.split_count = 6;
  split.split_lane_type = wds::chart_editor::SplitLaneType::Full;
  split.scratch_length = 1070;
  split.start_ms = 1000;
  split.end_ms = 5000;
  split.split_line_alpha = 1.0f;
  const float now_t = 0.80f;
  const float t01 = std::min(now_t / wds::chart_editor::kOfficialSplitFadeInSec, 1.0f);
  split.split_percent_start = wds::chart_editor::official_split_fade_in_from_judge_start(t01);
  split.split_percent_end = 1.0f;
  split.split_anim_phase = 0;
  const float travel = wds::chart_editor::kOfficialZ180LineHightY *
                       wds::chart_editor::kOfficialLineHeightScale;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  const int n = wds::chart_render::collect_split_effect_particles(
      split, wds::chart_editor::kOfficialSpritEffectRich, static_cast<double>(now_t), pool.data(),
      wds::chart_render::kSplitEffectParticlePool);
  CHECK(n > 0);
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);

  int n1 = 0, n3 = 0, n5 = 0, stretch_ok = 0, stretch_fail = 0, behind = 0, clip_n = 0;
  float fog_along = 0, fog_width = 0, fog_y0 = 1e9f, fog_y1 = -1e9f;
  float vel_min = 1e9f, leftover_y0 = 1e9f, leftover_y1 = -1e9f;
  float ndc_y0 = 1e9f, ndc_y1 = -1e9f;
  float wrap_pivot = 0.0f;
  float camz_min = 1e9f;
  const float aspect = std::max(geometry.content().w / std::max(geometry.content().h, 1e-6f), 1e-4f);
  float qu[4] = {0, 1, 1, 0};
  float qv[4] = {0, 0, 1, 1};
  float ox[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oy[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oz[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ou[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ov[wds::chart_editor::kOfficialViewClipMaxVerts];
  for (int i = 0; i < n; ++i) {
    const auto& p = pool[static_cast<size_t>(i)];
    if (p.line_index != 0 || p.a < 0.01f || p.catalog_index < begin) {
      continue;
    }
    const uint32_t local = p.catalog_index - begin;
    if (local == 0) {
      ++n1;
    } else if (local == 2) {
      ++n3;
    } else if (local == 3 || local == 4) {
      ++n5;
      wrap_pivot = p.pivot_y;
      if (!wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z)) {
        ++behind;
      }
      const float along =
          wds::chart_render::official_split_effect_stretch_height(p.size, p.size_y, p.size_3d);
      fog_along += along;
      fog_width += p.size;
      fog_y0 = std::min(fog_y0, p.main_y);
      fog_y1 = std::max(fog_y1, p.main_y);
      float svx = p.total_vx, svy = p.total_vy, svz = p.total_vz;
      if (svx * svx + svy * svy + svz * svz <= 1e-20f) {
        svx = p.vel_x;
        svy = p.vel_y;
        svz = p.vel_z;
      }
      const float vmag = std::sqrt(svx * svx + svy * svy + svz * svz);
      vel_min = std::min(vel_min, vmag);
      const float applied_ls = wds::chart_render::official_split_effect_stretch_applied_length_scale(
          p.stored_vx, p.stored_vy, p.stored_vz, p.total_vx, p.total_vy, p.total_vz, p.length_scale,
          p.effect_id, p.render_mode, p.uv_name_index);
      float qx[4], qy[4], qz[4];
      if (geometry.split_particle_stretch_world_corners(
              p.world_x, p.main_y, p.z, svx, svy, svz, p.size, along, p.pivot_y, p.roll, p.pivot_x,
              qx, qy, qz, applied_ls, p.velocity_scale, p.camera_velocity_scale, 0.0f, 0.0f, 0.0f,
              p.freeform_stretching, p.rotate_with_stretch)) {
        ++stretch_ok;
        for (int k = 0; k < 4; ++k) {
          camz_min = std::min(camz_min, wds::chart_editor::official_main_camera_z(qy[k], qz[k]));
        }
      } else {
        ++stretch_fail;
        continue;
      }
      const int cn = wds::chart_editor::official_main_clip_poly_view_uv(
          qx, qy, qz, qu, qv, 4, wds::chart_editor::kOfficialPreviewAspect, ox, oy, oz, ou, ov);
      if (cn < 3) {
        continue;
      }
      ++clip_n;
      for (int k = 0; k < cn; ++k) {
        leftover_y0 = std::min(leftover_y0, oy[k]);
        leftover_y1 = std::max(leftover_y1, oy[k]);
        const auto ndc = wds::chart_editor::project_main_xyz(ox[k], oy[k], oz[k], aspect);
        ndc_y0 = std::min(ndc_y0, ndc.y);
        ndc_y1 = std::max(ndc_y1, ndc.y);
      }
    }
  }
  CHECK(n1 == 3);
  CHECK(n3 == 1);
  CHECK(n5 > 50);
  CHECK(stretch_ok == n5);
  CHECK(stretch_fail == 0);
  const float inv_fog = 1.0f / static_cast<float>(n5);
  CHECK(fog_along * inv_fog > 8.0f);
  CHECK(fog_along * inv_fog > fog_width * inv_fog * 20.0f);
  CHECK(fog_y1 - fog_y0 > travel * 0.7f);
  CHECK(clip_n > 50);
  CHECK(leftover_y1 - leftover_y0 > travel * 0.7f);
  CHECK(std::fabs(wrap_pivot - 0.5f) < 1e-3f);
  CHECK(camz_min < 1.0f);

  wds::chart_render::SplitEffectPlaybackState play;
  std::vector<wds::chart_render::SplitEffectLiveParticle> play_pool(pool.size());
  (void)wds::chart_render::collect_split_effect_particles_playback(
      split, wds::chart_editor::kOfficialSpritEffectRich, 0.02, play, play_pool.data(),
      wds::chart_render::kSplitEffectParticlePool);
  const int n_play = wds::chart_render::collect_split_effect_particles_playback(
      split, wds::chart_editor::kOfficialSpritEffectRich, static_cast<double>(now_t), play,
      play_pool.data(), wds::chart_render::kSplitEffectParticlePool);
  int n5_play = 0;
  float play_y0 = 1e9f, play_y1 = -1e9f;
  for (int i = 0; i < n_play; ++i) {
    const auto& p = play_pool[static_cast<size_t>(i)];
    if (p.line_index != 0 || p.a < 0.01f || p.catalog_index < begin) {
      continue;
    }
    const uint32_t local = p.catalog_index - begin;
    if (local != 3 && local != 4) {
      continue;
    }
    ++n5_play;
    play_y0 = std::min(play_y0, p.main_y);
    play_y1 = std::max(play_y1, p.main_y);
  }
  std::printf(
      "1070 wrap 5/6 n=%d behind=%d stretch_ok=%d fail=%d clip=%d along=%.2f "
      "vel_min=%.3f y=[%.1f,%.1f] leftoverY=%.1f ndcY=%.2f pivot=%.2f camz_min=%.2f "
      "play_n=%d play_span=%.1f\n",
      n5, behind, stretch_ok, stretch_fail, clip_n, fog_along * inv_fog, vel_min, fog_y0, fog_y1,
      leftover_y1 - leftover_y0, ndc_y1 - ndc_y0, wrap_pivot, camz_min, n5_play,
      n5_play ? (play_y1 - play_y0) : 0.0f);
  CHECK(n5_play > 40);
  CHECK(n5_play ? (play_y1 - play_y0) > travel * 0.5f : false);
}

void test_world_rod_fog_stays_while_tip_moves() {
  // native collect + Unity official-1070-stay-v31:
  // 1070_5 World Stretch early births keep main_y while LineHight tip travels.
  // 1070_1 Local Billboard rides the live emitter (same particle follows the
  // tip). That Local follow is 2022.3 simulationSpace; do not treat it as the
  // World wrap layer.
  const auto* entry = wds::chart_render::official_split_effect(1070);
  CHECK(entry != nullptr);
  const uint32_t begin = entry->particle_begin;
  wds::chart_editor::PreviewSplitLaneInstance split;
  split.should_show = true;
  split.split_count = 6;
  split.split_lane_type = wds::chart_editor::SplitLaneType::Full;
  split.scratch_length = 1070;
  split.start_ms = 1000;
  split.end_ms = 5000;
  split.split_line_alpha = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  const float origin = wds::chart_editor::official_split_identity_origin_y();
  const float emit_travel = wds::chart_editor::kOfficialZ180LineHightY *
                            wds::chart_editor::kOfficialLineHeightScale;

  auto collect_layer = [&](double now_sec, uint32_t want_local, bool stretch_only,
                           std::map<int, float>& early_y, float& span, float& tip) {
    const float now_t = static_cast<float>(now_sec);
    const float t01 = std::min(now_t / wds::chart_editor::kOfficialSplitFadeInSec, 1.0f);
    split.split_percent_start = wds::chart_editor::official_split_fade_in_from_judge_start(t01);
    split.split_percent_end = 1.0f;
    split.split_anim_phase = now_t < wds::chart_editor::kOfficialSplitFadeInSec ? 0 : 1;
    const float cubic = wds::chart_editor::official_split_fade_in_scale(t01);
    tip = origin + emit_travel * cubic;
    const int n = wds::chart_render::collect_split_effect_particles(
        split, wds::chart_editor::kOfficialSpritEffectRich, now_sec, pool.data(),
        wds::chart_render::kSplitEffectParticlePool);
    float y0 = 1e9f, y1 = -1e9f;
    int vis = 0;
    early_y.clear();
    for (int i = 0; i < n; ++i) {
      const auto& p = pool[static_cast<size_t>(i)];
      if (p.line_index != 0) {
        continue;
      }
      if (stretch_only && p.render_mode != wds::chart_render::kSplitEffectRenderStretch) {
        continue;
      }
      if (p.catalog_index < begin) {
        continue;
      }
      const uint32_t local = p.catalog_index - begin;
      if (local != want_local) {
        continue;
      }
      if (p.a < 0.01f) {
        continue;
      }
      ++vis;
      y0 = std::min(y0, p.main_y);
      y1 = std::max(y1, p.main_y);
      if (p.birth_t < 0.08f) {
        const int key = static_cast<int>(p.catalog_index) * 100000 +
                        static_cast<int>(std::lround(static_cast<double>(p.birth_t) * 1000.0));
        early_y[key] = p.main_y;
      }
    }
    span = vis ? (y1 - y0) : 0.0f;
  };

  auto max_paired_dy = [](const std::map<int, float>& a, const std::map<int, float>& b,
                          int& paired) {
    paired = 0;
    float max_dy = 0.0f;
    for (const auto& kv : a) {
      auto it = b.find(kv.first);
      if (it == b.end()) {
        continue;
      }
      ++paired;
      max_dy = std::max(max_dy, std::fabs(it->second - kv.second));
    }
    return max_dy;
  };

  std::map<int, float> world20, world80, local20, local80;
  float span20 = 0, span80 = 0, tip20 = 0, tip80 = 0, local_span20 = 0, local_span80 = 0;
  float local_tip20 = 0, local_tip80 = 0;
  collect_layer(0.20, 3, true, world20, span20, tip20);
  collect_layer(0.80, 3, true, world80, span80, tip80);
  collect_layer(0.20, 0, false, local20, local_span20, local_tip20);
  collect_layer(0.80, 0, false, local80, local_span80, local_tip80);
  CHECK(!world20.empty());
  CHECK(!world80.empty());
  CHECK(!local20.empty());
  CHECK(!local80.empty());
  int world_paired = 0;
  int local_paired = 0;
  const float world_dy = max_paired_dy(world20, world80, world_paired);
  const float local_dy = max_paired_dy(local20, local80, local_paired);
  CHECK(world_paired >= 4);
  CHECK(local_paired >= 1);
  const float tip_move = std::fabs(tip80 - tip20);
  CHECK(tip_move > 20.0f);
  CHECK(world_dy < tip_move * 0.15f);
  CHECK(local_dy > tip_move * 0.5f);
  CHECK(span80 > (tip80 - origin) * 0.7f);
  (void)span20;
  (void)local_span20;
  (void)local_span80;
  (void)local_tip20;
  (void)local_tip80;
}

void test_world_rod_stretch_stays_then_dies_with_lifetime() {
  // live dump official-1070-stay-v31: World (2022.3 simulationSpace) RoD
  // Stretch keeps birth world pose (ps_local Y≈0, world Y almost constant
  // aside from startSpeed). Wrap peaks at fade-complete (chartMs≈736–991,
  // n≈76–113 along the grown line). Duration=1 + life≤1 then kills judge-side
  // seeds first; survivors cluster at the far tip (chartMs≈1507 n=9). Do not
  // keep dead particles for the hold, and do not re-parent World seeds to live
  // fade. Stretch mesh remains hop, not 2022.3.
  using wds::chart_render::kSplitEffectRenderStretch;
  using wds::chart_render::SplitEffectSimulationSpace;
  const auto ids = wds::chart_render::official_split_effect_ids();
  int world_rod_stretch = 0;
  int fog_1070 = 0;
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if (p.render_mode != kSplitEffectRenderStretch || p.rate_over_distance < 1e-6f) {
        continue;
      }
      if (p.simulation_space != static_cast<uint8_t>(SplitEffectSimulationSpace::World)) {
        continue;
      }
      ++world_rod_stretch;
      if (id == 1070) {
        ++fog_1070;
        CHECK(p.lifetime <= 1.0f + 1e-4f);
      }
    }
  }
  CHECK(fog_1070 == 3);
  CHECK(world_rod_stretch >= fog_1070);

  wds::chart_editor::PreviewSplitLaneInstance split;
  split.should_show = true;
  split.split_count = 6;
  split.split_lane_type = wds::chart_editor::SplitLaneType::Full;
  split.scratch_length = 1070;
  split.start_ms = 1000;
  split.end_ms = 5000;
  split.split_line_alpha = 1.0f;
  split.split_percent_start = 0.0f;
  split.split_percent_end = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  auto stretch_visible_span = [&](double now_sec, int32_t phase, float& out_span, int& out_n,
                                  float& out_bf0, float& out_bf1) {
    split.split_anim_phase = phase;
    const int n = wds::chart_render::collect_split_effect_particles(
        split, wds::chart_editor::kOfficialSpritEffectRich, now_sec, pool.data(),
        wds::chart_render::kSplitEffectParticlePool);
    float y0 = 1e9f, y1 = -1e9f, bf0 = 1e9f, bf1 = -1e9f;
    int vis = 0;
    for (int i = 0; i < n; ++i) {
      const auto& p = pool[static_cast<size_t>(i)];
      if (p.render_mode != kSplitEffectRenderStretch) {
        continue;
      }
      if (p.a < 0.01f || !wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z)) {
        continue;
      }
      ++vis;
      y0 = std::min(y0, p.main_y);
      y1 = std::max(y1, p.main_y);
      bf0 = std::min(bf0, p.birth_fade);
      bf1 = std::max(bf1, p.birth_fade);
    }
    out_n = vis;
    out_span = vis ? (y1 - y0) : 0.0f;
    out_bf0 = vis ? bf0 : 0.0f;
    out_bf1 = vis ? bf1 : 0.0f;
  };

  const float travel = wds::chart_editor::kOfficialZ180LineHightY *
                       wds::chart_editor::kOfficialLineHeightScale;
  float span_fade = 0, span_full = 0, span_hold = 0, bf0 = 0, bf1 = 0;
  int n_fade = 0, n_full = 0, n_hold = 0;
  stretch_visible_span(0.55, 0, span_fade, n_fade, bf0, bf1);
  CHECK(n_fade > 100);
  CHECK(span_fade > travel * 0.5f);
  stretch_visible_span(1.00, 1, span_full, n_full, bf0, bf1);
  CHECK(n_full > 100);
  CHECK(span_full > travel * 0.75f);
  CHECK(bf1 - bf0 > 0.5f);
  stretch_visible_span(1.50, 1, span_hold, n_hold, bf0, bf1);
  CHECK(n_hold > 0);
  CHECK(n_hold < n_full / 4);
  CHECK(span_hold < travel * 0.3f);
}

void test_stretch_sol_y_is_height_without_start_size_3d() {
  // live dump official-1070-fog-v30: GetCurrentSize3D.y follows SOL Y when
  // startSize3D is off. Shared helper must not swap in width.
  CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_height(0.35f, 17.5f, 0) -
                  17.5f) < 1e-4f);
  CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_height(0.35f, 17.5f, 1) -
                  17.5f) < 1e-4f);
  const auto ids = wds::chart_render::official_split_effect_ids();
  int stretch_sol_y = 0;
  int stretch_1070_fog = 0;
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if (p.render_mode != wds::chart_render::kSplitEffectRenderStretch || p.size_3d != 0) {
        continue;
      }
      if (p.sol_y <= 2.0f) {
        continue;
      }
      ++stretch_sol_y;
      CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_height(
                          p.size, p.size * p.sol_y, p.size_3d) -
                      p.size * p.sol_y) < 1e-3f);
      if (id == 1070) {
        ++stretch_1070_fog;
        CHECK(std::fabs(p.sol_y - 50.0f) < 1e-3f);
        CHECK(std::fabs(p.size - 0.35f) < 1e-3f);
      }
    }
  }
  CHECK(stretch_sol_y == 2);
  CHECK(stretch_1070_fog == 2);
}

void test_stretch_size3d_side_rails_share_11770_shape() {
  const auto ids = wds::chart_render::official_split_effect_ids();
  int rails = 0;
  int circle_loop = 0;
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if (p.shape_type == 10 && p.shape_arc_mode == 1) {
        ++circle_loop;
      }
      if (p.shape_type != 10 || p.shape_arc_mode != 1 || std::fabs(p.shape_arc_speed) > 1e-5f) {
        continue;
      }
      if (p.size_3d == 0 || p.render_mode != wds::chart_render::kSplitEffectRenderStretch) {
        continue;
      }
      ++rails;
      CHECK(std::fabs(p.shape_rot_z - 90.0f) < 1e-3f);
      CHECK(p.rot_ol_separate == 0 || p.rot_ol_enabled == 0);
      const uint16_t uv = wds::chart_render::official_split_effect_uv_frame(p, 0.0f, 0.0f);
      const char* name = wds::chart_render::official_split_effect_uv_name(uv);
      CHECK(name != nullptr);
      CHECK(std::strncmp(name, "Split_404_", 10) == 0);
      const float length = wds::chart_render::official_split_effect_stretch_length(
          p.size, p.start_size_y, p.size_3d, p.length_scale, 0.0f, 0.0f);
      CHECK(std::fabs(length - p.start_size_y * p.length_scale) < 1e-3f);
      CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_height(
                          p.size, p.start_size_y, p.size_3d) -
                      p.start_size_y) < 1e-3f);
    }
  }
  CHECK(rails == 26);
  CHECK(circle_loop == 152);
}

void test_catalog_shuriken_invariants() {
  const auto ids = wds::chart_render::official_split_effect_ids();
  CHECK(ids.size() == 318);
  int looping = 0;
  int sol_two_min = 0;
  int not_local_scale = 0;
  int vol_world = 0;
  int force_n = 0;
  int clamp_n = 0;
  int rbs_n = 0;
  int force_world = 0;
  int noise_n = 0;
  int noise_damp = 0;
  int custom1_n = 0;
  int billboard = 0;
  int ribbon = 0;
  int ptrail = 0;
  int stretch3d = 0;
  int stretch2d = 0;
  int mesh_butterfly = 0;
  int mesh_ring = 0;
  int mesh_cube = 0;
  int stretch_rm = 0;
  int stretch_ff0 = 0;
  int two_colors_a = 0;
  int two_colors_amin = 0;
  int start_delay_n = 0;
  int flip_nz = 0;
  int go_inactive = 0;
  int world_local_pos_x = 0;
  int start_speed_neg = 0;
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    for (uint16_t i = 0; i < n; ++i) {
      if ((ps[i].flags & wds::chart_render::kSplitEffectFlagLooping) != 0) {
        ++looping;
      }
      if (ps[i].sol_min_count > 0) {
        ++sol_two_min;
      }
      if (ps[i].scaling_mode != 1) {
        ++not_local_scale;
      }
      if (ps[i].color_random != 0 && ps[i].start_color_state == 2) {
        if (std::fabs(ps[i].color_a_min) > 1e-4f) {
          ++two_colors_amin;
        }
        if (std::fabs(ps[i].color_a - ps[i].color_a_min) > 1e-4f) {
          ++two_colors_a;
        }
      }
      CHECK(std::fabs(ps[i].start_delay -
                      wds::chart_render::official_split_effect_start_delay(ps[i])) < 1e-6f);
      if (std::fabs(ps[i].start_delay) > 1e-6f) {
        ++start_delay_n;
      }
      if (std::fabs(ps[i].flip_x) > 1e-6f || std::fabs(ps[i].flip_y) > 1e-6f) {
        ++flip_nz;
      }
      if (ps[i].go_active == 0) {
        ++go_inactive;
      }
      if (ps[i].simulation_space == 1 && std::fabs(ps[i].local_pos_x) > 1e-3f) {
        ++world_local_pos_x;
      }
      if (ps[i].start_speed < -1e-4f) {
        ++start_speed_neg;
      }
      if (ps[i].vol_in_world != 0) {
        ++vol_world;
      }
      if (ps[i].force_enabled != 0) {
        ++force_n;
      }
      if (ps[i].force_in_world != 0) {
        ++force_world;
      }
      if (ps[i].clamp_enabled != 0) {
        ++clamp_n;
      }
      if (ps[i].rbs_enabled != 0) {
        ++rbs_n;
      }
      if ((ps[i].flags & wds::chart_render::kSplitEffectFlagNoise) != 0) {
        ++noise_n;
        if (ps[i].noise_damping != 0) {
          ++noise_damp;
        }
      }
      if (ps[i].custom1_x_count > 0 || std::fabs(ps[i].custom1_x) > 1e-6f) {
        ++custom1_n;
      }
      if (ps[i].render_mode == wds::chart_render::kSplitEffectRenderStretch) {
        ++stretch_rm;
        if (ps[i].freeform_stretching == 0) {
          ++stretch_ff0;
        }
      }
      const bool mesh = ps[i].render_mode == wds::chart_render::kSplitEffectRenderMesh;
      const bool trails = (ps[i].flags & wds::chart_render::kSplitEffectFlagTrails) != 0;
      if (mesh) {
        if ((ps[i].flags & wds::chart_render::kSplitEffectFlagButterfly) != 0) {
          ++mesh_butterfly;
        } else if ((ps[i].flags & wds::chart_render::kSplitEffectFlagRingMesh) != 0) {
          ++mesh_ring;
        } else if ((ps[i].flags & wds::chart_render::kSplitEffectFlagCubeMesh) != 0) {
          ++mesh_cube;
        }
      } else if (trails && ps[i].trail_mode == 1) {
        ++ribbon;
      } else if (trails && ps[i].trail_mode == 0) {
        ++ptrail;
      } else if (ps[i].render_mode == wds::chart_render::kSplitEffectRenderStretch) {
        if (ps[i].size_3d != 0) {
          ++stretch3d;
        } else {
          ++stretch2d;
        }
      } else {
        ++billboard;
      }
    }
  }
  CHECK(looping == 0);
  CHECK(not_local_scale == 0);
  CHECK(two_colors_amin == 170);
  CHECK(two_colors_a == 104);
  CHECK(start_delay_n == 31);
  CHECK(flip_nz == 20);
  CHECK(go_inactive == 15);
  {
    int u0 = 0;
    int u1 = 0;
    int u05 = 0;
    int v05 = 0;
    int both_same = 0;
    for (uint32_t s = 1; s <= 32; ++s) {
      uint8_t fu = 0;
      uint8_t fv = 0;
      wds::chart_render::official_split_effect_psr_flip_uv(s, 0.0f, 0.0f, fu, fv);
      u0 += fu;
      wds::chart_render::official_split_effect_psr_flip_uv(s, 1.0f, 0.0f, fu, fv);
      u1 += fu;
      wds::chart_render::official_split_effect_psr_flip_uv(s, 0.5f, 0.5f, fu, fv);
      u05 += fu;
      v05 += fv;
      both_same += fu == fv ? 1 : 0;
    }
    CHECK(u0 == 0);
    CHECK(u1 == 32);
    CHECK(u05 > 0 && u05 < 32);
    CHECK(v05 > 0 && v05 < 32);
    CHECK(both_same < 32);
    float u0v = 0.2f;
    float v0v = 0.3f;
    float u1v = 0.8f;
    float v1v = 0.9f;
    wds::chart_render::official_split_effect_apply_psr_flip_uv(1, 1, u0v, v0v, u1v, v1v);
    CHECK(std::fabs(u0v - 0.8f) < 1e-6f);
    CHECK(std::fabs(u1v - 0.2f) < 1e-6f);
    CHECK(std::fabs(v0v - 0.9f) < 1e-6f);
    CHECK(std::fabs(v1v - 0.3f) < 1e-6f);
  }
  CHECK(world_local_pos_x == 2);
  CHECK(start_speed_neg == 5);
  CHECK(vol_world == 0);
  CHECK(force_world == 0);
  CHECK(noise_damp == noise_n);
  CHECK(stretch_ff0 == stretch_rm);
  CHECK(sol_two_min > 0);
  CHECK(force_n > 0);
  CHECK(clamp_n > 0);
  CHECK(rbs_n > 0);
  CHECK(noise_n > 0);
  CHECK(custom1_n > 0);
  CHECK(billboard > 0);
  CHECK(ribbon > 0);
  CHECK(ptrail > 0);
  CHECK(stretch3d > 0);
  CHECK(stretch2d > 0);
  CHECK(mesh_butterfly > 0);
  CHECK(mesh_ring > 0);
  CHECK(mesh_cube > 0);
  CHECK(stretch_rm > 0);
  {
    const auto* e10470 = wds::chart_render::official_split_effect(10470);
    CHECK(e10470 != nullptr);
    uint16_t n10470 = 0;
    const auto* p10470 = wds::chart_render::official_split_effect_particles(*e10470, &n10470);
    CHECK(p10470 != nullptr);
    bool z5 = false;
    for (uint16_t i = 0; i < n10470; ++i) {
      if (p10470[i].size_3d != 0 && std::fabs(p10470[i].start_size_z - 5.0f) < 1e-4f) {
        z5 = true;
      }
    }
    CHECK(z5);
  }
  const auto* g10410 = wds::chart_render::official_split_effect(10410);
  CHECK(g10410 != nullptr);
  uint16_t n10410 = 0;
  const auto* p10410 = wds::chart_render::official_split_effect_particles(*g10410, &n10410);
  CHECK(p10410 != nullptr);
  CHECK(n10410 >= 2);
  CHECK(std::fabs(p10410[0].gravity_modifier - 0.3f) < 1e-4f);
  CHECK(std::fabs(p10410[1].gravity_modifier - 0.3f) < 1e-4f);
  {
    float ay = 1.0f, az = 1.0f;
    wds::chart_editor::official_gravity_lane_accel(0.0f, ay, az);
    CHECK(std::fabs(ay) < 1e-6f);
    CHECK(std::fabs(az) < 1e-6f);
    wds::chart_editor::official_gravity_lane_accel(1.0f, ay, az);
    const float rad = wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
    CHECK(std::fabs(ay - wds::chart_editor::kUnityPhysicsGravityY * std::cos(rad)) < 1e-5f);
    CHECK(std::fabs(az + wds::chart_editor::kUnityPhysicsGravityY * std::sin(rad)) < 1e-5f);
  }
}

void test_z180_particles_use_split_color_slot() {
  CHECK(wds::chart_editor::split_color_slot(11612, 6, 1) == 5);
  const auto* entry = wds::chart_render::official_split_effect(11612);
  CHECK(entry != nullptr);
  CHECK(entry->element_count >= 6);
}

void test_split_effect_gate_emit_sets() {
  using wds::chart_editor::SplitLaneType;
  using wds::chart_editor::kOfficialSpritEffectLight;
  using wds::chart_editor::kOfficialSpritEffectNormal;
  using wds::chart_editor::kOfficialSpritEffectRich;
  using wds::chart_editor::official_split_effect_should_emit;
  CHECK(official_split_effect_should_emit(kOfficialSpritEffectNormal, SplitLaneType::BothEnds, 6, 0));
  CHECK(official_split_effect_should_emit(kOfficialSpritEffectNormal, SplitLaneType::BothEnds, 6, 6));
  CHECK(!official_split_effect_should_emit(kOfficialSpritEffectNormal, SplitLaneType::BothEnds, 6, 3));
  CHECK(official_split_effect_should_emit(kOfficialSpritEffectRich, SplitLaneType::BothEnds, 6, 3));
  CHECK(!official_split_effect_should_emit(kOfficialSpritEffectLight, SplitLaneType::Full, 6, 0));
  CHECK(official_split_effect_should_emit(kOfficialSpritEffectLight, SplitLaneType::Ignore, 6, 0));
}

void test_2020_live_particles_follow_emitter_not_shape_scale() {
  wds::chart_editor::PreviewSplitLaneInstance split;
  split.should_show = true;
  split.split_count = 1;
  split.split_lane_type = wds::chart_editor::SplitLaneType::Full;
  split.scratch_length = 2020;
  split.start_ms = 1000;
  split.split_anim_phase = 1;
  split.split_percent_start = 0.0f;
  split.split_percent_end = 1.0f;
  split.split_line_alpha = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  const int n = wds::chart_render::collect_split_effect_particles(
      split, wds::chart_editor::kOfficialSpritEffectRich, 2.0, pool.data(),
      wds::chart_render::kSplitEffectParticlePool);
  float y_min = 1e9f, y_max = -1e9f, s_min = 1e9f, s_max = -1e9f;
  float x_dev = 0.0f;
  int with_z = 0;
  int add_n = 0;
  int alpha_n = 0;
  int pivot_n = 0;
  std::vector<float> xoffs;
  xoffs.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    y_min = std::min(y_min, pool[i].main_y);
    y_max = std::max(y_max, pool[i].main_y);
    s_min = std::min(s_min, pool[i].size);
    s_max = std::max(s_max, pool[i].size);
    const float xo = std::min(std::fabs(pool[i].world_x - 5.55f),
                              std::fabs(pool[i].world_x + 5.55f));
    x_dev = std::max(x_dev, xo);
    xoffs.push_back(xo);
    if (std::fabs(pool[i].z) > 0.05f) {
      ++with_z;
    }
    if (pool[i].blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Additive)) {
      ++add_n;
    } else {
      ++alpha_n;
    }
    if (std::fabs(pool[i].pivot_y) > 0.1f) {
      ++pivot_n;
    }
  }
  std::sort(xoffs.begin(), xoffs.end());
  const float x_p50 = xoffs.empty() ? 0.0f : xoffs[xoffs.size() / 2];
  const float x_p90 = xoffs.empty() ? 0.0f : xoffs[(xoffs.size() * 9) / 10];
  float fog_a_min = 1e9f, fog_a_max = -1e9f, pet_a_min = 1e9f, pet_a_max = -1e9f;
  for (int i = 0; i < n; ++i) {
    if (std::fabs(pool[i].pivot_y) > 0.1f) {
      continue;
    }
    if (pool[i].blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Additive)) {
      fog_a_min = std::min(fog_a_min, pool[i].a);
      fog_a_max = std::max(fog_a_max, pool[i].a);
    } else {
      pet_a_min = std::min(pet_a_min, pool[i].a);
      pet_a_max = std::max(pet_a_max, pool[i].a);
    }
  }
  std::printf(
      "2020 collect n=%d y=[%.2f, %.2f] span=%.2f size=[%.3f, %.3f] z_nonzero=%d x_dev=%.2f "
      "x_p50=%.2f x_p90=%.2f add=%d alpha=%d pivot=%d fog_a=[%.3f, %.3f] pet_a=[%.3f, %.3f]\n",
      n, y_min, y_max, y_max - y_min, s_min, s_max, with_z, x_dev, x_p50, x_p90, add_n, alpha_n,
      pivot_n, fog_a_min, fog_a_max, pet_a_min, pet_a_max);
  CHECK(n > 50);
  // RoD follows the Element (parent Y×27) from origin to tip.
  CHECK(y_max - y_min > 10.0f);
  // Shape/VOL stay in PS local — not a field-wide X spray.
  CHECK(x_dev < 4.0f);

  split.split_count = 6;
  split.start_ms = 0;
  const int n6 = wds::chart_render::collect_split_effect_particles(
      split, wds::chart_editor::kOfficialSpritEffectRich, 0.0, pool.data(),
      wds::chart_render::kSplitEffectParticlePool);
  const float line_xs[] = {-5.55f, -3.7f, -1.85f, 0.0f, 1.85f, 3.7f, 5.55f};
  const int nlines = 7;
  std::vector<float> ndc_off;
  ndc_off.reserve(static_cast<size_t>(n6));
  float th_r = 0, th_g = 0, th_b = 0, th_a = 0, th_s = 0;
  int th_n = 0;
  int uv_ok = 0, uv_miss = 0;
  for (int i = 0; i < n6; ++i) {
    const auto& p = pool[static_cast<size_t>(i)];
    const auto ndc = wds::chart_editor::project_main_xyz(p.world_x, p.main_y, p.z);
    float best = 1e9f;
    for (int li = 0; li < nlines; ++li) {
      const auto ln = wds::chart_editor::project_main_xyz(line_xs[li], p.main_y, 0.0f);
      best = std::min(best, std::fabs(ndc.x - ln.x));
    }
    ndc_off.push_back(best);
    if (std::fabs(p.pivot_y) > 0.1f) {
      th_r += p.r;
      th_g += p.g;
      th_b += p.b;
      th_a += p.a;
      th_s += p.size;
      ++th_n;
      if (th_n <= 3) {
        std::printf(
            "2020 thorn[%d] x=%.2f y=%.2f z=%.2f size=%.3f sol_y=%.3f pivot=%.2f,%.2f roll=%.2f "
            "align=%u mode=%u uv=%u rgba=%.3f,%.3f,%.3f,%.3f\n",
            th_n, p.world_x, p.main_y, p.z, p.size, p.sol_y, p.pivot_x, p.pivot_y, p.roll,
            static_cast<unsigned>(p.render_alignment), static_cast<unsigned>(p.render_mode),
            static_cast<unsigned>(p.uv_name_index), p.r, p.g, p.b, p.a);
      }
    }
    bool present = false;
    int32_t ux = 0, uy = 0;
    if (wds::chart_render::official_split_effect_uv_gpu_px(p.uv_name_index, ux, uy, present) &&
        present) {
      ++uv_ok;
    } else {
      ++uv_miss;
    }
  }
  std::sort(ndc_off.begin(), ndc_off.end());
  const float n50 = ndc_off.empty() ? 0.0f : ndc_off[ndc_off.size() / 2];
  const float n90 = ndc_off.empty() ? 0.0f : ndc_off[(ndc_off.size() * 9) / 10];
  std::printf(
      "2020 n6=%d ndc_off p50=%.3f p90=%.3f thorns=%d rgba=%.3f,%.3f,%.3f,%.3f size=%.3f uv_ok=%d "
      "uv_miss=%d\n",
      n6, n50, n90, th_n, th_n ? th_r / th_n : 0, th_n ? th_g / th_n : 0, th_n ? th_b / th_n : 0,
      th_n ? th_a / th_n : 0, th_n ? th_s / th_n : 0, uv_ok, uv_miss);

  auto dump_layers = [&](const char* tag, double now_sec, int32_t start_ms) {
    split.start_ms = start_ms;
    const int n = wds::chart_render::collect_split_effect_particles(
        split, wds::chart_editor::kOfficialSpritEffectRich, now_sec, pool.data(),
        wds::chart_render::kSplitEffectParticlePool);
    int fog_n = 0, pet_n = 0, th_n2 = 0;
    int fog_keep = 0, pet_keep = 0, th_keep = 0;
    float fog_s_max = 0, fog_a_max = 0, fog_z_min = 1e9f, fog_y_min = 1e9f, fog_y_max = -1e9f;
    float pet_s_max = 0;
    int fog_below = 0;
    for (int i = 0; i < n; ++i) {
      const auto& p = pool[static_cast<size_t>(i)];
      const bool th = std::fabs(p.pivot_y) > 0.1f;
      const bool fog = !th && p.blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Additive);
      const bool pet = !th && !fog;
      if (fog) {
        ++fog_n;
        fog_s_max = std::max(fog_s_max, p.size);
        fog_a_max = std::max(fog_a_max, p.a);
        fog_z_min = std::min(fog_z_min, p.z);
        fog_y_min = std::min(fog_y_min, p.main_y);
        fog_y_max = std::max(fog_y_max, p.main_y);
        if (wds::chart_editor::official_main_y_to_percent(p.main_y) > 1.0f) {
          ++fog_below;
        }
      } else if (pet) {
        ++pet_n;
        pet_s_max = std::max(pet_s_max, p.size);
      } else {
        ++th_n2;
      }
      if (p.a < 0.001f || !wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z) ||
          wds::chart_editor::official_main_y_to_percent(p.main_y) > 1.0f) {
        continue;
      }
      if (fog) {
        ++fog_keep;
      } else if (pet) {
        ++pet_keep;
      } else {
        ++th_keep;
      }
    }
    std::printf(
        "2020 %s now=%.2f start_ms=%d n=%d fog=%d keep=%d below1=%.0f y=[%.2f,%.2f] zmin=%.2f "
        "smax=%.3f amax=%.3f pet=%d keep=%d smax=%.3f th=%d keep=%d\n",
        tag, now_sec, start_ms, n, fog_n, fog_keep, static_cast<float>(fog_below), fog_y_min,
        fog_y_max, fog_z_min, fog_s_max, fog_a_max, pet_n, pet_keep, pet_s_max, th_n2, th_keep);
  };
  dump_layers("t1", 0.0, 0);
  dump_layers("t0.4", 0.0, 600);
  dump_layers("t1.2", 0.2, 0);
}

void test_2020_thorns_unpaired_and_petals_not_along_line() {
  const auto* entry = wds::chart_render::official_split_effect(2020);
  CHECK(entry != nullptr);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  CHECK(n == 4);
  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 2020;
  ctx.line_index = 0;
  ctx.line_x = 0.0f;
  ctx.fade_scale = 1.0f;
  ctx.now_t = 1.2f;
  ctx.want_trail = false;
  std::vector<wds::chart_render::SplitEffectLiveParticle> a(256), b(256), pet(512);
  const int na = wds::chart_render::shuriken::evaluate(ps[2], ctx, a.data(), 256);
  const int nb = wds::chart_render::shuriken::evaluate(ps[3], ctx, b.data(), 256);
  CHECK(na > 4);
  CHECK(nb > 4);
  const int k = std::min(na, nb);
  int same_roll_pairs = 0;
  std::vector<float> gaps;
  gaps.reserve(static_cast<size_t>(std::max(na, 1)));
  auto by_y = [](const wds::chart_render::SplitEffectLiveParticle& l,
                 const wds::chart_render::SplitEffectLiveParticle& r) {
    return l.main_y < r.main_y;
  };
  std::sort(a.begin(), a.begin() + na, by_y);
  std::sort(b.begin(), b.begin() + nb, by_y);
  for (int i = 1; i < na; ++i) {
    gaps.push_back(a[static_cast<size_t>(i)].main_y - a[static_cast<size_t>(i - 1)].main_y);
  }
  float mean = 0.0f;
  for (float g : gaps) {
    mean += g;
  }
  if (!gaps.empty()) {
    mean /= static_cast<float>(gaps.size());
  }
  float var = 0.0f;
  for (float g : gaps) {
    var += (g - mean) * (g - mean);
  }
  if (gaps.size() > 1) {
    var /= static_cast<float>(gaps.size() - 1);
  }
  CHECK(var > 1e-4f);
  for (int i = 0; i < k; ++i) {
    const float d = std::fabs(std::fabs(a[static_cast<size_t>(i)].roll - b[static_cast<size_t>(i)].roll) -
                              3.14159265f);
    if (d < 0.05f) {
      ++same_roll_pairs;
    }
  }
  CHECK(same_roll_pairs < k);

  const int np = wds::chart_render::shuriken::evaluate(ps[1], ctx, pet.data(), 512);
  CHECK(np > 10);
  float abs_z = 0.0f;
  for (int i = 0; i < np; ++i) {
    abs_z += std::fabs(pet[static_cast<size_t>(i)].z);
  }
  abs_z /= static_cast<float>(np);
  CHECK(abs_z > 0.15f);
  std::printf("2020 thorns na=%d nb=%d gap_var=%.4f pairs=%d/%d petal_mean_|z|=%.3f\n", na, nb, var,
              same_roll_pairs, k, abs_z);

  wds::chart_render::shuriken::EmitContext ctx1 = ctx;
  ctx1.line_index = 1;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pet1(512);
  const int np1 = wds::chart_render::shuriken::evaluate(ps[1], ctx1, pet1.data(), 512);
  CHECK(np1 > 10);
  int same_pose = 0;
  for (int i = 0; i < std::min(np, np1); ++i) {
    const auto& a = pet[static_cast<size_t>(i)];
    const auto& b = pet1[static_cast<size_t>(i)];
    if (std::fabs(a.z - b.z) < 1e-5f && std::fabs(a.roll - b.roll) < 1e-5f &&
        std::fabs(a.size - b.size) < 1e-5f) {
      ++same_pose;
    }
  }
  CHECK(same_pose < np / 2);
}

void test_particles_keep_col_alpha_when_line_fades() {
  const auto* entry = wds::chart_render::official_split_effect(2020);
  CHECK(entry != nullptr);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 2020;
  ctx.line_alpha = 0.5f;
  ctx.now_t = 1.2f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pet(512);
  const int np = wds::chart_render::shuriken::evaluate(ps[1], ctx, pet.data(), 512);
  CHECK(np > 10);
  float a_max = 0.0f;
  for (int i = 0; i < np; ++i) {
    a_max = std::max(a_max, pet[static_cast<size_t>(i)].a);
  }
  CHECK(a_max > 0.7f);
}

void test_thorn_start_size_follows_system_age_not_parent_scale() {
  const auto* entry = wds::chart_render::official_split_effect(2020);
  CHECK(entry != nullptr);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  const auto& thorn = ps[2];
  CHECK(thorn.scaling_mode == 1);
  CHECK(thorn.size_curve_count >= 2);
  const float at0 = wds::chart_render::official_split_effect_eval_curve_keys(
      thorn.size_curve_begin, thorn.size_curve_count, 0.0f, thorn.size);
  const float at20 = wds::chart_render::official_split_effect_eval_curve_keys(
      thorn.size_curve_begin, thorn.size_curve_count, 0.2f, thorn.size);
  CHECK(std::fabs(at0 - 0.2f) < 1e-3f);
  CHECK(std::fabs(at20 - 0.1f) < 1e-3f);
  CHECK(at0 < 1.0f);
  CHECK(thorn.sol_y > 1.5f && thorn.sol_y < 2.5f);
  CHECK(std::fabs(wds::chart_render::official_split_effect_eval_sol(thorn, 0.5f) - 1.0f) < 1e-3f);
  CHECK(std::fabs(wds::chart_render::official_split_effect_eval_sol_y(thorn, 0.5f) - 2.0f) <
        1e-3f);

  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 2020;
  ctx.line_alpha = 0.4f;
  ctx.now_t = 0.4f;
  ctx.fade_scale = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> thorns(256);
  std::vector<wds::chart_render::SplitEffectLiveParticle> petals(512);
  const int thn = wds::chart_render::shuriken::evaluate(ps[2], ctx, thorns.data(), 256);
  const int pn = wds::chart_render::shuriken::evaluate(ps[1], ctx, petals.data(), 512);
  CHECK(thn > 0);
  float th_s_max = 0.0f;
  float th_h_max = 0.0f;
  for (int i = 0; i < thn; ++i) {
    const auto& p = thorns[static_cast<size_t>(i)];
    th_s_max = std::max(th_s_max, p.size);
    th_h_max = std::max(th_h_max, p.size * p.sol_y);
    CHECK(p.size < 1.0f);
    CHECK(p.sol_y > 2.8f && p.sol_y < 3.4f);
  }
  CHECK(th_s_max < 0.5f);
  CHECK(th_h_max > 0.15f);
  CHECK(th_h_max < 2.0f);
  float pet_a_max = 0.0f;
  for (int i = 0; i < pn; ++i) {
    pet_a_max = std::max(pet_a_max, petals[static_cast<size_t>(i)].a);
  }
  CHECK(pet_a_max > 0.7f);
}

void test_openwds_custom1_and_world_noise() {
  const auto* e2020 = wds::chart_render::official_split_effect(2020);
  CHECK(e2020 != nullptr);
  uint16_t n2020 = 0;
  const auto* p2020 = wds::chart_render::official_split_effect_particles(*e2020, &n2020);
  CHECK(n2020 >= 4);
  CHECK(p2020[1].custom1_x_count == 0);
  CHECK(std::fabs(p2020[1].custom1_x) < 1e-6f);
  CHECK(p2020[2].custom1_x_count == 0);

  const auto* e2030 = wds::chart_render::official_split_effect(2030);
  CHECK(e2030 != nullptr);
  uint16_t n2030 = 0;
  const auto* p2030 = wds::chart_render::official_split_effect_particles(*e2030, &n2030);
  CHECK(n2030 >= 2);
  const auto& petals = p2030[1];
  CHECK(petals.custom1_x_count >= 2);
  CHECK(std::fabs(petals.custom1_x - 5.0f) < 1e-3f);
  const float c1_peak = wds::chart_render::official_split_effect_eval_curve_keys(
      petals.custom1_x_begin, petals.custom1_x_count, 0.1f, petals.custom1_x);
  CHECK(c1_peak > 4.0f);
  CHECK(c1_peak * 0.5f + 1.0f > 3.0f);

  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 2030;
  ctx.now_t = 1.2f;
  ctx.fade_scale = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> live(512);
  const int np = wds::chart_render::shuriken::evaluate(petals, ctx, live.data(), 512);
  CHECK(np > 10);
  float c1_max = 0.0f;
  for (int i = 0; i < np; ++i) {
    c1_max = std::max(c1_max, live[static_cast<size_t>(i)].custom1_x);
  }
  CHECK(c1_max > 2.0f);

  ctx.color_id = 2020;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pet(512);
  const int npet = wds::chart_render::shuriken::evaluate(p2020[1], ctx, pet.data(), 512);
  CHECK(npet > 10);
  float z_var = 0.0f;
  float z_mean = 0.0f;
  for (int i = 0; i < npet; ++i) {
    z_mean += pet[static_cast<size_t>(i)].z;
  }
  z_mean /= static_cast<float>(npet);
  for (int i = 0; i < npet; ++i) {
    const float d = pet[static_cast<size_t>(i)].z - z_mean;
    z_var += d * d;
  }
  z_var /= static_cast<float>(npet);
  CHECK(z_var > 0.02f);
}

void test_2020_petals_clump_along_line_from_shape_transform() {
  const auto* entry = wds::chart_render::official_split_effect(2020);
  CHECK(entry != nullptr);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  CHECK(n >= 2);
  CHECK(std::fabs(ps[0].shape_scale_y - 8.0f) < 1e-4f);
  CHECK(std::fabs(ps[1].shape_scale_y - 8.0f) < 1e-4f);
  std::printf("2020 petal shape r=%.3f sy=%.3f posy=%.3f sim=%u speed=%.3f\n", ps[1].shape_radius,
              ps[1].shape_scale_y, ps[1].shape_pos_y, static_cast<unsigned>(ps[1].simulation_space),
              ps[1].start_speed);

  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 2020;
  ctx.line_index = 0;
  ctx.line_x = 0.0f;
  ctx.fade_scale = 1.0f;
  ctx.now_t = 1.2f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pet(512);
  const int np = wds::chart_render::shuriken::evaluate(ps[1], ctx, pet.data(), 512);
  CHECK(np > 10);

  float max_resid = 0.0f;
  float y_min = 1e9f;
  float y_max = -1e9f;
  for (int i = 0; i < np; ++i) {
    const auto& p = pet[static_cast<size_t>(i)];
    y_min = std::min(y_min, p.main_y);
    y_max = std::max(y_max, p.main_y);
    const float u = std::clamp(p.birth_t / wds::chart_editor::kOfficialSplitFadeInSec, 0.0f, 1.0f);
    const float fade = wds::chart_editor::official_split_fade_in_scale(u);
    const float origin = wds::chart_editor::official_split_identity_origin_y() +
                         wds::chart_editor::kOfficialZ180LineHightY *
                             wds::chart_editor::kOfficialLineHeightScale * fade;
    max_resid = std::max(max_resid, std::fabs(p.main_y - origin));
    CHECK(p.size < 1.0f);
  }
  std::printf("2020 petal shape resid max=%.2f y=[%.2f,%.2f] n=%d\n", max_resid, y_min, y_max, np);
  // Scaling Mode Local ignores parent 27. Shape is pos.y=-1 + r=0.2×scale_y=8.
  CHECK(max_resid < 8.0f);
  CHECK(max_resid > 0.5f);
}

float quad_aabb_x(const wds::renderer::Quad& q) {
  const float xs[] = {q.lb.x, q.rb.x, q.lt.x, q.rt.x};
  float xmin = xs[0], xmax = xs[0];
  for (int i = 1; i < 4; ++i) {
    xmin = std::min(xmin, xs[i]);
    xmax = std::max(xmax, xs[i]);
  }
  return xmax - xmin;
}

float lane_screen_width(const wds::renderer::StageGeometry& geometry, float y) {
  const float half = 0.5f * wds::chart_editor::kOfficialNoteWidthPerLane;
  return std::fabs(geometry.project_main(half, y).x - geometry.project_main(-half, y).x);
}

void test_official_thorn_size_fields() {
  const int32_t gold[] = {2020, 2030, 10100, 1, 1050, 11370, 11380};
  int gold_thorns = 0;
  for (int32_t id : gold) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    CHECK(ps != nullptr);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if (std::fabs(p.pivot_y) < 0.4f || p.sol_y < 1.5f) {
        continue;
      }
      ++gold_thorns;
      std::printf(
          "thorn id=%d i=%u size=%.3f size_y=%.3f size3d=%u curve=%u len=%.3f mode=%u "
          "lscale=%.3f align=%u rot3d=%u rot=%.3f/%.3f pivot=%.2f sol_y=%.3f\n",
          id, static_cast<unsigned>(i), p.size, p.start_size_y, static_cast<unsigned>(p.size_3d),
          static_cast<unsigned>(p.size_curve_count), p.length_in_sec,
          static_cast<unsigned>(p.render_mode), p.length_scale,
          static_cast<unsigned>(p.render_alignment), static_cast<unsigned>(p.rotation_3d),
          p.rot_z, p.rot_z_min, p.pivot_y, p.sol_y);
      CHECK(p.scaling_mode == 1);
      CHECK(p.size < 1.0f);
      CHECK(p.render_mode == 0);
    }
  }
  CHECK(gold_thorns >= 2);

  int all_thorns = 0;
  int size3d_n = 0;
  int view_n = 0;
  int curve_n = 0;
  const auto ids = wds::chart_render::official_split_effect_ids();
  CHECK(ids.size() == 318);
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if (std::fabs(p.pivot_y) < 0.4f || p.sol_y < 1.5f) {
        continue;
      }
      ++all_thorns;
      size3d_n += p.size_3d != 0 ? 1 : 0;
      view_n += p.render_alignment == 0 ? 1 : 0;
      curve_n += p.size_curve_count >= 2 ? 1 : 0;
    }
  }
  std::printf("thorn-like n=%d size3d=%d view=%d size_curve=%d /318\n", all_thorns, size3d_n,
              view_n, curve_n);
  CHECK(all_thorns >= 2);

  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const float y = wds::chart_editor::official_percent_to_main_y(0.85f);
  const float lane = lane_screen_width(geometry, y);
  int half_lane = 0;
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if (std::fabs(p.pivot_y) < 0.4f || p.sol_y < 1.5f || p.render_mode != 0) {
        continue;
      }
      if (p.size < 0.15f || p.size > 0.25f) {
        continue;
      }
      const float roll = 0.5f * (p.rot_z + p.rot_z_min);
      const float aspect = wds::chart_render::official_split_effect_uv_aspect_hw(p.uv_name_index);
      const auto q = geometry.split_particle_oriented_quad(
          0.0f, y, p.size, p.sol_y * aspect, -p.pivot_y, roll, 0.0f, 0.0f, 0.0f,
          p.render_alignment, p.pivot_x, p.pivot_z);
      const float ratio = quad_aabb_x(q) / std::max(lane, 1e-6f);
      CHECK(ratio > 0.50f);
      CHECK(ratio < 0.80f);
      ++half_lane;
    }
  }
  std::printf("thorn half-lane catalog n=%d\n", half_lane);
  CHECK(half_lane >= 2);
}

void test_thorn_cross_lane_width_follows_official_long_axis() {
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  const float y_judge = wds::chart_editor::official_percent_to_main_y(0.85f);
  const float lane_j = lane_screen_width(geometry, y_judge);
  const float kHalfPi = 1.57079632679f;
  const float rolls[] = {1.04719755f, kHalfPi, 2.09439510f};
  uint16_t thorn_uv = wds::chart_render::kSplitEffectNoUvName;
  for (uint16_t i = 0; i < 512; ++i) {
    const char* name = wds::chart_render::official_split_effect_uv_name(i);
    if (name == nullptr) {
      break;
    }
    if (std::strcmp(name, "Split_202_6") == 0) {
      thorn_uv = i;
      break;
    }
  }
  const float thorn_aspect = wds::chart_render::official_split_effect_uv_aspect_hw(thorn_uv);
  CHECK(thorn_aspect > 1.45f);
  CHECK(thorn_aspect < 1.60f);
  for (float roll : rolls) {
    const auto thorn = geometry.split_particle_oriented_quad(
        0.0f, y_judge, 0.2f, 2.0f * thorn_aspect, -0.5f, roll, 0.0f, 0.0f, 0.0f, 0);
    const float ratio = quad_aabb_x(thorn) / std::max(lane_j, 1e-6f);
    std::printf("thorn roll=%.3f aabb/lane=%.3f aspect=%.3f\n", roll, ratio, thorn_aspect);
    CHECK(ratio > 0.50f);
    CHECK(ratio < 0.80f);
  }

  const auto* entry = wds::chart_render::official_split_effect(2020);
  CHECK(entry != nullptr);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  CHECK(n >= 4);
  CHECK(ps[2].size_3d == 0);
  CHECK(std::fabs(ps[2].length_in_sec - 1.0f) < 1e-4f);
  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 2020;
  ctx.fade_scale = 1.0f;

  ctx.now_t = 0.15f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> early(256);
  const int en = wds::chart_render::shuriken::evaluate(ps[2], ctx, early.data(), 256);
  CHECK(en > 0);
  float early_ratio = 0.0f;
  float early_h = 0.0f;
  for (int i = 0; i < en; ++i) {
    const auto& p = early[static_cast<size_t>(i)];
    early_h = std::max(early_h, p.size * p.sol_y);
    CHECK(p.size < 1.0f);
    const auto q = geometry.split_particle_oriented_quad(
        p.world_x, p.main_y, p.size, p.sol_y, p.pivot_y, p.roll, p.tilt_x, p.tilt_y, p.z,
        p.render_alignment, p.pivot_x, p.pivot_z);
    early_ratio = std::max(early_ratio, quad_aabb_x(q) / std::max(lane_screen_width(geometry, p.main_y), 1e-6f));
  }
  std::printf("live now=0.15 n=%d h_max=%.3f ratio_max=%.3f\n", en, early_h, early_ratio);
  CHECK(early_h > 0.50f);
  CHECK(early_ratio > 0.50f);
  CHECK(early_ratio < 0.80f);

  // Official startSize Curve (0,0.2)→(0.2,0.1) × duration=1s. Lifetime ≤1s, so
  // after ~1s only later births remain — long axis 0.2 ≈ 1/4 lane. Do not
  // restretch those to 0.4; OpenWDS uses the same curve.
  ctx.now_t = 1.2f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> late(256);
  const int ln = wds::chart_render::shuriken::evaluate(ps[2], ctx, late.data(), 256);
  CHECK(ln > 0);
  float late_ratio = 0.0f;
  float late_size = 0.0f;
  float late_h = 0.0f;
  for (int i = 0; i < ln; ++i) {
    const auto& p = late[static_cast<size_t>(i)];
    late_size = std::max(late_size, p.size);
    late_h = std::max(late_h, p.size * p.sol_y);
    CHECK(p.size < 1.0f);
    const auto q = geometry.split_particle_oriented_quad(
        p.world_x, p.main_y, p.size, p.sol_y, p.pivot_y, p.roll, p.tilt_x, p.tilt_y, p.z,
        p.render_alignment, p.pivot_x, p.pivot_z);
    late_ratio = std::max(late_ratio, quad_aabb_x(q) / std::max(lane_screen_width(geometry, p.main_y), 1e-6f));
  }
  std::printf("live now=1.20 n=%d size_max=%.3f h_max=%.3f ratio_max=%.3f\n", ln, late_size,
              late_h, late_ratio);
  CHECK(late_size < 0.12f);
  CHECK(late_h > 0.25f);
  CHECK(late_h < 0.38f);
  CHECK(late_ratio > 0.28f);
  CHECK(late_ratio < 0.48f);
}

void test_split_202_6_atlas_tile_fills_tight_sprite() {
  uint16_t idx = 0xFFFF;
  for (uint16_t i = 0; i < 512; ++i) {
    const char* name = wds::chart_render::official_split_effect_uv_name(i);
    if (name == nullptr) {
      break;
    }
    if (std::strcmp(name, "Split_202_6") == 0) {
      idx = i;
      break;
    }
  }
  CHECK(idx != 0xFFFF);
  int32_t tx = 0, ty = 0;
  bool present = false;
  CHECK(wds::chart_render::official_split_effect_uv_gpu_px(idx, tx, ty, present));
  CHECK(present);

  const auto png = find_skins_png("split_effect_uvs.png");
  CHECK(!png.empty());
  if (png.empty()) {
    return;
  }
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
  CHECK(wds::renderer::load_png_rgba8(png.string(), rgba, w, h));
  const auto* cat = wds::chart_render::official_split_effect_uv_tile(idx);
  CHECK(cat != nullptr);
  const int tile_w = static_cast<int>(cat->w);
  const int tile_h = static_cast<int>(cat->h);
  CHECK(tile_w > 2);
  CHECK(tile_h > 2);
  CHECK(w >= tx + tile_w);
  CHECK(h >= ty + tile_h);
  int x0 = tile_w, x1 = 0, y0 = tile_h, y1 = 0;
  for (int row = 0; row < tile_h; ++row) {
    for (int col = 0; col < tile_w; ++col) {
      const int px = (ty + row) * w + (tx + col);
      if (rgba[static_cast<size_t>(px) * 4 + 3] < 16) {
        continue;
      }
      x0 = std::min(x0, col);
      x1 = std::max(x1, col);
      y0 = std::min(y0, row);
      y1 = std::max(y1, row);
    }
  }
  const int bw = x1 >= x0 ? x1 - x0 + 1 : 0;
  const int bh = y1 >= y0 ? y1 - y0 + 1 : 0;
  std::printf("Split_202_6 tile ink bbox %dx%d in %dx%d\n", bw, bh, tile_w, tile_h);
  // TSA Sprites maps textureRect onto UV 0–1. m_Rect padding left ~38×58.
  CHECK(bw > 50);
  CHECK(bh > 50);
}

void test_split_202_0_atlas_tile_keeps_official_halo() {
  uint16_t idx = 0xFFFF;
  for (uint16_t i = 0; i < 512; ++i) {
    const char* name = wds::chart_render::official_split_effect_uv_name(i);
    if (name == nullptr) {
      break;
    }
    if (std::strcmp(name, "Split_202_0") == 0) {
      idx = i;
      break;
    }
  }
  CHECK(idx != 0xFFFF);
  int32_t tx = 0, ty = 0;
  bool present = false;
  CHECK(wds::chart_render::official_split_effect_uv_gpu_px(idx, tx, ty, present));
  CHECK(present);

  const auto png = find_skins_png("split_effect_uvs.png");
  CHECK(!png.empty());
  if (png.empty()) {
    return;
  }
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
  CHECK(wds::renderer::load_png_rgba8(png.string(), rgba, w, h));
  const auto* cat = wds::chart_render::official_split_effect_uv_tile(idx);
  CHECK(cat != nullptr);
  const int tile_w = static_cast<int>(cat->w);
  const int tile_h = static_cast<int>(cat->h);
  const int gutter = wds::chart_render::kSplitEffectUvGutterPx;
  CHECK(tile_w > gutter * 2);
  CHECK(tile_h > gutter * 2);
  CHECK(w >= tx + tile_w);
  CHECK(h >= ty + tile_h);
  int n = 0;
  int a0 = 0;
  int soft = 0;
  for (int row = gutter; row < tile_h - gutter; ++row) {
    for (int col = gutter; col < tile_w - gutter; ++col) {
      const int px = (ty + row) * w + (tx + col);
      const int a = rgba[static_cast<size_t>(px) * 4 + 3];
      ++n;
      if (a == 0) {
        ++a0;
      } else if (a < 160) {
        ++soft;
      }
    }
  }
  CHECK(n > 20);
  const float a0_frac = static_cast<float>(a0) / static_cast<float>(n);
  const float soft_frac = static_cast<float>(soft) / static_cast<float>(n);
  std::printf("Split_202_0 content a0=%.3f soft=%.3f n=%d\n", a0_frac, soft_frac, n);
  // Official tight sprite a0≈0.247 soft(1–159)≈0.166. 64px LANCZOS raised a0
  // to ~0.29 and ate the outer halo that near-camera petals magnify.
  CHECK(a0_frac < 0.26f);
  CHECK(soft_frac > 0.12f);
}

uint16_t uv_index_named(const char* name) {
  for (uint16_t i = 0; i < 512; ++i) {
    const char* n = wds::chart_render::official_split_effect_uv_name(i);
    if (n == nullptr) {
      break;
    }
    if (std::strcmp(n, name) == 0) {
      return i;
    }
  }
  return 0xFFFF;
}

bool on_or_in_front_of_near(float local_y, float local_z) {
  return wds::chart_editor::official_main_camera_z(local_y, local_z) + 1e-5f >=
         wds::chart_editor::kOfficialCameraNearClip;
}

const wds::chart_render::SplitEffectParticle* layer_of(int32_t id, uint16_t i) {
  const auto* e = wds::chart_render::official_split_effect(id);
  if (e == nullptr) {
    return nullptr;
  }
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*e, &n);
  if (ps == nullptr || i >= n) {
    return nullptr;
  }
  return ps + i;
}

void test_stretch_view_clip_interpolates_sprite_uv() {
  // Circle Stretch rails (11770 / 10480 / …): 0.1 × 23.06, pivot 0 = center.
  // Frustum cuts the near end. Unity interpolates mesh UV; rewriting V to 0
  // on the leftover polygon samples the sprite's transparent edge.
  const float hw = 0.05f;
  const float half_len = 11.53f;
  const float mid_y = -8.3f;
  const float tail_y = mid_y - half_len;
  const float head_y = mid_y + half_len;
  const float qx[4] = {-hw, hw, hw, -hw};
  const float qy[4] = {tail_y, tail_y, head_y, head_y};
  const float qz[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const float qu[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  const float qv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float ox[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oy[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oz[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ou[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ov[wds::chart_editor::kOfficialViewClipMaxVerts];
  const int cn = wds::chart_editor::official_main_clip_poly_view_uv(
      qx, qy, qz, qu, qv, 4, wds::chart_editor::kOfficialPreviewAspect, ox, oy, oz, ou, ov);
  CHECK(cn >= 3);
  float min_y = 1e9f;
  float v_at_min = -1.0f;
  const float span = head_y - tail_y;
  for (int i = 0; i < cn; ++i) {
    if (oy[i] < min_y) {
      min_y = oy[i];
      v_at_min = ov[i];
    }
    const float expect_v = (oy[i] - tail_y) / span;
    CHECK(std::fabs(ov[i] - expect_v) < 0.03f);
    CHECK(ou[i] >= -0.02f && ou[i] <= 1.02f);
  }
  CHECK(min_y > tail_y + 0.5f);
  CHECK(v_at_min > 0.2f);
}

void test_billboard_near_clip_keeps_camera_front() {
  // OpenWDS playfield Camera.nearClipPlane 0.1 clips ParticleSystemRenderer
  // triangles (same helper as Trails / Stretch). 10160 GO2 Billboard +
  // Rotation by Speed tilts an old near-camera quad; projecting first
  // filled the lens with leftover COL fog.
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1280, 720);
  // Center camz at y=-12 is ~1.61; tilt_x=1.2 put one corner at camz~0.21,
  // which straddled the old Unity-default 0.3 but sits in front of playfield
  // near=0.1. Drop 0.5 Main-Y so the same tilt still crosses 0.1.
  const float y = -12.5f;
  const float size = 3.0f;
  const float tilt_x = 1.2f;
  float qx[4], qy[4], qz[4];
  CHECK(geometry.split_particle_oriented_world_corners(0.0f, y, size, 1.0f, 0.0f, 0.0f, tilt_x,
                                                       0.0f, 0.0f, 0, 0.0f, 0.0f, qx, qy, qz));
  int behind = 0;
  int in_front = 0;
  for (int i = 0; i < 4; ++i) {
    if (on_or_in_front_of_near(qy[i], qz[i])) {
      ++in_front;
    } else {
      ++behind;
    }
  }
  CHECK(behind > 0);
  CHECK(in_front > 0);
  const float qu[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  const float qv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float ox[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oy[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oz[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ou[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ov[wds::chart_editor::kOfficialViewClipMaxVerts];
  const int cn = wds::chart_editor::official_main_clip_poly_view_uv(
      qx, qy, qz, qu, qv, 4, wds::chart_editor::kOfficialPreviewAspect, ox, oy, oz, ou, ov);
  // Side planes may eat a huge near quad (Unity would not draw it either).
  // The bug was keeping behind-camera verts; leftover vertices stay in front.
  for (int i = 0; i < cn; ++i) {
    CHECK(on_or_in_front_of_near(oy[i], oz[i]));
  }

  const auto* fog = layer_of(10160, 1);
  CHECK(fog != nullptr);
  CHECK(fog->rbs_enabled != 0);
  CHECK(fog->rotation_3d != 0);
  CHECK(fog->start_color_state == 1);
  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 10160;
  ctx.fade_scale = 1.0f;
  ctx.line_alpha = 1.0f;
  ctx.now_t = 0.8f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> live(1024);
  const int n = wds::chart_render::shuriken::evaluate(*fog, ctx, live.data(), 1024);
  CHECK(n > 8);
  int clipped_straddle = 0;
  for (int k = 0; k < n; ++k) {
    const auto& p = live[static_cast<size_t>(k)];
    if (!wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z)) {
      continue;
    }
    float px[4], py[4], pz[4];
    if (!geometry.split_particle_oriented_world_corners(
            p.world_x, p.main_y, p.size, p.sol_y, p.pivot_y, p.roll, p.tilt_x, p.tilt_y, p.z,
            p.render_alignment, p.pivot_x, p.pivot_z, px, py, pz)) {
      continue;
    }
    int b = 0;
    for (int i = 0; i < 4; ++i) {
      if (!on_or_in_front_of_near(py[i], pz[i])) {
        ++b;
      }
    }
    if (b == 0) {
      continue;
    }
    const int pn = wds::chart_editor::official_main_clip_poly_view_uv(
        px, py, pz, qu, qv, 4, wds::chart_editor::kOfficialPreviewAspect, ox, oy, oz, ou, ov);
    for (int i = 0; i < pn; ++i) {
      CHECK(on_or_in_front_of_near(oy[i], oz[i]));
    }
    ++clipped_straddle;
  }
  std::printf("10160 GO2 straddling billboards clipped=%d / n=%d\n", clipped_straddle, n);
}

void test_local_sol_y_billboard_near_clip_keeps_leftover() {
  // ALIGN2-N6 10461_1 / 11332_1: Local Billboard, GetCurrentSize3D.y 150–153,
  // particle Main Y≈43.6, Bake long edge = Y, bottom viewport.z<0. Unity GPU
  // clips the near plane (OpenWDS 0.1) and still draws the leftover. maxP=10
  // samples GetCurrentSize (width) vs viewport width at viewZ; this X=2.25
  // stays under the cap. Height 150 is not the maxP sample.
  wds::renderer::StageGeometry geometry;
  geometry.configure({});
  geometry.resize(1920, 1080);
  const float main_y = 43.5f;
  const float size = 2.25f;
  const float height = 150.25f;
  const float sol_y = height / size;
  float qx[4], qy[4], qz[4];
  CHECK(geometry.split_particle_oriented_world_corners( -5.55f, main_y, size, sol_y, 0.0f, 0.0f,
                                                       0.0f, 0.0f, 0.0f, 2, 0.0f, 0.0f, qx, qy,
                                                       qz));
  int behind = 0;
  int in_front = 0;
  float along = 0.0f;
  float across = 0.0f;
  along = std::hypot(qx[3] - qx[0], qy[3] - qy[0], qz[3] - qz[0]);
  across = std::hypot(qx[1] - qx[0], qy[1] - qy[0], qz[1] - qz[0]);
  CHECK(std::fabs(along - height) < 1.0f);
  CHECK(std::fabs(across - size) < 0.1f);
  for (int i = 0; i < 4; ++i) {
    if (on_or_in_front_of_near(qy[i], qz[i])) {
      ++in_front;
    } else {
      ++behind;
      const auto ndc = wds::chart_editor::project_main_xyz(qx[i], qy[i], qz[i]);
      CHECK(std::fabs(ndc.x) > 2.0f || std::fabs(ndc.y) > 2.0f);
    }
  }
  CHECK(behind > 0);
  CHECK(in_front > 0);
  CHECK(wds::chart_editor::official_main_in_front_of_camera(main_y, 0.0f));
  const float qu[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  const float qv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float ox[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oy[wds::chart_editor::kOfficialViewClipMaxVerts];
  float oz[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ou[wds::chart_editor::kOfficialViewClipMaxVerts];
  float ov[wds::chart_editor::kOfficialViewClipMaxVerts];
  const int cn = wds::chart_editor::official_main_clip_poly_near_uv(
      qx, qy, qz, qu, qv, 4, ox, oy, oz, ou, ov);
  CHECK(cn >= 3);
  float min_px = 1e9f, max_px = -1e9f, min_py = 1e9f, max_py = -1e9f;
  float min_cz = 1e9f;
  for (int i = 0; i < cn; ++i) {
    CHECK(on_or_in_front_of_near(oy[i], oz[i]));
    const float cz = wds::chart_editor::official_main_camera_z(oy[i], oz[i]);
    min_cz = std::min(min_cz, cz);
    const auto ndc = wds::chart_editor::project_main_xyz(ox[i], oy[i], oz[i]);
    min_px = std::min(min_px, ndc.x);
    max_px = std::max(max_px, ndc.x);
    min_py = std::min(min_py, ndc.y);
    max_py = std::max(max_py, ndc.y);
  }
  CHECK(std::fabs(min_cz - wds::chart_editor::kOfficialCameraNearClip) < 2e-3f);
  // Leftover must cover a slab of the stage, not a sliver.
  CHECK(max_py - min_py > 0.5f);

  const float view_z = wds::chart_editor::official_main_camera_z(main_y, 0.0f);
  const float aspect = std::fabs(geometry.content().w) /
                       std::max(std::fabs(geometry.content().h), 1e-6f);
  const float tan_half = 1.0f / std::max(wds::chart_editor::official_camera_focal(), 1e-6f);
  const float cap = wds::chart_render::split_effect_billboard_maxp_world_cap(10.0f, view_z, tan_half,
                                                                            aspect);
  CHECK(size < cap);
  CHECK(wds::chart_render::split_effect_billboard_maxp_span(size, cap) == size);

  // Hierarchy / TransformPoint pose at Main Y≈−139 (parent 27×fade on
  // Shape.position.y=-8.73): whole quad behind near. Renderer + scalingMode
  // Local does not use this pose (see evaluate check below).
  float bx[4], by[4], bz[4];
  CHECK(geometry.split_particle_oriented_world_corners(
      -5.55f, -139.3f, 2.0f, 116.7f / 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2, 0.0f, 0.0f, bx, by,
      bz));
  CHECK(!wds::chart_editor::official_main_in_front_of_camera(-139.3f, 0.0f));
  float alpha_max_cz = -1e9f;
  float alpha_min_cz = 1e9f;
  for (int i = 0; i < 4; ++i) {
    const float cz = wds::chart_editor::official_main_camera_z(by[i], bz[i]);
    alpha_max_cz = std::max(alpha_max_cz, cz);
    alpha_min_cz = std::min(alpha_min_cz, cz);
  }
  CHECK(alpha_max_cz < wds::chart_editor::kOfficialCameraNearClip);
  const int bn = wds::chart_editor::official_main_clip_poly_near_uv(bx, by, bz, qu, qv, 4, ox, oy,
                                                                   oz, ou, ov);
  CHECK(bn < 3);

  // 10461 Alpha: Local sim + Local scaling. Shape.position.y=-8.73 stays in
  // PS-local units. Transform localPosition still × parent 27×fade
  // (local_pos_y=0 here). main_y = origin + shape.y + local_pos_y×(27×fade−1).
  const auto* alpha = layer_of(10461, 0);
  CHECK(alpha != nullptr);
  CHECK(alpha->simulation_space ==
        static_cast<uint8_t>(wds::chart_render::SplitEffectSimulationSpace::Local));
  CHECK(alpha->scaling_mode ==
        static_cast<uint8_t>(wds::chart_render::SplitEffectScalingMode::Local));
  CHECK(std::fabs(alpha->shape_pos_y + 8.73f) < 1e-3f);
  CHECK(std::fabs(alpha->local_pos_y) < 1e-4f);
  wds::chart_render::shuriken::EmitContext alpha_ctx;
  alpha_ctx.color_id = 10461;
  alpha_ctx.line_x = -5.55f;
  alpha_ctx.now_t = 0.501f;
  alpha_ctx.fade_scale = wds::chart_render::shuriken::fade_scale_at_play_t(0.501);
  alpha_ctx.line_alpha = 1.0f;
  alpha_ctx.want_trail = false;
  std::vector<wds::chart_render::SplitEffectLiveParticle> alpha_live(16);
  const int na = wds::chart_render::shuriken::evaluate(*alpha, alpha_ctx, alpha_live.data(), 16);
  CHECK(na > 0);
  const float origin = wds::chart_editor::official_split_identity_origin_y() +
                       wds::chart_editor::kOfficialZ180LineHightY *
                           wds::chart_editor::kOfficialLineHeightScale * alpha_ctx.fade_scale;
  const float live_y_scale =
      wds::chart_editor::kOfficialLineHeightScale * alpha_ctx.fade_scale;
  const float expect_y = origin + alpha->shape_pos_y +
                         alpha->local_pos_y * (live_y_scale - 1.0f);
  float a_min_cz = 1e9f;
  float a_max_cz = -1e9f;
  int a_clip_n = 0;
  for (int k = 0; k < na; ++k) {
    const auto& lp = alpha_live[static_cast<size_t>(k)];
    CHECK(std::fabs(lp.main_y - expect_y) < 0.15f);
    CHECK(wds::chart_editor::official_main_in_front_of_camera(lp.main_y, lp.z));
    float qx2[4], qy2[4], qz2[4];
    CHECK(geometry.split_particle_oriented_world_corners(
        lp.world_x, lp.main_y, lp.size, lp.sol_y, lp.pivot_y, lp.roll, lp.tilt_x, lp.tilt_y, lp.z,
        lp.render_alignment, lp.pivot_x, lp.pivot_z, qx2, qy2, qz2));
    for (int i = 0; i < 4; ++i) {
      const float cz = wds::chart_editor::official_main_camera_z(qy2[i], qz2[i]);
      a_min_cz = std::min(a_min_cz, cz);
      a_max_cz = std::max(a_max_cz, cz);
    }
    a_clip_n = wds::chart_editor::official_main_clip_poly_near_uv(qx2, qy2, qz2, qu, qv, 4, ox, oy,
                                                                 oz, ou, ov);
  }
  std::printf("10461 Alpha main_y=%.3f expect=%.3f fade=%.3f camz=%.2f..%.2f clip_n=%d\n",
              static_cast<double>(alpha_live[0].main_y), static_cast<double>(expect_y),
              static_cast<double>(alpha_ctx.fade_scale), static_cast<double>(a_min_cz),
              static_cast<double>(a_max_cz), a_clip_n);
  CHECK(a_max_cz > wds::chart_editor::kOfficialCameraNearClip);
  CHECK(a_clip_n >= 3);

  // Local + identity LineHight (10461 is not in split_fade_grows_from_tip).
  // 10461_1 shape.y=0, local_pos=0: compensation is a no-op; main_y = origin
  // (N2/N6 Bake center ≈43 at fade≈0.77).
  const auto* ident = layer_of(10461, 1);
  CHECK(ident != nullptr);
  if (ident != nullptr) {
    CHECK(ident->simulation_space ==
          static_cast<uint8_t>(wds::chart_render::SplitEffectSimulationSpace::Local));
    CHECK(ident->scaling_mode ==
          static_cast<uint8_t>(wds::chart_render::SplitEffectScalingMode::Local));
    CHECK(std::fabs(ident->shape_pos_y) < 1e-3f);
    CHECK(std::fabs(ident->local_pos_y) < 1e-4f);
    std::vector<wds::chart_render::SplitEffectLiveParticle> ident_live(16);
    const int ni = wds::chart_render::shuriken::evaluate(*ident, alpha_ctx, ident_live.data(), 16);
    CHECK(ni > 0);
    for (int k = 0; k < ni; ++k) {
      CHECK(std::fabs(ident_live[static_cast<size_t>(k)].main_y - origin) < 0.15f);
    }
    std::printf("10461_1 identity main_y=%.3f origin=%.3f\n",
                static_cast<double>(ident_live[0].main_y), static_cast<double>(origin));
  }

  // Local-sim + Local scaling: Transform localPos still × parent; Shape not.
  int local_pos_scaled = 0;
  for (int32_t id : wds::chart_render::official_split_effect_ids()) {
    if (wds::chart_editor::split_fade_grows_from_tip(id)) {
      continue;
    }
    const auto* e = wds::chart_render::official_split_effect(id);
    uint16_t pn = 0;
    const auto* rows = wds::chart_render::official_split_effect_particles(*e, &pn);
    for (uint16_t i = 0; i < pn; ++i) {
      const auto& row = rows[i];
      if (row.simulation_space !=
          static_cast<uint8_t>(wds::chart_render::SplitEffectSimulationSpace::Local)) {
        continue;
      }
      if (row.scaling_mode !=
          static_cast<uint8_t>(wds::chart_render::SplitEffectScalingMode::Local)) {
        continue;
      }
      if (std::fabs(row.local_pos_y) < 0.05f || row.start_speed > 0.01f) {
        continue;
      }
      wds::chart_render::shuriken::EmitContext c = alpha_ctx;
      c.color_id = id;
      std::vector<wds::chart_render::SplitEffectLiveParticle> live(16);
      const int n = wds::chart_render::shuriken::evaluate(row, c, live.data(), 16);
      if (n <= 0) {
        continue;
      }
      const float o = wds::chart_editor::official_split_identity_origin_y() +
                      wds::chart_editor::kOfficialZ180LineHightY *
                          wds::chart_editor::kOfficialLineHeightScale * c.fade_scale;
      const float pred = o + row.shape_pos_y + row.local_pos_y * (live_y_scale - 1.0f);
      const float unscaled = o + row.shape_pos_y + row.local_pos_y;
      float mean = 0.0f;
      for (int k = 0; k < n; ++k) {
        mean += live[static_cast<size_t>(k)].main_y;
      }
      mean /= static_cast<float>(n);
      CHECK(std::fabs(mean - pred) < 0.2f);
      CHECK(std::fabs(mean - pred) + 1e-4f < std::fabs(mean - unscaled));
      ++local_pos_scaled;
    }
  }
  CHECK(local_pos_scaled == 2);

  int local_sol_y = 0;
  int local_sol_y_effects = 0;
  int stretch_n = 0;
  int mesh_n = 0;
  int view_bb = 0;
  int world_bb = 0;
  std::map<int32_t, int> effect_hit;
  for (int32_t id : wds::chart_render::official_split_effect_ids()) {
    const auto* e = wds::chart_render::official_split_effect(id);
    if (e == nullptr) {
      continue;
    }
    uint16_t pn = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*e, &pn);
    for (uint16_t i = 0; i < pn; ++i) {
      if (ps[i].render_mode == wds::chart_render::kSplitEffectRenderStretch) {
        ++stretch_n;
      } else if (ps[i].render_mode == wds::chart_render::kSplitEffectRenderMesh) {
        ++mesh_n;
      } else if (ps[i].render_mode == wds::chart_render::kSplitEffectRenderBillboard) {
        if (ps[i].render_alignment == 0) {
          ++view_bb;
        } else if (ps[i].render_alignment == 1) {
          ++world_bb;
        }
        if (ps[i].render_alignment == 2 && ps[i].sol_y >= 10.0f) {
          ++local_sol_y;
          effect_hit[id] = 1;
        }
      }
    }
  }
  local_sol_y_effects = static_cast<int>(effect_hit.size());
  std::printf(
      "near-clip class Local+SOL Y>=10: %d systems / %d effects; Stretch=%d Mesh=%d ViewBB=%d "
      "WorldBB=%d leftover n=%d ndc_y=%.3f..%.3f near_edge_cz=%.4f size=%.2f maxp_cap=%.1f "
      "fully_behind=%d alpha_camz=%.2f..%.2f\n",
      local_sol_y, local_sol_y_effects, stretch_n, mesh_n, view_bb, world_bb, cn, min_py, max_py,
      min_cz, size, cap, bn, alpha_min_cz, alpha_max_cz);
  CHECK(local_sol_y >= 50);
  CHECK(local_sol_y_effects >= 22);
}

void test_10120_ribbon_near_clip_keeps_camera_front() {
  const auto* e = wds::chart_render::official_split_effect(10120);
  CHECK(e != nullptr);
  uint16_t pn = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*e, &pn);
  for (uint16_t i = 0; i < pn; ++i) {
    std::printf(
        "10120 L%u trail=%u mode=%u shape=%u pos=%.2f,%.2f,%.2f r=%.2f speed=%.2f rod=%.2f "
        "life=%.2f render=%u flags=%u\n",
        i, static_cast<unsigned>(ps[i].flags >> 4 & 1), static_cast<unsigned>(ps[i].trail_mode),
        static_cast<unsigned>(ps[i].shape_type), ps[i].shape_pos_x, ps[i].shape_pos_y,
        ps[i].shape_pos_z, ps[i].shape_radius, ps[i].start_speed, ps[i].rate_over_distance,
        ps[i].lifetime, static_cast<unsigned>(ps[i].render_mode),
        static_cast<unsigned>(ps[i].flags));
  }
  {
    float cx0 = 0, cy0 = 0, cz0 = 0, cx1 = 0, cy1 = 0, cz1 = 0, ct0 = 0, ct1 = 1;
    CHECK(wds::chart_editor::official_main_clip_segment_near(-5.55f, -14.9f, 0.0f, -5.55f, 20.0f,
                                                            0.0f, cx0, cy0, cz0, cx1, cy1, cz1,
                                                            ct0, ct1));
    CHECK(std::fabs(cx0 + 5.55f) < 1e-3f);
    CHECK(std::fabs(cx1 + 5.55f) < 1e-3f);
    CHECK(on_or_in_front_of_near(cy0, cz0));
    CHECK(on_or_in_front_of_near(cy1, cz1));
    const float hw = 0.035f;
    const float qx[4] = {-5.55f - hw, -5.55f + hw, -5.55f + hw, -5.55f - hw};
    const float qy[4] = {-14.9f, -14.9f, 20.0f, 20.0f};
    const float qz[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ox[wds::chart_editor::kOfficialViewClipMaxVerts];
    float oy[wds::chart_editor::kOfficialViewClipMaxVerts];
    float oz[wds::chart_editor::kOfficialViewClipMaxVerts];
    const int cn = wds::chart_editor::official_main_clip_poly_view(
        qx, qy, qz, 4, wds::chart_editor::kOfficialPreviewAspect, ox, oy, oz);
    CHECK(cn >= 3);
    for (int i = 0; i < cn; ++i) {
      CHECK(on_or_in_front_of_near(oy[i], oz[i]));
      const auto ndc = wds::chart_editor::project_main_xyz(ox[i], oy[i], oz[i]);
      CHECK(std::fabs(ndc.x) <= 1.02f);
      CHECK(std::fabs(ndc.y) <= 1.02f);
    }
  }
  wds::chart_editor::PreviewSplitLaneInstance split;
  split.should_show = true;
  split.split_count = 2;
  split.split_lane_type = wds::chart_editor::SplitLaneType::Full;
  split.scratch_length = 10120;
  split.start_ms = 1000;
  split.split_anim_phase = 1;
  split.split_percent_start = 0.0f;
  split.split_percent_end = 1.0f;
  split.split_line_alpha = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  for (double now_t : {0.15, 0.35, 0.55, 0.80}) {
    const double now_sec = 1.0 - wds::chart_editor::kOfficialSplitFadeInSec + now_t;
    const int n = wds::chart_render::collect_split_effect_particles(
        split, wds::chart_editor::kOfficialSpritEffectRich, now_sec, pool.data(),
        wds::chart_render::kSplitEffectParticlePool);
    std::map<uint32_t, std::vector<int>> chains;
    float pmax_dx = 0.0f;
    float pmax_ndc = 0.0f;
    for (int i = 0; i < n; ++i) {
      if (pool[static_cast<size_t>(i)].trail_chain != 0 &&
          pool[static_cast<size_t>(i)].trail_count >= 2) {
        chains[pool[static_cast<size_t>(i)].trail_chain].push_back(i);
      }
      const auto& p = pool[static_cast<size_t>(i)];
      if (p.trail_count >= 2 || p.trail_points < 2) {
        continue;
      }
      for (int t = 0; t + 1 < p.trail_points; ++t) {
        pmax_dx = std::max(pmax_dx, std::fabs(p.trail_x[t + 1] - p.trail_x[t]));
        const auto a = wds::chart_editor::project_main_xyz(p.trail_x[t], p.trail_y[t], p.trail_z[t]);
        const auto b =
            wds::chart_editor::project_main_xyz(p.trail_x[t + 1], p.trail_y[t + 1], p.trail_z[t + 1]);
        pmax_ndc = std::max(pmax_ndc, std::hypot(a.x - b.x, a.y - b.y));
      }
    }
    float cmax_dx = 0.0f;
    float cmax_ndc = 0.0f;
    float cmax_span = 0.0f;
    int worst_cn = 0;
    uint32_t worst_id = 0;
    for (auto& entry : chains) {
      auto& idxs = entry.second;
      std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
        return pool[static_cast<size_t>(a)].trail_vertex <
               pool[static_cast<size_t>(b)].trail_vertex;
      });
      float xmin = 1e9f, xmax = -1e9f;
      for (int idx : idxs) {
        xmin = std::min(xmin, pool[static_cast<size_t>(idx)].world_x);
        xmax = std::max(xmax, pool[static_cast<size_t>(idx)].world_x);
      }
      cmax_span = std::max(cmax_span, xmax - xmin);
      for (size_t k = 0; k + 1 < idxs.size(); ++k) {
        const auto& a = pool[static_cast<size_t>(idxs[k])];
        const auto& b = pool[static_cast<size_t>(idxs[k + 1])];
        const float dx = std::fabs(b.world_x - a.world_x);
        cmax_dx = std::max(cmax_dx, dx);
        float x0, y0, z0, x1, y1, z1, t0, t1;
        if (!wds::chart_editor::official_main_clip_segment_near(
                a.world_x, a.main_y, a.z, b.world_x, b.main_y, b.z, x0, y0, z0, x1, y1, z1, t0,
                t1)) {
          continue;
        }
        CHECK(on_or_in_front_of_near(y0, z0));
        CHECK(on_or_in_front_of_near(y1, z1));
        const auto na = wds::chart_editor::project_main_xyz(x0, y0, z0);
        const auto nb = wds::chart_editor::project_main_xyz(x1, y1, z1);
        float tpx = 0, tpy = 0, tpz = 0;
        wds::chart_editor::official_main_trail_perp(a.world_x, a.main_y, a.z, b.world_x - a.world_x,
                                                    b.main_y - a.main_y, b.z - a.z, tpx, tpy, tpz);
        const float h0 = std::max(a.trail_width, 1e-4f) * 0.5f;
        const float h1 = std::max(b.trail_width, 1e-4f) * 0.5f;
        const float qx[4] = {a.world_x + tpx * h0, a.world_x - tpx * h0, b.world_x - tpx * h1,
                             b.world_x + tpx * h1};
        const float qy[4] = {a.main_y + tpy * h0, a.main_y - tpy * h0, b.main_y - tpy * h1,
                             b.main_y + tpy * h1};
        const float qz[4] = {a.z + tpz * h0, a.z - tpz * h0, b.z - tpz * h1, b.z + tpz * h1};
        float mx[wds::chart_editor::kOfficialViewClipMaxVerts];
        float my[wds::chart_editor::kOfficialViewClipMaxVerts];
        float mz[wds::chart_editor::kOfficialViewClipMaxVerts];
        const int mn = wds::chart_editor::official_main_clip_poly_view(
            qx, qy, qz, 4, wds::chart_editor::kOfficialPreviewAspect, mx, my, mz);
        for (int vi = 0; vi < mn; ++vi) {
          const auto nv = wds::chart_editor::project_main_xyz(mx[vi], my[vi], mz[vi]);
          CHECK(std::fabs(nv.x) <= 1.02f);
          CHECK(std::fabs(nv.y) <= 1.02f);
        }
        const float nd = std::hypot(na.x - nb.x, na.y - nb.y);
        if (nd > cmax_ndc) {
          cmax_ndc = nd;
          worst_cn = static_cast<int>(idxs.size());
          worst_id = entry.first;
        }
      }
    }
    std::printf(
        "10120 collect t=%.2f n=%d chains=%zu chain_span=%.3f chain_dx=%.3f chain_ndc=%.3f "
        "particle_dx=%.3f particle_ndc=%.3f worst_n=%d id=%u\n",
        now_t, n, chains.size(), cmax_span, cmax_dx, cmax_ndc, pmax_dx, pmax_ndc, worst_cn,
        worst_id);
    CHECK(cmax_span < 1.2f);
  }
}

void test_openwds_layer_table_not_2020_only() {
  using wds::chart_render::SplitEffectBlend;
  using wds::chart_render::kSplitEffectFlagButterfly;
  using wds::chart_render::kSplitEffectFlagSizeOverLifetime;
  using wds::chart_render::kSplitEffectFlagTrails;
  using wds::chart_render::official_split_effect_eval_sol;
  using wds::chart_render::official_split_effect_eval_sol_y;
  using wds::chart_render::official_split_effect_late_hdr;
  using wds::chart_render::official_split_effect_src_alpha_one;
  using wds::chart_render::official_syuriken_line_uses_late_hdr;

  CHECK(!official_syuriken_line_uses_late_hdr());
  CHECK(wds::chart_render::kOfficialSyurikenLineSortingOrder == 0);
  CHECK(wds::chart_render::kOfficialSplitEffectSortingGroupOrder == 20);
  CHECK(wds::chart_render::kOfficialNoteSpriteSortingOrder == 10);
  CHECK(wds::chart_render::kOfficialBombSortingGroupOrder == 30);
  CHECK(wds::chart_render::kOfficialTimingEffectSortingOrder == 99);
  CHECK(wds::chart_render::kOfficialSplitEffectSortingGroupOrder >
        wds::chart_render::kOfficialNoteSpriteSortingOrder);
  CHECK(wds::chart_render::kOfficialBombSortingGroupOrder >
        wds::chart_render::kOfficialSplitEffectSortingGroupOrder);
  CHECK(wds::chart_render::kOfficialTimingEffectSortingOrder >
        wds::chart_render::kOfficialBombSortingGroupOrder);
  CHECK(!official_split_effect_late_hdr(static_cast<uint8_t>(SplitEffectBlend::Additive), 0));
  CHECK(official_split_effect_late_hdr(static_cast<uint8_t>(SplitEffectBlend::Additive), 1));
  CHECK(official_split_effect_late_hdr(static_cast<uint8_t>(SplitEffectBlend::Additive), 2));
  CHECK(!official_split_effect_late_hdr(static_cast<uint8_t>(SplitEffectBlend::Other), 2));
  CHECK(!official_split_effect_late_hdr(static_cast<uint8_t>(SplitEffectBlend::Alpha), 2));

  int trail_n = 0;
  int trail_add = 0;
  int butterfly_n = 0;
  int maxp0 = 0;
  int maxp10 = 0;
  const auto ids = wds::chart_render::official_split_effect_ids();
  for (int32_t id : ids) {
    const auto* e = wds::chart_render::official_split_effect(id);
    CHECK(e != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*e, &n);
    for (uint16_t i = 0; i < n; ++i) {
      if (ps[i].blend == static_cast<uint8_t>(SplitEffectBlend::Other)) {
        ++trail_n;
        if (official_split_effect_src_alpha_one(ps[i].blend)) {
          ++trail_add;
        }
      }
      if ((ps[i].flags & kSplitEffectFlagButterfly) != 0) {
        ++butterfly_n;
        CHECK(ps[i].sorting_order == 2);
        CHECK(official_split_effect_src_alpha_one(ps[i].blend) ||
              official_split_effect_late_hdr(static_cast<uint8_t>(SplitEffectBlend::Additive),
                                             ps[i].sorting_order));
      }
      if (ps[i].max_particle_size <= 1e-6f) {
        ++maxp0;
      }
      if (std::fabs(ps[i].max_particle_size - 10.0f) < 1e-4f) {
        ++maxp10;
      }
    }
  }
  CHECK(trail_n == 128);
  CHECK(trail_add == trail_n);
  CHECK(butterfly_n == 16);
  CHECK(maxp0 == 5);
  CHECK(maxp10 == 127);

  const auto* fog2020 = layer_of(2020, 0);
  CHECK(fog2020 != nullptr);
  CHECK((fog2020->flags & kSplitEffectFlagSizeOverLifetime) != 0);
  CHECK(fog2020->sol_y_count == 0);
  CHECK(std::fabs(fog2020->sol_y - 1.0f) < 1e-4f);
  for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    CHECK(std::fabs(official_split_effect_eval_sol(*fog2020, t) -
                    official_split_effect_eval_sol_y(*fog2020, t)) < 1e-5f);
  }

  const auto* fog11391 = layer_of(11391, 0);
  CHECK(fog11391 != nullptr);
  CHECK(fog11391->sorting_order == 2);
  CHECK(std::fabs(fog11391->size - 3.0f) < 1e-4f);
  CHECK(official_split_effect_src_alpha_one(fog11391->blend));
  CHECK(official_split_effect_late_hdr(fog11391->blend, fog11391->sorting_order));

  const auto* fog10160 = layer_of(10160, 0);
  CHECK(fog10160 != nullptr);
  CHECK(fog10160->sorting_order == 1);
  CHECK(fog10160->blend == static_cast<uint8_t>(SplitEffectBlend::Additive));
  CHECK(official_split_effect_late_hdr(fog10160->blend, fog10160->sorting_order));
  const auto* fw10220 = layer_of(10220, 3);
  CHECK(fw10220 != nullptr);
  CHECK(fw10220->sorting_order == 1);
  CHECK(fw10220->blend == static_cast<uint8_t>(SplitEffectBlend::Additive));
  CHECK(official_split_effect_late_hdr(fw10220->blend, fw10220->sorting_order));
  {
    using wds::chart_render::SplitEffectTrailMaterial;
    using wds::chart_render::official_split_effect_trail_blend;
    using wds::chart_render::official_split_effect_trail_intensity;
    using wds::chart_render::official_split_effect_trail_material;
    using wds::chart_render::official_split_effect_trail_uses_bomb_tex;
    const auto* e10220 = wds::chart_render::official_split_effect(10220);
    CHECK(e10220 != nullptr);
    if (e10220 != nullptr) {
      const uint32_t i11 = static_cast<uint32_t>(e10220->particle_begin) + 1u;
      CHECK(official_split_effect_trail_material(i11) == SplitEffectTrailMaterial::ParticleAdd);
      CHECK(!official_split_effect_trail_uses_bomb_tex(SplitEffectTrailMaterial::ParticleAdd));
      CHECK(std::fabs(official_split_effect_trail_intensity(SplitEffectTrailMaterial::ParticleAdd) -
                      1.0f) < 1e-6f);
      CHECK(official_split_effect_late_hdr(
          official_split_effect_trail_blend(SplitEffectTrailMaterial::ParticleAdd), 1));
    }
    int trail_add_mat = 0;
    int trail_alpha_mat = 0;
    for (int32_t id : wds::chart_render::official_split_effect_ids()) {
      const auto* e = wds::chart_render::official_split_effect(id);
      if (e == nullptr) {
        continue;
      }
      for (uint16_t i = 0; i < e->particle_count; ++i) {
        const auto tm = official_split_effect_trail_material(
            static_cast<uint32_t>(e->particle_begin) + i);
        if (tm == SplitEffectTrailMaterial::ParticleAdd) {
          ++trail_add_mat;
        } else if (tm == SplitEffectTrailMaterial::ParticleAlpha) {
          ++trail_alpha_mat;
        }
      }
    }
    CHECK(trail_add_mat == 9);
    CHECK(trail_alpha_mat == 5);
  }

  const auto* trail = layer_of(10120, 0);
  CHECK(trail != nullptr);
  CHECK(official_split_effect_src_alpha_one(trail->blend));
  CHECK(!official_split_effect_late_hdr(trail->blend, trail->sorting_order));
  CHECK(trail->trail_mode == 1);
  CHECK((trail->flags & wds::chart_render::kSplitEffectFlagTrails) != 0);
  CHECK(trail->trail_die_with == 0);
  CHECK(trail->trail_size_affects_width != 0);
  CHECK(trail->trail_width_count >= 2);
  CHECK(trail->trail_over_a0 > 0.9f);
  CHECK(trail->trail_over_a1 < 0.1f);
  CHECK(trail->uv_name_index == wds::chart_render::kSplitEffectNoUvName);
  CHECK(std::strcmp(wds::chart_render::official_split_effect_trail_tex_relpath(),
                    "bomb_effect_trail.png") == 0);
  CHECK(std::fabs(wds::chart_render::kOfficialSplitEffectTrailAdditiveIntensity - 2.0f) < 1e-4f);
  {
    const auto trail_png = find_skins_png("bomb_effect_trail.png");
    CHECK(!trail_png.empty());
    if (!trail_png.empty()) {
      std::vector<unsigned char> rgba;
      int w = 0, h = 0;
      CHECK(wds::renderer::load_png_rgba8(trail_png.string(), rgba, w, h));
      CHECK(w == 128);
      CHECK(h == 128);
      if (w == 128 && h == 128 && rgba.size() >= 4) {
        const size_t mid = (static_cast<size_t>(64) * 128 + 64) * 4;
        CHECK(rgba[mid + 3] > 200);
      }
    }
  }
  int persist_ribbons = 0;
  int width_curves = 0;
  int leftover_after_keep = 0;
  int leftover_die_with = 0;
  int trail_systems = 0;
  int particle_mode_trails = 0;
  int particle_mode_dead_kept = 0;
  std::vector<wds::chart_render::SplitEffectLiveParticle> leftover_buf(512);
  for (int32_t id : official_split_color_ids()) {
    const auto* e = wds::chart_render::official_split_effect(id);
    if (e == nullptr) {
      continue;
    }
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*e, &n);
    for (uint16_t i = 0; i < n; ++i) {
      if ((ps[i].flags & wds::chart_render::kSplitEffectFlagTrails) == 0) {
        continue;
      }
      ++trail_systems;
      if (ps[i].trail_mode == 0) {
        ++particle_mode_trails;
      }
      if (ps[i].trail_mode == 1) {
        if (ps[i].trail_die_with == 0) {
          ++persist_ribbons;
        }
        if (ps[i].trail_width_count >= 2) {
          ++width_curves;
        }
      }
      // Unity 2022.3 dieWithParticles=false keep = life + L×life is
      // PerParticle only. Ribbon drops the particle at death.
      const float length = ps[i].length_in_sec > 1e-6f ? ps[i].length_in_sec : 1.0f;
      const float part_life = std::max(ps[i].lifetime, ps[i].lifetime_min);
      const float speed = ps[i].simulation_speed > 1e-6f ? ps[i].simulation_speed : 1.0f;
      const float delay_wall = wds::chart_render::official_split_effect_start_delay(ps[i]) / speed;
      float trail_mul = std::max(ps[i].trail_lifetime, 0.0f);
      if (ps[i].trail_lifetime_random != 0) {
        trail_mul = std::max(trail_mul, ps[i].trail_lifetime_min);
      }
      static constexpr float kTrailPeakT[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
      for (float t : kTrailPeakT) {
        if (ps[i].trail_lifetime_count > 0) {
          trail_mul = std::max(
              trail_mul, std::fabs(wds::chart_render::official_split_effect_eval_curve_keys(
                             ps[i].trail_lifetime_begin, ps[i].trail_lifetime_count, t,
                             ps[i].trail_lifetime)));
        }
        if (ps[i].trail_lifetime_min_count > 0) {
          trail_mul = std::max(
              trail_mul, std::fabs(wds::chart_render::official_split_effect_eval_curve_keys(
                             ps[i].trail_lifetime_min_begin, ps[i].trail_lifetime_min_count, t,
                             ps[i].trail_lifetime_min)));
        }
      }
      float keep = part_life;
      if (ps[i].trail_die_with == 0 && ps[i].trail_mode == 0) {
        keep = part_life + trail_mul * part_life;
      }
      wds::chart_render::shuriken::EmitContext late_ctx;
      late_ctx.color_id = id;
      late_ctx.now_t = delay_wall + (length + keep) / speed + 0.05f;
      late_ctx.fade_scale = 1.0f;
      late_ctx.line_alpha = 1.0f;
      late_ctx.want_trail = true;
      const int nleft =
          wds::chart_render::shuriken::evaluate(ps[i], late_ctx, leftover_buf.data(), 512);
      leftover_after_keep += nleft;
      if (ps[i].trail_die_with != 0) {
        leftover_die_with += nleft;
      }
      if (ps[i].trail_mode == 0 && ps[i].trail_die_with == 0) {
        wds::chart_render::shuriken::EmitContext mid_ctx = late_ctx;
        mid_ctx.now_t = delay_wall + part_life / speed + 0.05f;
        const int nmid =
            wds::chart_render::shuriken::evaluate(ps[i], mid_ctx, leftover_buf.data(), 512);
        for (int k = 0; k < nmid; ++k) {
          if (leftover_buf[static_cast<size_t>(k)].remaining_lifetime <= 1e-5f) {
            ++particle_mode_dead_kept;
          }
        }
      }
    }
  }
  std::printf("trail scan systems=%d ribbon_die_with0=%d particle_mode=%d leftover=%d die_with=%d\n",
              trail_systems, persist_ribbons, particle_mode_trails, leftover_after_keep,
              leftover_die_with);
  CHECK(persist_ribbons >= 50);
  CHECK(width_curves >= 30);
  CHECK(trail_systems >= 100);
  CHECK(particle_mode_trails >= 20);
  CHECK(particle_mode_dead_kept > 0);
  CHECK(leftover_after_keep == 0);
  CHECK(leftover_die_with == 0);
  {
    const auto* cap = layer_of(11791, 0);
    CHECK(cap != nullptr && cap->max_particles == 2);
    if (cap != nullptr) {
      wds::chart_render::shuriken::EmitContext ctx;
      ctx.color_id = 11791;
      ctx.now_t = 0.15f;
      ctx.fade_scale = 1.0f;
      ctx.line_alpha = 1.0f;
      std::vector<wds::chart_render::SplitEffectLiveParticle> live(64);
      const int n = wds::chart_render::shuriken::evaluate(*cap, ctx, live.data(), 64);
      CHECK(n == 2);
      CHECK(n <= static_cast<int>(cap->max_particles));
    }
  }
  {
    CHECK(trail != nullptr && trail->trail_die_with == 0);
    if (trail != nullptr) {
      const float part_life = std::max(trail->lifetime, trail->lifetime_min);
      wds::chart_render::shuriken::EmitContext tctx;
      tctx.color_id = 10120;
      tctx.now_t = part_life + 0.05f;
      tctx.fade_scale = 1.0f;
      tctx.line_alpha = 1.0f;
      tctx.want_trail = true;
      std::vector<wds::chart_render::SplitEffectLiveParticle> live(512);
      const int n = wds::chart_render::shuriken::evaluate(*trail, tctx, live.data(), 512);
      CHECK(n > 0);
      int dead_body = 0;
      for (int i = 0; i < n; ++i) {
        if (live[static_cast<size_t>(i)].remaining_lifetime <= 1e-5f) {
          ++dead_body;
        }
      }
      CHECK(dead_body == 0);
    }
  }
  const auto* scatter = layer_of(10120, 2);
  CHECK(scatter != nullptr);
  CHECK(scatter->shape_type == 10);
  CHECK(std::fabs(scatter->shape_rot_x - 90.0f) < 1e-3f);
  CHECK(std::fabs(scatter->start_speed - 10.0f) < 1e-4f);
  {
    wds::chart_render::shuriken::EmitContext tctx;
    tctx.color_id = 10120;
    tctx.now_t = 0.55f;
    tctx.fade_scale = 1.0f;
    tctx.line_alpha = 1.0f;
    tctx.want_trail = true;
    tctx.now_t = 0.35f;
    std::vector<wds::chart_render::SplitEffectLiveParticle> ribbons(512);
    const int nr = wds::chart_render::shuriken::evaluate(*trail, tctx, ribbons.data(), 512);
    int segs = 0;
    float ribbon_len = 0.0f;
    for (int i = 0; i < nr; ++i) {
      const auto& p = ribbons[static_cast<size_t>(i)];
      if (p.trail_points < 2) {
        continue;
      }
      const float dx = p.trail_x[1] - p.trail_x[0];
      const float dy = p.trail_y[1] - p.trail_y[0];
      const float dz = p.trail_z[1] - p.trail_z[0];
      const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (len > 1e-4f) {
        ++segs;
        ribbon_len += len;
      }
    }
    std::printf("10120 ribbon n=%d segs=%d len=%.3f tint=%.3f,%.3f,%.3f\n", nr, segs, ribbon_len,
                trail->trail_r, trail->trail_g, trail->trail_b);
    CHECK(segs > 2);
    CHECK(ribbon_len > 0.5f);
    CHECK(trail->trail_g < 0.95f);

    std::vector<wds::chart_render::SplitEffectLiveParticle> burst(512);
    const int ns = wds::chart_render::shuriken::evaluate(*scatter, tctx, burst.data(), 512);
    float off = 0.0f;
    for (int i = 0; i < ns; ++i) {
      off += std::sqrt(burst[static_cast<size_t>(i)].world_x * burst[static_cast<size_t>(i)].world_x +
                       burst[static_cast<size_t>(i)].z * burst[static_cast<size_t>(i)].z);
    }
    if (ns > 0) {
      off /= static_cast<float>(ns);
    }
    float bmin = 1e9f, bmax = -1e9f;
    for (int i = 0; i < ns; ++i) {
      bmin = std::min(bmin, burst[static_cast<size_t>(i)].birth_t);
      bmax = std::max(bmax, burst[static_cast<size_t>(i)].birth_t);
    }
    std::printf("10120 scatter n=%d mean_xz=%.3f birth=[%.3f,%.3f]\n", ns, off, bmin, bmax);
    CHECK(ns > 4);
    CHECK(off > 0.12f);

    float tw_max = 0.0f;
    float tw_min = 1e9f;
    float rho_max = 0.0f;
    float max_seg = 0.0f;
    int cam_ok = 0;
    for (int i = 0; i < nr; ++i) {
      const auto& p = ribbons[static_cast<size_t>(i)];
      tw_max = std::max(tw_max, p.trail_width);
      if (p.trail_points >= 2) {
        tw_min = std::min(tw_min, p.trail_width);
        max_seg = std::max(max_seg, std::hypot(p.trail_x[1] - p.trail_x[0],
                                               p.trail_z[1] - p.trail_z[0]));
      }
      rho_max = std::max(rho_max, std::hypot(p.world_x - tctx.line_x, p.z));
      if (p.trail_points >= 2 &&
          wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z)) {
        ++cam_ok;
      }
    }
    std::printf("10120 ribbon xz rho=%.3f seg=%.3f width=[%.3f,%.3f]\n", rho_max, max_seg,
                tw_min > 1e8f ? 0.0f : tw_min, tw_max);
    // sizeAffectsWidth × current size (startSize × SOL). Birth startSize alone
    // peaked ~0.14 — thicker than OpenWDS. SOL Two Curves mid ≈ 0.5.
    CHECK(tw_max > 0.03f);
    CHECK(tw_max < 0.12f);
    CHECK(tw_min < tw_max * 0.55f);
    CHECK(rho_max > 0.08f);
    CHECK(max_seg > 0.05f);
    CHECK(cam_ok > 2);
    // Same Sphere+RoD current: 2010 layer 0, 10590 first Ribbon (not layer 0).
    for (int32_t sib : {2010, 10590}) {
      const auto* entry = wds::chart_render::official_split_effect(sib);
      CHECK(entry != nullptr);
      uint16_t pn = 0;
      const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &pn);
      const wds::chart_render::SplitEffectParticle* layer = nullptr;
      for (uint16_t i = 0; i < pn; ++i) {
        if (ps[i].trail_mode == 1 && ps[i].shape_radius > 0.15f) {
          layer = ps + i;
          break;
        }
      }
      CHECK(layer != nullptr);
      std::vector<wds::chart_render::SplitEffectLiveParticle> sibs(512);
      const int nsib = wds::chart_render::shuriken::evaluate(*layer, tctx, sibs.data(), 512);
      float sib_rho = 0.0f;
      float sib_seg = 0.0f;
      for (int i = 0; i < nsib; ++i) {
        const auto& p = sibs[static_cast<size_t>(i)];
        sib_rho = std::max(sib_rho, std::hypot(p.world_x - tctx.line_x, p.z));
        if (p.trail_points >= 2) {
          sib_seg = std::max(sib_seg, std::hypot(p.trail_x[1] - p.trail_x[0],
                                                 p.trail_z[1] - p.trail_z[0]));
        }
      }
      std::printf("%d ribbon xz rho=%.3f seg=%.3f n=%d\n", sib, sib_rho, sib_seg, nsib);
      CHECK(sib_rho > 0.08f);
      CHECK(sib_seg > 0.05f);
    }

    int stretch_n = 0;
    for (int i = 0; i < nr; ++i) {
      const auto& p = ribbons[static_cast<size_t>(i)];
      if (p.trail_points == 2 && std::fabs(p.trail_u1 - p.trail_u0) < 0.5f) {
        ++stretch_n;
      }
    }
    CHECK(stretch_n > 2);

    const auto* thin_trail = layer_of(10120, 1);
    CHECK(thin_trail != nullptr);
    CHECK(thin_trail->trail_mode == 1);
    // Unity Two Curves: curveMultiplier (scalar 0.2) applies to both curves.
    // Packing minCurve with minScalar 1.0 made 10122 as fat as 10121.
    CHECK(std::fabs(thin_trail->size - 0.2f) < 1e-4f);
    const float thin_vmin0 = wds::chart_render::official_split_effect_eval_curve_keys(
        thin_trail->size_min_curve_begin, thin_trail->size_min_curve_count, 0.0f,
        thin_trail->size_min);
    CHECK(std::fabs(thin_vmin0 - 0.1f) < 0.03f);
    std::vector<wds::chart_render::SplitEffectLiveParticle> thin_live(512);
    const int nt = wds::chart_render::shuriken::evaluate(*thin_trail, tctx, thin_live.data(), 512);
    float thin_smax = 0.0f;
    float fat_smax = 0.0f;
    for (int i = 0; i < nt; ++i) {
      thin_smax = std::max(thin_smax, thin_live[static_cast<size_t>(i)].size);
    }
    for (int i = 0; i < nr; ++i) {
      fat_smax = std::max(fat_smax, ribbons[static_cast<size_t>(i)].size);
    }
    std::printf("10120 two-curve size fat=%.3f thin=%.3f vmin0=%.3f\n", fat_smax, thin_smax,
                thin_vmin0);
    {
      auto lat = [&](const wds::chart_render::SplitEffectLiveParticle* ps, int n) {
        float rmax = 0, mean = 0, wmax = 0;
        for (int i = 0; i < n; ++i) {
          const auto& p = ps[static_cast<size_t>(i)];
          const float rho = std::hypot(p.world_x - tctx.line_x, p.z);
          rmax = std::max(rmax, rho);
          mean += rho;
          wmax = std::max(wmax, p.trail_width);
        }
        if (n > 0) {
          mean /= static_cast<float>(n);
        }
        return std::array<float, 3>{rmax, mean, wmax};
      };
      const auto fat_lat = lat(ribbons.data(), nr);
      const auto thin_lat = lat(thin_live.data(), nt);
      std::printf("10120 ribbon xz fat=%.3f mean=%.3f w=%.3f thin=%.3f mean=%.3f w=%.3f\n",
                  fat_lat[0], fat_lat[1], fat_lat[2], thin_lat[0], thin_lat[1], thin_lat[2]);
      // Unity Shape Sphere is 3D. Authored r=0.2 / 0.4: thin 3D radius from
      // the line is larger. Do not remap XZ onto X.
      CHECK(fat_lat[0] > 0.08f);
      CHECK(thin_lat[0] > fat_lat[0] * 1.2f);
      CHECK(thin_lat[1] > fat_lat[1]);
      CHECK(fat_lat[2] > thin_lat[2] * 1.8f);
      for (int32_t sib : {2010, 10371, 10590, 11140, 11322, 11380, 11511, 11790, 11811}) {
        const auto* entry = wds::chart_render::official_split_effect(sib);
        CHECK(entry != nullptr);
        uint16_t pn = 0;
        const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &pn);
        const wds::chart_render::SplitEffectParticle* fat_l = nullptr;
        const wds::chart_render::SplitEffectParticle* thin_l = nullptr;
        for (uint16_t i = 0; i < pn; ++i) {
          if (ps[i].trail_mode != 1 || ps[i].shape_type > 3) {
            continue;
          }
          if (fat_l == nullptr || ps[i].shape_radius < fat_l->shape_radius) {
            fat_l = ps + i;
          }
          if (thin_l == nullptr || ps[i].shape_radius > thin_l->shape_radius) {
            thin_l = ps + i;
          }
        }
        CHECK(fat_l != nullptr);
        CHECK(thin_l != nullptr);
        CHECK(thin_l->shape_radius > fat_l->shape_radius * 1.2f);
        std::vector<wds::chart_render::SplitEffectLiveParticle> fa(512);
        std::vector<wds::chart_render::SplitEffectLiveParticle> th(512);
        const int nfa = wds::chart_render::shuriken::evaluate(*fat_l, tctx, fa.data(), 512);
        const int nth = wds::chart_render::shuriken::evaluate(*thin_l, tctx, th.data(), 512);
        const auto fa_lat = lat(fa.data(), nfa);
        const auto th_lat = lat(th.data(), nth);
        std::printf("%d ribbon xz r=%.2f/%.2f rho=%.3f/%.3f\n", sib, fat_l->shape_radius,
                    thin_l->shape_radius, fa_lat[0], th_lat[0]);
        CHECK(nfa > 2);
        CHECK(nth > 2);
        CHECK(th_lat[0] > fa_lat[0] * 1.2f);
      }
    }
    CHECK(thin_smax > 1e-4f);
    CHECK(thin_smax < 0.28f);
    CHECK(fat_smax > thin_smax * 1.8f);
    const float fade35 = wds::chart_editor::official_split_fade_in_scale(0.35f);
    const float tip_y = wds::chart_editor::official_split_identity_origin_y() +
                        wds::chart_editor::kOfficialZ180LineHightY *
                            wds::chart_editor::kOfficialLineHeightScale * fade35;
    float ahead = 0.0f;
    for (int i = 0; i < nt; ++i) {
      ahead = std::max(ahead, thin_live[static_cast<size_t>(i)].main_y - tip_y);
    }
    for (int i = 0; i < nr; ++i) {
      ahead = std::max(ahead, ribbons[static_cast<size_t>(i)].main_y - tip_y);
    }
    std::printf("10120 tip ahead=%.3f tip=%.3f\n", ahead, tip_y);
    // Sphere r=0.4, Scaling Mode Local: not r×27 past the already-scaled tip.
    CHECK(ahead < 0.55f);
    CHECK(std::fabs(trail->trail_lifetime - 1.0f) < 1e-4f);
    // Unity Trails width is world units at the clipped vertex. Measuring
    // screen half at a behind-camera birth (project z-clamped to 1e-4)
    // explodes; that half must not be lerped onto the visible segment.
    const auto* yoff = layer_of(10120, 4);
    CHECK(yoff != nullptr);
    CHECK(yoff->trail_mode == 1);
    CHECK(yoff->shape_pos_y < -1.0f);
    {
      auto ndc_half = [](float x, float y, float z, float world_w) {
        const float h = std::max(world_w, 1e-4f) * 0.5f;
        const auto a = wds::chart_editor::project_main_xyz(x + h, y, z);
        const auto b = wds::chart_editor::project_main_xyz(x - h, y, z);
        return 0.5f * std::fabs(a.x - b.x);
      };
      wds::chart_render::shuriken::EmitContext octx = tctx;
      octx.now_t = 0.15f;
      octx.line_index = 2;
      octx.line_x = 3.7f;
      std::vector<wds::chart_render::SplitEffectLiveParticle> yoff_live(512);
      const int ny = wds::chart_render::shuriken::evaluate(*yoff, octx, yoff_live.data(), 512);
      int yoff_behind = 0;
      float yoff_ndc = 0.0f;
      float raw_half = 0.0f;
      float clip_half = 0.0f;
      for (int i = 0; i < ny; ++i) {
        const auto& p = yoff_live[static_cast<size_t>(i)];
        if (wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z)) {
          continue;
        }
        ++yoff_behind;
        const auto ndc = wds::chart_editor::project_main_xyz(p.world_x, p.main_y, p.z);
        yoff_ndc = std::max(yoff_ndc, std::fabs(ndc.x));
        raw_half = std::max(raw_half, ndc_half(p.world_x, p.main_y, p.z,
                                              std::max(p.trail_width, 0.2f)));
        for (int j = 0; j < ny; ++j) {
          const auto& q = yoff_live[static_cast<size_t>(j)];
          if (!wds::chart_editor::official_main_in_front_of_camera(q.main_y, q.z)) {
            continue;
          }
          float x0, y0, z0, x1, y1, z1, t0, t1;
          if (!wds::chart_editor::official_main_clip_segment_near(
                  p.world_x, p.main_y, p.z, q.world_x, q.main_y, q.z, x0, y0, z0, x1, y1, z1, t0,
                  t1)) {
            continue;
          }
          CHECK(on_or_in_front_of_near(y0, z0));
          CHECK(on_or_in_front_of_near(y1, z1));
          const float w0 = p.trail_width + (q.trail_width - p.trail_width) * t0;
          clip_half = std::max(clip_half, ndc_half(x0, y0, z0, w0));
          clip_half = std::max(clip_half, ndc_half(x1, y1, z1, q.trail_width));
        }
      }
      std::printf("10125 behind=%d ndc_x=%.1f raw_half=%.1f clip_half=%.3f n=%d\n", yoff_behind,
                  yoff_ndc, raw_half, clip_half, ny);
      CHECK(ny > 2);
      CHECK(yoff_behind > 0);
      CHECK(yoff_ndc > 2.0f);
      CHECK(raw_half > 10.0f * clip_half);
      CHECK(clip_half > 0.0f);
      CHECK(clip_half < 0.35f);
      float cx0 = 0, cy0 = 0, cz0 = 0, cx1 = 0, cy1 = 0, cz1 = 0, ct0 = 0, ct1 = 1;
      CHECK(wds::chart_editor::official_main_clip_segment_near(3.7f, -14.9f, 0.0f, 3.7f, 20.0f, 0.0f,
                                                              cx0, cy0, cz0, cx1, cy1, cz1, ct0,
                                                              ct1));
      CHECK(std::fabs(cx0 - 3.7f) < 1e-4f);
      CHECK(std::fabs(cx1 - 3.7f) < 1e-4f);
      CHECK(on_or_in_front_of_near(cy0, cz0));
      CHECK(on_or_in_front_of_near(cy1, cz1));
      const auto clip_ndc = wds::chart_editor::project_main_xyz(cx0, cy0, cz0);
      const auto line_ndc = wds::chart_editor::project_main_xyz(3.7f, cy0, 0.0f);
      CHECK(std::fabs(clip_ndc.x - line_ndc.x) < 1e-4f);
      CHECK(std::fabs(wds::chart_editor::official_main_camera_z(cy0, cz0) -
                      wds::chart_editor::kOfficialCameraNearClip) < 2e-3f);
      // near=0.1 → |ndc.x|≈45 at x=3.7. z-clamping wz to 1e-4 explodes to ~4e4.
      CHECK(std::fabs(clip_ndc.x) < 80.0f);
    }
    {
      wds::chart_render::shuriken::EmitContext lctx = tctx;
      lctx.now_t = 0.35f;
      lctx.line_index = 0;
      lctx.line_x = -3.7f;
      std::vector<wds::chart_render::SplitEffectLiveParticle> left(512);
      std::vector<wds::chart_render::SplitEffectLiveParticle> right(512);
      const int nl = wds::chart_render::shuriken::evaluate(*trail, lctx, left.data(), 512);
      lctx.line_index = 1;
      lctx.line_x = 3.7f;
      const int nr2 = wds::chart_render::shuriken::evaluate(*trail, lctx, right.data(), 512);
      CHECK(nl >= 2);
      CHECK(nr2 >= 2);
      CHECK(left[0].trail_chain != 0);
      CHECK(left[0].trail_chain != right[0].trail_chain);
    }
    int offset_ribbons = 0;
    int offset_behind_systems = 0;
    int mixed_ribbons = 0;
    std::vector<wds::chart_render::SplitEffectLiveParticle> scan_live(512);
    for (int32_t id : wds::chart_render::official_split_effect_ids()) {
      const auto* entry = wds::chart_render::official_split_effect(id);
      if (entry == nullptr) {
        continue;
      }
      uint16_t pn = 0;
      const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &pn);
      for (uint16_t i = 0; i < pn; ++i) {
        if (ps[i].trail_mode != 1) {
          continue;
        }
        if (std::fabs(ps[i].shape_pos_y) < 0.5f && std::fabs(ps[i].shape_pos_z) < 0.5f) {
          continue;
        }
        ++offset_ribbons;
        wds::chart_render::shuriken::EmitContext sctx = tctx;
        sctx.now_t = 0.15f;
        sctx.line_index = 2;
        sctx.line_x = 3.7f;
        sctx.color_id = id;
        const int ns = wds::chart_render::shuriken::evaluate(ps[i], sctx, scan_live.data(), 512);
        int behind = 0;
        int front = 0;
        for (int k = 0; k < ns; ++k) {
          if (wds::chart_editor::official_main_in_front_of_camera(scan_live[static_cast<size_t>(k)].main_y,
                                                                 scan_live[static_cast<size_t>(k)].z)) {
            ++front;
          } else {
            ++behind;
          }
        }
        if (behind > 0) {
          ++offset_behind_systems;
        }
        if (behind > 0 && front > 0) {
          ++mixed_ribbons;
          auto ndc_half = [](float x, float y, float z, float world_w) {
            const float h = std::max(world_w, 1e-4f) * 0.5f;
            const auto a = wds::chart_editor::project_main_xyz(x + h, y, z);
            const auto b = wds::chart_editor::project_main_xyz(x - h, y, z);
            return 0.5f * std::fabs(a.x - b.x);
          };
          for (int a = 0; a < ns; ++a) {
            const auto& pa = scan_live[static_cast<size_t>(a)];
            if (wds::chart_editor::official_main_in_front_of_camera(pa.main_y, pa.z)) {
              continue;
            }
            for (int b = 0; b < ns; ++b) {
              const auto& pb = scan_live[static_cast<size_t>(b)];
              if (!wds::chart_editor::official_main_in_front_of_camera(pb.main_y, pb.z)) {
                continue;
              }
              float x0, y0, z0, x1, y1, z1, t0, t1;
              if (!wds::chart_editor::official_main_clip_segment_near(
                      pa.world_x, pa.main_y, pa.z, pb.world_x, pb.main_y, pb.z, x0, y0, z0, x1, y1,
                      z1, t0, t1)) {
                continue;
              }
              CHECK(on_or_in_front_of_near(y0, z0));
              CHECK(on_or_in_front_of_near(y1, z1));
              const float w0 = pa.trail_width + (pb.trail_width - pa.trail_width) * t0;
              const float raw = ndc_half(pa.world_x, pa.main_y, pa.z, std::max(pa.trail_width, 0.2f));
              const float hh = std::max(ndc_half(x0, y0, z0, w0), ndc_half(x1, y1, z1, pb.trail_width));
              CHECK(hh < raw);
            }
          }
        }
      }
    }
    std::printf("ribbon offset-YZ systems=%d behind=%d mixed=%d\n", offset_ribbons,
                offset_behind_systems, mixed_ribbons);
    CHECK(offset_ribbons >= 2);
    CHECK(offset_behind_systems >= 1);
    tctx.now_t = 1.20f;
    const int late = wds::chart_render::shuriken::evaluate(*trail, tctx, ribbons.data(), 512);
    int late_segs = 0;
    for (int i = 0; i < late; ++i) {
      if (ribbons[static_cast<size_t>(i)].trail_points >= 2) {
        ++late_segs;
      }
    }
    std::printf("10120 after life 1.20 n=%d segs=%d\n", late, late_segs);
    // Ribbon dieWith=false: particle leaves at death (life=0.2). Last
    // Duration births are gone by 1.20. PerParticle keep=life+L×life does
    // not apply. trail_lifetime as extra seconds would persist to ~2.2.
    CHECK(late == 0);
    tctx.now_t = 1.45f;
    const int expired = wds::chart_render::shuriken::evaluate(*trail, tctx, ribbons.data(), 512);
    CHECK(expired == 0);
  }

  // 10620 / 11070 Butterfly pathId is the dedicated bat wing. The sheet also
  // names a heart Split_10620_1 — those are different pathIds.
  const auto* bat_wing = layer_of(10620, 2);
  CHECK(bat_wing != nullptr);
  CHECK((bat_wing->flags & kSplitEffectFlagButterfly) != 0);
  CHECK(bat_wing->sorting_order == 2);
  CHECK(std::strcmp(wds::chart_render::official_split_effect_uv_name(bat_wing->uv_name_index),
                    "Split_10620_1") == 0);
  const auto* sheet = layer_of(10620, 1);
  CHECK(sheet != nullptr);
  CHECK(sheet->uv_frame_count >= 2);
  const uint16_t sheet_heart = wds::chart_render::official_split_effect_uv_frame(*sheet, 0.9999f, 0.0f,
                                                                               0.0f);
  CHECK(sheet_heart != bat_wing->uv_name_index);
  CHECK(std::strcmp(wds::chart_render::official_split_effect_uv_name(sheet_heart),
                    "Split_10620/Split_10620_1") == 0);
  const auto* bat_11070 = layer_of(11070, 2);
  CHECK(bat_11070 != nullptr);
  CHECK(bat_11070->uv_name_index == bat_wing->uv_name_index);

  const auto* moth = layer_of(3010, 0);
  CHECK(moth != nullptr);
  CHECK((moth->flags & kSplitEffectFlagButterfly) != 0);
  CHECK(moth->sorting_order == 2);
  CHECK(std::strcmp(wds::chart_render::official_split_effect_uv_name(moth->uv_name_index),
                    "SenceFx_301") == 0);

  const auto* maxp = layer_of(11320, 2);
  CHECK(maxp != nullptr);
  CHECK(maxp->max_particle_size <= 1e-6f);
  CHECK(layer_of(11791, 5) != nullptr && layer_of(11791, 5)->max_particle_size <= 1e-6f);
  CHECK(layer_of(11792, 0) != nullptr && layer_of(11792, 0)->max_particle_size <= 1e-6f);

  const auto* chain = layer_of(11320, 1);
  CHECK(chain != nullptr);
  CHECK(chain->render_mode == wds::chart_render::kSplitEffectRenderStretch);
  CHECK(chain->size_3d != 0);
  CHECK(chain->size_y_curve_count >= 2);
  CHECK(std::fabs(wds::chart_render::official_split_effect_eval_curve_keys(
                      chain->size_y_curve_begin, chain->size_y_curve_count, 0.0f,
                      chain->start_size_y) -
                  1.1f) < 0.05f);
  CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_length(
                      chain->size, chain->start_size_y, chain->size_3d, chain->length_scale, 0.0f,
                      0.0f) -
                  chain->start_size_y * chain->length_scale) < 1e-3f);
  CHECK(std::fabs(wds::chart_render::official_split_effect_stretch_height(
                      chain->size, chain->start_size_y, chain->size_3d) -
                  chain->start_size_y) < 1e-3f);
  {
    wds::chart_render::shuriken::EmitContext chain_ctx;
    chain_ctx.color_id = 11320;
    chain_ctx.now_t = 1.2f;
    chain_ctx.fade_scale = 1.0f;
    chain_ctx.line_alpha = 1.0f;
    std::vector<wds::chart_render::SplitEffectLiveParticle> links(256);
    const int nlinks = wds::chart_render::shuriken::evaluate(*chain, chain_ctx, links.data(), 256);
    CHECK(nlinks > 0);
    for (int i = 0; i < nlinks; ++i) {
      const auto& lp = links[static_cast<size_t>(i)];
      CHECK(std::fabs(lp.vel_y) > std::fabs(lp.vel_x) * 2.0f + 1e-4f);
    }
  }

  CHECK(std::fabs(maxp->random_position_amount - 0.1f) < 1e-4f);
  CHECK(maxp->rod_curve_count >= 2);
  CHECK(std::fabs(wds::chart_render::official_split_effect_eval_curve_keys(
                      maxp->rod_curve_begin, maxp->rod_curve_count, 0.0f, maxp->rate_over_distance) -
                  8.0f) < 0.2f);
  CHECK(std::fabs(wds::chart_render::official_split_effect_eval_curve_keys(
                      maxp->rod_curve_begin, maxp->rod_curve_count, 1.0f, maxp->rate_over_distance)) <
        0.2f);
  const auto* cur4 = layer_of(11320, 3);
  CHECK(cur4 != nullptr);
  CHECK(std::fabs(cur4->random_position_amount - 0.1f) < 1e-4f);
  CHECK(cur4->rod_curve_count >= 2);

  wds::chart_render::shuriken::EmitContext ctx;
  ctx.now_t = 1.2f;
  ctx.fade_scale = 1.0f;
  ctx.line_alpha = 1.0f;

  ctx.color_id = 2020;
  std::vector<wds::chart_render::SplitEffectLiveParticle> fog(256);
  const int nf = wds::chart_render::shuriken::evaluate(*fog2020, ctx, fog.data(), 256);
  CHECK(nf > 0);
  const float fog_aspect =
      wds::chart_render::official_split_effect_uv_aspect_hw(fog2020->uv_name_index);
  for (int i = 0; i < nf; ++i) {
    CHECK(std::fabs(fog[static_cast<size_t>(i)].sol_y - fog_aspect) < 0.08f);
  }

  ctx.color_id = 10620;
  std::vector<wds::chart_render::SplitEffectLiveParticle> hearts(256);
  const int nh = wds::chart_render::shuriken::evaluate(*bat_wing, ctx, hearts.data(), 256);
  CHECK(nh > 0);
  for (int i = 0; i < nh; ++i) {
    const auto& p = hearts[static_cast<size_t>(i)];
    CHECK(p.blend == static_cast<uint8_t>(SplitEffectBlend::Additive));
    CHECK(std::fabs(p.custom1_x + 1.5f) < 0.05f);
    // Official (x+1) on custom1=-1.5 flips authored RGB; Add (x*0.5+1) would
    // have left a positive 0.25 scale.
    CHECK(p.r < 0.0f || p.g < 0.0f || p.b < 0.0f);
  }

  ctx.color_id = 3010;
  std::vector<wds::chart_render::SplitEffectLiveParticle> moths(256);
  const int nm = wds::chart_render::shuriken::evaluate(*moth, ctx, moths.data(), 256);
  CHECK(nm > 0);
  for (int i = 0; i < nm; ++i) {
    CHECK(moths[static_cast<size_t>(i)].blend ==
          static_cast<uint8_t>(SplitEffectBlend::Additive));
  }

  CHECK(moth->force_enabled != 0);
  CHECK(std::fabs(moth->force_z + 1.0f) < 1e-4f);
  // OpenWDS Butterfly mesh AABB: ±0.2 x ±0.1, two quads, full sprite per wing.
  CHECK(std::fabs(moth->butterfly_wing_x - wds::chart_render::kSplitEffectButterflyMeshHalfX) <
        1e-4f);
  CHECK(moth->vel_x_count >= 2);
  CHECK(std::fabs(wds::chart_render::official_split_effect_eval_curve_keys(
                      moth->vel_x_begin, moth->vel_x_count, 0.0f, moth->vel_x)) < 0.05f);
  ctx.now_t = 0.45f;
  ctx.line_x = 0.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> early(256);
  const int ne = wds::chart_render::shuriken::evaluate(*moth, ctx, early.data(), 256);
  CHECK(ne > 0);
  float mean_ax = 0.0f;
  float mean_az = 0.0f;
  for (int i = 0; i < ne; ++i) {
    mean_ax += std::fabs(early[static_cast<size_t>(i)].world_x);
    mean_az += std::fabs(early[static_cast<size_t>(i)].z);
  }
  mean_ax /= static_cast<float>(ne);
  mean_az /= static_cast<float>(ne);
  // Official: VOL X curve is 0 at birth; Force Z=-1 leaves the lane plane.
  // Baking VOL scalar 3 as +X made them slide along the track.
  CHECK(mean_az > 0.04f);
  CHECK(mean_ax < 0.80f);

  const auto* e10350 = wds::chart_render::official_split_effect(10350);
  CHECK(e10350 != nullptr);
  uint16_t n10350 = 0;
  const auto* p10350 = wds::chart_render::official_split_effect_particles(*e10350, &n10350);
  CHECK(p10350 != nullptr);
  CHECK(n10350 == 3);
  int ring_n = 0;
  for (uint16_t i = 0; i < n10350; ++i) {
    if ((p10350[i].flags & wds::chart_render::kSplitEffectFlagRingMesh) != 0) {
      ++ring_n;
      CHECK(p10350[i].render_mode == wds::chart_render::kSplitEffectRenderMesh);
      CHECK(p10350[i].render_alignment == 2);
      CHECK(p10350[i].rotation_3d != 0);
    }
  }
  CHECK(ring_n == 2);
  CHECK(std::fabs(wds::chart_render::kSplitEffectRingMeshRadius - 1.0f) < 1e-6f);
  CHECK(std::fabs(wds::chart_render::kSplitEffectRingMeshHalfZ - 0.1f) < 1e-6f);
  CHECK(wds::chart_render::kSplitEffectRingMeshVertCount == 72);
  CHECK(wds::chart_render::kSplitEffectRingMeshTriCount == 64);
  float rx_max = 0.0f;
  float rz_max = 0.0f;
  for (int i = 0; i < wds::chart_render::kSplitEffectRingMeshVertCount; ++i) {
    const float x = wds::chart_render::kSplitEffectRingMeshPos[i * 3];
    const float y = wds::chart_render::kSplitEffectRingMeshPos[i * 3 + 1];
    const float z = wds::chart_render::kSplitEffectRingMeshPos[i * 3 + 2];
    rx_max = std::max(rx_max, std::sqrt(x * x + y * y));
    rz_max = std::max(rz_max, std::fabs(z));
  }
  CHECK(std::fabs(rx_max - 1.0f) < 1e-5f);
  CHECK(std::fabs(rz_max - 0.1f) < 1e-5f);

  int mesh_other = 0;
  for (int32_t id : wds::chart_render::official_split_effect_ids()) {
    const auto* e = wds::chart_render::official_split_effect(id);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*e, &n);
    for (uint16_t i = 0; i < n; ++i) {
      if (ps[i].render_mode != wds::chart_render::kSplitEffectRenderMesh) {
        continue;
      }
      const bool named = (ps[i].flags & wds::chart_render::kSplitEffectFlagButterfly) != 0 ||
                         (ps[i].flags & wds::chart_render::kSplitEffectFlagCubeMesh) != 0 ||
                         (ps[i].flags & wds::chart_render::kSplitEffectFlagRingMesh) != 0;
      if (!named) {
        ++mesh_other;
      }
    }
  }
  CHECK(mesh_other == 0);

  ctx.color_id = 10350;
  ctx.now_t = 1.2f;
  ctx.line_x = 0.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> rings(256);
  const int nr = wds::chart_render::shuriken::evaluate(p10350[0], ctx, rings.data(), 256);
  CHECK(nr > 0);
  float z_ratio = 0.0f;
  int z_n = 0;
  for (int i = 0; i < nr; ++i) {
    const auto& live = rings[static_cast<size_t>(i)];
    if (live.size < 1e-4f) {
      continue;
    }
    z_ratio += live.size_z / live.size;
    ++z_n;
  }
  CHECK(z_n > 0);
  z_ratio /= static_cast<float>(z_n);
  // Official SOL Z scalar 1.5 is constant; XY grow to 1. Late particles
  // should keep depth / width ≈ 1.5, not the discarded (void)sol_z.
  CHECK(z_ratio > 1.35f && z_ratio < 1.65f);

  ctx.color_id = 11320;
  std::vector<wds::chart_render::SplitEffectLiveParticle> zmax(64);
  const int nz = wds::chart_render::shuriken::evaluate(*maxp, ctx, zmax.data(), 64);
  for (int i = 0; i < nz; ++i) {
    CHECK(zmax[static_cast<size_t>(i)].max_particle_size <= 1e-6f);
  }

  const char* recovered[] = {"SenceFx_301",  "10250_0",
                             "10250_1",      "10250_2",
                             "10250_3",      "10250_4",
                             "BombEffectSakura_6", "BombEffectSakura_7",
                             "TapEffect_1",  "TapEffect_2",
                             "Game_13",      "HoldLongNotes"};
  for (const char* name : recovered) {
    const uint16_t idx = uv_index_named(name);
    CHECK(idx != 0xFFFF);
    int32_t x = 0, y = 0;
    bool present = false;
    CHECK(wds::chart_render::official_split_effect_uv_gpu_px(idx, x, y, present));
    CHECK(present);
  }

  // TSA: keep per-frame aspect. Split_202_6 tight box 77.85×118.85.
  const uint16_t thorn_uv = uv_index_named("Split_202_6");
  CHECK(thorn_uv != 0xFFFF);
  CHECK(std::fabs(wds::chart_render::official_split_effect_uv_aspect_hw(thorn_uv) - 1.527f) <
        0.02f);
}

void test_trail_color_over_trail_rgb_catalog_and_playback() {
  using wds::chart_render::kSplitEffectFlagTrails;
  using wds::chart_render::official_split_effect;
  using wds::chart_render::official_split_effect_eval_trail_over;
  using wds::chart_render::official_split_effect_ids;
  using wds::chart_render::official_split_effect_particles;
  int enabled = 0;
  int state0 = 0;
  int state1 = 0;
  int state2 = 0;
  int nonwhite_rgb = 0;
  int gradient_rgb = 0;
  int end_to_end = 0;
  for (int32_t id : official_split_effect_ids()) {
    const auto* e = official_split_effect(id);
    CHECK(e != nullptr);
    if (e == nullptr) {
      continue;
    }
    uint16_t n = 0;
    const auto* ps = official_split_effect_particles(*e, &n);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if ((p.flags & kSplitEffectFlagTrails) == 0) {
        continue;
      }
      ++enabled;
      CHECK(p.trail_over_state <= 3);
      if (p.trail_over_state == 0) {
        ++state0;
      } else if (p.trail_over_state == 1) {
        ++state1;
        ++gradient_rgb;
        CHECK(p.trail_over_color_count > 0 || p.trail_over_alpha_count > 0);
      } else if (p.trail_over_state == 2) {
        ++state2;
      }
      const bool rgb_nonwhite =
          std::fabs(p.trail_over_r - 1.0f) > 1e-4f ||
          std::fabs(p.trail_over_g - 1.0f) > 1e-4f ||
          std::fabs(p.trail_over_b - 1.0f) > 1e-4f ||
          std::fabs(p.trail_over_r_min - 1.0f) > 1e-4f ||
          std::fabs(p.trail_over_g_min - 1.0f) > 1e-4f ||
          std::fabs(p.trail_over_b_min - 1.0f) > 1e-4f;
      if (rgb_nonwhite) {
        ++nonwhite_rgb;
      }
      if (end_to_end == 0 && p.trail_over_state == 0 && rgb_nonwhite) {
        float er = 0.0f, eg = 0.0f, eb = 0.0f, ea = 0.0f;
        official_split_effect_eval_trail_over(p, 0.0f, 0.0f, er, eg, eb, ea);
        CHECK(std::fabs(er - p.trail_over_r) < 1e-5f);
        CHECK(std::fabs(eg - p.trail_over_g) < 1e-5f);
        CHECK(std::fabs(eb - p.trail_over_b) < 1e-5f);
        wds::chart_render::shuriken::EmitContext ctx;
        ctx.color_id = id;
        ctx.now_t = 0.8f;
        ctx.fade_scale = 1.0f;
        ctx.line_alpha = 1.0f;
        ctx.want_trail = true;
        std::vector<wds::chart_render::SplitEffectLiveParticle> live(512);
        const int count = wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 512);
        for (int k = 0; k < count; ++k) {
          if (live[static_cast<size_t>(k)].trail_points >= 2 &&
              live[static_cast<size_t>(k)].trail_color_r.size() >= 2) {
            CHECK(std::fabs(live[static_cast<size_t>(k)].trail_color_r[0] -
                            live[static_cast<size_t>(k)].trail_r * p.trail_over_r) < 1e-4f);
            ++end_to_end;
            break;
          }
        }
      }
    }
  }
  CHECK(enabled == 189);
  CHECK(state0 == 116);
  CHECK(state1 == 70);
  CHECK(state2 == 3);
  CHECK(gradient_rgb == 70);
  CHECK(nonwhite_rgb == 56);
  CHECK(end_to_end >= 1);
}

void test_force_family_limit_drag_keeps_stored_and_animated() {
  // Official 2020/2030/10100 petals share Force local z=+2, VOL local z
  // TwoConstants [-3,-1], Limit drag=1, mulVel=true.
  // live dump force-drag-2020-class-v1 e0000 GO 2021: drag composed total
  // (iso, k not ×|v|), stored = total − anim. Unity age≥1 Force wins
  // (|stored|≈2.78 > |anim|≈2.21, total.z +0.28). Do not ID-split Limit.
  const int32_t ids[] = {2020, 2030, 10100};
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    CHECK(n >= 2);
    const auto& p = ps[1];
    CHECK(p.force_enabled != 0);
    CHECK(std::fabs(p.force_z - 2.0f) < 1e-4f);
    CHECK((p.flags & wds::chart_render::kSplitEffectFlagVelocity) != 0);
    CHECK(p.vel_z < -0.5f);
    CHECK(p.clamp_enabled != 0);
    CHECK(std::fabs(p.clamp_drag - 1.0f) < 1e-4f);
    CHECK(p.clamp_mul_vel != 0);
    wds::chart_render::shuriken::EmitContext ctx;
    ctx.color_id = id;
    ctx.fade_scale = 1.0f;
    ctx.now_t = 1.6f;
    ctx.want_trail = false;
    std::vector<wds::chart_render::SplitEffectLiveParticle> live(512);
    const int np = wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 512);
    CHECK(np > 20);
    int aged = 0;
    float stored_z = 0.0f;
    float vol_z = 0.0f;
    float total_z = 0.0f;
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    for (int i = 0; i < np; ++i) {
      const auto& q = live[static_cast<size_t>(i)];
      const float age = ctx.now_t - q.birth_t;
      if (age < 1.0f) {
        continue;
      }
      ++aged;
      stored_z += q.stored_vz;
      vol_z += q.vol_vz;
      total_z += q.total_vz;
      ax += std::fabs(q.vel_x);
      ay += std::fabs(q.vel_y);
      az += std::fabs(q.vel_z);
    }
    CHECK(aged > 5);
    const float inv = 1.0f / static_cast<float>(aged);
    std::printf("%d petal aged=%d stored_z=%.3f vol_z=%.3f total_z=%.3f |v|=%.3f,%.3f,%.3f\n", id,
                aged, stored_z * inv, vol_z * inv, total_z * inv, ax * inv, ay * inv, az * inv);
    CHECK(stored_z * inv > 2.0f);
    CHECK(vol_z * inv < -0.5f);
    CHECK(total_z * inv > 0.0f);
    CHECK(ax * inv > 0.2f);
    CHECK(ay * inv > 0.2f);
  }
}

void test_11423_red_const_drag_keeps_vol_toward_camera() {
  // 11423_0/1/2: VOL local y ∈ [−12,−10], Force off, drag=2, mulVel=false.
  // live dump force-drag-2020-class-v1 e0009: const-decel on total, stored
  // opposite anim (|stored|≈k·age), total still toward −Y. Do not iso-drag
  // this branch (that cancelled VOL Y=−12).
  const auto* entry = wds::chart_render::official_split_effect(11423);
  CHECK(entry != nullptr);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  CHECK(n >= 5);
  int red_layers = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const auto& p = ps[i];
    if ((p.flags & wds::chart_render::kSplitEffectFlagVelocity) == 0 || p.vel_y > -8.0f ||
        p.force_enabled != 0) {
      continue;
    }
    ++red_layers;
    CHECK(p.clamp_enabled != 0);
    CHECK(std::fabs(p.clamp_drag - 2.0f) < 1e-4f);
    CHECK(p.clamp_mul_vel == 0);
    wds::chart_render::shuriken::EmitContext ctx;
    ctx.color_id = 11423;
    ctx.fade_scale = 1.0f;
    ctx.now_t = 1.2f;
    ctx.want_trail = false;
    std::vector<wds::chart_render::SplitEffectLiveParticle> live(512);
    const int np = wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 512);
    CHECK(np > 5);
    float vol_y = 0.0f;
    float stored_y = 0.0f;
    float total_y = 0.0f;
    int aged = 0;
    for (int j = 0; j < np; ++j) {
      const auto& q = live[static_cast<size_t>(j)];
      if (ctx.now_t - q.birth_t < 0.4f) {
        continue;
      }
      ++aged;
      vol_y += q.vol_vy;
      stored_y += q.stored_vy;
      total_y += q.total_vy;
    }
    CHECK(aged > 3);
    const float inv = 1.0f / static_cast<float>(aged);
    std::printf("11423 i=%u aged=%d stored_y=%.3f vol_y=%.3f total_y=%.3f\n", i, aged,
                stored_y * inv, vol_y * inv, total_y * inv);
    CHECK(vol_y * inv < -8.0f);
    CHECK(stored_y * inv > 0.2f);
    CHECK(total_y * inv < -4.0f);
  }
  CHECK(red_layers >= 2);
}

void test_vol_only_limit_drag_keeps_animated_velocity() {
  // 11570: VOL local z TwoConstants [-0.5,-1], Force off, drag curve (scalar 5).
  // live dump force-drag-2020-class-v1: drag composed total, stored = total −
  // anim. This-frame VOL stays in the moving speed (anim is re-evaluated).
  const auto* entry = wds::chart_render::official_split_effect(11570);
  CHECK(entry != nullptr);
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  CHECK(n >= 1);
  const auto& p = ps[0];
  CHECK(p.force_enabled == 0);
  CHECK((p.flags & wds::chart_render::kSplitEffectFlagVelocity) != 0);
  CHECK(p.vel_z < 0.0f);
  CHECK(p.clamp_enabled != 0);
  wds::chart_render::shuriken::EmitContext ctx;
  ctx.color_id = 11570;
  ctx.fade_scale = 1.0f;
  ctx.now_t = 0.6f;
  ctx.want_trail = false;
  std::vector<wds::chart_render::SplitEffectLiveParticle> live(512);
  const int np = wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 512);
  CHECK(np > 5);
  float z_sum = 0.0f;
  float vz_sum = 0.0f;
  for (int i = 0; i < np; ++i) {
    z_sum += live[static_cast<size_t>(i)].z;
    vz_sum += live[static_cast<size_t>(i)].vel_z;
  }
  const float mean_z = z_sum / static_cast<float>(np);
  const float mean_vz = vz_sum / static_cast<float>(np);
  std::printf("11570 vol-only n=%d mean_z=%.3f mean_vz=%.3f\n", np, mean_z, mean_vz);
  CHECK(mean_z < 0.0f);
  CHECK(mean_vz < 0.0f);
}

void test_shuriken_catalog_fields_shared_path() {
  const auto ids = wds::chart_render::official_split_effect_ids();
  int shape_off = 0;
  int shape_off_tight = 0;
  int rot_curve_fewer = 0;
  int rot_curve_n = 0;
  int start_grad_keys = 0;
  int start_two = 0;
  int life_curves = 0;
  int trail_col_wired = 0;
  int trail_col_n = 0;
  int ptrail_width = 0;
  int copies = 0;
  std::vector<wds::chart_render::SplitEffectLiveParticle> live(512);
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    CHECK(ps != nullptr);
    for (uint16_t i = 0; i < n; ++i) {
      const auto& p = ps[i];
      if (p.start_color_state == 2 && p.color_random != 0) {
        CHECK(std::fabs(p.color_a_min) > 1e-4f);
        float r = 0, g = 0, b = 0, a = 0;
        wds::chart_render::official_split_effect_eval_start_color(p, 0.0f, r, g, b, a);
        CHECK(std::fabs(a - p.color_a_min) < 1e-5f);
        wds::chart_render::official_split_effect_eval_start_color(p, 1.0f, r, g, b, a);
        CHECK(std::fabs(a - p.color_a) < 1e-5f);
      }
      if (p.start_color_state == 1 && p.start_col_color_count > 0) {
        ++start_grad_keys;
        float r0 = 0, g0 = 0, b0 = 0, a0 = 0;
        float r1 = 0, g1 = 0, b1 = 0, a1 = 0;
        wds::chart_render::official_split_effect_eval_start_color_keys(p, 0.0f, r0, g0, b0, a0);
        wds::chart_render::official_split_effect_eval_start_color_keys(p, 1.0f, r1, g1, b1, a1);
        CHECK(r0 >= 0.0f && r1 >= 0.0f);
      }
      if (p.start_color_state == 3) {
        ++start_two;
        CHECK(p.start_col_color_count > 0);
        CHECK(p.start_col_min_color_count > 0);
        float mr = 0, mg = 0, mb = 0, ma = 0;
        float xr = 0, xg = 0, xb = 0, xa = 0;
        float r0 = 0, g0 = 0, b0 = 0, a0 = 0;
        float r1 = 0, g1 = 0, b1 = 0, a1 = 0;
        wds::chart_render::official_split_effect_eval_gradient(
            p.start_col_min_color_begin, p.start_col_min_color_count, p.start_col_min_alpha_begin,
            p.start_col_min_alpha_count, 0.0f, mr, mg, mb, ma);
        wds::chart_render::official_split_effect_eval_gradient(
            p.start_col_color_begin, p.start_col_color_count, p.start_col_alpha_begin,
            p.start_col_alpha_count, 0.0f, xr, xg, xb, xa);
        wds::chart_render::official_split_effect_eval_start_color_keys(p, 0.0f, 0.0f, r0, g0, b0,
                                                                      a0);
        wds::chart_render::official_split_effect_eval_start_color_keys(p, 0.0f, 1.0f, r1, g1, b1,
                                                                      a1);
        CHECK(std::fabs(r0 - mr) < 1e-5f && std::fabs(g0 - mg) < 1e-5f &&
              std::fabs(b0 - mb) < 1e-5f && std::fabs(a0 - ma) < 1e-5f);
        CHECK(std::fabs(r1 - xr) < 1e-5f && std::fabs(g1 - xg) < 1e-5f &&
              std::fabs(b1 - xb) < 1e-5f && std::fabs(a1 - xa) < 1e-5f);
        CHECK(!(std::fabs(r1 - 1.0f) < 1e-5f && std::fabs(g1 - 1.0f) < 1e-5f &&
                std::fabs(b1 - 1.0f) < 1e-5f));
        if (!(std::fabs(mr - 1.0f) < 1e-5f && std::fabs(mg - 1.0f) < 1e-5f &&
              std::fabs(mb - 1.0f) < 1e-5f)) {
          CHECK(!(std::fabs(r0 - 1.0f) < 1e-5f && std::fabs(g0 - 1.0f) < 1e-5f &&
                  std::fabs(b0 - 1.0f) < 1e-5f));
        }
      }
      if (p.life_curve_count > 0) {
        ++life_curves;
        CHECK(wds::chart_render::official_split_effect_eval_start_lifetime(p, 0.0f) > 1e-4f);
        CHECK(wds::chart_render::official_split_effect_eval_start_lifetime(p, 1.0f) > 1e-4f);
      }
      const bool hop = p.shape_enabled == 0 || p.rot_curve_count > 0 || p.life_curve_count > 0 ||
                       (p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0;
      if (!hop) {
        continue;
      }
      wds::chart_render::shuriken::EmitContext ctx;
      ctx.color_id = id;
      ctx.fade_scale = 1.0f;
      ctx.line_alpha = 1.0f;
      ctx.now_t = p.shape_enabled == 0 ? 0.05f : 0.35f;
      ctx.want_trail = (p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0;
      const int np = wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 512);
      for (int k = 0; k < np; ++k) {
        const auto& q = live[static_cast<size_t>(k)];
        CHECK(std::fabs(q.camera_velocity_scale - p.camera_velocity_scale) < 1e-6f);
        CHECK(q.freeform_stretching == p.freeform_stretching);
        CHECK(q.rotate_with_stretch == p.rotate_with_stretch);
        CHECK(std::fabs(q.noise_rotation_amount - p.noise_rotation_amount) < 1e-6f);
        CHECK(std::fabs(q.noise_size_amount - p.noise_size_amount) < 1e-6f);
        ++copies;
      }
      if (p.shape_enabled == 0) {
        ++shape_off;
        if (np > 0 && std::fabs(p.local_pos_x) < 1e-4f && std::fabs(p.local_pos_z) < 1e-4f) {
          float ax = 0.0f;
          float az = 0.0f;
          for (int k = 0; k < np; ++k) {
            ax += std::fabs(live[static_cast<size_t>(k)].world_x - ctx.line_x - p.local_pos_x);
            az += std::fabs(live[static_cast<size_t>(k)].z - p.local_pos_z);
          }
          ax /= static_cast<float>(np);
          az /= static_cast<float>(np);
          const float travel = std::fabs(p.start_speed) * ctx.now_t + 0.08f;
          if (ax < travel && az < travel) {
            ++shape_off_tight;
          }
        }
      }
      if ((p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0 && p.trail_col_color_count > 0 &&
          np > 0) {
        ++trail_col_n;
        float er = 0, eg = 0, eb = 0, ea = 0;
        const float age = std::max(ctx.now_t - live[0].birth_t, 0.0f);
        const float life = std::max(p.lifetime, 1e-4f);
        wds::chart_render::official_split_effect_eval_trail_col(
            p, std::clamp(age / life, 0.0f, 1.0f), er, eg, eb, ea);
        if (p.trail_inherit_particle_color != 0) {
          er *= live[0].current_r;
        }
        if (std::fabs(live[0].trail_r - er) < 0.35f) {
          ++trail_col_wired;
        }
      }
      if ((p.flags & wds::chart_render::kSplitEffectFlagTrails) != 0 && p.trail_mode == 0 &&
          p.trail_width_count > 0 && np > 0) {
        const float curve0 = wds::chart_render::official_split_effect_eval_curve_keys(
            p.trail_width_begin, p.trail_width_count, 0.0f, p.trail_width);
        const float size_mul = p.trail_size_affects_width != 0 ? live[0].size : 1.0f;
        if (std::fabs(live[0].trail_width - curve0 * size_mul) < 1e-3f) {
          ++ptrail_width;
        }
      }
      if (p.rot_curve_count > 0) {
        ++rot_curve_n;
        const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
        const float r0 = wds::chart_render::official_split_effect_eval_rate_over_time(p, 0.0f);
        const float r1 = wds::chart_render::official_split_effect_eval_rate_over_time(p, 1.0f);
        CHECK(r0 >= 0.0f);
        if (r1 + 1e-4f < p.rate_over_time) {
          ctx.now_t = length;
          ctx.want_trail = false;
          const int ncurve = wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 512);
          const int nconst = static_cast<int>(std::floor(length * p.rate_over_time));
          if (ncurve < nconst) {
            ++rot_curve_fewer;
          }
        }
      }
    }
  }
  CHECK(shape_off == 82);
  CHECK(shape_off_tight >= 1);
  CHECK(start_grad_keys == 20);
  CHECK(start_two == 5);
  CHECK(life_curves == 38);
  CHECK(rot_curve_n == 39);
  CHECK(rot_curve_fewer >= 1);
  CHECK(copies > 10);
  CHECK(trail_col_n >= 1);
  CHECK(trail_col_wired >= 1);
  CHECK(ptrail_width >= 1);
}

void test_inactive_particle_gameobjects_do_not_emit() {
  // OpenWDS GameObject.m_IsActive=false on the ParticleSystem GO.
  // SplitEffectController Play()s LineEffects without SetActive(true).
  const int32_t all_inactive[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10393};
  for (int32_t id : all_inactive) {
    const auto* e = wds::chart_render::official_split_effect(id);
    CHECK(e != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*e, &n);
    CHECK(ps != nullptr);
    CHECK(n >= 1);
    for (uint16_t i = 0; i < n; ++i) {
      CHECK(ps[i].go_active == 0);
      wds::chart_render::shuriken::EmitContext ctx;
      ctx.color_id = id;
      ctx.now_t = 0.5f;
      ctx.fade_scale = 1.0f;
      ctx.line_alpha = 1.0f;
      std::vector<wds::chart_render::SplitEffectLiveParticle> live(64);
      CHECK(wds::chart_render::shuriken::evaluate(ps[i], ctx, live.data(), 64) == 0);
    }
  }
  struct Mixed {
    int32_t id;
    int inactive;
  };
  const Mixed mixed[] = {{10560, 1}, {11470, 1}, {11791, 3}};
  for (const Mixed& m : mixed) {
    const auto* e = wds::chart_render::official_split_effect(m.id);
    CHECK(e != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*e, &n);
    int off = 0;
    int on = 0;
    for (uint16_t i = 0; i < n; ++i) {
      if (ps[i].go_active == 0) {
        ++off;
      } else {
        ++on;
      }
    }
    CHECK(off == m.inactive);
    CHECK(on > 0);
  }
  wds::chart_editor::PreviewSplitLaneInstance split;
  split.should_show = true;
  split.split_count = 1;
  split.split_lane_type = wds::chart_editor::SplitLaneType::Full;
  split.scratch_length = 1;
  split.start_ms = 1000;
  split.end_ms = 5000;
  split.split_line_alpha = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(256);
  const int n = wds::chart_render::collect_split_effect_particles(
      split, wds::chart_editor::kOfficialSpritEffectRich, 0.5, pool.data(), 256);
  CHECK(n == 0);
}

void test_gold_family_shares_noise_damping_and_line_seed() {
  const int32_t ids[] = {2020, 2030, 10100, 1, 1050, 11370, 11380};
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    uint16_t n = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
    CHECK(ps != nullptr);
    for (uint16_t i = 0; i < n; ++i) {
      CHECK(ps[i].scaling_mode == 1);
      if ((ps[i].flags & wds::chart_render::kSplitEffectFlagNoise) != 0) {
        CHECK(ps[i].noise_damping != 0);
      }
    }
  }
}

void test_trail_width_over_trail_minmax_contract() {
  using wds::chart_render::SplitEffectLiveParticle;
  using wds::chart_render::official_split_effect_eval_curve_keys;
  using wds::chart_render::shuriken::EmitContext;
  auto find_state = [](uint8_t state, int32_t& id_out) -> const wds::chart_render::SplitEffectParticle* {
    for (int32_t id : wds::chart_render::official_split_effect_ids()) {
      const auto* e = wds::chart_render::official_split_effect(id);
      uint16_t n = 0;
      const auto* ps = e ? wds::chart_render::official_split_effect_particles(*e, &n) : nullptr;
      for (uint16_t i = 0; ps != nullptr && i < n; ++i) {
        if ((ps[i].flags & wds::chart_render::kSplitEffectFlagTrails) != 0 &&
            ps[i].trail_mode == 0 && ps[i].trail_width_state == state) {
          id_out = id;
          return ps + i;
        }
      }
    }
    return nullptr;
  };
  int32_t curve_id = 0;
  const auto* curve = find_state(2, curve_id);
  CHECK(curve != nullptr);
  CHECK(curve != nullptr && curve->trail_width_count >= 2 && curve->trail_width_min_count >= 2);
  if (curve != nullptr) {
    EmitContext ctx;
    ctx.color_id = curve_id;
    ctx.now_t = 0.8f;
    ctx.fade_scale = 1.0f;
    ctx.line_alpha = 1.0f;
    std::vector<SplitEffectLiveParticle> a(512), b(512);
    const int na = wds::chart_render::shuriken::evaluate(*curve, ctx, a.data(), 512);
    const int nb = wds::chart_render::shuriken::evaluate(*curve, ctx, b.data(), 512);
    CHECK(na > 0);
    CHECK(na == nb);
    for (int i = 0; i < na; ++i) {
      const auto& p = a[static_cast<size_t>(i)];
      if (p.trail_points < 2 || p.trail_widths.size() != p.trail_points) {
        continue;
      }
      CHECK(p.trail_widths.size() == b[static_cast<size_t>(i)].trail_widths.size());
      for (size_t k = 0; k < p.trail_widths.size(); ++k) {
        const float t = p.trail_points > 1 ? static_cast<float>(k) /
                                             static_cast<float>(p.trail_points - 1)
                                           : 0.0f;
        const float hi = official_split_effect_eval_curve_keys(
            curve->trail_width_begin, curve->trail_width_count, t, curve->trail_width);
        const float lo = official_split_effect_eval_curve_keys(
            curve->trail_width_min_begin, curve->trail_width_min_count, t,
            curve->trail_width_min);
        const float low = std::min(lo, hi) * (curve->trail_size_affects_width ? p.size : 1.0f);
        const float high = std::max(lo, hi) * (curve->trail_size_affects_width ? p.size : 1.0f);
        CHECK(p.trail_widths[k] + 1e-4f >= low);
        CHECK(p.trail_widths[k] - 1e-4f <= high);
        CHECK(std::fabs(p.trail_widths[k] - b[static_cast<size_t>(i)].trail_widths[k]) < 1e-6f);
      }
      break;
    }
  }
  int32_t constants_id = 0;
  const auto* constants = find_state(3, constants_id);
  CHECK(constants != nullptr);
  CHECK(constants != nullptr && constants->trail_width_count == 0 &&
        constants->trail_width_min_count == 0);
  if (constants != nullptr) {
    EmitContext ctx;
    ctx.color_id = constants_id;
    ctx.now_t = 0.8f;
    ctx.fade_scale = 1.0f;
    ctx.line_alpha = 1.0f;
    std::vector<SplitEffectLiveParticle> live(512);
    const int n = wds::chart_render::shuriken::evaluate(*constants, ctx, live.data(), 512);
    CHECK(n > 0);
    for (int i = 0; i < n; ++i) {
      const auto& p = live[static_cast<size_t>(i)];
      if (p.trail_points < 2 || p.trail_widths.size() != p.trail_points) continue;
      const float lo = constants->trail_width_min *
                       (constants->trail_size_affects_width ? p.size : 1.0f);
      const float hi = constants->trail_width *
                       (constants->trail_size_affects_width ? p.size : 1.0f);
      for (float w : p.trail_widths) {
        CHECK(w + 1e-4f >= std::min(lo, hi));
        CHECK(w - 1e-4f <= std::max(lo, hi));
      }
      break;
    }
  }
}

void test_world_local_transform_point_parent_scales_local_position_x() {
  // Unity 2022.3 Scaling Mode Local ignores parent for size/shape;
  // Transform.position still TransformPoint. OpenWDS Line localScale.x=0.1.
  using wds::chart_editor::kOfficialSplitLineLocalScaleX;
  using wds::chart_render::SplitEffectScalingMode;
  using wds::chart_render::SplitEffectSimulationSpace;
  const auto ids = wds::chart_render::official_split_effect_ids();
  int n = 0;
  int closer = 0;
  std::vector<wds::chart_render::SplitEffectLiveParticle> live(256);
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    uint16_t count = 0;
    const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &count);
    for (uint16_t i = 0; i < count; ++i) {
      const auto& p = ps[i];
      if (p.simulation_space != static_cast<uint8_t>(SplitEffectSimulationSpace::World)) {
        continue;
      }
      if (p.scaling_mode != static_cast<uint8_t>(SplitEffectScalingMode::Local)) {
        continue;
      }
      if (std::fabs(p.local_pos_x) < 0.5f) {
        continue;
      }
      ++n;
      wds::chart_render::shuriken::EmitContext ctx;
      ctx.color_id = id;
      ctx.fade_scale = 1.0f;
      ctx.line_alpha = 1.0f;
      ctx.now_t = 0.05f;
      const int np = wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 256);
      CHECK(np > 0);
      if (np <= 0) {
        continue;
      }
      float mean = 0.0f;
      for (int k = 0; k < np; ++k) {
        mean += live[static_cast<size_t>(k)].world_x - ctx.line_x;
      }
      mean /= static_cast<float>(np);
      const float scaled = p.local_pos_x * kOfficialSplitLineLocalScaleX;
      const float unscaled = p.local_pos_x;
      if (std::fabs(mean - scaled) + 1e-4f < std::fabs(mean - unscaled)) {
        ++closer;
      }
    }
  }
  CHECK(n == 2);
  CHECK(closer == n);
}

void test_subemitter_visual_clock_and_parent_pose_provenance() {
  const auto* entry = wds::chart_render::official_split_effect(10220);
  CHECK(entry != nullptr);
  if (entry == nullptr) return;
  uint16_t count = 0;
  const auto* particles = wds::chart_render::official_split_effect_particles(*entry, &count);
  CHECK(particles != nullptr);
  if (particles == nullptr || count == 0) return;

  using wds::chart_render::SplitEffectLiveParticle;
  using wds::chart_render::shuriken::EmitContext;
  EmitContext with_pose;
  with_pose.color_id = 10220;
  with_pose.line_x = 0.0f;
  with_pose.fade_scale = 1.0f;
  with_pose.line_alpha = 1.0f;
  with_pose.now_t = 1.2f;
  with_pose.want_trail = true;
  with_pose.parent_motion.simulation_play_clock_candidate = true;
  with_pose.parent_motion.simulation_play_origin_seconds = 0.0;
  with_pose.parent_motion.visual_fade_origin_seconds = 0.0;
  with_pose.parent_motion.birth_pose_candidate = true;
  with_pose.parent_motion.birth_origin_y_delta = 3.9f;

  std::vector<SplitEffectLiveParticle> posed(4096);
  const int posed_n = wds::chart_render::shuriken::evaluate_subemitters(
      particles, count, static_cast<int>(particles[0].element_index), with_pose, posed.data(),
      static_cast<int>(posed.size()));
  CHECK(posed_n > 0);
  int non_one_fade = 0;
  int non_zero_parent_z = 0;
  for (int i = 0; i < posed_n; ++i) {
    const auto& p = posed[static_cast<size_t>(i)];
    const float expected_fade = wds::chart_render::shuriken::fade_scale_at_play_t(p.birth_t);
    CHECK(std::fabs(p.birth_fade - expected_fade) < 1e-5f);
    if (std::fabs(p.birth_fade - 1.0f) > 1e-5f) ++non_one_fade;
    CHECK(std::fabs(p.birth_origin_y_delta - 3.9f) < 1e-5f);
    if (std::fabs(p.birth_parent_z) > 1e-5f) ++non_zero_parent_z;
  }
  // This catalog has actual Death/Birth sub-emitters, so the clock and pose
  // assertions exercise the event path rather than a synthetic child.
  CHECK(non_one_fade > 0);
  CHECK(non_zero_parent_z > 0);

  EmitContext without_pose = with_pose;
  without_pose.parent_motion.birth_origin_y_delta = 0.0f;
  std::vector<SplitEffectLiveParticle> unposed(4096);
  const int unposed_n = wds::chart_render::shuriken::evaluate_subemitters(
      particles, count, static_cast<int>(particles[0].element_index), without_pose, unposed.data(),
      static_cast<int>(unposed.size()));
  CHECK(unposed_n == posed_n);
  const int paired_n = std::min(posed_n, unposed_n);
  for (int i = 0; i < paired_n; ++i) {
    CHECK(posed[static_cast<size_t>(i)].seed == unposed[static_cast<size_t>(i)].seed);
    CHECK(std::fabs(posed[static_cast<size_t>(i)].birth_t -
                    unposed[static_cast<size_t>(i)].birth_t) < 1e-6f);
    // Parent origin is applied once through the event pose, not once by the
    // parent and again by the child.
    CHECK(std::fabs((posed[static_cast<size_t>(i)].main_y -
                     unposed[static_cast<size_t>(i)].main_y) -
                    3.9f) < 1e-3f);
  }
}

void test_start_delay_wall_is_delay_over_simulation_speed() {
  // live dump 11423-birth-v1: ParticleSystem.time = now_t * speed − startDelay.
  // Official 11423_1 first Unity particle play_t≈0.24 (delay 0.3 / speed 1.5
  // plus RoD). Hamming delay=0 first at 0.02; simSpeed=1 first at 0.36.
  // 2022.3 startDelay page does not write this algebra. hop, not 2022.3.
  using wds::chart_editor::kOfficialLineHeightScale;
  using wds::chart_editor::kOfficialZ180LineHightY;
  using wds::chart_editor::official_split_fade_in_scale;
  using wds::chart_editor::official_split_identity_origin_y;
  using wds::chart_render::official_split_effect_start_delay;
  using wds::chart_render::SplitEffectLiveParticle;
  using wds::chart_render::shuriken::EmitContext;
  const auto* entry = wds::chart_render::official_split_effect(11423);
  CHECK(entry != nullptr);
  if (entry == nullptr) return;
  uint16_t n = 0;
  const auto* ps = wds::chart_render::official_split_effect_particles(*entry, &n);
  CHECK(n >= 5);
  std::vector<SplitEffectLiveParticle> live(512);
  int delayed_fast = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const auto& p = ps[i];
    const float delay = official_split_effect_start_delay(p);
    const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
    if (delay < 0.15f || speed < 1.2f) {
      continue;
    }
    ++delayed_fast;
    const float wall = delay / speed;
    auto count_at = [&](float now_t) {
      EmitContext ctx;
      ctx.color_id = 11423;
      ctx.fade_scale = wds::chart_render::shuriken::fade_scale_at_play_t(now_t);
      ctx.line_alpha = 1.0f;
      ctx.now_t = now_t;
      ctx.want_trail = false;
      return wds::chart_render::shuriken::evaluate(p, ctx, live.data(), 512);
    };
    CHECK(count_at(wall - 0.04f) == 0);
    CHECK(count_at(wall + 0.08f) > 0);
    CHECK(count_at(delay - 0.02f) > 0);
  }
  CHECK(delayed_fast >= 3);

  const auto& petal = ps[0];
  CHECK(std::fabs(official_split_effect_start_delay(petal) - 0.3f) < 1e-3f);
  CHECK(std::fabs(petal.simulation_speed - 1.5f) < 1e-3f);
  CHECK(std::fabs(petal.local_pos_y + 1.5f) < 1e-3f);
  EmitContext ctx;
  ctx.color_id = 11423;
  ctx.now_t = 0.24f;
  ctx.fade_scale = wds::chart_render::shuriken::fade_scale_at_play_t(0.24f);
  ctx.line_alpha = 1.0f;
  ctx.want_trail = false;
  const int np = wds::chart_render::shuriken::evaluate(petal, ctx, live.data(), 512);
  CHECK(np > 0);
  int young = 0;
  float my = 0.0f;
  for (int j = 0; j < np; ++j) {
    const float age = ctx.now_t - live[static_cast<size_t>(j)].birth_t;
    if (age < 0.0f || age > 0.06f) continue;
    ++young;
    my += live[static_cast<size_t>(j)].main_y;
  }
  CHECK(young > 0);
  if (young > 0) {
    my /= static_cast<float>(young);
  }
  const float pred = official_split_identity_origin_y() +
                     (kOfficialZ180LineHightY + petal.local_pos_y) * kOfficialLineHeightScale *
                         ctx.fade_scale;
  CHECK(std::fabs(my - pred) < 1.5f);
  CHECK(std::fabs(my - 3.094f) < 1.5f);

  int nclass = 0;
  int early = 0;
  const auto ids = wds::chart_render::official_split_effect_ids();
  for (int32_t id : ids) {
    const auto* e = wds::chart_render::official_split_effect(id);
    uint16_t count = 0;
    const auto* rows = wds::chart_render::official_split_effect_particles(*e, &count);
    for (uint16_t i = 0; i < count; ++i) {
      const auto& p = rows[i];
      const float delay = official_split_effect_start_delay(p);
      const float speed = p.simulation_speed > 1e-6f ? p.simulation_speed : 1.0f;
      if (p.go_active == 0) continue;
      if (delay < 0.15f || speed < 1.2f) continue;
      if (p.rate_over_time <= 1e-6f && p.rate_over_distance <= 1e-6f && p.rot_curve_count == 0 &&
          p.rod_curve_count == 0) {
        continue;
      }
      ++nclass;
      EmitContext c;
      c.color_id = id;
      c.now_t = delay * 0.85f;
      c.fade_scale = wds::chart_render::shuriken::fade_scale_at_play_t(c.now_t);
      c.line_alpha = 1.0f;
      c.want_trail = false;
      if (wds::chart_render::shuriken::evaluate(p, c, live.data(), 512) > 0) {
        ++early;
      }
    }
  }
  CHECK(nclass >= 5);
  CHECK(early == nclass);
}

}  // namespace

int main() {
  test_4txt_official_element_slots();
  test_official_default_split_line_opacity_is_100();
  test_official_split_fade_out_window_is_0_3s();
  test_apply_split_line_opacity_keeps_alpha();
  test_split_line_tip_follows_opacity();
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
  test_adjacent_tap_notes_leave_official_margin_gap();
  test_star_quad_is_centered_official_size_not_span_width();
  test_star_quad_ignores_note_height_level();
  test_jacket_quad_ndc_width_is_half_screen_at_16_9();
  test_content_aspect_is_official_16_9();
  test_split_view_billboard_uses_z_roll_and_pivot();
  test_2020_catalog_four_layers();
  test_force_family_limit_drag_keeps_stored_and_animated();
  test_11423_red_const_drag_keeps_vol_toward_camera();
  test_start_delay_wall_is_delay_over_simulation_speed();
  test_vol_only_limit_drag_keeps_animated_velocity();
  test_shared_force_family_not_2020_only();
  test_split_line_plate_uses_tight_sprite_mesh();
  test_11770_and_10170_catalog_particle_counts();
  test_1070_wrap_is_world_stretch_rod();
  test_world_rod_fog_stays_while_tip_moves();
  test_world_rod_stretch_stays_then_dies_with_lifetime();
  test_stretch_sol_y_is_height_without_start_size_3d();
  test_stretch_size3d_side_rails_share_11770_shape();
  test_catalog_shuriken_invariants();
  test_shuriken_catalog_fields_shared_path();
  test_z180_particles_use_split_color_slot();
  test_split_effect_gate_emit_sets();
  test_2020_live_particles_follow_emitter_not_shape_scale();
  test_2020_thorns_unpaired_and_petals_not_along_line();
  test_particles_keep_col_alpha_when_line_fades();
  test_thorn_start_size_follows_system_age_not_parent_scale();
  test_2020_petals_clump_along_line_from_shape_transform();
  test_official_thorn_size_fields();
  test_thorn_cross_lane_width_follows_official_long_axis();
  test_split_202_6_atlas_tile_fills_tight_sprite();
  test_split_202_0_atlas_tile_keeps_official_halo();
  test_inactive_particle_gameobjects_do_not_emit();
  test_gold_family_shares_noise_damping_and_line_seed();
  test_openwds_custom1_and_world_noise();
  test_openwds_layer_table_not_2020_only();
  test_trail_color_over_trail_rgb_catalog_and_playback();
  test_stretch_view_clip_interpolates_sprite_uv();
  test_billboard_near_clip_keeps_camera_front();
  test_local_sol_y_billboard_near_clip_keeps_leftover();
  test_10120_ribbon_near_clip_keeps_camera_front();
  test_world_local_transform_point_parent_scales_local_position_x();
  test_subemitter_visual_clock_and_parent_pose_provenance();
  test_trail_width_over_trail_minmax_contract();
  if (g_fails == 0) {
    std::printf("All split_line official color tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", g_fails);
  return 1;
}

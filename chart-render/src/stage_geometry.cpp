#include <wds/chart_render/stage_geometry.hpp>
#include <wds/chart_render/openwds_hud_layout.hpp>

#include <wds/core/official_split_x.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wds::renderer {

namespace {

using wds::chart_editor::kOfficialConcurrentLineLocalRotationX;
using wds::chart_editor::kOfficialNoteSpriteHeight;
using wds::chart_editor::kOfficialSoundNoteLocalZ;
using wds::chart_editor::kOfficialSoundNoteSpriteSize;
using wds::chart_editor::official_hidden_line_center_percent;
using wds::chart_editor::official_hidden_line_center_y;
using wds::chart_editor::official_lane_mask_bottom_percent;
using wds::chart_editor::official_lane_mask_bottom_y;
using wds::chart_editor::official_start_line_sprite_height;
using wds::chart_editor::official_judge_y_to_percent;
using wds::chart_editor::official_main_y_to_percent;
using wds::chart_editor::official_judgeline_percent;
using wds::chart_editor::official_lane_edge_x;
using wds::chart_editor::official_lane_left_x;
using wds::chart_editor::official_lane_right_x;
using wds::chart_editor::official_note_height_rotation_x;
using wds::chart_editor::official_note_visible_position_y;
using wds::chart_editor::official_percent_to_judge_y;
using wds::chart_editor::official_percent_to_main_y;
using wds::chart_editor::official_concurrent_line_visual_width;
using wds::chart_editor::official_hold_line_visual_width;
using wds::chart_editor::official_span_center_x;
using wds::chart_editor::official_span_left_x;
using wds::chart_editor::official_span_right_x;
using wds::chart_editor::official_span_width;
using wds::chart_editor::official_tap_visual_width;
using wds::chart_editor::project_judge_xyz;
using wds::chart_editor::project_main_xyz;
using wds::chart_editor::project_note_layer;

}  // namespace

void StageGeometry::configure(const PreviewVisualConfig& config) {
  config_ = config;
  auto position_config = wds::chart_editor::PreviewConfig{};
  position_config.note_speed = config.note_speed;
  position_calculator_.set_config(position_config);
  rebuild_stage();
}

void StageGeometry::set_scroll_speed_points(
    std::vector<wds::chart_editor::ScrollSpeedPoint> points,
    const wds::chart_editor::MusicTiming& timing) {
  position_calculator_.set_scroll_speed_points(std::move(points), timing);
}

void StageGeometry::resize(int framebuffer_width, int framebuffer_height) {
  const float w = static_cast<float>(std::max(1, framebuffer_width));
  const float h = static_cast<float>(std::max(1, framebuffer_height));
  fb_w_ = w;
  fb_h_ = h;
  screen_.aspect_ratio = w / h;
  screen_.l = -screen_.aspect_ratio;
  screen_.r = screen_.aspect_ratio;
  screen_.b = -1.0f;
  screen_.t = 1.0f;
  screen_.w = screen_.aspect_ratio * 2.0f;
  screen_.h = 2.0f;
  rebuild_stage();
  rebuild_panel();
}

void StageGeometry::set_content_rect(int x, int y, int width, int height) {
  content_x_ = x;
  content_y_ = y;
  content_w_ = width;
  content_h_ = height;
  has_content_rect_ = width > 0 && height > 0;
  rebuild_stage();
}

void StageGeometry::clear_content_rect() {
  has_content_rect_ = false;
  rebuild_stage();
}

void StageGeometry::set_panel_rect(int x, int y, int width, int height) {
  panel_x_ = x;
  panel_y_ = y;
  panel_w_ = width;
  panel_h_ = height;
  has_panel_rect_ = width > 0 && height > 0;
  rebuild_panel();
}

void StageGeometry::clear_panel_rect() {
  has_panel_rect_ = false;
  rebuild_panel();
}

void StageGeometry::rebuild_panel() {
  if (!has_panel_rect_) {
    panel_ = content_;
    return;
  }
  const float x0 = std::clamp(static_cast<float>(panel_x_), 0.0f, fb_w_);
  const float y0 = std::clamp(static_cast<float>(panel_y_), 0.0f, fb_h_);
  const float x1 = std::clamp(static_cast<float>(panel_x_ + panel_w_), 0.0f, fb_w_);
  const float y1 = std::clamp(static_cast<float>(panel_y_ + panel_h_), 0.0f, fb_h_);
  panel_.l = screen_.l + x0 / fb_w_ * screen_.w;
  panel_.r = screen_.l + x1 / fb_w_ * screen_.w;
  panel_.t = screen_.t - y0 / fb_h_ * screen_.h;
  panel_.b = screen_.t - y1 / fb_h_ * screen_.h;
  panel_.w = std::max(0.0f, panel_.r - panel_.l);
  panel_.h = std::max(0.0f, panel_.t - panel_.b);
  panel_.aspect_ratio = panel_.w / std::max(panel_.h, 1e-6f);
}

float StageGeometry::content_aspect() const noexcept {
  return std::max(content_.w / std::max(content_.h, 1e-6f), 1e-4f);
}

Vec2 StageGeometry::ndc_to_content(wds::chart_editor::OfficialNdc ndc) const noexcept {
  const float cx = (content_.l + content_.r) * 0.5f;
  const float cy = (content_.t + content_.b) * 0.5f;
  const float z = std::max(config_.view_zoom, 1e-4f);
  return {cx + ndc.x * content_.w * 0.5f * z, cy + ndc.y * content_.h * 0.5f * z};
}

Vec2 StageGeometry::project_judge(float local_x, float judge_y, float local_z) const noexcept {
  return ndc_to_content(project_judge_xyz(local_x, judge_y, local_z, content_aspect()));
}

Vec2 StageGeometry::project_main(float local_x, float main_y, float local_z) const noexcept {
  return ndc_to_content(project_main_xyz(local_x, main_y, local_z, content_aspect()));
}

Vec2 StageGeometry::project_world(float world_x, float world_y, float world_z) const noexcept {
  return ndc_to_content(
      wds::chart_render::project_world_xyz(world_x, world_y, world_z, content_aspect()));
}

float StageGeometry::percent_to_judge_y(float percent) const noexcept {
  return official_percent_to_judge_y(percent);
}

float StageGeometry::judge_y_to_percent(float judge_y) const noexcept {
  return official_judge_y_to_percent(judge_y);
}

void StageGeometry::rebuild_stage() {
  const bool lock = config_.lock_aspect_ratio;
  const float target = config_.target_aspect_ratio;
  content_ = screen_;
  if (has_content_rect_) {
    const float x0 = std::clamp(static_cast<float>(content_x_), 0.0f, fb_w_);
    const float y0 = std::clamp(static_cast<float>(content_y_), 0.0f, fb_h_);
    const float x1 = std::clamp(static_cast<float>(content_x_ + content_w_), 0.0f, fb_w_);
    const float y1 = std::clamp(static_cast<float>(content_y_ + content_h_), 0.0f, fb_h_);
    content_.l = screen_.l + x0 / fb_w_ * screen_.w;
    content_.r = screen_.l + x1 / fb_w_ * screen_.w;
    content_.t = screen_.t - y0 / fb_h_ * screen_.h;
    content_.b = screen_.t - y1 / fb_h_ * screen_.h;
    content_.w = std::max(0.0f, content_.r - content_.l);
    content_.h = std::max(0.0f, content_.t - content_.b);
    content_.aspect_ratio = content_.w / std::max(content_.h, 1e-6f);
  }

  if (!lock || content_.aspect_ratio < target) {
    stage_.w = content_.w * config_.extra_width;
  } else {
    stage_.w = content_.h * target * config_.extra_width;
  }

  if (!lock || content_.aspect_ratio > target) {
    stage_.h = content_.h;
  } else {
    stage_.h = content_.w / target;
  }

  const float cx = (content_.l + content_.r) * 0.5f;
  const float cy = (content_.t + content_.b) * 0.5f;
  stage_.l = cx - stage_.w * 0.5f;
  stage_.r = cx + stage_.w * 0.5f;
  stage_.t = cy + stage_.h * 0.5f;
  stage_.b = cy - stage_.h * 0.5f;
  stage_.h = stage_.t - stage_.b;

  const float left = -0.5f * wds::chart_editor::kOfficialJudgeSpriteWidth;
  const float right = 0.5f * wds::chart_editor::kOfficialJudgeSpriteWidth;
  const float half = std::max(config_.judgeline_height, 1e-4f) * 0.5f;
  const Vec2 lb = project_judge(left, -half);
  const Vec2 rb = project_judge(right, -half);
  const Vec2 lt = project_judge(left, half);
  const Vec2 rt = project_judge(right, half);
  judgeline_.lb_x = lb.x;
  judgeline_.lb_y = lb.y;
  judgeline_.rb_x = rb.x;
  judgeline_.rb_y = rb.y;
  judgeline_.lt_x = lt.x;
  judgeline_.lt_y = lt.y;
  judgeline_.rt_x = rt.x;
  judgeline_.rt_y = rt.y;

  rebuild_panel();
}

Vec2 StageGeometry::lane_position(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  const float x = 0.5f * (official_lane_left_x(i) + official_lane_right_x(i));
  return project_judge(x, percent_to_judge_y(percent));
}

float StageGeometry::lane_width(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  const float y = percent_to_judge_y(percent);
  const Vec2 l = project_judge(official_lane_left_x(i), y);
  const Vec2 r = project_judge(official_lane_right_x(i), y);
  return std::fabs(r.x - l.x);
}

Vec2 StageGeometry::lane_full_position(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  const float x = 0.5f * (official_lane_left_x(i) + official_lane_right_x(i));
  return project_main(x, official_percent_to_main_y(percent));
}

float StageGeometry::lane_full_width(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  const float y = official_percent_to_main_y(percent);
  const Vec2 l = project_main(official_lane_left_x(i), y);
  const Vec2 r = project_main(official_lane_right_x(i), y);
  return std::fabs(r.x - l.x);
}

float StageGeometry::note_percent(double note_time_sec, double now_sec) const {
  const int64_t target_ms = static_cast<int64_t>(std::llround(note_time_sec * 1000.0));
  const int64_t passed_ms = static_cast<int64_t>(std::llround(now_sec * 1000.0));
  const float y = position_calculator_.calculate_position_y(target_ms, passed_ms);
  return judge_y_to_percent(y);
}

float StageGeometry::judgeline_percent() const noexcept { return official_judgeline_percent(); }

float StageGeometry::judgeline_half_percent() const noexcept {
  const float half = std::max(config_.judgeline_height, 1e-4f) * 0.5f;
  return std::fabs(judge_y_to_percent(half) - judge_y_to_percent(-half)) * 0.5f;
}

float StageGeometry::hidden_line_center_percent() const noexcept {
  return official_hidden_line_center_percent(config_.note_start_offset);
}

float StageGeometry::hidden_line_half_percent() const noexcept {
  const float y = official_hidden_line_center_y(config_.note_start_offset);
  const float world_h = std::fabs(config_.hidden_line_height -
                                 wds::chart_editor::kOfficialStartLineSpriteHeight) < 1e-4f
                           ? official_start_line_sprite_height(config_.note_start_offset)
                           : config_.hidden_line_height;
  const float half = std::max(world_h, 1e-4f) * 0.5f;
  return std::fabs(official_main_y_to_percent(y + half) - official_main_y_to_percent(y - half)) *
         0.5f;
}

float StageGeometry::lane_mask_bottom_percent() const noexcept {
  return official_lane_mask_bottom_percent(config_.note_start_offset);
}

float StageGeometry::lane_mask_bottom_content_y() const noexcept {
  return project_main(0.0f, official_lane_mask_bottom_y(config_.note_start_offset)).y;
}

bool StageGeometry::clip_quad_outside_lane_mask(Quad& q, float& far_t) const noexcept {
  const float y_max = lane_mask_bottom_content_y();
  const float y_near = 0.5f * (q.lb.y + q.rb.y);
  const float y_far = 0.5f * (q.lt.y + q.rt.y);
  // Content Y grows toward the screen top (spawn). The mask covers y > y_max.
  if (y_near > y_max + 1e-6f) {
    far_t = 0.0f;
    return false;
  }
  if (y_far <= y_max + 1e-6f) {
    far_t = 1.0f;
    return true;
  }
  const float denom = y_far - y_near;
  if (std::fabs(denom) < 1e-8f) {
    far_t = 0.0f;
    return false;
  }
  const float t = std::clamp((y_max - y_near) / denom, 0.0f, 1.0f);
  auto lerp2 = [](Vec2 a, Vec2 b, float u) {
    return Vec2{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u};
  };
  q.lt = lerp2(q.lb, q.lt, t);
  q.rt = lerp2(q.rb, q.rt, t);
  far_t = t;
  return t > 1e-5f;
}

Quad StageGeometry::stage_quad() const {
  // Official BG_Lane sits on Main (not JudgeArea): 11.11×640, center (0,0).
  // The sprite is long enough to cover the camera frustum, so the visible plate
  // is the lane-plane ∩ NDC box — not NoteStart Y=58 and not the judgeline.
  const float left = -0.5f * wds::chart_editor::kOfficialBgLaneWidth;
  const float right = 0.5f * wds::chart_editor::kOfficialBgLaneWidth;
  const float y_far = official_percent_to_main_y(0.0f);
  const float y_near = official_percent_to_main_y(1.0f);
  return {
      project_main(left, y_near),
      project_main(left, y_far),
      project_main(right, y_far),
      project_main(right, y_near),
  };
}

Quad StageGeometry::bg_shadow_quad(float scale_x, float scale_y) const {
  float left = 0.0f;
  float bottom = 0.0f;
  float right = 0.0f;
  float top = 0.0f;
  wds::chart_editor::official_bg_shadow_local_rect(left, bottom, right, top);
  const float z = wds::chart_editor::kOfficialBgShadowLocalZ;
  return {
      project_world(scale_x * left, scale_y * bottom, z),
      project_world(scale_x * left, scale_y * top, z),
      project_world(scale_x * right, scale_y * top, z),
      project_world(scale_x * right, scale_y * bottom, z),
  };
}

Quad StageGeometry::lane_border_quad(int32_t edge_index) const {
  const float x = official_lane_edge_x(edge_index, config_.lane_count);
  const float half = wds::chart_editor::kOfficialLaneBorderVisualWidth * 0.5f;
  const float y_far = official_percent_to_main_y(0.0f);
  const float y_near = official_percent_to_main_y(1.0f);
  return {
      project_main(x - half, y_near),
      project_main(x - half, y_far),
      project_main(x + half, y_far),
      project_main(x + half, y_near),
  };
}

float StageGeometry::note_half_height_percent(int32_t /*lane*/, float percent) const {
  const float y = percent_to_judge_y(percent);
  const float tilt = official_note_height_rotation_x(config_.note_height_level);
  const float half = kOfficialNoteSpriteHeight * 0.5f * std::cos(tilt * (3.14159265358979323846f / 180.0f));
  return std::fabs(judge_y_to_percent(y + half) - judge_y_to_percent(y - half)) * 0.5f;
}

Quad StageGeometry::note_quad(int32_t lane, int32_t end_lane, float percent) const {
  return note_quad(lane, end_lane, percent, 0.0f);
}

Quad StageGeometry::note_quad(int32_t lane, int32_t end_lane, float percent,
                              float unity_local_z) const {
  const float y = percent_to_judge_y(percent);
  const float w = official_tap_visual_width(official_span_width(lane, end_lane));
  const float x = 0.5f * (official_span_left_x(lane, end_lane) + official_span_right_x(lane, end_lane));
  const float h = kOfficialNoteSpriteHeight;
  const float tilt = official_note_height_rotation_x(config_.note_height_level);
  const float aspect = content_aspect();
  auto corner = [&](float sx, float sy) {
    return ndc_to_content(project_note_layer(x, y, sx, sy, unity_local_z, tilt, aspect));
  };
  Quad q;
  q.lb = corner(-w * 0.5f, -h * 0.5f);
  q.rb = corner(w * 0.5f, -h * 0.5f);
  q.lt = corner(-w * 0.5f, h * 0.5f);
  q.rt = corner(w * 0.5f, h * 0.5f);
  return q;
}

Quad StageGeometry::note_span_quad(int32_t lane, int32_t end_lane, float percent_near,
                                   float percent_far, float unity_local_z) const {
  const float y_near = percent_to_judge_y(percent_near);
  const float y_far = percent_to_judge_y(percent_far);
  const float y = 0.5f * (y_near + y_far);
  const float w = official_tap_visual_width(official_span_width(lane, end_lane));
  const float x = 0.5f * (official_span_left_x(lane, end_lane) + official_span_right_x(lane, end_lane));
  const float tilt = official_note_height_rotation_x(config_.note_height_level);
  const float aspect = content_aspect();
  const float sy_near = y_near - y;
  const float sy_far = y_far - y;
  auto corner = [&](float sx, float sy) {
    return ndc_to_content(project_note_layer(x, y, sx, sy, unity_local_z, tilt, aspect));
  };
  Quad q;
  q.lb = corner(-w * 0.5f, sy_near);
  q.rb = corner(w * 0.5f, sy_near);
  q.lt = corner(-w * 0.5f, sy_far);
  q.rt = corner(w * 0.5f, sy_far);
  return q;
}

Quad StageGeometry::hold_body_quad(int32_t lane, int32_t end_lane, float percent_near,
                                   float percent_far) const {
  const float y0 = percent_to_judge_y(percent_near);
  const float y1 = percent_to_judge_y(percent_far);
  const float left = official_span_left_x(lane, end_lane);
  const float right = official_span_right_x(lane, end_lane);
  Quad q;
  q.lb = project_judge(left, y0);
  q.rb = project_judge(right, y0);
  q.lt = project_judge(left, y1);
  q.rt = project_judge(right, y1);
  return q;
}

Quad StageGeometry::hold_line_quad(int32_t lane, int32_t end_lane, float percent_near,
                                   float percent_far) const {
  const float y0 = percent_to_judge_y(percent_near);
  const float y1 = percent_to_judge_y(percent_far);
  const float cx = official_span_center_x(lane, end_lane);
  const float half = official_hold_line_visual_width(official_span_width(lane, end_lane)) * 0.5f;
  Quad q;
  q.lb = project_judge(cx - half, y0);
  q.rb = project_judge(cx + half, y0);
  q.lt = project_judge(cx - half, y1);
  q.rt = project_judge(cx + half, y1);
  return q;
}

Quad StageGeometry::tick_quad(int32_t lane, int32_t end_lane, float percent) const {
  const float half = note_half_height_percent(lane, percent) * 0.55f;
  return hold_body_quad(lane, end_lane, percent + half, percent - half);
}

Quad StageGeometry::star_quad(int32_t lane, int32_t end_lane, float percent) const {
  const float y = percent_to_judge_y(percent);
  const float x = official_span_center_x(lane, end_lane);
  const float half = kOfficialSoundNoteSpriteSize * 0.5f;
  // SoundNote.prefab Rx is identity; SoundNoteObject never writes GetNoteHeight.
  constexpr float kSoundTilt = 0.0f;
  const float aspect = content_aspect();
  auto corner = [&](float sx, float sy) {
    return ndc_to_content(
        project_note_layer(x, y, sx, sy, kOfficialSoundNoteLocalZ, kSoundTilt, aspect));
  };
  Quad q;
  q.lb = corner(-half, -half);
  q.rb = corner(half, -half);
  q.lt = corner(-half, half);
  q.rt = corner(half, half);
  return q;
}

Quad StageGeometry::sync_line_quad(int32_t lane, int32_t end_lane, float percent) const {
  const float y = percent_to_judge_y(percent);
  const float w = official_concurrent_line_visual_width(official_span_width(lane, end_lane));
  const float x = official_span_center_x(lane, end_lane);
  const float half = std::max(config_.sync_line_height, 1e-4f) * 0.5f;
  const float aspect = content_aspect();
  auto corner = [&](float sx, float sy) {
    return ndc_to_content(
        project_note_layer(x, y, sx, sy, 0.0f, kOfficialConcurrentLineLocalRotationX, aspect));
  };
  Quad q;
  q.lb = corner(-w * 0.5f, -half);
  q.rb = corner(w * 0.5f, -half);
  q.lt = corner(-w * 0.5f, half);
  q.rt = corner(w * 0.5f, half);
  return q;
}

Quad StageGeometry::effect_quad(int32_t lane, int32_t end_lane) const {
  const float half = std::max(config_.judgeline_height, 1e-4f) * 0.5f;
  const float left = official_span_left_x(lane, end_lane);
  const float right = official_span_right_x(lane, end_lane);
  Quad q;
  q.lb = project_judge(left, -half);
  q.rb = project_judge(right, -half);
  q.lt = project_judge(left, half);
  q.rt = project_judge(right, half);
  return q;
}

Quad StageGeometry::bomb_frame_quad(int32_t lane, int32_t end_lane, float width_scale,
                                   float height_unity) const {
  const float cx = official_span_center_x(lane, end_lane);
  const float half_w =
      official_span_width(lane, end_lane) * 0.5f * std::max(width_scale, 1e-4f);
  const float half_h = std::max(height_unity, 1e-4f) * 0.5f;
  Quad q;
  q.lb = project_judge(cx - half_w, -half_h);
  q.rb = project_judge(cx + half_w, -half_h);
  q.lt = project_judge(cx - half_w, half_h);
  q.rt = project_judge(cx + half_w, half_h);
  return q;
}

Quad StageGeometry::bomb_flare_billboard_quad(int32_t lane, int32_t end_lane,
                                             float size_unity) const {
  const float cx_local = official_span_center_x(lane, end_lane);
  const Vec2 center = project_judge(cx_local, 0.0f);
  const float half_u = std::max(size_unity, 1e-4f) * 0.5f;
  // Billboard size is world units at the judgeline, independent of note span.
  const float half = 0.5f * std::fabs(project_judge(half_u, 0.0f).x - project_judge(-half_u, 0.0f).x);
  Quad q;
  q.lb = {center.x - half, center.y - half};
  q.rb = {center.x + half, center.y - half};
  q.lt = {center.x - half, center.y + half};
  q.rt = {center.x + half, center.y + half};
  return q;
}

Quad StageGeometry::split_particle_billboard_quad(float world_x, float main_y,
                                                 float size_unity, float world_z) const {
  const Vec2 center = project_main(world_x, main_y, world_z);
  const float half_u = std::max(size_unity, 1e-4f) * 0.5f;
  const float half =
      0.5f * std::fabs(project_main(world_x + half_u, main_y, world_z).x -
                       project_main(world_x - half_u, main_y, world_z).x);
  Quad q;
  q.lb = {center.x - half, center.y - half};
  q.rb = {center.x + half, center.y - half};
  q.lt = {center.x - half, center.y + half};
  q.rt = {center.x + half, center.y + half};
  return q;
}

bool StageGeometry::split_particle_oriented_world_corners(
    float world_x, float main_y, float size_unity, float height_scale, float pivot_y, float roll,
    float tilt_x, float tilt_y, float world_z, uint8_t render_alignment, float pivot_x,
    float pivot_z, float* out_x, float* out_y, float* out_z) const {
  if (out_x == nullptr || out_y == nullptr || out_z == nullptr) {
    return false;
  }
  const float width = std::max(size_unity, 1e-4f);
  const float height = width * std::max(height_scale, 1e-4f);
  const float cr = std::cos(roll);
  const float sr = std::sin(roll);
  const float cxr = std::cos(tilt_x);
  const float sxr = std::sin(tilt_x);
  const float cyr = std::cos(tilt_y);
  const float syr = std::sin(tilt_y);
  auto rotate_local = [&](float lx, float ly, float lz, float& ox, float& oy, float& oz) {
    float x = lx * cr - ly * sr;
    float y = lx * sr + ly * cr;
    float z = lz;
    const float x2 = x * cyr + z * syr;
    const float z2 = -x * syr + z * cyr;
    ox = x2;
    oy = y * cxr - z2 * sxr;
    oz = y * sxr + z2 * cxr;
  };
  // hop / live dump align2-u1-unity-v1 4011 fi085: Unity Bake
  // centroid−particle = pivot ⊙ size (camera.right / camera.up), X and Y
  // same sign. Native Y already matched via live_y=−PSR_y then oyp=−live_y×h.
  // X had no evaluate negate, so oxp=−live_x×w flipped the sign. Billboard
  // only (this helper). Stretch / Mesh do not read oxp. Not 2022.3.
  const float oxp = pivot_x * width;
  const float oyp = -pivot_y * height;
  const float ozp = -pivot_z * width;
  const float hw = width * 0.5f;
  const float hh = height * 0.5f;
  const float lx[4] = {-hw, hw, hw, -hw};
  const float ly[4] = {-hh, -hh, hh, hh};
  const bool world_axes = render_alignment == 1 || render_alignment == 2;
  const float tilt =
      wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
  const float cam_up_y = std::cos(tilt);
  const float cam_up_z = -std::sin(tilt);
  const float cam_fwd_y = std::sin(tilt);
  const float cam_fwd_z = std::cos(tilt);
  for (int i = 0; i < 4; ++i) {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    rotate_local(lx[i] + oxp, ly[i] + oyp, ozp, px, py, pz);
    if (world_axes) {
      // World (1) / Local (2): corners in LaneGroup / Main axes.
      // world_x / main_y / world_z are already Main (integrate_pose).
      out_x[i] = world_x + px;
      out_y[i] = main_y + py;
      out_z[i] = world_z + pz;
    } else {
      // View (0): camera-facing in LaneGroup, then PSR rotation3D in view
      // space. Mapping lane-XY through rotate_local and then treating (px,py)
      // as screen offsets foreshortened 711 official systems.
      out_x[i] = world_x + px;
      out_y[i] = main_y + py * cam_up_y + pz * cam_fwd_y;
      out_z[i] = world_z + py * cam_up_z + pz * cam_fwd_z;
    }
  }
  return true;
}

Quad StageGeometry::split_particle_oriented_quad(float world_x, float main_y, float size_unity,
                                                float height_scale, float pivot_y, float roll,
                                                float tilt_x, float tilt_y, float world_z,
                                                uint8_t render_alignment, float pivot_x,
                                                float pivot_z) const {
  float cx[4], cy[4], cz[4];
  if (!split_particle_oriented_world_corners(world_x, main_y, size_unity, height_scale, pivot_y,
                                             roll, tilt_x, tilt_y, world_z, render_alignment,
                                             pivot_x, pivot_z, cx, cy, cz)) {
    Quad q{};
    return q;
  }
  Quad q;
  q.lb = project_main(cx[0], cy[0], cz[0]);
  q.rb = project_main(cx[1], cy[1], cz[1]);
  q.rt = project_main(cx[2], cy[2], cz[2]);
  q.lt = project_main(cx[3], cy[3], cz[3]);
  return q;
}

// Corners and velocity are LaneGroup / Main. vel_* is fill_live's Main
// finite difference, not Unity World and not PS-local.
bool StageGeometry::split_particle_stretch_world_corners(
    float world_x, float main_y, float world_z, float vel_x, float vel_y, float vel_z, float across,
    float along, float pivot_y, float roll, float pivot_x, float* out_x, float* out_y, float* out_z,
    float length_scale, float velocity_scale, float camera_velocity_scale, float cam_vel_x,
    float cam_vel_y, float cam_vel_z, uint8_t freeform_stretching, uint8_t rotate_with_stretch) const {
  if (out_x == nullptr || out_y == nullptr || out_z == nullptr) {
    return false;
  }
  // Official playfield camera: Rx(60°) + (0,0,12). View Z is camera-forward
  // depth. Preview / official playfield camera is static (vel = 0,0,0).
  const float rad =
      wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float cam_z = wds::chart_editor::kOfficialLaneGroupZ;
  auto to_view_point = [&](float x, float y, float z, float& vx, float& vy, float& vz) {
    vx = x;
    vy = y * c - z * s;
    vz = cam_z + y * s + z * c;
  };
  auto to_main_point = [&](float vx, float vy, float vz, float& x, float& y, float& z) {
    x = vx;
    y = vy * c + (vz - cam_z) * s;
    z = (vz - cam_z) * c - vy * s;
  };
  auto store_view = [&](int i, float vx, float vy, float vz) {
    to_main_point(vx, vy, vz, out_x[i], out_y[i], out_z[i]);
  };

  float px = 0.0f, py = 0.0f, pz = 0.0f;
  to_view_point(world_x, main_y, world_z, px, py, pz);

  if (freeform_stretching != 0) {
    // Approved unofficial Freeform (pcwalton gist, experimental, not Unity
    // 2022.3 ops). Particle space: origin at the particle, +Z = view +Z
    // (View-aligned billboard plane), +Y = camera up, then roll around +Z.
    // Camera C and up U are rotated into that frame. Do not mix with the
    // kettlewell strip. Rotate With Stretch only on this branch.
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);
    const float Cx_view = -px;
    const float Cy_view = -py;
    const float Cz_view = -pz;
    const float Cx = Cx_view * cr + Cy_view * sr;
    const float Cy = -Cx_view * sr + Cy_view * cr;
    const float Cz = Cz_view;
    const float c2 = Cx * Cx + Cy * Cy + Cz * Cz;
    if (c2 <= 1e-30f) {
      return false;
    }
    const float inv_c = 1.0f / std::sqrt(c2);
    const float z0x = -Cx * inv_c;
    const float z0y = -Cy * inv_c;
    const float z0z = -Cz * inv_c;
    const float size_s = std::max(across, 1e-4f);
    const float ell = length_scale;
    float Xx = 0.0f, Xy = 0.0f, Xz = 0.0f;
    float Yx = 0.0f, Yy = 0.0f, Yz = 0.0f;
    if (rotate_with_stretch != 0) {
      const float s2 = size_s * size_s;
      const float Zx = s2 * ell * z0x;
      const float Zy = s2 * ell * z0y;
      const float Zz = s2 * z0z;
      const float zxy2 = Zy * Zy + Zx * Zx;
      if (zxy2 <= 1e-30f) {
        return false;
      }
      const float inv_zxy = size_s / std::sqrt(zxy2);
      Xx = Zy * inv_zxy;
      Xy = -Zx * inv_zxy;
      Xz = 0.0f;
      const float xlen = std::sqrt(Xx * Xx + Xy * Xy + Xz * Xz);
      const float zlen = std::sqrt(Zx * Zx + Zy * Zy + Zz * Zz);
      if (xlen <= 1e-30f || zlen <= 1e-30f) {
        return false;
      }
      const float xhx = Xx / xlen;
      const float xhy = Xy / xlen;
      const float xhz = Xz / xlen;
      const float zhx = Zx / zlen;
      const float zhy = Zy / zlen;
      const float zhz = Zz / zlen;
      float yhx = xhy * zhz - xhz * zhy;
      float yhy = xhz * zhx - xhx * zhz;
      float yhz = xhx * zhy - xhy * zhx;
      const float yh2 = yhx * yhx + yhy * yhy + yhz * yhz;
      if (yh2 <= 1e-30f) {
        return false;
      }
      const float inv_yh = 1.0f / std::sqrt(yh2);
      yhx *= inv_yh;
      yhy *= inv_yh;
      yhz *= inv_yh;
      const float ell_safe = std::fabs(ell) > 1e-8f ? ell : 1e-8f;
      const float ymag_den =
          1.0f + yhz * yhz * (1.0f / (ell_safe * ell_safe) - 1.0f);
      if (ymag_den <= 1e-30f) {
        return false;
      }
      const float ymag = size_s / std::sqrt(ymag_den);
      Yx = yhx * ymag;
      Yy = yhy * ymag;
      Yz = yhz * ymag;
    } else {
      const float Ux = sr;
      const float Uy = cr;
      const float Uz = 0.0f;
      const float udot = Ux * z0x + Uy * z0y + Uz * z0z;
      float x0x = Ux - z0x * udot;
      float x0y = Uy - z0y * udot;
      float x0z = Uz - z0z * udot;
      const float x02 = x0x * x0x + x0y * x0y + x0z * x0z;
      if (x02 <= 1e-30f) {
        return false;
      }
      const float inv_x0 = 1.0f / std::sqrt(x02);
      x0x *= inv_x0;
      x0y *= inv_x0;
      x0z *= inv_x0;
      const float y0x = x0y * z0z - x0z * z0y;
      const float y0y = x0z * z0x - x0x * z0z;
      const float y0z = x0x * z0y - x0y * z0x;
      Xx = size_s * x0x;
      Xy = size_s * x0y;
      Xz = size_s * ell * x0z;
      Yx = size_s * y0x;
      Yy = size_s * y0y;
      Yz = size_s * ell * y0z;
    }
    auto store_particle = [&](int i, float ax, float ay) {
      const float lx = (ax * 0.5f - pivot_x) * Xx + (ay * 0.5f - pivot_y) * Yx;
      const float ly = (ax * 0.5f - pivot_x) * Xy + (ay * 0.5f - pivot_y) * Yy;
      const float lz = (ax * 0.5f - pivot_x) * Xz + (ay * 0.5f - pivot_y) * Yz;
      const float vx = px + lx * cr - ly * sr;
      const float vy = py + lx * sr + ly * cr;
      const float vz = pz + lz;
      store_view(i, vx, vy, vz);
    };
    // Same UV corner order as kettlewell: lb, rb, rt, lt.
    store_particle(0, -1.0f, -1.0f);
    store_particle(1, 1.0f, -1.0f);
    store_particle(2, 1.0f, 1.0f);
    store_particle(3, -1.0f, 1.0f);
    return true;
  }

  // Approved unofficial kettlewell 2025-02 strip (not Unity 2022.3).
  // cameraVelocityScale: velocityVS -= cameraVel_vs * scale.
  float vvx = vel_x;
  float vvy = vel_y * c - vel_z * s;
  float vvz = vel_y * s + vel_z * c;
  const float cvx = cam_vel_x;
  const float cvy = cam_vel_y * c - cam_vel_z * s;
  const float cvz = cam_vel_y * s + cam_vel_z * c;
  vvx -= cvx * camera_velocity_scale;
  vvy -= cvy * camera_velocity_scale;
  vvz -= cvz * camera_velocity_scale;
  const float v2 = vvx * vvx + vvy * vvy + vvz * vvz;
  if (v2 <= 1e-30f) {
    return false;
  }
  const float speed_vs = 1.0f / std::sqrt(v2);
  const float size_y = along;
  px += vvx * (speed_vs * pivot_y * size_y);
  py += vvy * (speed_vs * pivot_y * size_y);
  pz += vvz * (speed_vs * pivot_y * size_y);
  const float stretch = velocity_scale + speed_vs * (length_scale * size_y);
  float ex = px - vvx * stretch;
  float ey = py - vvy * stretch;
  float ez = pz - vvz * stretch;
  float dx = pz * ey - py * ez;
  float dy = px * ez - pz * ex;
  const float d2 = dx * dx + dy * dy;
  if (d2 <= 1e-30f) {
    return false;
  }
  // hsize.x is half the clamped Start Size width so ±n equals Main X width.
  const float hsize_x = std::max(across, 1e-4f) * 0.5f;
  const float inv_d = hsize_x / std::sqrt(d2);
  dx *= inv_d;
  dy *= inv_d;
  px += dx * pivot_x;
  py += dy * pivot_x;
  ex += dx * pivot_x;
  ey += dy * pivot_x;
  const float cr = std::cos(roll);
  const float sr = std::sin(roll);
  const float nx = dx * cr - dy * sr;
  const float ny = dx * sr + dy * cr;
  // U (lb→rb) along +velocity: tail (end) → head (start). V across ±n.
  store_view(0, ex - nx, ey - ny, ez);
  store_view(1, px - nx, py - ny, pz);
  store_view(2, px + nx, py + ny, pz);
  store_view(3, ex + nx, ey + ny, ez);
  return true;
}

Quad StageGeometry::split_particle_stretch_quad(float world_x, float main_y, float world_z,
                                               float vel_x, float vel_y, float vel_z, float across,
                                               float along, float pivot_y, float roll,
                                               float pivot_x, float length_scale,
                                               float velocity_scale, float camera_velocity_scale,
                                               float cam_vel_x, float cam_vel_y, float cam_vel_z,
                                               uint8_t freeform_stretching,
                                               uint8_t rotate_with_stretch) const {
  float cx[4], cy[4], cz[4];
  if (!split_particle_stretch_world_corners(
          world_x, main_y, world_z, vel_x, vel_y, vel_z, across, along, pivot_y, roll, pivot_x, cx,
          cy, cz, length_scale, velocity_scale, camera_velocity_scale, cam_vel_x, cam_vel_y,
          cam_vel_z, freeform_stretching, rotate_with_stretch)) {
    return split_particle_oriented_quad(world_x, main_y, std::max(across, 1e-4f),
                                        along / std::max(across, 1e-4f), pivot_y, roll, 0.0f, 0.0f,
                                        world_z, 0, pivot_x, 0.0f);
  }
  Quad q;
  q.lb = project_main(cx[0], cy[0], cz[0]);
  q.rb = project_main(cx[1], cy[1], cz[1]);
  q.rt = project_main(cx[2], cy[2], cz[2]);
  q.lt = project_main(cx[3], cy[3], cz[3]);
  return q;
}

Quad StageGeometry::split_line_quad_world_x(float world_x, float percent_start,
                                            float percent_end, float u0, float u1,
                                            float width_scale) const {
  const float width_scale_k = std::max(width_scale, 1e-4f);
  const float uu0 = std::clamp(u0, wds::chart_editor::kOfficialSplitLineMeshU0,
                               wds::chart_editor::kOfficialSplitLineMeshU1);
  const float uu1 = std::clamp(u1, wds::chart_editor::kOfficialSplitLineMeshU0,
                               wds::chart_editor::kOfficialSplitLineMeshU1);
  const float x0 = world_x + wds::chart_editor::official_split_line_local_x(uu0) * width_scale_k;
  const float x1 = world_x + wds::chart_editor::official_split_line_local_x(uu1) * width_scale_k;
  const float y0 = official_percent_to_main_y(percent_start);
  const float y1 = official_percent_to_main_y(percent_end);
  Quad q;
  q.lt = project_main(x0, y0);
  q.rt = project_main(x1, y0);
  q.lb = project_main(x0, y1);
  q.rb = project_main(x1, y1);
  return q;
}

Quad StageGeometry::split_line_quad(int32_t boundary_after_lane, float percent_start,
                                    float percent_end, float length_override) const {
  const int32_t lane = std::clamp(boundary_after_lane + 1, 0, config_.lane_count - 1);
  const float w_ref = std::max(lane_full_width(lane, judgeline_percent()), 1e-6f);
  const float p0 = percent_start;
  const float p1 = percent_end;

  const Vec2 c1 = lane_full_position(lane, p0);
  const Vec2 c2 = lane_full_position(lane, p1);
  const float w1 = lane_full_width(lane, p0);
  const float w2 = lane_full_width(lane, p1);
  const Vec2 left1 = c1 + Vec2{-w1 * 0.5f, 0.0f};
  const Vec2 left2 = c2 + Vec2{-w2 * 0.5f, 0.0f};

  const float length =
      (length_override > 0.0f ? length_override : config_.split_line_length) * content_unit();
  const float move = length * 0.5f;
  const float move1 = move * w1 / w_ref;
  const float move2 = move * w2 / w_ref;

  Quad q;
  q.lb = left2 + Vec2{-move2, 0.0f};
  q.lt = left1 + Vec2{-move1, 0.0f};
  q.rb = left2 + Vec2{move2, 0.0f};
  q.rt = left1 + Vec2{move1, 0.0f};
  return q;
}

Quad StageGeometry::lane_span_quad(float percent_start, float percent_end) const {
  const int32_t lane = 0;
  const int32_t end_lane = std::max(0, config_.lane_count - 1);
  return hold_body_quad(lane, end_lane, std::max(percent_start, percent_end),
                        std::min(percent_start, percent_end));
}

Quad StageGeometry::hidden_line_quad(float percent) const {
  const float half = hidden_line_half_percent();
  return lane_span_quad(percent - half, percent + half);
}

Quad StageGeometry::split_end_line_quad(float percent_start, float percent_end,
                                        float length_override) const {
  const int32_t lane = config_.lane_count - 1;
  const float w_ref = std::max(lane_full_width(lane, judgeline_percent()), 1e-6f);
  const float p0 = percent_start;
  const float p1 = percent_end;

  const Vec2 c1 = lane_full_position(lane, p0);
  const Vec2 c2 = lane_full_position(lane, p1);
  const float w1 = lane_full_width(lane, p0);
  const float w2 = lane_full_width(lane, p1);
  const Vec2 right1 = c1 + Vec2{w1 * 0.5f, 0.0f};
  const Vec2 right2 = c2 + Vec2{w2 * 0.5f, 0.0f};

  const float length =
      (length_override > 0.0f ? length_override : config_.split_line_length) * content_unit();
  const float move = length * 0.5f;
  const float move1 = move * w1 / w_ref;
  const float move2 = move * w2 / w_ref;

  Quad q;
  q.lb = right2 + Vec2{-move2, 0.0f};
  q.lt = right1 + Vec2{-move1, 0.0f};
  q.rb = right2 + Vec2{move2, 0.0f};
  q.rt = right1 + Vec2{move1, 0.0f};
  return q;
}

}  // namespace wds::renderer

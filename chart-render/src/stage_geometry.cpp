#include <wds/chart_render/stage_geometry.hpp>

#include <algorithm>
#include <cmath>

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
using wds::chart_editor::official_note_local_y;
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
  rebuild_stage();
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
  return {cx + ndc.x * content_.w * 0.5f, cy + ndc.y * content_.h * 0.5f};
}

Vec2 StageGeometry::project_judge(float local_x, float judge_y, float local_z) const noexcept {
  return ndc_to_content(project_judge_xyz(local_x, judge_y, local_z, content_aspect()));
}

Vec2 StageGeometry::project_main(float local_x, float main_y, float local_z) const noexcept {
  return ndc_to_content(project_main_xyz(local_x, main_y, local_z, content_aspect()));
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
  const float y = official_note_local_y(target_ms, passed_ms, static_cast<double>(config_.note_speed));
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

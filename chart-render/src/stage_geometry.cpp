#include <wds/chart_render/stage_geometry.hpp>

#include <algorithm>

namespace wds::renderer {

namespace {

float lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

float sirius_ease(float x) noexcept {
  // Remap(pow(1.06, -45), 1.06, 0, 1.06, pow(1.06, 45*(x-1)))
  const float a = std::pow(1.06f, -45.0f);
  const float b = 1.06f;
  const float v = std::pow(1.06f, 45.0f * (x - 1.0f));
  if (std::abs(b - a) < 1e-12f) {
    return 0.0f;
  }
  return (v - a) / (b - a) * 1.06f;
}

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

void StageGeometry::rebuild_stage() {
  const bool lock = config_.lock_aspect_ratio;
  const float target = config_.target_aspect_ratio;
  content_ = screen_;
  if (has_content_rect_) {
    const float x0 = std::clamp(static_cast<float>(content_x_), 0.0f, fb_w_);
    const float y0 = std::clamp(static_cast<float>(content_y_), 0.0f, fb_h_);
    const float x1 =
        std::clamp(static_cast<float>(content_x_ + content_w_), 0.0f, fb_w_);
    const float y1 =
        std::clamp(static_cast<float>(content_y_ + content_h_), 0.0f, fb_h_);
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

  // Extend tip above the visible content so spawn (p=0) stays off-screen.
  // overscan is a fraction of the pre-extend stage height (taller panel → higher tip).
  const float base_h = stage_.h;
  const float overscan = std::max(0.0f, config_.stage_top_overscan) * base_h;
  stage_.t += overscan;
  stage_.h = stage_.t - stage_.b;

  // Match constants.cpp judgline corners. Height/move are authored for full-screen
  // half-height = 1; scale into the preview content rect so embedded panels stay thin.
  const float unit = content_unit();
  const float margin = config_.judgeline_margin_bottom;
  const float jh = config_.judgeline_height * unit;
  const float move = config_.judgline_move_length * unit;
  const float top_l = taper_x(stage_.l);
  const float top_r = taper_x(stage_.r);

  judgeline_.lb_y = stage_.b + stage_.h * margin - jh * 0.5f;
  judgeline_.rb_y = judgeline_.lb_y;
  judgeline_.lt_y = judgeline_.lb_y + jh;
  judgeline_.rt_y = judgeline_.lt_y;

  const float y0 = margin - jh / stage_.h * 0.5f;
  const float y1 = margin + jh / stage_.h * 0.5f;
  judgeline_.lb_x = lerp(stage_.l, top_l, y0) - move;
  judgeline_.lt_x = lerp(stage_.l, top_l, y1) - move;
  judgeline_.rb_x = lerp(stage_.r, top_r, y0) + move;
  judgeline_.rt_x = lerp(stage_.r, top_r, y1) + move;
}

float StageGeometry::taper_x(float bottom_x) const noexcept {
  const float cx = (stage_.l + stage_.r) * 0.5f;
  return cx + (bottom_x - cx) * config_.high_width;
}

Vec2 StageGeometry::lane_position(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  // Sonolus lines[offset] with offset in 1..n → left=(offset-1)/n, right=offset/n
  const float lbx = lerp(stage_.l, stage_.r, static_cast<float>(i) / static_cast<float>(n));
  const float rbx = lerp(stage_.l, stage_.r, static_cast<float>(i + 1) / static_cast<float>(n));
  const float top_l = taper_x(stage_.l);
  const float top_r = taper_x(stage_.r);
  const float ltx = lerp(top_l, top_r, static_cast<float>(i) / static_cast<float>(n));
  const float rtx = lerp(top_l, top_r, static_cast<float>(i + 1) / static_cast<float>(n));
  const float lty = stage_.t;
  const float lby = stage_.b;

  const float t = percent * (1.0f - config_.judgeline_margin_bottom);
  return {lerp((ltx + rtx) * 0.5f, (lbx + rbx) * 0.5f, t), lerp(lty, lby, t)};
}

float StageGeometry::lane_width(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  const float lbx = lerp(stage_.l, stage_.r, static_cast<float>(i) / static_cast<float>(n));
  const float rbx = lerp(stage_.l, stage_.r, static_cast<float>(i + 1) / static_cast<float>(n));
  const float top_l = taper_x(stage_.l);
  const float top_r = taper_x(stage_.r);
  const float ltx = lerp(top_l, top_r, static_cast<float>(i) / static_cast<float>(n));
  const float rtx = lerp(top_l, top_r, static_cast<float>(i + 1) / static_cast<float>(n));

  const float t = percent * (1.0f - config_.judgeline_margin_bottom);
  return lerp(rtx - ltx, rbx - lbx, t);
}

Vec2 StageGeometry::lane_full_position(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  const float lbx = lerp(stage_.l, stage_.r, static_cast<float>(i) / static_cast<float>(n));
  const float rbx = lerp(stage_.l, stage_.r, static_cast<float>(i + 1) / static_cast<float>(n));
  const float top_l = taper_x(stage_.l);
  const float top_r = taper_x(stage_.r);
  const float ltx = lerp(top_l, top_r, static_cast<float>(i) / static_cast<float>(n));
  const float rtx = lerp(top_l, top_r, static_cast<float>(i + 1) / static_cast<float>(n));
  return {lerp((ltx + rtx) * 0.5f, (lbx + rbx) * 0.5f, percent),
          lerp(stage_.t, stage_.b, percent)};
}

float StageGeometry::lane_full_width(int32_t lane, float percent) const {
  const int32_t n = std::max(1, config_.lane_count);
  const int32_t i = std::clamp(lane, 0, n - 1);
  const float lbx = lerp(stage_.l, stage_.r, static_cast<float>(i) / static_cast<float>(n));
  const float rbx = lerp(stage_.l, stage_.r, static_cast<float>(i + 1) / static_cast<float>(n));
  const float top_l = taper_x(stage_.l);
  const float top_r = taper_x(stage_.r);
  const float ltx = lerp(top_l, top_r, static_cast<float>(i) / static_cast<float>(n));
  const float rtx = lerp(top_l, top_r, static_cast<float>(i + 1) / static_cast<float>(n));
  return lerp(rtx - ltx, rbx - lbx, percent);
}

float StageGeometry::note_percent(double note_time_sec, double now_sec) const {
  const float appear = config_.appear_time();
  const float x = static_cast<float>((now_sec - note_time_sec) / static_cast<double>(appear) + 1.0);
  return sirius_ease(x);
}

float StageGeometry::judgeline_half_percent() const noexcept {
  // lane_position: t = percent * (1 - margin); dy = stage.h * (1 - margin) * dp
  const float jh = config_.judgeline_height * content_unit();
  const float denom = stage_.h * (1.0f - config_.judgeline_margin_bottom);
  return (jh * 0.5f) / std::max(denom, 1e-6f);
}

Quad StageGeometry::stage_quad() const {
  return {
      {stage_.l, stage_.b},
      {taper_x(stage_.l), stage_.t},
      {taper_x(stage_.r), stage_.t},
      {stage_.r, stage_.b},
  };
}

Quad StageGeometry::note_quad(int32_t lane, int32_t end_lane, float percent) const {
  const float unit = content_unit();
  const float w_ref = lane_width(lane, 1.0f);
  const float w = lane_width(lane, percent);
  // note_height is authored in full-screen half-height units (=1); scale with content.
  const float multiplier = config_.note_height * unit * 0.5f / stage_.h * w / w_ref;

  const Vec2 c1 = lane_position(lane, percent - multiplier);
  const Vec2 c2 = lane_position(lane, percent + multiplier);
  const Vec2 c3 = lane_position(end_lane, percent - multiplier);
  const Vec2 c4 = lane_position(end_lane, percent + multiplier);
  const float w1 = lane_width(lane, percent - multiplier);
  const float w2 = lane_width(lane, percent + multiplier);
  const float move1 = config_.note_move_length * unit * w1 / w_ref;
  const float move2 = config_.note_move_length * unit * w2 / w_ref;

  Quad q;
  q.lb = c2 - Vec2{w2 * 0.5f - move2, 0.0f};
  q.lt = c1 - Vec2{w1 * 0.5f - move1, 0.0f};
  q.rb = c4 + Vec2{w2 * 0.5f - move2, 0.0f};
  q.rt = c3 + Vec2{w1 * 0.5f - move1, 0.0f};
  return q;
}

Quad StageGeometry::hold_body_quad(int32_t lane, int32_t end_lane, float percent_near,
                                   float percent_far) const {
  const float unit = content_unit();
  const float w_ref = lane_width(lane, 1.0f);
  const Vec2 c1 = lane_position(lane, percent_near);
  const Vec2 c2 = lane_position(lane, percent_far);
  const Vec2 c3 = lane_position(end_lane, percent_near);
  const Vec2 c4 = lane_position(end_lane, percent_far);
  const float w1 = lane_width(lane, percent_near);
  const float w2 = lane_width(lane, percent_far);
  const float move1 = config_.note_move_length * unit * w1 / w_ref;
  const float move2 = config_.note_move_length * unit * w2 / w_ref;

  Quad q;
  q.lb = c1 - Vec2{w1 * 0.5f - move1, 0.0f};
  q.lt = c2 - Vec2{w2 * 0.5f - move2, 0.0f};
  q.rb = c3 + Vec2{w1 * 0.5f - move1, 0.0f};
  q.rt = c4 + Vec2{w2 * 0.5f - move2, 0.0f};
  return q;
}

Quad StageGeometry::tick_quad(int32_t lane, int32_t end_lane, float percent) const {
  const float unit = content_unit();
  const float w_ref = lane_width(lane, 1.0f);
  const float w = lane_width(lane, percent);
  const float multiplier = w / w_ref;
  const float half_h = config_.tick_height * unit * 0.5f / stage_.h * multiplier;

  const Vec2 c1 = lane_position(lane, percent - half_h);
  const Vec2 c2 = lane_position(lane, percent + half_h);
  const Vec2 c3 = lane_position(end_lane, percent - half_h);
  const Vec2 c4 = lane_position(end_lane, percent + half_h);
  const float m1 = lane_width(lane, percent - half_h) / w_ref;
  const float m2 = lane_width(lane, percent + half_h) / w_ref;

  const Vec2 cb{(c2.x + c4.x) * 0.5f, (c2.y + c4.y) * 0.5f};
  const Vec2 ct{(c1.x + c3.x) * 0.5f, (c1.y + c3.y) * 0.5f};
  Quad q;
  q.lb = cb - Vec2{m2 * config_.tick_width * unit * 0.5f, 0.0f};
  q.lt = ct - Vec2{m1 * config_.tick_width * unit * 0.5f, 0.0f};
  q.rb = cb + Vec2{m2 * config_.tick_width * unit * 0.5f, 0.0f};
  q.rt = ct + Vec2{m1 * config_.tick_width * unit * 0.5f, 0.0f};
  return q;
}

Quad StageGeometry::sync_line_quad(int32_t lane, int32_t end_lane, float percent) const {
  const float unit = content_unit();
  const float w_ref = lane_width(lane, 1.0f);
  const float w = lane_width(lane, percent);
  const float multiplier = w / w_ref;
  const float half_h = config_.sync_line_height * unit * 0.5f / stage_.h * multiplier;

  const Vec2 c1 = lane_position(lane, percent - half_h);
  const Vec2 c2 = lane_position(lane, percent + half_h);
  const Vec2 c3 = lane_position(end_lane, percent - half_h);
  const Vec2 c4 = lane_position(end_lane, percent + half_h);
  const float w1 = lane_width(lane, percent - half_h);
  const float w2 = lane_width(lane, percent + half_h);
  const float move = config_.note_move_length * unit * multiplier;

  Quad q;
  q.lb = c2 - Vec2{w2 * 0.5f - move, 0.0f};
  q.lt = c1 - Vec2{w1 * 0.5f - move, 0.0f};
  q.rb = c4 + Vec2{w2 * 0.5f - move, 0.0f};
  q.rt = c3 + Vec2{w1 * 0.5f - move, 0.0f};
  return q;
}

Quad StageGeometry::effect_quad(int32_t lane, int32_t end_lane) const {
  // Match the judgment line's vertical band exactly; horizontal span = note lanes.
  const float y0 = judgeline_.lb_y;
  const float y1 = judgeline_.lt_y;
  const float denom = std::max(stage_.h * (1.0f - config_.judgeline_margin_bottom), 1e-4f);
  // y = stage.t - stage.h * p * (1-margin)  →  p = (stage.t - y) / denom
  auto p_at_y = [&](float y) { return (stage_.t - y) / denom; };
  const float p0 = p_at_y(y0);  // bottom of judgeline (near)
  const float p1 = p_at_y(y1);  // top of judgeline (slightly farther)

  const Vec2 c_l0 = lane_position(lane, p0);
  const Vec2 c_l1 = lane_position(lane, p1);
  const Vec2 c_r0 = lane_position(end_lane, p0);
  const Vec2 c_r1 = lane_position(end_lane, p1);
  const float w_l0 = lane_width(lane, p0);
  const float w_l1 = lane_width(lane, p1);
  const float w_r0 = lane_width(end_lane, p0);
  const float w_r1 = lane_width(end_lane, p1);

  Quad q;
  q.lb = {c_l0.x - w_l0 * 0.5f, y0};
  q.rb = {c_r0.x + w_r0 * 0.5f, y0};
  q.lt = {c_l1.x - w_l1 * 0.5f, y1};
  q.rt = {c_r1.x + w_r1 * 0.5f, y1};
  return q;
}

Quad StageGeometry::split_line_quad(int32_t boundary_after_lane, float percent_start,
                                    float percent_end, float length_override) const {
  // boundary_after_lane: draw on the left edge of lane (boundary_after_lane + 1),
  // matching Sonolus drawLine(id) which uses lines[id+1] left edge.
  const int32_t lane = std::clamp(boundary_after_lane + 1, 0, config_.lane_count - 1);
  const float w_ref = std::max(lane_full_width(lane, 1.0f), 1e-6f);
  const float p0 = sirius_ease(percent_start);
  const float p1 = sirius_ease(percent_end);

  const Vec2 c1 = lane_full_position(lane, p0);
  const Vec2 c2 = lane_full_position(lane, p1);
  const float w1 = lane_full_width(lane, p0);
  const float w2 = lane_full_width(lane, p1);
  const Vec2 left1 = c1 + Vec2{-w1 * 0.5f, 0.0f};
  const Vec2 left2 = c2 + Vec2{-w2 * 0.5f, 0.0f};

  // Sirius drawLine: move = splitLineLength/2; offset *= localWidth / widthAt(1).
  // Far (top) is narrower — do not invert with screen-space thickness boosts.
  const float length =
      length_override > 0.0f ? length_override : config_.split_line_length;
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

Quad StageGeometry::split_end_line_quad(float percent_start, float percent_end,
                                        float length_override) const {
  const int32_t lane = config_.lane_count - 1;
  const float w_ref = std::max(lane_full_width(lane, 1.0f), 1e-6f);
  const float p0 = sirius_ease(percent_start);
  const float p1 = sirius_ease(percent_end);

  const Vec2 c1 = lane_full_position(lane, p0);
  const Vec2 c2 = lane_full_position(lane, p1);
  const float w1 = lane_full_width(lane, p0);
  const float w2 = lane_full_width(lane, p1);
  const Vec2 right1 = c1 + Vec2{w1 * 0.5f, 0.0f};
  const Vec2 right2 = c2 + Vec2{w2 * 0.5f, 0.0f};

  const float length =
      length_override > 0.0f ? length_override : config_.split_line_length;
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

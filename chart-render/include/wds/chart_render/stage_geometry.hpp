#pragma once

#include <wds/chart_render/preview_visual_config.hpp>
#include <wds/renderer/draw_types.hpp>

#include <cmath>
#include <cstdint>

namespace wds::renderer {

struct JudgelineQuad {
  float lb_x = 0, lb_y = 0, lt_x = 0, lt_y = 0;
  float rb_x = 0, rb_y = 0, rt_x = 0, rt_y = 0;
};

// Sirius ease from shared/utils.cpp:
// Remap(pow(1.06,-45), 1.06, 0, 1.06, pow(1.06, 45*(x-1)))
float sirius_ease(float x) noexcept;

class StageGeometry {
 public:
  void configure(const PreviewVisualConfig& config);
  void resize(int framebuffer_width, int framebuffer_height);
  // Restrict the stage to a UI content rectangle in framebuffer pixels (top-left origin).
  // The framebuffer itself remains full-size so UI and preview share one NDC space.
  void set_content_rect(int x, int y, int width, int height);
  void clear_content_rect();
  // Full preview column (may include gutters around the aspect-locked stage content).
  // Background cover uses this; stage geometry still uses content().
  void set_panel_rect(int x, int y, int width, int height);
  void clear_panel_rect();

  const PreviewVisualConfig& config() const noexcept { return config_; }
  const ScreenBounds& screen() const noexcept { return screen_; }
  // Aspect-locked stage host rect in NDC. Equals screen() when no content rect is set.
  const ScreenBounds& content() const noexcept { return content_; }
  // Full preview panel in NDC. Equals content() when no panel rect is set.
  const ScreenBounds& panel() const noexcept { return has_panel_rect_ ? panel_ : content_; }
  // Preview panel in framebuffer pixels (top-left origin). False if unset.
  bool panel_framebuffer_rect(int& x, int& y, int& w, int& h) const noexcept {
    if (!has_panel_rect_) {
      return false;
    }
    x = panel_x_;
    y = panel_y_;
    w = panel_w_;
    h = panel_h_;
    return w > 0 && h > 0;
  }
  const StageBounds& stage() const noexcept { return stage_; }
  const JudgelineQuad& judgeline() const noexcept { return judgeline_; }
  // Scale for constants authored against full-screen half-height (=1).
  float content_unit() const noexcept { return std::max(content_.h * 0.5f, 1e-6f); }

  // Lane index is 0-based (core convention). Internally matches Sonolus lines[lane+1].
  Vec2 lane_position(int32_t lane, float percent) const;
  float lane_width(int32_t lane, float percent) const;
  Vec2 lane_full_position(int32_t lane, float percent) const;
  float lane_full_width(int32_t lane, float percent) const;

  // Progress percent from note time (seconds) and now (seconds), matching play utils.
  float note_percent(double note_time_sec, double now_sec) const;

  // Half of the judgment-line band height, in note-percent units (p=1 is band center).
  float judgeline_half_percent() const noexcept;

  // Sirius Hidden Line center / half-height in note-percent (0=tip). Notes above the
  // bar are culled; they fade in across [center-half, center+half].
  float hidden_line_center_percent() const noexcept;
  float hidden_line_half_percent() const noexcept;

  Quad stage_quad() const;
  // Half note height along approach axis (percent units), same as note_quad span.
  float note_half_height_percent(int32_t lane, float percent) const;
  // Flat note on the stage plane (local Z = 0).
  Quad note_quad(int32_t lane, int32_t end_lane, float percent) const;
  // Same plane footprint, then pinhole-project Unity local Z as height above the plane.
  Quad note_quad(int32_t lane, int32_t end_lane, float percent, float unity_local_z) const;
  // Flat-note footprint between two percents (near=judgeline side, far=tip side).
  Quad note_span_quad(int32_t lane, int32_t end_lane, float percent_near, float percent_far,
                      float unity_local_z = 0.0f) const;
  Quad hold_body_quad(int32_t lane, int32_t end_lane, float percent_near,
                      float percent_far) const;
  Quad tick_quad(int32_t lane, int32_t end_lane, float percent) const;
  Quad sync_line_quad(int32_t lane, int32_t end_lane, float percent) const;
  // Hit VFX lying on the judgeline band (same vertical span as the judgment line cells).
  Quad effect_quad(int32_t lane, int32_t end_lane) const;
  // BomSquare frame: same lane-edge taper as the stage, centered on the judgeline.
  // width_scale multiplies the note span; height_screen is full vertical size in NDC.
  Quad bomb_frame_quad(int32_t lane, int32_t end_lane, float width_scale,
                       float height_screen) const;
  Quad split_line_quad(int32_t boundary_after_lane, float percent_start,
                       float percent_end, float length_override = -1.0f) const;
  Quad split_end_line_quad(float percent_start, float percent_end,
                           float length_override = -1.0f) const;
  // Upper spawn mask across all lanes, centered at percent (0=tip).
  Quad hidden_line_quad(float percent) const;
  // Full-lane band from percent_start→percent_end (0=tip).
  Quad lane_span_quad(float percent_start, float percent_end) const;

 private:
  void rebuild_stage();
  void rebuild_panel();
  // Narrow toward the stage center (not world origin). Multiplying x by high_width is only
  // valid when the stage is centered on x=0; embedded preview panels break that assumption.
  float taper_x(float bottom_x) const noexcept;

  PreviewVisualConfig config_;
  ScreenBounds screen_;
  ScreenBounds content_;
  ScreenBounds panel_;
  StageBounds stage_;
  JudgelineQuad judgeline_;
  float fb_w_ = 1.0f;
  float fb_h_ = 1.0f;
  int content_x_ = 0;
  int content_y_ = 0;
  int content_w_ = 0;
  int content_h_ = 0;
  bool has_content_rect_ = false;
  int panel_x_ = 0;
  int panel_y_ = 0;
  int panel_w_ = 0;
  int panel_h_ = 0;
  bool has_panel_rect_ = false;
};

}  // namespace wds::renderer

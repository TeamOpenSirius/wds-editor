#pragma once

#include <wds/chart_render/preview_visual_config.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/note_position_calculator.hpp>
#include <wds/renderer/draw_types.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace wds::renderer {

struct JudgelineQuad {
  float lb_x = 0, lb_y = 0, lt_x = 0, lt_y = 0;
  float rb_x = 0, rb_y = 0, rt_x = 0, rt_y = 0;
};

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
  // 16:9 stage height in framebuffer pixels. Official / old-editor floor uses 720.
  int content_height_px() const noexcept {
    return has_content_rect_ ? std::max(1, content_h_) : std::max(1, static_cast<int>(fb_h_));
  }

  // Lane index is 0-based (core convention). Internally matches Sonolus lines[lane+1].
  Vec2 lane_position(int32_t lane, float percent) const;
  float lane_width(int32_t lane, float percent) const;
  // Same projection as lane_position / lane_width without clamping to 0..n-1.
  Vec2 authored_lane_position(int32_t lane, float percent) const;
  float authored_lane_width(int32_t lane, float percent) const;
  Vec2 lane_full_position(int32_t lane, float percent) const;
  float lane_full_width(int32_t lane, float percent) const;

  // Screen percent (0=top, 1=bottom) from official Y → LaneGroup+FOV50.
  float note_percent(double note_time_sec, double now_sec) const;
  void set_scroll_speed_points(std::vector<wds::chart_editor::ScrollSpeedPoint> points,
                               const wds::chart_editor::MusicTiming& timing);
  float judgeline_percent() const noexcept;

  // Half of the judgment-line band height, in note-percent units.
  float judgeline_half_percent() const noexcept;

  // Official StartLine sprite center / half-height in screen percent (0=top).
  float hidden_line_center_percent() const noexcept;
  float hidden_line_half_percent() const noexcept;
  // Official LaneMask bottom (GetNoteVisiblePositionY). Notes appear here.
  float lane_mask_bottom_percent() const noexcept;
  // Content-space Y of that mask edge (horizontal after projection; larger = top).
  float lane_mask_bottom_content_y() const noexcept;
  // Official SpriteMask: hide the spawn-side of a projected quad (y > mask).
  // far_t is the kept fraction along each near→far edge (1 = unclipped).
  bool clip_quad_outside_lane_mask(Quad& q, float& far_t) const noexcept;

  Quad stage_quad() const;
  // Persistent BG_LaneBorder edge. Pass a logical edge (0 … lane_count);
  // visual 6-track borders use official_visual_border_edge_index (0,2,…,12).
  Quad lane_border_quad(int32_t edge_index) const;
  // OpenWDS BGShadow child: sprite local rect × child localScale, z=12,
  // through the identity Main Camera (same path as project_world).
  Quad bg_shadow_quad(float scale_x, float scale_y) const;
  // OpenWDS GameBackground/Jacket: identity camera, no LaneGroup Rx=60°.
  // Center (0, 2*fit, 12), half-edge 4 * 0.6 * fit. fit is sample cover-fit.
  Quad jacket_quad() const;
  // Half note height along approach axis (percent units), same as note_quad span.
  float note_half_height_percent(int32_t lane, float percent) const;
  // Official tap visual (notation − 0.15) × 0.64 at the given percent.
  Quad note_quad(int32_t lane, int32_t end_lane, float percent) const;
  // Bottom/Top each project their own sprite with Note Rx + layer Z.
  Quad note_quad(int32_t lane, int32_t end_lane, float percent, float unity_local_z) const;
  // Visible sprite strip between two percents (near=judgeline side, far=tip side).
  Quad note_span_quad(int32_t lane, int32_t end_lane, float percent_near, float percent_far,
                      float unity_local_z = 0.0f) const;
  // Notation-width ribbon (sync / tick). Hold art uses hold_line_quad.
  Quad hold_body_quad(int32_t lane, int32_t end_lane, float percent_near,
                      float percent_far) const;
  // Official HoldLongNotes: notation − 0.15 + 0.10, centered on the span.
  Quad hold_line_quad(int32_t lane, int32_t end_lane, float percent_near,
                      float percent_far) const;
  Quad tick_quad(int32_t lane, int32_t end_lane, float percent) const;
  // Mid-star: official 1.12×1.12 SoundNote, Rx=0, centered, never stretched or tilted.
  Quad star_quad(int32_t lane, int32_t end_lane, float percent) const;
  // Official ConcurrentLine: notation-wide × 0.1, Rx=90° on JudgeArea (no tap margin).
  Quad sync_line_quad(int32_t lane, int32_t end_lane, float percent) const;
  // Hit VFX lying on the judgeline band (same vertical span as the judgment line cells).
  Quad effect_quad(int32_t lane, int32_t end_lane) const;
  // BomSquare on JudgeArea (Local alignment): width_scale × note span, height in
  // official world Y. Corners go through project_judge so the frame tapers.
  Quad bomb_frame_quad(int32_t lane, int32_t end_lane, float width_scale,
                       float height_unity) const;
  // BomFlare: RenderAlignment=View, startSize=8, never scaled by note width.
  // Axis-aligned screen disc at the OnBomb midpoint.
  Quad bomb_flare_billboard_quad(int32_t lane, int32_t end_lane, float size_unity) const;
  // Official Main-space projection (LaneGroup + FOV50).
  Vec2 project_main(float local_x, float main_y, float local_z = 0.0f) const noexcept;
  // World-space point through the identity Main Camera (FOV 50 at origin).
  Vec2 project_world(float world_x, float world_y, float world_z) const noexcept;
  // View-aligned billboard at a Main-space point. Size uses the same
  // world→screen scale as bomb_flare_billboard_quad, measured at main_y.
  Quad split_particle_billboard_quad(float world_x, float main_y, float size_unity,
                                     float world_z = 0.0f) const;
  // Official PSR: View (0) camera right/up + Z roll, and view-space 3D tumble
  // when startRotation 3D is on; World (1) / Local (2) use LaneGroup axes.
  // pivot is a fraction of size.
  Quad split_particle_oriented_quad(float world_x, float main_y, float size_unity, float height_scale,
                                    float pivot_y, float roll, float tilt_x, float tilt_y,
                                    float world_z = 0.0f, uint8_t render_alignment = 0,
                                    float pivot_x = 0.0f, float pivot_z = 0.0f) const;
  // Main-space corners of that same quad, order lb, rb, rt, lt (Stretch UV).
  // Preview draws Billboard/Stretch/Mesh/Trails through
  // official_main_clip_poly_near_uv — Unity Camera clips
  // ParticleSystemRenderer triangles at nearClipPlane 0.1
  // (OpenWDS OfflineRhythmPreview.unity + N6 camera_volumes.json).
  // Rotation by Speed / 3D
  // startRotation / tall Local SOL Y can put a corner behind the camera;
  // projecting first explodes NDC. False only if out pointers are null.
  bool split_particle_oriented_world_corners(float world_x, float main_y, float size_unity,
                                             float height_scale, float pivot_y, float roll,
                                             float tilt_x, float tilt_y, float world_z,
                                             uint8_t render_alignment, float pivot_x,
                                             float pivot_z, float* out_x, float* out_y,
                                             float* out_z) const;
  // Stretch corners. Two exclusive branches (do not blend, not per-ID):
  //   freeform==0: approved unofficial kettlewell 2025-02 strip (not 2022.3).
  //     View-space start/end; width is the 2D cross of those points, not
  //     Cross(cam_fwd, vel). endVS = pos - vel*(VS + (1/|v|)*LS*size.y).
  //     cameraVelocityScale (approved unofficial, not 2022.3):
  //       velocityVS -= cameraVel_vs * camera_velocity_scale.
  //     cam_vel_* is Main-space camera velocity (official playfield is static
  //     → 0,0,0). Preview may pass zeros and still keep the multiply.
  //   freeform!=0: approved unofficial pcwalton particle-space basis
  //     (gist/pcwalton, experimental, not Unity 2022.3 ops). Rotate With
  //     Stretch only when rotate_with_stretch!=0 (2022.3: only if Freeform on).
  // across = clamped Start Size width (hsize.x); along = size.y for the
  // kettlewell LS term (3D height / non-3D startSize). World corners are
  // lb, rb, rt, lt. lb→rb is +velocity (tail→head) on the kettlewell path.
  // False when the active branch is degenerate (caller fallback).
  bool split_particle_stretch_world_corners(float world_x, float main_y, float world_z, float vel_x,
                                           float vel_y, float vel_z, float across, float along,
                                           float pivot_y, float roll, float pivot_x, float* out_x,
                                           float* out_y, float* out_z, float length_scale = 1.0f,
                                           float velocity_scale = 0.0f,
                                           float camera_velocity_scale = 0.0f,
                                           float cam_vel_x = 0.0f, float cam_vel_y = 0.0f,
                                           float cam_vel_z = 0.0f, uint8_t freeform_stretching = 0,
                                           uint8_t rotate_with_stretch = 0) const;
  Quad split_particle_stretch_quad(float world_x, float main_y, float world_z, float vel_x,
                                   float vel_y, float vel_z, float across, float along,
                                   float pivot_y, float roll, float pivot_x = 0.0f,
                                   float length_scale = 1.0f, float velocity_scale = 0.0f,
                                   float camera_velocity_scale = 0.0f, float cam_vel_x = 0.0f,
                                   float cam_vel_y = 0.0f, float cam_vel_z = 0.0f,
                                   uint8_t freeform_stretching = 0,
                                   uint8_t rotate_with_stretch = 0) const;
  Quad split_line_quad(int32_t boundary_after_lane, float percent_start,
                       float percent_end, float length_override = -1.0f) const;
  // Official SplitLine at a C# world X. u0/u1 are sprite UV (uvTransform);
  // the tight mesh is kOfficialSplitLineMeshU0–U1, not the full m_Rect.
  Quad split_line_quad_world_x(float world_x, float percent_start, float percent_end,
                               float u0 = 0.0f, float u1 = 1.0f, float width_scale = 1.0f) const;
  Quad split_end_line_quad(float percent_start, float percent_end,
                           float length_override = -1.0f) const;
  // Upper spawn mask across all lanes, centered at percent (0=tip).
  Quad hidden_line_quad(float percent) const;
  // Full-lane band from percent_start→percent_end (0=tip).
  Quad lane_span_quad(float percent_start, float percent_end) const;

 private:
  void rebuild_stage();
  void rebuild_panel();
  float content_aspect() const noexcept;
  Vec2 ndc_to_content(wds::chart_editor::OfficialNdc ndc) const noexcept;
  Vec2 project_judge(float local_x, float judge_y, float local_z = 0.0f) const noexcept;
  float percent_to_judge_y(float percent) const noexcept;
  float judge_y_to_percent(float judge_y) const noexcept;

  PreviewVisualConfig config_;
  wds::chart_editor::NotePositionCalculator position_calculator_;
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

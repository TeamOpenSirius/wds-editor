#pragma once

#include <wds/core/official_playfield.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wds::chart_render {

// GameHudRuntime CanvasScaler: 1920×1080, matchWidthOrHeight = 0.5.
inline constexpr float kOpenWdsHudRefWidth = 1920.0f;
inline constexpr float kOpenWdsHudRefHeight = 1080.0f;
inline constexpr float kOpenWdsHudMatch = 0.5f;

// Combo.prefab root RectTransform + ComboPanel.SetComboDigitImages.
inline constexpr float kOpenWdsComboPosX = -20.0f;
inline constexpr float kOpenWdsComboPosY = -20.0f;
inline constexpr float kOpenWdsComboWidth = 408.0f;
inline constexpr float kOpenWdsComboHeight = 192.0f;
inline constexpr float kOpenWdsComboDigitW = 118.0f;
inline constexpr float kOpenWdsComboDigitH = 148.0f;
inline constexpr float kOpenWdsComboLabelW = 172.0f;
inline constexpr float kOpenWdsComboLabelH = 48.0f;

inline constexpr float kOpenWdsComboDigitXs1[] = {-138.0f};
inline constexpr float kOpenWdsComboDigitXs2[] = {-92.0f, -184.0f};
inline constexpr float kOpenWdsComboDigitXs3[] = {-46.0f, -138.0f, -230.0f};
inline constexpr float kOpenWdsComboDigitXs4[] = {0.0f, -92.0f, -184.0f, -276.0f};

// CreateOfflineRhythmPreviewScene Game/Effects: (0, −0.46, 10), then
// GameHudRuntime overwrites Y with CalculatePositionY(offset).
inline constexpr float kOpenWdsEffectsWorldX = 0.0f;
inline constexpr float kOpenWdsEffectsWorldZ = 10.0f;
inline constexpr int kOpenWdsTimingEffectOffsetDefault = 60;
inline constexpr int kOpenWdsTimingEffectScaleTypeDefault = 1;

// TimingEffect / TimingAssistEffect SpriteRenderer is Simple (prefab
// m_DrawMode=0, m_WasSpriteAssigned=0). Unity 2022.3 SpriteRenderer.size
// only applies to Sliced/Tiled, so the empty prefab m_Size is not the
// rendered size. Assigned sprite world size is Sprite.rect / pixelsPerUnit;
// YAML m_Pivot (normalized) sits on the Game/Effects transform.
// Auto: Assets/Resources/Sprite/txt_game_common_judgment_auto.asset
inline constexpr float kOpenWdsTimingSpritePpu = 100.0f;
inline constexpr float kOpenWdsTimingAutoRectW = 215.89714f;
inline constexpr float kOpenWdsTimingAutoRectH = 75.84776f;
inline constexpr float kOpenWdsTimingAutoPivotX = 0.49085066f;
inline constexpr float kOpenWdsTimingAutoPivotY = 0.4868157f;
inline constexpr float kOpenWdsTimingAutoSpriteW =
    kOpenWdsTimingAutoRectW / kOpenWdsTimingSpritePpu;
inline constexpr float kOpenWdsTimingAutoSpriteH =
    kOpenWdsTimingAutoRectH / kOpenWdsTimingSpritePpu;

struct OpenWdsSpriteWorldQuad {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
};

inline OpenWdsSpriteWorldQuad openwds_sprite_world_quad(float cx, float cy, float world_w,
                                                       float world_h, float pivot_x,
                                                       float pivot_y) noexcept {
  OpenWdsSpriteWorldQuad q;
  q.x0 = cx - pivot_x * world_w;
  q.x1 = cx + (1.0f - pivot_x) * world_w;
  q.y0 = cy - pivot_y * world_h;
  q.y1 = cy + (1.0f - pivot_y) * world_h;
  return q;
}

inline float openwds_timing_position_y(int offset = kOpenWdsTimingEffectOffsetDefault) noexcept {
  return ((static_cast<float>(offset) / -100.0f) + 1.0f) * 6.5f - 2.2f;
}

inline float openwds_timing_effect_scale(int type = kOpenWdsTimingEffectScaleTypeDefault) noexcept {
  switch (type) {
    case 0:
      return 0.8f;
    case 1:
      return 1.0f;
    case 2:
      return 1.3f;
    default:
      return 1.0f;
  }
}

inline float openwds_canvas_scale(float content_w, float content_h) noexcept {
  const float sw = content_w / kOpenWdsHudRefWidth;
  const float sh = content_h / kOpenWdsHudRefHeight;
  return (1.0f - kOpenWdsHudMatch) * sw + kOpenWdsHudMatch * sh;
}

struct OpenWdsCanvasMap {
  float scale = 1.0f;
  float origin_x = 0.0f;
  float origin_y = 0.0f;

  float x(float canvas_x) const noexcept { return origin_x + canvas_x * scale; }
  float y(float canvas_y) const noexcept { return origin_y + canvas_y * scale; }
};

inline OpenWdsCanvasMap openwds_canvas_map(float content_l, float content_r, float content_b,
                                           float content_t) noexcept {
  const float w = content_r - content_l;
  const float h = content_t - content_b;
  OpenWdsCanvasMap m;
  m.scale = openwds_canvas_scale(w, h);
  const float ref_w = kOpenWdsHudRefWidth * m.scale;
  const float ref_h = kOpenWdsHudRefHeight * m.scale;
  m.origin_x = content_l + 0.5f * (w - ref_w);
  m.origin_y = content_b + 0.5f * (h - ref_h);
  return m;
}

inline const float* openwds_combo_digit_xs(int digits, int& count) noexcept {
  const int n = std::clamp(digits, 1, 4);
  count = n;
  switch (n) {
    case 1:
      return kOpenWdsComboDigitXs1;
    case 2:
      return kOpenWdsComboDigitXs2;
    case 3:
      return kOpenWdsComboDigitXs3;
    default:
      return kOpenWdsComboDigitXs4;
  }
}

inline wds::chart_editor::OfficialNdc project_world_xyz(
    float world_x, float world_y, float world_z,
    float aspect = wds::chart_editor::kOfficialPreviewAspect) noexcept {
  const float f = wds::chart_editor::official_camera_focal();
  const float z = std::max(world_z, 1e-4f);
  const float a = std::max(aspect, 1e-4f);
  wds::chart_editor::OfficialNdc ndc;
  ndc.x = (world_x / z) * f / a;
  ndc.y = (world_y / z) * f;
  return ndc;
}

}  // namespace wds::chart_render

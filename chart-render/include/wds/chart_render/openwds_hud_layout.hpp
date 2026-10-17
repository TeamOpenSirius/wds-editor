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

// ComboCount / ComboCountPer100 stretch-fill Combo root; scale about this pivot.
// Combo.prefab ComboCount m_Pivot (0.5, 0.5). ComboLabel is a sibling — do not scale it.
inline constexpr float kOpenWdsComboCountPivotX = 0.5f;
inline constexpr float kOpenWdsComboCountPivotY = 0.5f;

// Combo_animator Count Layer: Any State → Combo_anim on OnCount
// (transition duration 0, CanTransitionToSelf 1). Clip LoopTime 0, StopTime 10/60.
// Keys: Assets/Resources/Animation/Game/Combo_anim.anim Scale ComboCount.
inline constexpr float kOpenWdsComboCountAnimDuration = 0.16666667f;

// Combo_animator Base Layer: Any State → Combo_countper100_anim on OnFire
// when combo/100 changes. Clip LoopTime 0, StopTime 0.25.
// Keys: Assets/Resources/Animation/Game/Combo_countper100_anim.anim.
inline constexpr float kOpenWdsComboFireAnimDuration = 0.25f;

struct OpenWdsAnimKey {
  float t = 0.0f;
  float v = 0.0f;
  float in_s = 0.0f;
  float out_s = 0.0f;
};

struct OpenWdsComboScale {
  float x = 1.0f;
  float y = 1.0f;
};

// Unity 2022.3 AnimationCurve cubic Hermite (slopes are dValue/dTime).
// https://docs.unity3d.com/2022.3/Documentation/Manual/animeditor-AnimationCurves.html
// Clip LoopTime 0: sample min(max(time, first), last) — hold the last key.
inline float openwds_eval_clip_curve(const OpenWdsAnimKey* keys, int count, float time) noexcept {
  if (keys == nullptr || count <= 0) {
    return 0.0f;
  }
  if (time <= keys[0].t) {
    return keys[0].v;
  }
  if (time >= keys[count - 1].t) {
    return keys[count - 1].v;
  }
  for (int i = 1; i < count; ++i) {
    if (time > keys[i].t) {
      continue;
    }
    const OpenWdsAnimKey& a = keys[i - 1];
    const OpenWdsAnimKey& b = keys[i];
    const float dt = b.t - a.t;
    if (dt <= 1e-8f) {
      return b.v;
    }
    if (!std::isfinite(a.out_s) || !std::isfinite(b.in_s)) {
      return a.v;
    }
    const float u = (time - a.t) / dt;
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    const float y = h00 * a.v + h10 * (a.out_s * dt) + h01 * b.v + h11 * (b.in_s * dt);
    return std::isfinite(y) ? y : a.v + (b.v - a.v) * u;
  }
  return keys[count - 1].v;
}

inline OpenWdsComboScale openwds_combo_count_scale(float age_sec) noexcept {
  // Combo_anim.anim m_ScaleCurves path ComboCount (11 keys @ 60 fps).
  constexpr OpenWdsAnimKey kX[] = {
      {0.0f, 0.8f, 0.0f, 5.3884397f},
      {0.016666668f, 0.88980734f, 5.3884397f, 2.991936f},
      {0.033333335f, 0.93967295f, 2.991936f, 2.276223f},
      {0.05f, 0.97761f, 2.276223f, 1.8559166f},
      {0.06666667f, 1.008542f, 1.8559166f, 1.5492083f},
      {0.083333336f, 1.0343621f, 1.5492083f, 1.2938548f},
      {0.1f, 1.0559263f, 1.2938548f, 1.0584213f},
      {0.11666667f, 1.0735667f, 1.0584215f, 0.81984013f},
      {0.13333334f, 1.0872307f, 0.81984013f, 0.55260664f},
      {0.15f, 1.0964408f, 0.55260664f, 0.21355392f},
      {0.16666667f, 1.1f, 0.21355392f, 0.0f},
  };
  constexpr OpenWdsAnimKey kY[] = {
      {0.0f, 0.8f, 0.0f, 5.3237376f},
      {0.016666668f, 0.888729f, 5.3237376f, 2.9964242f},
      {0.033333335f, 0.9386694f, 2.9964242f, 2.2858074f},
      {0.05f, 0.97676617f, 2.2858074f, 1.8665203f},
      {0.06666667f, 1.0078748f, 1.8665203f, 1.5595866f},
      {0.083333336f, 1.033868f, 1.5595866f, 1.303382f},
      {0.1f, 1.055591f, 1.303382f, 1.0665895f},
      {0.11666667f, 1.0733675f, 1.0665895f, 0.8262417f},
      {0.13333334f, 1.0871382f, 0.8262417f, 0.5567551f},
      {0.15f, 1.0964174f, 0.5567551f, 0.21495582f},
      {0.16666667f, 1.1f, 0.21495582f, 0.0f},
  };
  OpenWdsComboScale s;
  s.x = openwds_eval_clip_curve(kX, 11, age_sec);
  s.y = openwds_eval_clip_curve(kY, 11, age_sec);
  return s;
}

inline OpenWdsComboScale openwds_combo_fire_scale(float age_sec) noexcept {
  constexpr OpenWdsAnimKey kS[] = {
      {0.0f, 1.0f, 0.0f, 4.0f},
      {0.25f, 2.0f, 0.0f, 0.0f},
  };
  const float v = openwds_eval_clip_curve(kS, 2, age_sec);
  return {v, v};
}

inline float openwds_combo_fire_alpha(float age_sec) noexcept {
  constexpr OpenWdsAnimKey kA[] = {
      {0.0f, 0.0f, 0.0f, 56.0703f},
      {0.05f, 1.0f, -4.236987e-6f, -11.88784f},
      {0.25f, 0.0f, -1.0913026e-6f, 0.0f},
  };
  return openwds_eval_clip_curve(kA, 3, age_sec);
}

inline void openwds_scale_about(float px, float py, float sx, float sy, float& x, float& y) noexcept {
  x = px + (x - px) * sx;
  y = py + (y - py) * sy;
}

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

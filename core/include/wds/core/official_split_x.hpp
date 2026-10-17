#pragma once

#include <wds/core/gimmick.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <cstdint>

namespace wds::chart_editor {

// Official SplitLine sprite is 256px / 100 ppu; Line localScale.x = 0.1.
inline constexpr float kOfficialSplitLineLocalScaleX = 0.1f;
// textures.bundle SplitLine: m_Rect 256², pivot (0.5, 0),
// textureRect (10.076, 0, 236.848, 247.944), uvTransform (100, 128, 100, 0).
// Unity 2022.3 Sprite.textureRect is the mesh UV span — not the full 0–1
// m_Rect. Drawing u=0–1 on the 0.256 plate invents extra texels past the
// tight mesh (empty on the left, tail fade on the right).
// https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Sprite-textureRect.html
inline constexpr float kOfficialSplitLineTextureRectX = 10.076120376586914f;
inline constexpr float kOfficialSplitLineTextureRectW = 236.84774780273438f;
inline constexpr float kOfficialSplitLineMeshU0 =
    kOfficialSplitLineTextureRectX / kOfficialSplitLineSpritePixels;
inline constexpr float kOfficialSplitLineMeshU1 =
    (kOfficialSplitLineTextureRectX + kOfficialSplitLineTextureRectW) /
    kOfficialSplitLineSpritePixels;
inline constexpr int kOfficialSplitLineMaxCount = 6;

inline float official_split_line_world_width() noexcept {
  return kOfficialSplitLineSprite * kOfficialSplitLineLocalScaleX;
}

// Tight-mesh U along the plate. t=0 is textureRect left, not m_Rect u=0.
inline float official_split_line_mesh_u(float t01) noexcept {
  const float t = std::clamp(t01, 0.0f, 1.0f);
  return kOfficialSplitLineMeshU0 + (kOfficialSplitLineMeshU1 - kOfficialSplitLineMeshU0) * t;
}

// Pivot is the 256px rect center (Unity 2022.3 Sprite.pivot in pixels).
// Shader UV is texture-space (Sprite.uv), so u=0.5 sits on the Line origin.
inline float official_split_line_local_x(float mesh_u) noexcept {
  return (mesh_u - 0.5f) * official_split_line_world_width();
}

// catalog.json lineHight.localPosition.y (OpenWDS LineHight Transform).
// Identity LineHight is y=0 and does not use this helper. z=180 prefabs:
// default 2.5; 11511/11700=2.45; 11792=1.0. Origin offset only — element
// fade lever is OpenWDS EffectOffsetPosition 2.5 (emit_local_y).
inline float official_split_z180_line_hight_y(int32_t color_id) noexcept {
  switch (color_id) {
    case 11511:
    case 11700:
      return 2.45f;
    case 11792:
      return 1.0f;
    default:
      return kOfficialZ180LineHightY;
  }
}

inline float official_split_z180_origin_y(int32_t color_id) noexcept {
  return official_split_identity_origin_y() +
         official_split_z180_line_hight_y(color_id) * kOfficialLineHeightScale;
}

// OnFadeIn SetLocalPositionX table (so.c 3702850). Left → right, lanes = split_count.
// Index 0 = leftmost (−5.55). Returns split_count+1 positions.
inline int official_split_line_x_count(int32_t split_count) noexcept {
  if (split_count < 1 || split_count > kOfficialSplitLineMaxCount) {
    return 0;
  }
  return split_count + 1;
}

inline bool official_split_line_world_x(int32_t split_count, int32_t index,
                                        float& out_x) noexcept {
  const int n = official_split_line_x_count(split_count);
  if (n <= 0 || index < 0 || index >= n) {
    return false;
  }
  switch (split_count) {
    case 1: {
      static constexpr float kXs[] = {-5.55f, 5.55f};
      out_x = kXs[index];
      return true;
    }
    case 2: {
      static constexpr float kXs[] = {-5.55f, 0.0f, 5.55f};
      out_x = kXs[index];
      return true;
    }
    case 3: {
      static constexpr float kXs[] = {-5.55f, -1.85f, 1.85f, 5.55f};
      out_x = kXs[index];
      return true;
    }
    case 4: {
      static constexpr float kXs[] = {-5.55f, -2.775f, 0.0f, 2.775f, 5.55f};
      out_x = kXs[index];
      return true;
    }
    case 5: {
      static constexpr float kXs[] = {-5.55f, -2.775f, -0.925f, 0.925f, 2.775f, 5.55f};
      out_x = kXs[index];
      return true;
    }
    case 6: {
      static constexpr float kXs[] = {-5.55f, -3.7f, -1.85f, 0.0f, 1.85f, 3.7f, 5.55f};
      out_x = kXs[index];
      return true;
    }
    default:
      return false;
  }
}

// fadeIn: reverse LineHight scale.y from the visible tip percent. Full-length = 1.
inline float official_split_fade_scale_from_percent(int32_t color_id, float percent_start,
                                                    float percent_end,
                                                    int32_t anim_phase) noexcept {
  if (anim_phase != 0) {
    return 1.0f;
  }
  const float world_len = official_split_line_world_length();
  if (world_len < 1e-6f) {
    return 1.0f;
  }
  if (split_fade_grows_from_tip(color_id)) {
    const float tip_y = official_percent_to_main_y(percent_end);
    return std::clamp((official_split_z180_origin_y(color_id) - tip_y) / world_len, 0.0f,
                      1.0f);
  }
  const float tip_y = official_percent_to_main_y(percent_start);
  return std::clamp((tip_y - official_split_identity_origin_y()) / world_len, 0.0f, 1.0f);
}

// Line object-space Y. Identity: +Y toward the tip. z=180: +Y toward the judge.
inline float official_split_line_local_y(int32_t color_id, float main_y,
                                         float fade_scale) noexcept {
  const float world_y_scale = kOfficialLineHeightScale * std::max(fade_scale, 1e-6f);
  if (split_fade_grows_from_tip(color_id)) {
    return (official_split_z180_origin_y(color_id) - main_y) / world_y_scale;
  }
  return (main_y - official_split_identity_origin_y()) / world_y_scale;
}

}  // namespace wds::chart_editor

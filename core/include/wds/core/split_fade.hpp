#pragma once

#include <wds/core/official_playfield.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wds::chart_editor {

// Official SplitEffect clips (resources.unitypackage SplitEffect_fadeIn_anim.anim
// and client 1.96.0 animators.bundle — same Hermite keys):
//   fadeIn  — LineHight localScale.y 0→1 over 1.0s, a stays 1
//             t=0 value.y=0 outSlope.y=2.3040817; t=1 value.y=1 inSlope.y=0
//             cubic 0.30408168 t^3 − 1.60816336 t^2 + 2.30408168 t
//   fadeOut — SpriteRenderer.m_Color.a 1→0, keys at t=0 and t=0.3
//             1 − smoothstep(t/0.3)
// Scheduler: fadeIn at start−1s; fadeOut at EndTickCount.
//
// That cubic is Unity local Y, not screen Y. The playfield is LaneGroup
// Rx=60° at z=12 (LaneGroup.prefab), rendered by a perspective camera
// FOV 50 (GameSimulationCamera / GameConfig._cameraFOV). Split effects
// parent to JudgeArea/EffectParent (local y=-3.9 on Main). Initialize
// writes SplitEffect scale.y=27 pos.y=-5. Sprite 256px / 100 ppu = 2.56.
// 10392 LineHight is pos.y=2.5, rot z=180.
//
// Preview maps those local-Y endpoints through the same tilt+FOV, then
// NDC y ∈ [+1,−1] → stage percent [0,1]. Do not substitute sirius_ease
// (note-time curve) or put the cubic on linear screen Y.
inline constexpr float kOfficialSplitFadeInSec = 1.0f;
inline constexpr float kOfficialSplitFadeOutSec = 0.3f;
inline constexpr float kOfficialSplitEffectY = -5.0f;
inline constexpr float kOfficialLineHeightScale = 27.0f;
inline constexpr float kOfficialSplitLineSprite = 2.56f;
inline constexpr float kOfficialZ180LineHightY = 2.5f;

inline float official_split_fade_in_scale(float t01) noexcept {
  const float t = std::clamp(t01, 0.0f, 1.0f);
  return ((0.30408167839050293f * t - 1.6081633567810059f) * t + 2.304081678390503f) * t;
}

inline float official_lane_local_y_to_percent(float lane_y) noexcept {
  return official_main_y_to_percent(lane_y);
}

inline float official_split_line_world_length() noexcept {
  return kOfficialSplitLineSprite * kOfficialLineHeightScale;
}

inline float official_split_identity_origin_y() noexcept {
  return kOfficialJudgeAreaY + kOfficialSplitEffectY;
}

inline float official_split_z180_origin_y() noexcept {
  return official_split_identity_origin_y() +
         kOfficialZ180LineHightY * kOfficialLineHeightScale;
}

// SplitLine plate is 256px / 100 ppu. VFX SplitEffect_all SplitLine Height=45
// (OffSet=5) is the bright-head length in those texels; the output shader is
// solid (1,1,1,1) × expr-68 alpha (peak at t=0.05, long tail). After ×27 that
// is 12.15 world units. A 40–56% percent span still looked like ~15% solid
// white when we used smoothstep + pale LineColor + glow only at t>0.55.
// Intensity must follow expr 68. Identity: 45/256 of the visible ribbon at p0
// (far-end 12.15 wu is only ~2% of the screen). z=180: 12.15 wu from the mesh
// tip (rotZ=180 texture TOP = judge end, origin 58.6 − 69.12 = −10.52).
inline constexpr float kOfficialSplitLineSpritePixels = 256.0f;
inline constexpr float kOfficialSplitLineVfxHeight = 45.0f;
inline constexpr float kOfficialSplitLineVfxOffset = 5.0f;
inline constexpr float kOfficialSplitLineSpriteTipFrac =
    kOfficialSplitLineVfxHeight / kOfficialSplitLineSpritePixels;
inline constexpr float kOfficialSplitLineTextureTipFrac = 0.05f;

inline float official_split_line_tip_world() noexcept {
  return official_split_line_world_length() * kOfficialSplitLineSpriteTipFrac;
}

inline float official_split_line_texture_tip_world() noexcept {
  return official_split_line_world_length() * kOfficialSplitLineTextureTipFrac;
}

inline float official_split_visible_tip_span(float percent_start, float percent_end,
                                            bool judge_anchored = false) noexcept {
  if (!judge_anchored) {
    // Far tip: 12.15 wu is only ~2% of the screen after perspective. Keep the
    // VFX Height/256 visible-ribbon floor so the upward particle head stays
    // readable (≈45 wu at the frustum top).
    const float y0 = official_percent_to_main_y(percent_start);
    const float world_span = official_main_y_to_percent(y0 - official_split_line_tip_world()) -
                             percent_start;
    const float frac_span = (percent_end - percent_start) * kOfficialSplitLineSpriteTipFrac;
    return std::max(std::max(world_span, frac_span), 1e-4f);
  }
  const float mesh_tip_y = (percent_end >= 1.0f - 1e-3f)
                               ? official_split_z180_origin_y() - official_split_line_world_length()
                               : official_percent_to_main_y(percent_end);
  const float inner_p =
      official_main_y_to_percent(mesh_tip_y + official_split_line_tip_world());
  return std::max(percent_end - std::max(percent_start, inner_p), 1e-4f);
}

inline float official_split_fade_in_from_tip_end(float t01) noexcept {
  const float tip_y =
      official_split_z180_origin_y() - official_split_line_world_length() *
                                           official_split_fade_in_scale(t01);
  return std::clamp(official_lane_local_y_to_percent(tip_y), 0.0f, 1.0f);
}

inline float official_split_fade_in_from_judge_start(float t01) noexcept {
  const float tip_y =
      official_split_identity_origin_y() +
      official_split_line_world_length() * official_split_fade_in_scale(t01);
  return std::clamp(official_lane_local_y_to_percent(tip_y), 0.0f, 1.0f);
}

inline float official_split_fade_in_visible(float t01) noexcept {
  return official_split_fade_in_from_tip_end(t01);
}

inline float official_split_fade_out_alpha(float t01) noexcept {
  const float u = std::clamp(t01, 0.0f, 1.0f);
  return 1.0f - u * u * (3.0f - 2.0f * u);
}

inline int64_t split_fade_sec_to_ms(float seconds) noexcept {
  return std::max<int64_t>(
      1, static_cast<int64_t>(std::llround(static_cast<double>(seconds) * 1000.0)));
}

}  // namespace wds::chart_editor

#pragma once

#include <wds/core/official_playfield.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wds::chart_editor {

// Official SplitEffect clips, runtime 1.96.0 animators.bundle
// (game_splitlanecommonassets_assets_spliteffectcommonassets, m_StreamedClip):
//   fadeIn  — LineHight localScale.y 0→1 over 1.0s, a stays 1
//             t=0 value.y=0 outSlope.y=2.3040817; t=1 value.y=1 inSlope.y=0
//             cubic 0.30408168 t^3 − 1.60816336 t^2 + 2.30408168 t
//   fadeOut — Line1..7 m_Color.a 1→0, keys t=0 / t=0.3 (cubic 74.074 t^3
//             − 33.333 t^2 + 1), i.e. 1 − smoothstep(t/0.3). LineHight
//             scale stays (1,1,1). The loose Assets/Resources fadeOut .anim
//             keys t=1 but is not the clip the runtime controller plays.
// Scheduler (OpenWDS SplitLaneRuntime.Tick + SplitLaneAssetRuntime):
// fadeIn at start−1s. OnFadeOut at EndTickCount (expire = EndMs).
// Destroy(gameObject, 0.5s); lines reach a=0 at +0.3s. No 1500ms
// minimum-showing pad.
//
// That cubic is Unity local Y, not screen Y. The playfield is LaneGroup
// Rx=60° at z=12 (LaneGroup.prefab), rendered by a perspective camera
// FOV 50 (GameSimulationCamera / GameConfig._cameraFOV). Split effects
// parent to JudgeArea/EffectParent (local y=-3.9 on Main). Initialize
// writes SplitEffect scale.y=27 pos.y=-5. Sprite 256px / 100 ppu = 2.56.
// 10392 LineHight is pos.y=2.5, rot z=180.
//
// Preview maps those local-Y endpoints through the same tilt+FOV, then
// NDC y → stage percent. Identity origin sits below NDC −1 (percent > 1);
// do not clamp that floor to the 16:9 edge. Do not substitute sirius_ease
// (note-time curve) or put the cubic on linear screen Y.
inline constexpr float kOfficialSplitFadeInSec = 1.0f;
inline constexpr float kOfficialSplitFadeOutSec = 0.3f;
// SplitLaneRuntime.HideAnimationMilliseconds / Destroy(..., 0.5f).
inline constexpr int64_t kOfficialSplitHideDelayMs = 500;
inline constexpr float kOfficialSplitEffectY = -5.0f;
inline constexpr float kOfficialLineHeightScale = 27.0f;
inline constexpr float kOfficialSplitLineSprite = 2.56f;
inline constexpr float kOfficialZ180LineHightY = 2.5f;

// t01 is fadeIn clip time (clip = 1.0s). LineHight sits one frame behind
// ParticleSystem.time; apply that lag at the play_t call site
// (official_split_fade_scale_at_elapsed / fade_scale_at_play_t), not inside
// this cubic.
//
// X is not on this clock. SplitEffectController.OnFadeIn calls
// ApplyOriginalSplitPositions (SetLocalPositionX table) once; fadeIn_anim
// has m_PositionCurves=[] (only LineHight localScale.y 0→1).
inline float official_split_fade_in_scale(float t01) noexcept {
  const float t = std::clamp(t01, 0.0f, 1.0f);
  return ((0.30408167839050293f * t - 1.6081633567810059f) * t + 2.304081678390503f) * t;
}

// ds/dt of the official fadeIn cubic on t ∈ [0,1]. Outside the clip the
// LineHight scale is held, so the derivative is 0.
// LineHight.localScale.y vs seconds since OnFadeIn (chart clock, not wall).
// OpenWDS: SplitLaneRuntime.ShowAnimationMilliseconds=1000; Tick HasReached
// (start−1s) then SplitLaneAssetRuntime.OnFadeIn → Animator.Play(fadeIn,0,0).
// GameRuntime.ApplyFrameRate(IsLowFrameRate=true) → 60 fps. live dump
// orb-11791-v1 hop: LineHight is one frame behind ParticleSystem.time.
// Closed form of that 60 fps clip, not Time.deltaTime easing of X.
inline float official_split_fade_scale_at_elapsed(float elapsed_sec) noexcept {
  const float anim_t = elapsed_sec - (1.0f / 60.0f);
  if (anim_t <= 0.0f) {
    return 0.0f;
  }
  if (anim_t >= kOfficialSplitFadeInSec) {
    return 1.0f;
  }
  return official_split_fade_in_scale(anim_t);
}

inline float official_split_fade_in_scale_deriv(float t01) noexcept {
  if (t01 <= 0.0f || t01 >= 1.0f) {
    return 0.0f;
  }
  const float t = t01;
  return (0.9122450351715088f * t - 3.2163267135620117f) * t + 2.304081678390503f;
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

// OpenWDS Initialize: JudgeArea(-3.9) + animator LineOffset(-5) = -8.9.
// Perspective FOV50 + Rx60° puts that Main Y below NDC −1 (percent > 1).
// Do not clamp to the 16:9 floor — Unity still simulates those births.
inline float official_split_identity_origin_percent() noexcept {
  return official_main_y_to_percent(official_split_identity_origin_y());
}

inline float official_split_identity_far_percent() noexcept {
  return official_main_y_to_percent(official_split_identity_origin_y() +
                                    official_split_line_world_length());
}

inline float official_split_z180_near_percent() noexcept {
  return official_main_y_to_percent(official_split_z180_origin_y() -
                                    official_split_line_world_length());
}

// SplitLine plate is 256px / 100 ppu. VFX SplitEffect_all SplitLine Height=45
// (OffSet=5) is the bright-head length in those texels; the output shader is
// solid (1,1,1,1) × expr-68 alpha (peak at t=0.05, long tail). After ×27 that
// is 12.15 world units. Intensity must follow expr 68.
// Identity: max(12.15 wu at p0, 45/256 of the ribbon). The 45/256 floor was
// authored for the official / old-editor 720p stage. Scale it by
// content_h / 720 so a smaller Dock does not go unreadably short and a larger
// stage does not keep a 17% screen cap. z=180: 12.15 wu from the mesh tip
// (rotZ=180 texture TOP = judge end, origin 58.6 − 69.12 = −10.52).
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

// Project a Main-Y length at `percent` toward the judgeline into stage percent.
inline float official_split_world_span_at_percent(float percent, float world_len) noexcept {
  const float p = std::clamp(percent, 0.0f, 1.0f);
  const float y = official_percent_to_main_y(p);
  return std::max(official_main_y_to_percent(y - world_len) - p, 1e-4f);
}

// Official PlayerSettings defaultScreen is 1280×720. The 45/256 ribbon floor
// was a constant on that canvas (and on the old editor's fixed pane). Scale
// it by content_h / 720 so a smaller 16:9 stage does not keep a 17% cap.
inline float official_split_tip_floor_scale(float content_height_px) noexcept {
  return std::max(content_height_px, 1.0f) / kOfficialDefaultScreenHeight;
}

inline float official_split_tip_percent_floor(float ribbon, float content_height_px) noexcept {
  return std::max(ribbon, 0.0f) * kOfficialSplitLineSpriteTipFrac *
         official_split_tip_floor_scale(content_height_px);
}

inline float official_split_visible_tip_span(
    float percent_start, float percent_end, bool judge_anchored = false,
    float content_height_px = kOfficialDefaultScreenHeight) noexcept {
  if (!judge_anchored) {
    const float world_span =
        official_split_world_span_at_percent(percent_start, official_split_line_tip_world());
    const float frac_span =
        official_split_tip_percent_floor(percent_end - percent_start, content_height_px);
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
  return official_lane_local_y_to_percent(tip_y);
}

inline float official_split_fade_in_from_judge_start(float t01) noexcept {
  const float tip_y =
      official_split_identity_origin_y() +
      official_split_line_world_length() * official_split_fade_in_scale(t01);
  return official_lane_local_y_to_percent(tip_y);
}

inline float official_split_fade_in_visible(float t01) noexcept {
  return official_split_fade_in_from_tip_end(t01);
}

inline float official_split_fade_out_alpha(float t01) noexcept {
  const float u = std::clamp(t01, 0.0f, 1.0f);
  return 1.0f - u * u * (3.0f - 2.0f * u);
}

// OpenWDS LaneGroup.InitializeSplitLaneVisual / OnSplitLane: BG_LaneBorder a
// starts at 0. ShouldShow (start−1s) restarts DOFade(target, 1s).SetDelay(1s),
// so the fade runs [start, start+1s]; !IsContinued at EndMs restarts
// DOFade(0, 0.5s). DOTween default ease OutQuad: align2-n5 hide matches
// a_E·(1−e/0.5)² within 1e-6 on 17 effects. Hide starts from the value at
// EndMs; DOTween's captured from-value for splits shorter than 1s or
// overlapping splits is 未证实 (callers take the max over live splits).
inline constexpr float kOfficialLaneBorderShowSec = 1.0f;
inline constexpr float kOfficialLaneBorderHideSec = 0.5f;

inline float official_dotween_out_quad(float t01) noexcept {
  const float u = std::clamp(t01, 0.0f, 1.0f);
  return 1.0f - (1.0f - u) * (1.0f - u);
}

inline float official_split_lane_border_alpha(double now_ms, int64_t start_ms, int64_t end_ms,
                                              float target) noexcept {
  auto shown = [&](double t_ms) {
    return target * official_dotween_out_quad(
                        static_cast<float>((t_ms - static_cast<double>(start_ms)) /
                                           (1000.0 * kOfficialLaneBorderShowSec)));
  };
  if (now_ms < static_cast<double>(start_ms)) {
    return 0.0f;
  }
  if (now_ms < static_cast<double>(end_ms)) {
    return shown(now_ms);
  }
  const float from = end_ms > start_ms ? shown(static_cast<double>(end_ms)) : 0.0f;
  const float e = static_cast<float>((now_ms - static_cast<double>(end_ms)) /
                                     (1000.0 * kOfficialLaneBorderHideSec));
  return from * (1.0f - official_dotween_out_quad(e));
}

inline int64_t split_fade_sec_to_ms(float seconds) noexcept {
  return std::max<int64_t>(
      1, static_cast<int64_t>(std::llround(static_cast<double>(seconds) * 1000.0)));
}

inline int64_t official_split_expire_ms(int64_t start_ms, int64_t end_ms) noexcept {
  (void)start_ms;
  return end_ms;
}

// Visible after EndMs until Destroy(0.5s). Clip length is not the hide tail.
inline int64_t official_split_hide_tail_ms(float fade_out_sec = kOfficialSplitFadeOutSec) noexcept {
  (void)fade_out_sec;
  return kOfficialSplitHideDelayMs;
}

inline int64_t official_split_destroy_ms(int64_t start_ms, int64_t end_ms) noexcept {
  return official_split_expire_ms(start_ms, end_ms) + kOfficialSplitHideDelayMs;
}

}  // namespace wds::chart_editor

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace wds::chart_editor {

// Official playfield recovered from GameConfig / LaneGroup / NotePositionCalculator.
// See wds-resources/scripts RecoveredGameConfigValues and RecoveredOriginalGameConfig.

inline constexpr float kOfficialLaneTiltDeg = 60.0f;
inline constexpr float kOfficialLaneGroupZ = 12.0f;
inline constexpr float kOfficialCameraFovDeg = 50.0f;
inline constexpr float kOfficialJudgeAreaY = -3.9f;
inline constexpr float kOfficialNoteWidthPerLane = 0.915f;
inline constexpr float kOfficialLaneBorderWidth = 0.01f;
// GameConfig._noteMarginWidth / _holdNoteLineAdditionalWidth. Applied to
// SpriteRenderer.size.x only — notation / GetNotePositionX stay full width.
inline constexpr float kOfficialNoteMarginWidth = 0.15f;
inline constexpr float kOfficialHoldNoteLineAdditionalWidth = 0.10f;
// ConcurrentLineNote.prefab: Sliced NoteConcurrentLine (12×8 @ 100 ppu,
// m_Border L/R=4 T/B=3), m_Size.y=0.1, local Rx=90°. size.x = GetNoteWidth.
inline constexpr int kOfficialConcurrentLineSpriteWidthPx = 12;
inline constexpr int kOfficialConcurrentLineSpriteHeightPx = 8;
inline constexpr float kOfficialConcurrentLineBorderL = 4.0f;
inline constexpr float kOfficialConcurrentLineBorderR = 4.0f;
inline constexpr float kOfficialConcurrentLineSpriteHeight = 0.1f;
inline constexpr float kOfficialConcurrentLineLocalRotationX = 90.0f;
inline constexpr float kOfficialNoteSpritePpu = 100.0f;
inline constexpr float kOfficialNoteSpriteHeight = 0.64f;
// FlickNoteEntity / NotesArrowsObject (A_ScratchNotes_Arrow 68×112 @ 100 ppu).
// NotesLeft/Right localScale = 0.7; ArrowInterval = 0.35; even widths in each
// pair add +0.01. SetActive count table is width pairs 1-2 .. 11-12.
inline constexpr float kOfficialArrowSpriteWidth = 0.68f;
inline constexpr float kOfficialArrowSpriteHeight = 1.12f;
inline constexpr float kOfficialArrowGroupScale = 0.7f;
inline constexpr float kOfficialArrowInterval = 0.35f;
inline constexpr float kOfficialArrowIntervalEvenExtra = 0.01f;
inline constexpr float kOfficialArrowGroupInset = 0.145f;
inline constexpr float kOfficialArrowAnimLength = 0.5f;

inline int official_scratch_arrow_count(int32_t lane_width, bool is_jump_scratch) noexcept {
  const int w = std::clamp(static_cast<int>(lane_width), 1, 12);
  static constexpr int kFlick[] = {3, 3, 5, 5, 9, 9, 12, 12, 16, 16, 20, 20};
  static constexpr int kJump[] = {6, 6, 11, 11, 18, 18, 26, 26, 34, 34, 42, 42};
  return is_jump_scratch ? kJump[w - 1] : kFlick[w - 1];
}

inline float official_scratch_arrow_interval(int32_t lane_width) noexcept {
  const int w = std::clamp(static_cast<int>(lane_width), 1, 12);
  if (w <= 2) return kOfficialArrowInterval;
  return (w % 2 == 0) ? (kOfficialArrowInterval + kOfficialArrowIntervalEvenExtra)
                      : kOfficialArrowInterval;
}
inline constexpr float kOfficialNoteLocalZBottom = -0.01f;
inline constexpr float kOfficialNoteLocalZTop = -0.1f;
// A_SoundNotes / SoundPurpleNotes: 112×112 @ 100 ppu. SoundNote NotesTop Z=-0.05.
// Prefab m_Size 1.56×0.64 is stale (Simple draw uses native 1.12×1.12).
// SoundNote.prefab Rx is identity. SoundNoteObject.Set only stores the entity
// (libil2cpp Sirius_Game_SoundNoteObject__Set); GetNoteHeight writes Tap only.
inline constexpr float kOfficialSoundNoteSpriteSize = 1.12f;
inline constexpr float kOfficialSoundNoteLocalZ = -0.05f;
inline constexpr float kOfficialNoteStartPositionY = 58.0f;
inline constexpr float kOfficialLaneMaskOffsetY = 1.0f;
inline constexpr float kOfficialLaneMaskSpriteHeight = 0.08f;
// StartLineSprite: local Y=0.4, pivot center. Prefab m_Size.y=1 is overwritten
// by LaneNoteStartLine.Initialize: size.y = sprite.rect.height / ppu.
// img_game_common_start_line_{72..500} @ 100 ppu. Far offset uses the
// tallest plate so the band stays readable after perspective.
inline constexpr float kOfficialStartLineSpriteLocalY = 0.4f;
inline constexpr float kOfficialStartLinePpu = 100.0f;
// Native rect heights (start_line_500 is 498px). Index = offset/10.
inline constexpr float kOfficialStartLinePixelHeights[] = {
    498.0f, 400.0f, 350.0f, 300.0f, 250.0f, 200.0f, 150.0f, 120.0f, 90.0f, 72.0f,
};
inline constexpr int kOfficialStartLinePixelHeightCount = 10;
inline constexpr float kOfficialStartLineSpriteHeight =
    498.0f / kOfficialStartLinePpu;
// BG_Lane / BG_LaneBorder on Main: sliced 11.11×640, center pivot (LaneGroup.prefab).
inline constexpr float kOfficialBgLaneWidth = 11.11f;
inline constexpr float kOfficialBgLaneHeight = 640.0f;
// OpenWDS LaneGroup.ApplySettings: prefab BG_Lane a=0.8 × LaneAlphaValue/100
// (default 80 → 0.64; align2-n5 Unity BG_Lane a=0.64).
inline constexpr float kOfficialBgLanePrefabAlpha = 0.8f;
inline constexpr float kOfficialBgLaneAlpha = kOfficialBgLanePrefabAlpha * 80.0f / 100.0f;
// Texture2D/ingame_bg: 1920×1180. SpriteRendererScreenFitter cover-fits this to the camera.
inline constexpr float kOfficialIngameBgWidth = 1920.0f;
inline constexpr float kOfficialIngameBgHeight = 1180.0f;
inline constexpr float kOfficialIngameBgPpu = 100.0f;
inline constexpr float kOfficialIngameBgWorldWidth = kOfficialIngameBgWidth / kOfficialIngameBgPpu;
inline constexpr float kOfficialIngameBgWorldHeight = kOfficialIngameBgHeight / kOfficialIngameBgPpu;
// OpenWDS GameBackground/Jacket Sprite: 800×800 @ 100 ppu, pivot (0.5, 0.5),
// textureRect = full image. CreateOfflineRhythmPreviewScene.PrepareMusicJacket
// requires one 800×800 Sprite; jacket.bundle TypeTree m_PixelsToUnits=100,
// m_Rect=800×800. World edge = 800/100 = 8.
inline constexpr int kOfficialJacketPixelSize = 800;
inline constexpr float kOfficialJacketPpu = 100.0f;
inline constexpr float kOfficialJacketWorldSize =
    static_cast<float>(kOfficialJacketPixelSize) / kOfficialJacketPpu;
inline constexpr float kOfficialJacketPivotX = 0.5f;
inline constexpr float kOfficialJacketPivotY = 0.5f;
// GameBackground.prefab: parent sample local (0,0,12); child Jacket local
// (0, 2, 0), localScale (0.6, 0.6, 1). sortingOrder Jacket −11, sample/ingame_bg
// −10, BGShadow −9. Default sorting layer; Sprites-Default, color white.
inline constexpr float kOfficialJacketParentLocalZ = 12.0f;
inline constexpr float kOfficialJacketLocalY = 2.0f;
inline constexpr float kOfficialJacketLocalScaleX = 0.6f;
inline constexpr float kOfficialJacketLocalScaleY = 0.6f;
inline constexpr int kOfficialJacketSortingOrder = -11;
inline constexpr int kOfficialIngameBgSortingOrder = -10;
// OpenWDS GameBackground.prefab BGShadow/{TopLeft,TopRight,BottomLeft,BottomRight}.
// Sprite `img_game_common_bg_shadow`: m_Rect / textureRect (0,644,960,540),
// ppu=100, pivot (1,0). RD mesh is 4 verts / 2 tris covering that rect
// (CreateOfflineRhythmPreviewScene + prefab SpriteRenderer).
// SpriteRenderer: color (1,1,1,1), Default, sortingOrder −9, flipX/Y=0,
// drawMode=Simple, size 9.6×5.4, material Sprites-Default.
// Child localPosition (0,0,12), localScale (±1.21875, ±1.33333, 1).
// GameBackgroundShadow.Initialize copies sample.localScale (OpenWDS
// SpriteRendererScreenFitter uniform cover) onto BGShadow; that scale is
// from the origin so x/z and y/z cancel. Screen uses child scale × sprite
// local rect at authored z=12. 禁止用闭源非均匀 Adjust。
inline constexpr float kOfficialBgShadowPixelWidth = 960.0f;
inline constexpr float kOfficialBgShadowPixelHeight = 540.0f;
inline constexpr float kOfficialBgShadowPpu = 100.0f;
inline constexpr float kOfficialBgShadowWidth =
    kOfficialBgShadowPixelWidth / kOfficialBgShadowPpu;
inline constexpr float kOfficialBgShadowHeight =
    kOfficialBgShadowPixelHeight / kOfficialBgShadowPpu;
inline constexpr float kOfficialBgShadowPivotX = 1.0f;
inline constexpr float kOfficialBgShadowPivotY = 0.0f;
inline constexpr float kOfficialBgShadowLocalZ = 12.0f;
inline constexpr float kOfficialBgShadowScaleX = 1.21875f;
inline constexpr float kOfficialBgShadowScaleY = 1.33333f;
inline constexpr float kOfficialBgShadowColorR = 1.0f;
inline constexpr float kOfficialBgShadowColorG = 1.0f;
inline constexpr float kOfficialBgShadowColorB = 1.0f;
inline constexpr float kOfficialBgShadowColorA = 1.0f;
inline constexpr int kOfficialBgShadowSortingOrder = -9;

inline void official_bg_shadow_local_rect(float& left, float& bottom, float& right,
                                          float& top) noexcept {
  left = -kOfficialBgShadowWidth * kOfficialBgShadowPivotX;
  right = kOfficialBgShadowWidth * (1.0f - kOfficialBgShadowPivotX);
  bottom = -kOfficialBgShadowHeight * kOfficialBgShadowPivotY;
  top = kOfficialBgShadowHeight * (1.0f - kOfficialBgShadowPivotY);
}
// img_ingame_judgment_area3: 1119×72 @ 100 ppu, Simple draw (native 11.19×0.72).
// Prefab m_Size.y=0.08 is stale (LaneMask height). Keep native width — the
// outer pink stroke peaks (px 5 / 1113) sit on the 12-lane edges (±5.545).
// Squashing to BG_Lane 11.11 pulls those peaks inward.
inline constexpr int kOfficialJudgeSpritePixelWidth = 1119;
inline constexpr int kOfficialJudgeSpritePixelHeight = 72;
inline constexpr float kOfficialJudgeSpriteHeight = 0.72f;
inline constexpr float kOfficialJudgeSpriteWidth =
    static_cast<float>(kOfficialJudgeSpritePixelWidth) / kOfficialNoteSpritePpu;
inline constexpr float kOfficialMaxNoteVisiblePositionY = 4.45f;
inline constexpr int kOfficialNoteVisibleTimeRate1 = 5000;
inline constexpr int kOfficialNoteVisibleTimeRate2 = 3;
inline constexpr float kOfficialMaxNoteMoveSeconds = 4.6f;
inline constexpr float kOfficialSpeedCorrectValue = 0.6f;
inline constexpr double kOfficialPositionPow3Rate = 0.2;
inline constexpr double kOfficialPositionPow1Rate = 10.0;
inline constexpr float kOfficialCenterLane1Based = 7.0f;
inline constexpr int kOfficialDefaultNoteHeightLevel = 8;
inline constexpr int kOfficialDefaultNoteStartOffset = 0;
inline constexpr int kOfficialDefaultSplitEffectLineOpacity = 100;
// Official GameSettings.LaneAlphaValue default. Border 14+37x/100; fill SetA.
inline constexpr int kOfficialDefaultLaneAlphaValue = 80;
// AppConst.SpritEffectSettingType (official spelling). Persist these integers.
inline constexpr int kOfficialSpritEffectRich = 0;
inline constexpr int kOfficialSpritEffectNormal = 1;
inline constexpr int kOfficialSpritEffectLight = 2;

inline int official_clamp_sprit_effect_setting(int value) noexcept {
  if (value <= kOfficialSpritEffectRich) return kOfficialSpritEffectRich;
  if (value >= kOfficialSpritEffectLight) return kOfficialSpritEffectLight;
  return kOfficialSpritEffectNormal;
}
// RecoveredGameSettings: NoteSpeed 1..25 step 0.1; NoteHeight 1..10;
// NoteStartOffset 0..100 step 5; SplitEffectLineOpacity 10..100 step 10.
inline constexpr double kOfficialMinNoteSpeed = 1.0;
inline constexpr double kOfficialMaxNoteSpeed = 25.0;
inline constexpr double kOfficialNoteSpeedFineStep = 0.1;
inline constexpr int kOfficialMinNoteHeightLevel = 1;
inline constexpr int kOfficialMaxNoteHeightLevel = 10;
inline constexpr int kOfficialMinNoteStartOffset = 0;
inline constexpr int kOfficialMaxNoteStartOffset = 100;
inline constexpr int kOfficialNoteStartOffsetStep = 5;
inline constexpr int kOfficialMinSplitEffectLineOpacity = 10;
inline constexpr int kOfficialMaxSplitEffectLineOpacity = 100;
inline constexpr int kOfficialSplitEffectLineOpacityStep = 10;
// PlayerSettings defaultScreen 1280×720; HUD/result Canvas 1920×1080; Android landscape only.
inline constexpr float kOfficialPreviewAspect = 16.0f / 9.0f;
inline constexpr int kOfficialDefaultScreenWidth = 1280;
inline constexpr int kOfficialDefaultScreenHeight = 720;
// LaneGroup.Initialize: ((opacity * 37 / 100) + 14) / 255. Default opacity 100.
inline constexpr float kOfficialLaneBorderAlphaMin = 14.0f;
inline constexpr float kOfficialLaneBorderAlphaRange = 37.0f;
inline constexpr float kOfficialLaneBorderVisualWidth = 0.045f;
// img_ingame_lane_border2 in atlas 01_Game_New3_01_Common (1115×640 @ 100 ppu,
// 9-slice top/bottom 55). Seven vertical lines → 6 visual tracks. Logical
// lane_count stays 12 (notes / colliders). Fallback geometry uses the 7 edges.
inline constexpr int kOfficialLaneBorderSpriteWidth = 1115;
inline constexpr int kOfficialLaneBorderSpriteHeight = 640;
inline constexpr int kOfficialVisualLaneCount = 6;
inline constexpr int kOfficialLogicalLanesPerVisual = 2;
inline constexpr int kOfficialVisualBorderEdgeCount = kOfficialVisualLaneCount + 1;

inline constexpr float kOfficialNoteHeightRotationX[] = {
    6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -15.0f, -18.0f, -21.0f,
};

inline constexpr float kOfficialNoteVisiblePositionY[] = {
    58.0f,  53.5f, 48.9f, 44.7f, 40.7f, 36.9f, 33.2f,
    29.9f,  26.6f, 23.5f, 20.5f, 17.7f, 15.0f, 12.5f,
    9.9f,   7.5f,  5.1f,  2.9f,  0.5f,  -1.7f, -4.2f,
};

inline constexpr int kOfficialNoteVisiblePositionYCount = 21;

struct OfficialNdc {
  float x = 0.0f;
  float y = 0.0f;
};

inline float official_deg_to_rad(float deg) noexcept {
  return deg * (3.14159265358979323846f / 180.0f);
}

// Unity PhysicsManager default. 1.96.0 splitlane dump has no DynamicsManager
// override; ParticleSystem.gravityModifier multiplies this world vector.
inline constexpr float kUnityPhysicsGravityY = -9.81f;

// World (0, gravityModifier * Physics.gravity.y, 0) in LaneGroup-local axes
// (inverse Rx(kOfficialLaneTiltDeg)). All 121 nonzero catalog gravities are
// Constant + World (moveWithTransform=1). 2020 scalar is 0.
inline void official_gravity_lane_accel(float modifier, float& ay, float& az) noexcept {
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  const float gy = kUnityPhysicsGravityY * modifier;
  ay = gy * std::cos(rad);
  az = -gy * std::sin(rad);
}

inline float official_camera_focal() noexcept {
  return 1.0f / std::tan(official_deg_to_rad(kOfficialCameraFovDeg) * 0.5f);
}

// OpenWDS SpriteRendererScreenFitter.Adjust: uniform cover
// fit = max(visibleW / 19.2, visibleH / 11.8) with
// visibleH = 2 * |dot(sample.pos − camera.pos, camera.forward)| * tan(fov/2)
// and visibleW = visibleH * aspect. sample is at z=12 on the identity Main
// Camera. Jacket world Z stays 12 (child local z=0); XY inherit sample scale.
// 16:9 fit ≈ 1.03655; projected Jacket width is NDC 0.50 (half of the screen).
inline float official_ingame_bg_cover_fit(float aspect) noexcept {
  const float a = std::max(aspect, 1e-4f);
  const float visible_h = 2.0f * kOfficialJacketParentLocalZ / official_camera_focal();
  const float visible_w = visible_h * a;
  return std::max(visible_w / kOfficialIngameBgWorldWidth,
                  visible_h / kOfficialIngameBgWorldHeight);
}

inline bool official_note_speed_valid(double value) noexcept {
  if (!(value >= kOfficialMinNoteSpeed && value <= kOfficialMaxNoteSpeed)) {
    return false;
  }
  const double steps = (value - kOfficialMinNoteSpeed) / kOfficialNoteSpeedFineStep;
  return std::fabs(steps - std::round(steps)) < 0.000001;
}

inline double official_clamp_note_speed(double value) noexcept {
  const double clamped = std::clamp(value, kOfficialMinNoteSpeed, kOfficialMaxNoteSpeed);
  const double steps = std::round((clamped - kOfficialMinNoteSpeed) / kOfficialNoteSpeedFineStep);
  return kOfficialMinNoteSpeed + steps * kOfficialNoteSpeedFineStep;
}

inline bool official_note_height_level_valid(int value) noexcept {
  return value >= kOfficialMinNoteHeightLevel && value <= kOfficialMaxNoteHeightLevel;
}

inline int official_clamp_note_height_level(int value) noexcept {
  return std::clamp(value, kOfficialMinNoteHeightLevel, kOfficialMaxNoteHeightLevel);
}

inline bool official_note_start_offset_valid(int value) noexcept {
  return value >= kOfficialMinNoteStartOffset && value <= kOfficialMaxNoteStartOffset &&
         (value - kOfficialMinNoteStartOffset) % kOfficialNoteStartOffsetStep == 0;
}

inline int official_clamp_note_start_offset(int value) noexcept {
  const int clamped = std::clamp(value, kOfficialMinNoteStartOffset, kOfficialMaxNoteStartOffset);
  const int steps = (clamped + kOfficialNoteStartOffsetStep / 2) / kOfficialNoteStartOffsetStep;
  return std::clamp(steps * kOfficialNoteStartOffsetStep, kOfficialMinNoteStartOffset,
                    kOfficialMaxNoteStartOffset);
}

inline bool official_split_effect_line_opacity_valid(int value) noexcept {
  return value >= kOfficialMinSplitEffectLineOpacity &&
         value <= kOfficialMaxSplitEffectLineOpacity &&
         (value - kOfficialMinSplitEffectLineOpacity) % kOfficialSplitEffectLineOpacityStep == 0;
}

inline int official_clamp_split_effect_line_opacity(int value) noexcept {
  const int clamped =
      std::clamp(value, kOfficialMinSplitEffectLineOpacity, kOfficialMaxSplitEffectLineOpacity);
  const int rel = clamped - kOfficialMinSplitEffectLineOpacity;
  const int steps =
      (rel + kOfficialSplitEffectLineOpacityStep / 2) / kOfficialSplitEffectLineOpacityStep;
  return kOfficialMinSplitEffectLineOpacity + steps * kOfficialSplitEffectLineOpacityStep;
}

inline float official_note_height_rotation_x(int note_height_level) noexcept {
  const int i = std::clamp(note_height_level, 1, 10) - 1;
  return kOfficialNoteHeightRotationX[i];
}

inline float official_note_visible_position_y(int note_start_offset) noexcept {
  const int index = std::clamp(note_start_offset / 5, 0, 20);
  return kOfficialNoteVisiblePositionY[index];
}

inline float official_start_line_sprite_height(
    int note_start_offset = kOfficialDefaultNoteStartOffset) noexcept {
  const int index = std::clamp(note_start_offset / 10, 0, kOfficialStartLinePixelHeightCount - 1);
  return kOfficialStartLinePixelHeights[index] / kOfficialStartLinePpu;
}

inline float official_lane_mask_scale_y(int note_start_offset) noexcept {
  return (kOfficialNoteStartPositionY + kOfficialLaneMaskOffsetY -
          official_note_visible_position_y(note_start_offset)) /
         kOfficialLaneMaskSpriteHeight;
}

inline float official_speed_rate(double note_speed) noexcept {
  return static_cast<float>(note_speed) * kOfficialSpeedCorrectValue;
}

inline float official_move_seconds(double note_speed) noexcept {
  const double speed = std::max(note_speed, 1e-6);
  return static_cast<float>(
      (static_cast<double>(kOfficialNoteVisibleTimeRate1) *
       static_cast<double>(kOfficialMaxNoteVisiblePositionY) /
       static_cast<double>(kOfficialNoteVisibleTimeRate2) / speed) /
      1000.0);
}

inline float official_note_local_y(int64_t target_ms, int64_t passed_ms, double note_speed,
                                   double offset_value = 0.0,
                                   double pow3 = kOfficialPositionPow3Rate,
                                   double pow1 = kOfficialPositionPow1Rate) noexcept {
  const float speed_rate = official_speed_rate(note_speed);
  const float time = speed_rate * static_cast<float>(target_ms - passed_ms) / 1000.0f;
  return static_cast<float>(offset_value + pow3 * static_cast<double>(time) *
                                               static_cast<double>(time) *
                                               static_cast<double>(time) +
                            pow1 * static_cast<double>(time));
}

inline float official_note_width(int32_t lane_count,
                                 float note_width_per_lane = kOfficialNoteWidthPerLane,
                                 float lane_border_width = kOfficialLaneBorderWidth) noexcept {
  const int32_t n = std::max(1, lane_count);
  return static_cast<float>(n) * note_width_per_lane +
         static_cast<float>(n - 1) * lane_border_width;
}

inline float official_tap_visual_width(float notation_width) noexcept {
  return notation_width - kOfficialNoteMarginWidth;
}

inline float official_hold_line_visual_width(float notation_width) noexcept {
  return notation_width - kOfficialNoteMarginWidth + kOfficialHoldNoteLineAdditionalWidth;
}

// Concurrent line: full notation width (no tap margin), so it peeks past note sides.
inline float official_concurrent_line_visual_width(float notation_width) noexcept {
  return notation_width;
}

// Unity SpriteRenderer Sliced: corner world size = border_px / PPU (constant).
inline float official_sliced_cap_world(float border_px,
                                       float ppu = kOfficialNoteSpritePpu) noexcept {
  return border_px / std::max(ppu, 1e-6f);
}

inline float official_sliced_cap_fraction(float border_px, float dest_world_width,
                                          float ppu = kOfficialNoteSpritePpu) noexcept {
  return official_sliced_cap_world(border_px, ppu) / std::max(dest_world_width, 1e-6f);
}

// Official 1-based laneNumber. Center of a note that starts at that lane.
inline float official_note_position_x(int32_t lane_number_1based, float note_width,
                                      float note_width_per_lane = kOfficialNoteWidthPerLane,
                                      float lane_border_width = kOfficialLaneBorderWidth) noexcept {
  return note_width * 0.5f +
         (static_cast<float>(lane_number_1based) - kOfficialCenterLane1Based) *
             (note_width_per_lane + lane_border_width) +
         lane_border_width * 0.5f;
}

// ScratchHoldNoteObject.SetJumpScratch: dest = GetNotePositionX(Lane, jumpW),
// start = GetNotePositionX(Lane, bodyW); negative GimmickValue flips the sign.
// Lane is 0-based here (official 1-based = lane + 1).
inline float official_jump_scratch_end_offset_x(int32_t lane_0based, int32_t body_width,
                                                int32_t gimmick_value) noexcept {
  const int32_t jump_lanes = std::max(1, std::abs(gimmick_value));
  const float body_w = official_note_width(std::max(1, body_width));
  const float jump_w = official_note_width(jump_lanes);
  const int32_t lane1 = std::max(1, lane_0based + 1);
  const float offset =
      official_note_position_x(lane1, jump_w) - official_note_position_x(lane1, body_w);
  return gimmick_value < 0 ? -offset : offset;
}

inline float official_lane_center_x(int32_t lane_0based) noexcept {
  const int32_t lane1 = std::max(1, lane_0based + 1);
  return official_note_position_x(lane1, kOfficialNoteWidthPerLane);
}

inline float official_span_width(int32_t lane_0based, int32_t end_lane_0based) noexcept {
  const int32_t lo = std::min(lane_0based, end_lane_0based);
  const int32_t hi = std::max(lane_0based, end_lane_0based);
  return official_note_width(hi - lo + 1);
}

inline float official_span_center_x(int32_t lane_0based, int32_t end_lane_0based) noexcept {
  const int32_t lo = std::min(lane_0based, end_lane_0based);
  const float width = official_span_width(lane_0based, end_lane_0based);
  return official_note_position_x(lo + 1, width);
}

inline float official_span_left_x(int32_t lane_0based, int32_t end_lane_0based) noexcept {
  return official_span_center_x(lane_0based, end_lane_0based) -
         official_span_width(lane_0based, end_lane_0based) * 0.5f;
}

inline float official_span_right_x(int32_t lane_0based, int32_t end_lane_0based) noexcept {
  return official_span_center_x(lane_0based, end_lane_0based) +
         official_span_width(lane_0based, end_lane_0based) * 0.5f;
}

inline float official_lane_left_x(int32_t lane_0based) noexcept {
  return official_span_left_x(lane_0based, lane_0based);
}

inline float official_lane_right_x(int32_t lane_0based) noexcept {
  return official_span_right_x(lane_0based, lane_0based);
}

// Camera-space Z after LaneGroup Rx(60°) + (0,0,12). Unity clips z <= near;
// we must not project those with max(z, 1e-4) or NDC X explodes across the stage.
inline float official_main_camera_z(float local_y, float local_z) noexcept {
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  return kOfficialLaneGroupZ + local_y * std::sin(rad) + local_z * std::cos(rad);
}

// OpenWDS OfflineRhythmPreview.unity Main Camera `near clip plane: 0.1`
// (CreateOfflineRhythmPreviewScene.cs writes the same). N6 runtime dump
// docs/split-effect-parity/results/align2-n6-unity-v1/dump/camera_volumes.json
// `"near":0.1`. Runtime C# never assigns Camera.nearClipPlane (LiveParticleDump
// only reads it). Menu scenes keep Unity's default 0.3 — not the playfield.
inline constexpr float kOfficialCameraNearClip = 0.1f;
// OpenWDS OfflineRhythmPreview.unity Main Camera (clearFlags=2 Solid Color):
// `m_BackGroundColor: {r: 0.015, g: 0.02, b: 0.04, a: 1}`. Same literals in
// CreateOfflineRhythmPreviewScene.CreateCamera. 2022.3 Camera.backgroundColor
// is the solid clear when clearFlags is SolidColor
// (docs.unity3d.com/2022.3/Documentation/ScriptReference/Camera-backgroundColor.html).
// ingame_bg starfield texels are mostly α=0; the far lane shows this clear
// through BG_Lane a=0.64. Menu scenes use other colors — not the playfield.
inline constexpr float kOfficialCameraBackgroundR = 0.015f;
inline constexpr float kOfficialCameraBackgroundG = 0.02f;
inline constexpr float kOfficialCameraBackgroundB = 0.04f;

inline bool official_main_in_front_of_camera(float local_y, float local_z) noexcept {
  return official_main_camera_z(local_y, local_z) > kOfficialCameraNearClip;
}

// Project Main/LaneGroup-local (x, y, z) through Rx(60°) + (0,0,12) + FOV50.
// aspect is content width/height (Unity vertical FOV: X is divided by aspect).
inline OfficialNdc project_main_xyz(float local_x, float local_y, float local_z,
                                    float aspect = kOfficialPreviewAspect) noexcept {
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float wy = local_y * c - local_z * s;
  const float wz = official_main_camera_z(local_y, local_z);
  const float f = official_camera_focal();
  const float z = std::max(wz, 1e-4f);
  const float a = std::max(aspect, 1e-4f);
  OfficialNdc ndc;
  ndc.x = (local_x / z) * f / a;
  ndc.y = (wy / z) * f;
  return ndc;
}

// Unity Camera clips the ParticleSystem ribbon mesh at nearClipPlane (0.1).
// camera_z is linear in Main y/z, so the intersection stays on the 3D segment
// (same world X as the split). Rewriting z to 1e-4 is not Unity and explodes
// NDC off that ray. Writes two visible endpoints and their t in [0,1].
inline bool official_main_clip_segment_near(float x0, float y0, float z0, float x1, float y1,
                                            float z1, float& ox0, float& oy0, float& oz0,
                                            float& ox1, float& oy1, float& oz1, float& t0,
                                            float& t1) noexcept {
  const float c0 = official_main_camera_z(y0, z0);
  const float c1 = official_main_camera_z(y1, z1);
  const float n = kOfficialCameraNearClip;
  const bool f0 = c0 > n;
  const bool f1 = c1 > n;
  if (!f0 && !f1) {
    return false;
  }
  ox0 = x0;
  oy0 = y0;
  oz0 = z0;
  ox1 = x1;
  oy1 = y1;
  oz1 = z1;
  t0 = 0.0f;
  t1 = 1.0f;
  if (f0 && f1) {
    return true;
  }
  const float den = c1 - c0;
  if (std::fabs(den) < 1e-8f) {
    return false;
  }
  const float t = (n - c0) / den;
  const float ix = x0 + t * (x1 - x0);
  const float iy = y0 + t * (y1 - y0);
  const float iz = z0 + t * (z1 - z0);
  if (f0) {
    ox1 = ix;
    oy1 = iy;
    oz1 = iz;
    t1 = t;
  } else {
    ox0 = ix;
    oy0 = iy;
    oz0 = iz;
    t0 = t;
  }
  return true;
}

// Unity ParticleSystemRenderer trail mesh: GPU clips triangles at near and the
// four side planes (−w ≤ clip.xy ≤ w). Software projection must clip those
// mesh vertices before the perspective divide.
inline constexpr int kOfficialViewClipMaxVerts = 16;

inline void official_main_camera_lane_pos(float& x, float& y, float& z) noexcept {
  x = 0.0f;
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  y = -kOfficialLaneGroupZ * std::sin(rad);
  z = -kOfficialLaneGroupZ * std::cos(rad);
}

// Camera +Z after LaneGroup Rx(60°). Unity ParticleSystemRenderSpace.View
// (catalog default 0) faces this plane, not the eye ray to each vertex.
inline void official_main_camera_forward(float& fx, float& fy, float& fz) noexcept {
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  fx = 0.0f;
  fy = std::sin(rad);
  fz = std::cos(rad);
}

inline void official_main_clip_coord(float x, float y, float z, float aspect, float& cx, float& cy,
                                     float& cw) noexcept {
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  const float f = official_camera_focal();
  const float a = std::max(aspect, 1e-4f);
  cw = official_main_camera_z(y, z);
  cx = x * (f / a);
  cy = (y * std::cos(rad) - z * std::sin(rad)) * f;
}

// Camera-facing trail offset in LaneGroup. Unity Trails width is world units
// perpendicular to the path and the camera plane (View): Cross(forward, tangent).
// Facing (vertex − camera) is ParticleSystemRenderSpace.Facing, not the default.
inline void official_main_trail_perp(float px, float py, float pz, float dx, float dy, float dz,
                                     float& ox, float& oy, float& oz) noexcept {
  (void)px;
  (void)py;
  (void)pz;
  float fx = 0.0f, fy = 0.0f, fz = 0.0f;
  official_main_camera_forward(fx, fy, fz);
  ox = fy * dz - fz * dy;
  oy = fz * dx - fx * dz;
  oz = fx * dy - fy * dx;
  const float len = std::sqrt(ox * ox + oy * oy + oz * oz);
  if (len < 1e-8f) {
    ox = 1.0f;
    oy = 0.0f;
    oz = 0.0f;
    return;
  }
  ox /= len;
  oy /= len;
  oz /= len;
}

inline int official_main_clip_poly_view(const float* ix, const float* iy, const float* iz, int n,
                                        float aspect, float* ox, float* oy, float* oz) noexcept {
  if (n < 3 || n > kOfficialViewClipMaxVerts) {
    return 0;
  }
  float ax[kOfficialViewClipMaxVerts];
  float ay[kOfficialViewClipMaxVerts];
  float az[kOfficialViewClipMaxVerts];
  float bx[kOfficialViewClipMaxVerts];
  float by[kOfficialViewClipMaxVerts];
  float bz[kOfficialViewClipMaxVerts];
  for (int i = 0; i < n; ++i) {
    ax[i] = ix[i];
    ay[i] = iy[i];
    az[i] = iz[i];
  }
  int an = n;
  auto plane_val = [&](float x, float y, float z, int plane) {
    float cx = 0.0f, cy = 0.0f, cw = 0.0f;
    official_main_clip_coord(x, y, z, aspect, cx, cy, cw);
    switch (plane) {
      case 0:
        return cw - kOfficialCameraNearClip;
      case 1:
        return cw + cx;
      case 2:
        return cw - cx;
      case 3:
        return cw + cy;
      default:
        return cw - cy;
    }
  };
  for (int plane = 0; plane < 5; ++plane) {
    int bn = 0;
    if (an < 3) {
      return 0;
    }
    for (int i = 0; i < an; ++i) {
      const int j = (i + an - 1) % an;
      const float v0 = plane_val(ax[j], ay[j], az[j], plane);
      const float v1 = plane_val(ax[i], ay[i], az[i], plane);
      const bool in0 = v0 >= 0.0f;
      const bool in1 = v1 >= 0.0f;
      if (in0 != in1) {
        const float den = v0 - v1;
        const float t = std::fabs(den) < 1e-8f ? 0.0f : v0 / den;
        if (bn < kOfficialViewClipMaxVerts) {
          bx[bn] = ax[j] + (ax[i] - ax[j]) * t;
          by[bn] = ay[j] + (ay[i] - ay[j]) * t;
          bz[bn] = az[j] + (az[i] - az[j]) * t;
          ++bn;
        }
      }
      if (in1 && bn < kOfficialViewClipMaxVerts) {
        bx[bn] = ax[i];
        by[bn] = ay[i];
        bz[bn] = az[i];
        ++bn;
      }
    }
    for (int i = 0; i < bn; ++i) {
      ax[i] = bx[i];
      ay[i] = by[i];
      az[i] = bz[i];
    }
    an = bn;
  }
  for (int i = 0; i < an; ++i) {
    ox[i] = ax[i];
    oy[i] = ay[i];
    oz[i] = az[i];
  }
  return an;
}

// Same five planes as official_main_clip_poly_view. Unity GPU clip interpolates
// vertex attributes; Stretch Billboard UVs must ride the cut, not be rewritten
// to 0–1 of the leftover polygon (that puts the sprite's transparent edge on
// the near / bottom plane).
inline int official_main_clip_poly_view_uv(const float* ix, const float* iy, const float* iz,
                                           const float* iu, const float* iv, int n, float aspect,
                                           float* ox, float* oy, float* oz, float* ou,
                                           float* ov) noexcept {
  if (n < 3 || n > kOfficialViewClipMaxVerts || iu == nullptr || iv == nullptr) {
    return 0;
  }
  float ax[kOfficialViewClipMaxVerts];
  float ay[kOfficialViewClipMaxVerts];
  float az[kOfficialViewClipMaxVerts];
  float au[kOfficialViewClipMaxVerts];
  float av[kOfficialViewClipMaxVerts];
  float bx[kOfficialViewClipMaxVerts];
  float by[kOfficialViewClipMaxVerts];
  float bz[kOfficialViewClipMaxVerts];
  float bu[kOfficialViewClipMaxVerts];
  float bv[kOfficialViewClipMaxVerts];
  for (int i = 0; i < n; ++i) {
    ax[i] = ix[i];
    ay[i] = iy[i];
    az[i] = iz[i];
    au[i] = iu[i];
    av[i] = iv[i];
  }
  int an = n;
  auto plane_val = [&](float x, float y, float z, int plane) {
    float cx = 0.0f, cy = 0.0f, cw = 0.0f;
    official_main_clip_coord(x, y, z, aspect, cx, cy, cw);
    switch (plane) {
      case 0:
        return cw - kOfficialCameraNearClip;
      case 1:
        return cw + cx;
      case 2:
        return cw - cx;
      case 3:
        return cw + cy;
      default:
        return cw - cy;
    }
  };
  for (int plane = 0; plane < 5; ++plane) {
    int bn = 0;
    if (an < 3) {
      return 0;
    }
    for (int i = 0; i < an; ++i) {
      const int j = (i + an - 1) % an;
      const float v0 = plane_val(ax[j], ay[j], az[j], plane);
      const float v1 = plane_val(ax[i], ay[i], az[i], plane);
      const bool in0 = v0 >= 0.0f;
      const bool in1 = v1 >= 0.0f;
      if (in0 != in1) {
        const float den = v0 - v1;
        const float t = std::fabs(den) < 1e-8f ? 0.0f : v0 / den;
        if (bn < kOfficialViewClipMaxVerts) {
          bx[bn] = ax[j] + (ax[i] - ax[j]) * t;
          by[bn] = ay[j] + (ay[i] - ay[j]) * t;
          bz[bn] = az[j] + (az[i] - az[j]) * t;
          bu[bn] = au[j] + (au[i] - au[j]) * t;
          bv[bn] = av[j] + (av[i] - av[j]) * t;
          ++bn;
        }
      }
      if (in1 && bn < kOfficialViewClipMaxVerts) {
        bx[bn] = ax[i];
        by[bn] = ay[i];
        bz[bn] = az[i];
        bu[bn] = au[i];
        bv[bn] = av[i];
        ++bn;
      }
    }
    for (int i = 0; i < bn; ++i) {
      ax[i] = bx[i];
      ay[i] = by[i];
      az[i] = bz[i];
      au[i] = bu[i];
      av[i] = bv[i];
    }
    an = bn;
  }
  for (int i = 0; i < an; ++i) {
    ox[i] = ax[i];
    oy[i] = ay[i];
    oz[i] = az[i];
    ou[i] = au[i];
    ov[i] = av[i];
  }
  return an;
}

// View-space clip against Camera.nearClipPlane only, then the caller projects.
// Preview is a 2D overlay: there is no 3D GPU clip, so a behind-camera corner
// projected with max(z, 1e-4) explodes NDC. Unity rasterizes the leftover
// after homogeneous near clip. Plane value is OpenWDS playfield 0.1 (scene +
// N6 dump), not Unity's 0.3 default.
// Side planes are the 2D viewport after projection — not a new size measure.
// CPU Sutherland–Hodgman here is hop (preview is not a 3D GPU); 禁止标 2022.3.
inline int official_main_clip_poly_near_uv(const float* ix, const float* iy, const float* iz,
                                           const float* iu, const float* iv, int n, float* ox,
                                           float* oy, float* oz, float* ou, float* ov) noexcept {
  if (n < 3 || n > kOfficialViewClipMaxVerts || iu == nullptr || iv == nullptr || ox == nullptr ||
      oy == nullptr || oz == nullptr || ou == nullptr || ov == nullptr) {
    return 0;
  }
  const float near_z = kOfficialCameraNearClip;
  int behind = 0;
  int in_front = 0;
  for (int i = 0; i < n; ++i) {
    if (official_main_camera_z(iy[i], iz[i]) > near_z) {
      ++in_front;
    } else {
      ++behind;
    }
  }
  if (in_front == 0) {
    return 0;
  }
  if (behind == 0) {
    for (int i = 0; i < n; ++i) {
      ox[i] = ix[i];
      oy[i] = iy[i];
      oz[i] = iz[i];
      ou[i] = iu[i];
      ov[i] = iv[i];
    }
    return n;
  }
  float ax[kOfficialViewClipMaxVerts];
  float ay[kOfficialViewClipMaxVerts];
  float az[kOfficialViewClipMaxVerts];
  float au[kOfficialViewClipMaxVerts];
  float av[kOfficialViewClipMaxVerts];
  float bx[kOfficialViewClipMaxVerts];
  float by[kOfficialViewClipMaxVerts];
  float bz[kOfficialViewClipMaxVerts];
  float bu[kOfficialViewClipMaxVerts];
  float bv[kOfficialViewClipMaxVerts];
  for (int i = 0; i < n; ++i) {
    ax[i] = ix[i];
    ay[i] = iy[i];
    az[i] = iz[i];
    au[i] = iu[i];
    av[i] = iv[i];
  }
  int bn = 0;
  auto plane_val = [&](float y, float z) { return official_main_camera_z(y, z) - near_z; };
  for (int i = 0; i < n; ++i) {
    const int j = (i + n - 1) % n;
    const float v0 = plane_val(ay[j], az[j]);
    const float v1 = plane_val(ay[i], az[i]);
    const bool in0 = v0 >= 0.0f;
    const bool in1 = v1 >= 0.0f;
    if (in0 != in1) {
      const float den = v0 - v1;
      const float t = std::fabs(den) < 1e-8f ? 0.0f : v0 / den;
      if (bn < kOfficialViewClipMaxVerts) {
        bx[bn] = ax[j] + (ax[i] - ax[j]) * t;
        by[bn] = ay[j] + (ay[i] - ay[j]) * t;
        bz[bn] = az[j] + (az[i] - az[j]) * t;
        bu[bn] = au[j] + (au[i] - au[j]) * t;
        bv[bn] = av[j] + (av[i] - av[j]) * t;
        ++bn;
      }
    }
    if (in1 && bn < kOfficialViewClipMaxVerts) {
      bx[bn] = ax[i];
      by[bn] = ay[i];
      bz[bn] = az[i];
      bu[bn] = au[i];
      bv[bn] = av[i];
      ++bn;
    }
  }
  if (bn < 3) {
    return 0;
  }
  for (int i = 0; i < bn; ++i) {
    ox[i] = bx[i];
    oy[i] = by[i];
    oz[i] = bz[i];
    ou[i] = bu[i];
    ov[i] = bv[i];
  }
  return bn;
}

inline OfficialNdc project_judge_xyz(float local_x, float local_y, float local_z,
                                     float aspect = kOfficialPreviewAspect) noexcept {
  return project_main_xyz(local_x, local_y + kOfficialJudgeAreaY, local_z, aspect);
}

inline float official_ndc_y_to_percent(float ndc_y) noexcept {
  return (1.0f - ndc_y) * 0.5f;
}

inline float official_percent_to_ndc_y(float percent) noexcept {
  return 1.0f - 2.0f * percent;
}

inline float official_main_y_to_percent(float main_y) noexcept {
  return official_ndc_y_to_percent(project_main_xyz(0.0f, main_y, 0.0f).y);
}

inline float official_judge_y_to_percent(float judge_y) noexcept {
  return official_ndc_y_to_percent(project_judge_xyz(0.0f, judge_y, 0.0f).y);
}

// Inverse of project_main_xyz(0, y, 0).y for points on the LaneGroup plane.
inline float official_percent_to_main_y(float percent) noexcept {
  const float ndc = official_percent_to_ndc_y(percent);
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float f = official_camera_focal();
  const float denom = f * c - ndc * s;
  if (std::fabs(denom) < 1e-6f) {
    return 0.0f;
  }
  return (kOfficialLaneGroupZ * ndc) / denom;
}

inline float official_percent_to_judge_y(float percent) noexcept {
  return official_percent_to_main_y(percent) - kOfficialJudgeAreaY;
}

inline OfficialNdc project_note_layer(float lane_x, float note_y, float sprite_x, float sprite_y,
                                      float layer_z, float tilt_deg, float aspect) noexcept {
  const float rad = official_deg_to_rad(tilt_deg);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float y = sprite_y * c - layer_z * s;
  const float z = sprite_y * s + layer_z * c;
  return project_judge_xyz(lane_x + sprite_x, note_y + y, z, aspect);
}

// NDC height of a note-layer sprite at the judgeline (same path as preview).
inline float official_preview_sprite_ndc_height(float sprite_half, float layer_z, float tilt_deg,
                                                float aspect = kOfficialPreviewAspect) noexcept {
  const OfficialNdc top =
      project_note_layer(0.0f, 0.0f, 0.0f, sprite_half, layer_z, tilt_deg, aspect);
  const OfficialNdc bot =
      project_note_layer(0.0f, 0.0f, 0.0f, -sprite_half, layer_z, tilt_deg, aspect);
  return std::fabs(top.y - bot.y);
}

inline float official_preview_note_ndc_height(int note_height_level) noexcept {
  return official_preview_sprite_ndc_height(kOfficialNoteSpriteHeight * 0.5f, 0.0f,
                                            official_note_height_rotation_x(note_height_level));
}

inline float official_preview_sound_note_ndc_height() noexcept {
  return official_preview_sprite_ndc_height(kOfficialSoundNoteSpriteSize * 0.5f,
                                            kOfficialSoundNoteLocalZ, 0.0f);
}

// content_h is the preview stage height in pixels (NDC ±1 spans that height).
inline float official_preview_note_height_px(int note_height_level, float content_h) noexcept {
  return official_preview_note_ndc_height(note_height_level) * 0.5f * std::max(content_h, 1.0f);
}

inline float official_preview_sound_note_height_px(float content_h) noexcept {
  return official_preview_sound_note_ndc_height() * 0.5f * std::max(content_h, 1.0f);
}

// Judgeline screen-width ratio: SoundNote sprite vs one logical lane.
// Same projection as preview star_quad / lane_width. Aspect cancels; the
// sprite's local Z (-0.05) makes it slightly wider than 1.12 / 0.915.
inline float official_preview_sound_note_to_lane_width_ratio() noexcept {
  constexpr float kAspect = kOfficialPreviewAspect;
  const float half = kOfficialSoundNoteSpriteSize * 0.5f;
  const OfficialNdc star_l =
      project_note_layer(0.0f, 0.0f, -half, 0.0f, kOfficialSoundNoteLocalZ, 0.0f, kAspect);
  const OfficialNdc star_r =
      project_note_layer(0.0f, 0.0f, half, 0.0f, kOfficialSoundNoteLocalZ, 0.0f, kAspect);
  const float lane = official_note_width(1);
  const OfficialNdc lane_l = project_judge_xyz(-lane * 0.5f, 0.0f, 0.0f, kAspect);
  const OfficialNdc lane_r = project_judge_xyz(lane * 0.5f, 0.0f, 0.0f, kAspect);
  const float star_w = std::fabs(star_r.x - star_l.x);
  const float lane_w = std::fabs(lane_r.x - lane_l.x);
  return star_w / std::max(lane_w, 1e-6f);
}

inline float official_hidden_line_center_y(
    int note_start_offset = kOfficialDefaultNoteStartOffset) noexcept {
  // LaneGroup.InitializeLaneStart writes NoteStartLine.localY =
  // GetNoteVisiblePositionY(offset). StartLineSprite sits +0.4 on that node
  // (center pivot), so the visible plate is at visibleY + 0.4.
  return official_note_visible_position_y(note_start_offset) + kOfficialStartLineSpriteLocalY;
}

inline float official_hidden_line_center_percent(
    int note_start_offset = kOfficialDefaultNoteStartOffset) noexcept {
  return official_main_y_to_percent(official_hidden_line_center_y(note_start_offset));
}

// LaneMask pivot is top (0.5, 1). ScaleY stretches it so the bottom edge sits
// at GetNoteVisiblePositionY — not the StartLine sprite center (+0.4) and not
// the plate's judgeline-side half. Notes use VisibleOutsideMask against this.
inline float official_lane_mask_bottom_y(
    int note_start_offset = kOfficialDefaultNoteStartOffset) noexcept {
  return official_note_visible_position_y(note_start_offset);
}

inline float official_lane_mask_bottom_percent(
    int note_start_offset = kOfficialDefaultNoteStartOffset) noexcept {
  return official_main_y_to_percent(official_lane_mask_bottom_y(note_start_offset));
}

inline float official_judgeline_percent() noexcept {
  return official_judge_y_to_percent(0.0f);
}

inline float official_lane_border_alpha(int /*split_line_opacity*/ = 100) noexcept {
  const int o = kOfficialDefaultLaneAlphaValue;
  return (kOfficialLaneBorderAlphaMin +
          kOfficialLaneBorderAlphaRange * static_cast<float>(o) / 100.0f) /
         255.0f;
}

inline float official_lane_edge_x(int32_t edge_index, int32_t lane_count = 12) noexcept {
  const int32_t n = std::max(1, lane_count);
  const int32_t e = std::clamp(edge_index, 0, n);
  if (e >= n) {
    return official_lane_right_x(n - 1);
  }
  return official_lane_left_x(e);
}

// Logical edge index of the i-th visual-track border (0, 2, 4, …, 12).
inline int32_t official_visual_border_edge_index(int32_t visual_edge,
                                                int32_t lane_count = 12) noexcept {
  const int32_t n = std::max(1, lane_count);
  const int32_t visual = std::max(1, n / kOfficialLogicalLanesPerVisual);
  const int32_t e = std::clamp(visual_edge, 0, visual);
  return e * (n / visual);
}

}  // namespace wds::chart_editor

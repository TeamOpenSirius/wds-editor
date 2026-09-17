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
inline constexpr float kOfficialBgLaneAlpha = 0.8f;
// Texture2D/ingame_bg: 1920×1180. SpriteRendererScreenFitter cover-fits this to the camera.
inline constexpr float kOfficialIngameBgWidth = 1920.0f;
inline constexpr float kOfficialIngameBgHeight = 1180.0f;
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

inline float official_camera_focal() noexcept {
  return 1.0f / std::tan(official_deg_to_rad(kOfficialCameraFovDeg) * 0.5f);
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

// Project Main/LaneGroup-local (x, y, z) through Rx(60°) + (0,0,12) + FOV50.
// aspect is content width/height (Unity vertical FOV: X is divided by aspect).
inline OfficialNdc project_main_xyz(float local_x, float local_y, float local_z,
                                    float aspect = kOfficialPreviewAspect) noexcept {
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float wy = local_y * c - local_z * s;
  const float wz = kOfficialLaneGroupZ + local_y * s + local_z * c;
  const float f = official_camera_focal();
  const float z = std::max(wz, 1e-4f);
  const float a = std::max(aspect, 1e-4f);
  OfficialNdc ndc;
  ndc.x = (local_x / z) * f / a;
  ndc.y = (wy / z) * f;
  return ndc;
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

inline float official_lane_border_alpha(int split_line_opacity = 100) noexcept {
  const int o = std::clamp(split_line_opacity, 0, 100);
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

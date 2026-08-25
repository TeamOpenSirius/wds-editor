#pragma once

#include "types.hpp"

#include <algorithm>
#include <cmath>

namespace wds::interaction::theme {

// Material Design 3 dark surface tokens (simplified).
inline constexpr Color kBackground{0.07f, 0.07f, 0.09f, 1.0f};
inline constexpr Color kSurface{0.11f, 0.11f, 0.14f, 1.0f};
inline constexpr Color kSurfaceVariant{0.16f, 0.16f, 0.20f, 1.0f};
inline constexpr Color kPrimary{0.67f, 0.55f, 0.98f, 1.0f};
inline constexpr Color kOnPrimary{0.10f, 0.06f, 0.18f, 1.0f};
inline constexpr Color kOnSurface{0.90f, 0.89f, 0.93f, 1.0f};
inline constexpr Color kOnSurfaceMuted{0.70f, 0.69f, 0.74f, 1.0f};
inline constexpr Color kOutline{0.38f, 0.38f, 0.42f, 1.0f};
inline constexpr Color kError{0.96f, 0.45f, 0.45f, 1.0f};
inline constexpr Color kWarning{0.96f, 0.78f, 0.35f, 1.0f};
inline constexpr Color kHoverOverlay{1.0f, 1.0f, 1.0f, 0.08f};
inline constexpr Color kPressedOverlay{1.0f, 1.0f, 1.0f, 0.14f};

inline constexpr float kPressedScale = 0.97f;

// --- Logical-pixel metrics (1× design / GLFW window coordinates) --------------
// Layout, hit-testing, and UiPainter commands use these values directly.
// Framebuffer scaling happens only at flush / Vulkan boundaries via
// ui_content_scale() (= fb_px / window_px) / to_fb().
// On Windows/X11 this ratio is 1; on macOS Retina it is typically 2.
// Do not feed OS DPI % (glfwGetWindowContentScale) into apply_content_scale.

inline constexpr float kCornerRadiusSm = 2.0f;
inline constexpr float kCornerRadiusMd = 4.0f;
inline constexpr float kCornerRadiusLg = 6.0f;

// Body UI text. Icon-button tips and edit-gutter tags stay smaller.
inline constexpr float kFontSizeMd = 26.0f;
inline constexpr float kFontSizeSm = 26.0f;
inline constexpr float kFontSizeLg = 26.0f;
inline constexpr float kFontSizeLabelMin = 26.0f;
inline constexpr float kFontSizeLabelMax = 26.0f;
inline constexpr float kFontSizeTooltip = 13.0f;
inline constexpr float kFontSizeGutter = 22.0f;
// Shared tip size for every icon button of a given cell. ~25% of the host
// (13px in a 52px cell) so long CJK tips wrap at one size instead of shrinking
// per string. Larger cells (fullscreen / higher window resolution at
// content-scale 1) still scale the shared size with the host.
inline constexpr float kTooltipDesignHostPx = 52.0f;

inline float tooltip_px_for_host(float host_min_side) noexcept {
  const float host = std::max(1.0f, host_min_side);
  return std::max(kFontSizeTooltip, host * (kFontSizeTooltip / kTooltipDesignHostPx));
}

// Discrete bake sizes so a resize does not rebuild the atlas every pixel.
inline float tooltip_bake_bucket(float logical_tip_px) noexcept {
  static constexpr float kBuckets[] = {16.0f, 20.0f, 26.0f, 32.0f, 40.0f,
                                       52.0f, 64.0f, 80.0f, 104.0f};
  const float s = std::clamp(logical_tip_px, kFontSizeTooltip, kBuckets[8]);
  for (float bucket : kBuckets) {
    if (s <= bucket + 0.001f) {
      return bucket;
    }
  }
  return kBuckets[8];
}

inline constexpr float kControlHeight = 29.0f;
inline constexpr float kStatusBarHeight = 26.0f;
inline constexpr float kToolbarIconSize = 22.0f;
inline constexpr float kToolbarGap = 6.0f;
inline constexpr float kUiPad = 7.0f;
inline constexpr float kUiGap = 7.0f;

// Label columns sized for kFontSizeMd CJK (≈1em per char).
inline constexpr float kLabelW2 = 56.0f;   // 流速 / 音乐 / 音效
inline constexpr float kLabelW4 = 110.0f;  // 播放速度 / 谱面延迟 / 可见范围 …
inline constexpr float kFieldW = 78.0f;
inline constexpr float kFieldWChart = 78.0f;
inline constexpr float kStepButtonW = 20.0f;
inline constexpr float kMuteLabelW = 56.0f;
// Total width of [-][field][+] — delay/chart clusters match this outer width.
inline constexpr float kControlClusterW =
    kStepButtonW + kUiGap + kFieldW + kUiGap + kStepButtonW;

// Back-compat aliases used by existing call sites.
inline constexpr float kFieldLabelWidth = kLabelW2;
inline constexpr float kSettingsLabelW = kLabelW4;
inline constexpr float kFieldWVolume = kFieldW;
inline constexpr float kFieldWRate = kFieldW;
inline constexpr float kFieldWSpeed = kFieldW;
inline constexpr float kFieldWDelay = kFieldW;
inline constexpr float kFieldWShort = kFieldW;

inline constexpr float kMinEditW = 180.0f;
inline constexpr float kMinPreviewW = 130.0f;
inline constexpr float kMinIconPx = 18.0f;

float ui_content_scale() noexcept;

// Store framebuffer/window ratio (not OS DPI %). Does not mutate design metrics.
void apply_content_scale(float content_scale) noexcept;

// Discrete bake tier for fonts/icons (ceil to nearest supported scale bucket).
float content_scale_tier(float content_scale = ui_content_scale()) noexcept;

// Logical length → framebuffer pixels (Vulkan / texture raster boundaries).
inline float to_fb(float logical_1x) noexcept { return logical_1x * ui_content_scale(); }

// Identity helper: call sites that historically meant "logical → FB via theme"
// now stay in logical space; flush applies scale. Prefer bare constants over px().
inline float px(float logical_1x) noexcept { return logical_1x; }

}  // namespace wds::interaction::theme

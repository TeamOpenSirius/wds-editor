#include "wds/interaction/theme.hpp"

namespace wds::interaction::theme {
namespace {

float g_content_scale = 1.0f;
ColorScheme g_color_scheme = ColorScheme::Dark;

void apply_dark_colors() {
  kBackground = {0.07f, 0.07f, 0.09f, 1.0f};
  kSurface = {0.11f, 0.11f, 0.14f, 1.0f};
  kSurfaceVariant = {0.16f, 0.16f, 0.20f, 1.0f};
  kPrimary = {0.67f, 0.55f, 0.98f, 1.0f};
  kOnPrimary = {0.10f, 0.06f, 0.18f, 1.0f};
  kOnSurface = {0.90f, 0.89f, 0.93f, 1.0f};
  kOnSurfaceMuted = {0.70f, 0.69f, 0.74f, 1.0f};
  kOutline = {0.38f, 0.38f, 0.42f, 1.0f};
  kError = {0.96f, 0.45f, 0.45f, 1.0f};
  kWarning = {0.96f, 0.78f, 0.35f, 1.0f};
  kHoverOverlay = {1.0f, 1.0f, 1.0f, 0.08f};
  kPressedOverlay = {1.0f, 1.0f, 1.0f, 0.14f};
  kEditChrome = {18.0f / 255.0f, 20.0f / 255.0f, 27.0f / 255.0f, 1.0f};
  kEditCanvas = {24.0f / 255.0f, 27.0f / 255.0f, 36.0f / 255.0f, 1.0f};
  kEditGutter = {0.06f, 0.07f, 0.09f, 1.0f};
  kEditLaneEdge = {110.0f / 255.0f, 115.0f / 255.0f, 130.0f / 255.0f, 1.0f};
  kEditLaneInner = {55.0f / 255.0f, 60.0f / 255.0f, 72.0f / 255.0f, 1.0f};
  kEditGridBeat = {125.0f / 255.0f, 135.0f / 255.0f, 158.0f / 255.0f, 125.0f / 255.0f};
  kEditGridSubdiv = {75.0f / 255.0f, 82.0f / 255.0f, 99.0f / 255.0f, 65.0f / 255.0f};
  kEditGridMeasure = {0.72f, 0.74f, 0.78f, 0.95f};
  kEditGridNegative = {0.10f, 0.10f, 0.12f, 0.85f};
  kEditWaveform = {145.0f / 255.0f, 160.0f / 255.0f, 188.0f / 255.0f, 70.0f / 255.0f};
  kEditMeasureIndex = {0.78f, 0.80f, 0.84f, 0.95f};
}

void apply_light_colors() {
  kBackground = {0.93f, 0.93f, 0.95f, 1.0f};
  kSurface = {0.98f, 0.98f, 0.99f, 1.0f};
  kSurfaceVariant = {0.91f, 0.91f, 0.93f, 1.0f};
  kPrimary = {0.35f, 0.42f, 0.82f, 1.0f};
  kOnPrimary = {0.98f, 0.98f, 1.0f, 1.0f};
  kOnSurface = {0.16f, 0.16f, 0.18f, 1.0f};
  kOnSurfaceMuted = {0.42f, 0.42f, 0.46f, 1.0f};
  kOutline = {0.70f, 0.70f, 0.74f, 1.0f};
  kError = {0.82f, 0.22f, 0.22f, 1.0f};
  kWarning = {0.78f, 0.52f, 0.08f, 1.0f};
  kHoverOverlay = {0.0f, 0.0f, 0.0f, 0.06f};
  kPressedOverlay = {0.0f, 0.0f, 0.0f, 0.10f};
  kEditChrome = {0.91f, 0.91f, 0.93f, 1.0f};
  kEditCanvas = {0.96f, 0.96f, 0.97f, 1.0f};
  kEditGutter = {0.90f, 0.90f, 0.92f, 1.0f};
  kEditLaneEdge = {0.42f, 0.44f, 0.50f, 1.0f};
  kEditLaneInner = {0.72f, 0.73f, 0.76f, 1.0f};
  kEditGridBeat = {0.32f, 0.35f, 0.42f, 0.42f};
  kEditGridSubdiv = {0.50f, 0.52f, 0.56f, 0.26f};
  kEditGridMeasure = {0.22f, 0.24f, 0.28f, 0.70f};
  kEditGridNegative = {0.82f, 0.83f, 0.85f, 0.90f};
  kEditWaveform = {0.32f, 0.40f, 0.55f, 0.20f};
  kEditMeasureIndex = {0.22f, 0.23f, 0.26f, 0.95f};
}

}  // namespace

Color kBackground{};
Color kSurface{};
Color kSurfaceVariant{};
Color kPrimary{};
Color kOnPrimary{};
Color kOnSurface{};
Color kOnSurfaceMuted{};
Color kOutline{};
Color kError{};
Color kWarning{};
Color kHoverOverlay{};
Color kPressedOverlay{};
Color kEditChrome{};
Color kEditCanvas{};
Color kEditGutter{};
Color kEditLaneEdge{};
Color kEditLaneInner{};
Color kEditGridBeat{};
Color kEditGridSubdiv{};
Color kEditGridMeasure{};
Color kEditGridNegative{};
Color kEditWaveform{};
Color kEditMeasureIndex{};

namespace {
[[maybe_unused]] const bool kColorsInit = [] {
  apply_dark_colors();
  return true;
}();
}  // namespace

void apply_color_scheme(ColorScheme scheme) noexcept {
  g_color_scheme = scheme;
  if (scheme == ColorScheme::Light) apply_light_colors();
  else apply_dark_colors();
}

ColorScheme color_scheme() noexcept { return g_color_scheme; }

float ui_content_scale() noexcept { return g_content_scale; }

void apply_content_scale(float content_scale) noexcept {
  g_content_scale = std::clamp(content_scale, 0.5f, 4.0f);
}

float content_scale_tier(float content_scale) noexcept {
  static constexpr float kTiers[] = {1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
  const float s = std::clamp(content_scale, 0.5f, 4.0f);
  for (float tier : kTiers) {
    if (s <= tier + 0.001f) {
      return tier;
    }
  }
  return kTiers[sizeof(kTiers) / sizeof(kTiers[0]) - 1];
}

}  // namespace wds::interaction::theme

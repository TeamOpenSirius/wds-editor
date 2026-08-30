#pragma once

#include <wds/interaction/theme.hpp>
#include <wds/interaction/types.hpp>

#include <algorithm>

namespace wds::ui {

// Editor shell (see requests/2/design.jpg) plus a full-width status strip:
//   preview (top-left) | edit (right, above status)
//   settings           |
//   toolbar (split)    |
//   -------------- status bar (full width) --------------
struct EditorLayoutRects {
  wds::interaction::Rect preview{};
  wds::interaction::Rect settings{};
  wds::interaction::Rect toolbar{};
  wds::interaction::Rect edit{};
  wds::interaction::Rect status{};
};

struct PreviewContentRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct EditorLayoutResult {
  EditorLayoutRects regions{};
  PreviewContentRect preview_content{};
};

// Shared vertical metrics for the left column below the preview stage.
struct LeftColumnMetrics {
  float left_w = 0.0f;
  float icon = 0.0f;
  float icon_block_h = 0.0f;
  float settings_h = 0.0f;
  float toolbar_h = 0.0f;
  float toolbar_ctrl_h = 0.0f;
};

// Toolbar 2×4 icon cell size from left-column width (matches EditorToolbar::layout).
inline float estimate_toolbar_icon_px(float left_w) noexcept {
  namespace th = wds::interaction::theme;
  const float half_w =
      std::max(1.0f, (left_w - th::kUiPad * 2.0f - th::kUiGap) * 0.5f);
  return std::max(th::kMinIconPx, (half_w - th::kUiGap * 5.0f) / 4.0f);
}

inline LeftColumnMetrics compute_left_column_metrics(float left_w, float rest_h) noexcept {
  namespace th = wds::interaction::theme;
  const float gap = th::kUiGap;
  const float pad = th::kUiPad;
  const float ctrl_h = th::kControlHeight;

  LeftColumnMetrics m;
  m.left_w = left_w;

  m.icon = estimate_toolbar_icon_px(left_w);
  const float grid_h = m.icon * 2.0f + gap;
  m.icon_block_h = grid_h + gap;  // tight band around the 2×4 grids

  const float leftover = std::max(0.0f, rest_h - m.icon_block_h);

  constexpr float kTopShare = 0.85f;
  constexpr float kBottomShare = 1.15f;
  const float std_settings = pad * 2.0f + ctrl_h * 2.0f + gap * 3.0f;
  const float std_ctrl = pad * 2.0f + ctrl_h * 5.0f + gap * 6.0f;
  const float squeeze_settings = pad * 2.0f + ctrl_h * 2.0f;
  const float squeeze_ctrl = pad * 2.0f + ctrl_h * 5.0f;
  const float std_need = std_settings + std_ctrl;
  const float squeeze_need = squeeze_settings + squeeze_ctrl;

  if (leftover >= std_need) {
    const float extra = leftover - std_need;
    m.settings_h = std_settings + extra * (kTopShare / (kTopShare + kBottomShare));
    m.toolbar_ctrl_h = leftover - m.settings_h;
  } else if (leftover >= squeeze_need) {
    const float span = std_need - squeeze_need;
    const float t = span > 0.0f ? (leftover - squeeze_need) / span : 1.0f;
    const float gap_eff = t * gap;
    m.settings_h = pad * 2.0f + ctrl_h * 2.0f + gap_eff * 3.0f;
    m.toolbar_ctrl_h = leftover - m.settings_h;
  } else {
    m.settings_h = leftover * (squeeze_settings / std::max(squeeze_need, 1.0f));
    m.toolbar_ctrl_h = leftover - m.settings_h;
  }

  m.toolbar_h = m.icon_block_h + m.toolbar_ctrl_h;
  return m;
}

// Outer layouter: computes region bounds and the Vulkan stage content rect.
class EditorLayouter {
 public:
  // Official preview canvas is 16:9 landscape (1280×720).
  static constexpr float kPreviewAspect = 16.0f / 9.0f;
  // Uniform scale of the fitted stage (slightly larger than the tight fit).
  static constexpr float kPreviewScale = 1.08f;
  // Stage width as a fraction of the preview column (small side gutters).
  static constexpr float kStageWidthFill = 0.94f;
  static constexpr float kEditFrac = 0.55f;
  static constexpr float kMaxEditFrac = 0.62f;

  EditorLayoutResult compute(int framebuffer_width, int framebuffer_height) const noexcept;
};

}  // namespace wds::ui

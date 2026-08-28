#include "wds/ui/layout/editor_layout.hpp"

#include <algorithm>

namespace wds::ui {

EditorLayoutResult EditorLayouter::compute(int framebuffer_width,
                                           int framebuffer_height) const noexcept {
  EditorLayoutResult out;
  const float W = static_cast<float>(std::max(1, framebuffer_width));
  const float H = static_cast<float>(std::max(1, framebuffer_height));

  namespace th = wds::interaction::theme;
  const float gap = th::kUiGap;
  const float pad = th::kUiPad;
  const float ctrl_h = th::kControlHeight;
  const float status_h = th::kStatusBarHeight;
  const float work_h = std::max(1.0f, H - status_h);
  const float min_edit_w = th::kMinEditW;
  const float min_preview_w = th::kMinPreviewW;
  const auto min_below_squeeze = [&](float lw) {
    const float icon = estimate_toolbar_icon_px(lw);
    const float icon_block_h = icon * 2.0f + gap * 2.0f;
    return icon_block_h + (pad * 2.0f + ctrl_h * 2.0f) + (pad * 2.0f + ctrl_h * 5.0f);
  };

  const float fill = std::clamp(kStageWidthFill, 0.80f, 1.0f);
  const float scale = std::clamp(kPreviewScale, 1.0f, 1.2f);

  float edit_w = std::clamp(W * kEditFrac, min_edit_w, W * kMaxEditFrac);
  edit_w = std::min(edit_w, W - min_preview_w);
  float left_budget = std::max(min_preview_w, W - edit_w);
  float max_preview_h = std::max(1.0f, work_h - min_below_squeeze(left_budget));

  float stage_w = left_budget * fill * scale;
  float stage_h = stage_w / kPreviewAspect;
  if (stage_h > max_preview_h) {
    stage_h = max_preview_h;
    stage_w = stage_h * kPreviewAspect;
  }

  // Hug the 16:9 stage when it is larger than the split, but never shrink the
  // left column below the horizontal budget — height-capped stages letterbox.
  float left_w = std::max(stage_w / fill, left_budget);
  left_w = std::clamp(left_w, min_preview_w, W - min_edit_w);
  max_preview_h = std::max(1.0f, work_h - min_below_squeeze(left_w));
  stage_w = std::min(stage_w, left_w * fill);
  stage_h = stage_w / kPreviewAspect;
  if (stage_h > max_preview_h) {
    stage_h = max_preview_h;
    stage_w = stage_h * kPreviewAspect;
  }
  edit_w = std::max(min_edit_w, W - left_w);

  const float preview_h = stage_h;
  const float rest = std::max(1.0f, work_h - preview_h);
  const LeftColumnMetrics col = compute_left_column_metrics(left_w, rest);

  out.regions.preview = {0, 0, left_w, preview_h};
  out.regions.settings = {0, preview_h, left_w, col.settings_h};
  out.regions.toolbar = {0, preview_h + col.settings_h, left_w, col.toolbar_h};
  out.regions.edit = {left_w, 0, edit_w, work_h};
  out.regions.status = {0, work_h, W, status_h};

  out.preview_content.x = static_cast<int>((left_w - stage_w) * 0.5f);
  out.preview_content.y = static_cast<int>((preview_h - stage_h) * 0.5f);
  out.preview_content.width = std::max(1, static_cast<int>(stage_w));
  out.preview_content.height = std::max(1, static_cast<int>(stage_h));
  return out;
}

}  // namespace wds::ui

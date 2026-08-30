#include "wds/ui/toolbar_curve_selection.hpp"

#include "wds/ui/layout/editor_layout.hpp"

#include <wds/interaction/font_atlas.hpp>
#include <wds/interaction/theme.hpp>
#include <wds/interaction/ui_painter.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace wds::ui {

CurveFillSelection make_curve_fill_selection(const CurveTemplateUiState& live) {
  CurveFillSelection selection;
  selection.template_id = live.selected_id;
  if (selection.template_id != 0 &&
      find_curve_template_by_id(live.templates, selection.template_id) == nullptr) {
    selection.template_id = 0;
  }
  selection.easing =
      resolve_selected_curve_easing(live.templates, selection.template_id, live.direction);
  return selection;
}

std::vector<std::string> curve_template_dropdown_labels(
    const std::vector<CurveTemplate>& templates) {
  std::vector<std::string> labels;
  labels.reserve(templates.size());
  for (const auto& tmpl : templates) {
    labels.push_back(tmpl.name);
  }
  return labels;
}

int dropdown_index_for_curve_id(const std::vector<CurveTemplate>& templates, std::uint64_t id) {
  if (id == 0) return -1;
  for (std::size_t i = 0; i < templates.size(); ++i) {
    if (templates[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

std::uint64_t curve_id_for_dropdown_index(const std::vector<CurveTemplate>& templates, int index) {
  if (index < 0) return 0;
  const std::size_t i = static_cast<std::size_t>(index);
  if (i >= templates.size()) return 0;
  return templates[i].id;
}

int index_for_curve_direction(wds::chart_editor::EasingDirection direction) {
  for (int i = 0; i < static_cast<int>(kCurveDirectionValues.size()); ++i) {
    if (kCurveDirectionValues[static_cast<std::size_t>(i)] == direction) return i;
  }
  return 0;
}

wds::chart_editor::EasingDirection curve_direction_from_index(int index) {
  if (index < 0 || index >= static_cast<int>(kCurveDirectionValues.size())) {
    return wds::chart_editor::EasingDirection::In;
  }
  return kCurveDirectionValues[static_cast<std::size_t>(index)];
}

ToolbarControlLayout compute_toolbar_control_layout(float toolbar_w) noexcept {
  namespace th = wds::interaction::theme;
  ToolbarControlLayout layout;
  layout.pad = th::kUiPad;
  layout.gap = th::kUiGap;
  layout.step_w = th::kStepButtonW;
  const float full_w = std::max(1.0f, toolbar_w - layout.pad * 2.0f);
  const float min_field = kToolbarMinFieldW;
  const float min_cluster = layout.step_w * 2.0f + layout.gap * 2.0f + min_field;

  layout.label_w = th::kLabelW4;
  layout.cluster_w = std::max(th::kControlClusterW, min_cluster);

  float col_w = std::max(1.0f, (full_w - layout.gap) * 0.5f);
  while (layout.label_w + layout.cluster_w > col_w && layout.cluster_w > min_cluster) {
    layout.cluster_w -= th::px(2.0f);
  }
  if (layout.label_w + layout.cluster_w > col_w && layout.label_w > th::kLabelW2) {
    layout.label_w = std::max(th::kLabelW2, col_w - min_cluster);
    layout.cluster_w = min_cluster;
  }
  layout.cluster_w =
      std::max(min_cluster, std::min(layout.cluster_w, col_w - layout.label_w));

  layout.col_x[0] = layout.pad;
  layout.col_w[0] = col_w;
  layout.col_x[1] = layout.pad + col_w + layout.gap;
  layout.col_w[1] = std::max(1.0f, layout.pad + full_w - layout.col_x[1]);
  // Sentinel right edge of the two-column block; col_w[2] stays 0 (no 3rd column).
  layout.col_x[2] = layout.pad + full_w;
  layout.col_w[2] = 0.0f;
  layout.field_in_step =
      std::max(min_field, layout.cluster_w - layout.step_w * 2.0f - layout.gap * 2.0f);
  layout.field_in_chart = std::max(min_field, layout.cluster_w - layout.step_w - layout.gap);
  return layout;
}

float toolbar_min_usable_width() noexcept {
  namespace th = wds::interaction::theme;
  const float min_cluster = th::kStepButtonW * 2.0f + th::kUiGap * 2.0f + kToolbarMinFieldW;
  const float min_pair = th::kLabelW2 + min_cluster;
  return th::kUiPad * 2.0f + min_pair * 2.0f + th::kUiGap;
}

namespace {

struct ToolbarGroupMetrics {
  float col0 = 0.0f;
  float col1 = 0.0f;
  float group_w = 0.0f;
  float label_w = 0.0f;
  float cluster_w = 0.0f;
};

ToolbarGroupMetrics toolbar_group_metrics(const ToolbarControlLayout& cols) noexcept {
  ToolbarGroupMetrics g;
  g.label_w = cols.label_w;
  g.cluster_w = cols.cluster_w;
  g.group_w = cols.label_w + cols.cluster_w;
  g.col0 = cols.col_x[0] + std::max(0.0f, (cols.col_w[0] - g.group_w) * 0.5f);
  g.col1 = cols.col_x[1] + std::max(0.0f, (cols.col_w[1] - g.group_w) * 0.5f);
  return g;
}

std::size_t utf8_codepoint_count(const std::string& text) noexcept {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if ((lead & 0x80u) == 0) {
      i += 1;
    } else if ((lead & 0xE0u) == 0xC0u) {
      i += 2;
    } else if ((lead & 0xF0u) == 0xE0u) {
      i += 3;
    } else if ((lead & 0xF8u) == 0xF0u) {
      i += 4;
    } else {
      i += 1;
    }
    ++count;
  }
  return count;
}

}  // namespace

float toolbar_painted_label_width(const std::string& text) noexcept {
  namespace th = wds::interaction::theme;
  const float fallback = static_cast<float>(utf8_codepoint_count(text)) * th::kFontSizeMd;
  if (wds::interaction::FontAtlas::instance().atlas_width() > 0) {
    wds::interaction::UiPainter painter;
    const float measured = painter.measure_text(text, th::kFontSizeMd).x;
    if (measured > 0.0f) return measured;
  }
  return fallback;
}

float toolbar_dropdown_chrome_width() noexcept {
  namespace th = wds::interaction::theme;
  return 4.0f + std::max(18.0f, th::px(11.0f));
}

float toolbar_direction_button_min_width() noexcept {
  return toolbar_painted_label_width("IO");
}

float toolbar_empty_dropdown_min_width() noexcept {
  return toolbar_painted_label_width(kEmptyCurveTemplateLabel) + toolbar_dropdown_chrome_width();
}

float toolbar_checkbox_required_width(const std::string& label) noexcept {
  namespace th = wds::interaction::theme;
  const float side = std::clamp(th::kControlHeight * 0.72f, th::px(9.0f), th::px(14.0f));
  return side + th::px(5.0f) + toolbar_painted_label_width(label) + th::px(4.0f);
}

bool toolbar_checkboxes_need_stack(const ToolbarControlLayout& cols, float measured0,
                                   float measured1) noexcept {
  // Conservative CJK estimates often exceed col_w at supported narrow widths.
  // Keep one row; height crush (if any) is decided by the caller.
  (void)cols;
  (void)measured0;
  (void)measured1;
  return false;
}

ToolbarCurveRowLayout compute_toolbar_curve_row_layout(const ToolbarControlLayout& cols, float y,
                                                       float row_h) noexcept {
  const auto g = toolbar_group_metrics(cols);
  ToolbarCurveRowLayout out;
  out.label = {g.col0, y, g.label_w, row_h};
  out.dropdown = {g.col0 + g.label_w, y, g.cluster_w, row_h};
  const float dir_gap = cols.gap * 0.5f;
  const float dir_w = (g.group_w - dir_gap * 3.0f) / 4.0f;
  for (int i = 0; i < 4; ++i) {
    out.dirs[static_cast<std::size_t>(i)] = {
        g.col1 + static_cast<float>(i) * (dir_w + dir_gap), y, dir_w, row_h};
  }
  return out;
}

ToolbarControlVerticalLayout compute_toolbar_control_vertical(float toolbar_w, float toolbar_h,
                                                              bool stacked) noexcept {
  namespace th = wds::interaction::theme;
  ToolbarControlVerticalLayout out;
  out.icon = estimate_toolbar_icon_px(toolbar_w);
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  const float grid_h = out.icon * 2.0f + gap;
  out.icon_block_h = grid_h + gap;
  out.rows = stacked ? 5 : 4;
  const float kRows = static_cast<float>(out.rows);
  const float kSlots = kRows + 1.0f;
  const float ctrl_h_avail = std::max(1.0f, toolbar_h - out.icon_block_h);
  const float free = std::max(0.0f, ctrl_h_avail - ctrl_h * kRows);
  out.slot = free / kSlots;
  out.y_delay = out.icon_block_h + out.slot;
  out.y_range = out.y_delay + ctrl_h + out.slot;
  out.y_curve = out.y_range + ctrl_h + out.slot;
  out.y_checkbox = out.y_curve + ctrl_h + out.slot;
  return out;
}

ToolbarCheckboxLayout compute_toolbar_checkbox_layout(const ToolbarControlLayout& cols,
                                                      float measured0, float measured1, float y,
                                                      float row_h) noexcept {
  ToolbarCheckboxLayout out;
  out.h0 = row_h;
  out.h1 = row_h;
  out.w0 = std::max(0.0f, measured0);
  out.w1 = std::max(0.0f, measured1);
  out.stacked = toolbar_checkboxes_need_stack(cols, out.w0, out.w1);
  const float pair_left = cols.pad;
  const float pair_right = cols.col_x[2];
  const float min_gap = cols.gap;
  if (out.stacked) {
    out.x0 = cols.pad;
    out.x1 = cols.pad;
    out.y0 = y;
    out.y1 = y + row_h + cols.gap;
    if (out.x0 < pair_left) out.x0 = pair_left;
    if (out.x1 < pair_left) out.x1 = pair_left;
    if (out.x0 + out.w0 > pair_right) out.x0 = pair_right - out.w0;
    if (out.x1 + out.w1 > pair_right) out.x1 = pair_right - out.w1;
    return out;
  }

  const auto g = toolbar_group_metrics(cols);
  const float c0 = g.col0 + g.group_w * 0.5f;
  const float c1 = g.col1 + g.group_w * 0.5f;
  out.x0 = c0 - out.w0 * 0.5f;
  out.x1 = c1 - out.w1 * 0.5f;
  out.y0 = y;
  out.y1 = y;
  const float avail = std::max(0.0f, pair_right - pair_left);
  const auto shift_pair = [&](float x0) {
    out.x0 = x0;
    out.x1 = x0 + out.w0 + min_gap;
  };
  if (out.x0 + out.w0 + min_gap > out.x1) {
    const float packed = out.w0 + min_gap + out.w1;
    float x0 = (c0 + c1) * 0.5f - packed * 0.5f;
    if (packed <= avail + 0.01f) {
      if (x0 < pair_left) x0 = pair_left;
      if (x0 + packed > pair_right) x0 = pair_right - packed;
    } else {
      x0 = pair_left + (avail - packed) * 0.5f;
    }
    shift_pair(x0);
  } else {
    const float left = std::min(out.x0, out.x1);
    const float right = std::max(out.x0 + out.w0, out.x1 + out.w1);
    if (left < pair_left - 0.01f || right > pair_right + 0.01f) {
      const float span = right - left;
      float shift = 0.0f;
      if (span <= avail + 0.01f) {
        if (left < pair_left) shift = pair_left - left;
        if (right + shift > pair_right) shift = pair_right - right;
      } else {
        shift = pair_left + (avail - span) * 0.5f - left;
      }
      out.x0 += shift;
      out.x1 += shift;
    }
  }
  return out;
}

wds::interaction::Rect toolbar_action_icon_rect(float toolbar_w, std::size_t index) noexcept {
  namespace th = wds::interaction::theme;
  const float pad = th::kUiPad;
  const float gap = th::kUiGap;
  const float half_w = std::max(1.0f, (toolbar_w - pad * 2.0f - gap) * 0.5f);
  const float icon = estimate_toolbar_icon_px(toolbar_w);
  const float pitch = icon + gap;
  const float grid_h = icon * 2.0f + gap;
  const float icon_block_h = grid_h + gap;
  const float grid_w = icon * 4.0f + gap * 3.0f;
  const float ox = pad + (half_w - grid_w) * 0.5f;
  const float oy = (icon_block_h - grid_h) * 0.5f;
  const int col = static_cast<int>(index % 4);
  const int row = static_cast<int>(index / 4);
  return {ox + static_cast<float>(col) * pitch, oy + static_cast<float>(row) * pitch, icon, icon};
}

void CurveToolbarController::sync_view(const CurveTemplateUiState& live) {
  labels_ = curve_template_dropdown_labels(live.templates);
  dropdown_index_ = dropdown_index_for_curve_id(live.templates, live.selected_id);
  direction_index_ = index_for_curve_direction(live.direction);
  selection_ = make_curve_fill_selection(live);
}

void CurveToolbarController::publish_if_changed(const CurveTemplateUiState& live, bool emit) {
  sync_view(live);
  if (emit && on_changed_) on_changed_(selection_);
}

void CurveToolbarController::refresh_from(CurveTemplateUiState& live) {
  if (live.selected_id != 0 && find_curve_template_by_id(live.templates, live.selected_id) == nullptr) {
    live.selected_id = 0;
  }
  sync_view(live);
}

bool CurveToolbarController::select_dropdown_index(CurveTemplateUiState& live, int index) {
  const std::uint64_t id = curve_id_for_dropdown_index(live.templates, index);
  if (id == live.selected_id) return false;
  live.selected_id = id;
  publish_if_changed(live, true);
  return true;
}

bool CurveToolbarController::select_direction_index(CurveTemplateUiState& live, int index) {
  const auto direction = curve_direction_from_index(index);
  if (direction == live.direction) return false;
  live.direction = direction;
  publish_if_changed(live, true);
  return true;
}

}  // namespace wds::ui

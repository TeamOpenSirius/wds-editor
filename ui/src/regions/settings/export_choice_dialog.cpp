#include "wds/ui/regions/settings/export_choice_dialog.hpp"

#include <wds/interaction/theme.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/combo_box.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace wds::ui {
namespace {

namespace th = wds::interaction::theme;

// Tip column sized to "格式" at full UI font — do not shrink the glyph to free
// width. Extra space came from the old oversized label gutter + large gap.
float format_label_w() { return th::kLabelW2; }
float format_label_gap() { return th::kUiGap; }

}  // namespace

ExportChoiceDialog::ExportChoiceDialog() {
  set_visible(false);

  auto format = std::make_unique<wds::interaction::ComboBox>();
  format->set_items({"官方 CSV", "SUS"});
  format->set_dropdown_only(true);
  format->set_text("官方 CSV");
  format->set_opens_upward(false);
  format->on_commit([this](const std::string&) { format_ = format_from_combo(); });
  format_combo_ = format.get();
  add_child(std::move(format));

  auto project = std::make_unique<wds::interaction::Button>("导出整个项目");
  project->set_tone(wds::interaction::ButtonTone::Confirm);
  project->on_click([this] {
    format_ = format_from_combo();
    close();
    if (on_export_project_) on_export_project_(format_);
  });
  project_button_ = project.get();
  add_child(std::move(project));

  auto chart = std::make_unique<wds::interaction::Button>("仅导出当前谱面");
  chart->set_tone(wds::interaction::ButtonTone::Default);
  chart->on_click([this] {
    format_ = format_from_combo();
    close();
    if (on_export_chart_) on_export_chart_(format_);
  });
  chart_button_ = chart.get();
  add_child(std::move(chart));

  auto cancel = std::make_unique<wds::interaction::Button>("取消");
  cancel->set_tone(wds::interaction::ButtonTone::Cancel);
  cancel->on_click([this] { close(); });
  cancel_button_ = cancel.get();
  add_child(std::move(cancel));
}

ExportFormat ExportChoiceDialog::format_from_combo() const {
  const auto* combo = static_cast<const wds::interaction::ComboBox*>(format_combo_);
  if (combo != nullptr && combo->text().find("SUS") != std::string::npos) {
    return ExportFormat::Sus;
  }
  return ExportFormat::OfficialCsv;
}

void ExportChoiceDialog::open() {
  open_ = true;
  set_visible(true);
  format_ = ExportFormat::OfficialCsv;
  if (auto* combo = static_cast<wds::interaction::ComboBox*>(format_combo_)) {
    combo->set_text("官方 CSV");
  }
}

void ExportChoiceDialog::close() {
  open_ = false;
  set_visible(false);
  if (auto* combo = static_cast<wds::interaction::ComboBox*>(format_combo_)) {
    combo->dismiss_popups({-1.0f, -1.0f});
  }
}

void ExportChoiceDialog::layout_content(const wds::interaction::Rect& host) {
  const float pad = th::kUiPad * 1.5f;
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  const float title_h = ctrl_h;
  const float panel_w = std::min(th::px(260.0f), std::max(th::px(180.0f), host.w * 0.36f));
  const float panel_h = pad * 2.0f + title_h + gap + ctrl_h * 4.0f + gap * 4.0f;
  content_bounds_ = {(host.w - panel_w) * 0.5f, (host.h - panel_h) * 0.5f, panel_w, panel_h};

  const float inner_w = panel_w - pad * 2.0f;
  const float row_right = content_bounds_.x + pad + inner_w;
  float y = content_bounds_.y + pad + title_h + gap;
  const float combo_x = content_bounds_.x + pad + format_label_w() + format_label_gap();
  format_combo_->set_bounds({combo_x, y, std::max(th::px(40.0f), row_right - combo_x), ctrl_h});
  y += ctrl_h + gap;
  project_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
  y += ctrl_h + gap;
  chart_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
  y += ctrl_h + gap;
  cancel_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
}

void ExportChoiceDialog::layout(const wds::interaction::Rect& parent_bounds) {
  bounds_ = {0.0f, 0.0f, parent_bounds.w, parent_bounds.h};
  if (open_) {
    layout_content(bounds_);
  }
  Widget::layout(parent_bounds);
}

void ExportChoiceDialog::paint(wds::interaction::UiPainter& /*painter*/) const {
  // Painted via paint_modal() in the modal post-pass.
}

void ExportChoiceDialog::paint_popup_layers(wds::interaction::UiPainter& /*painter*/) const {
  // Format combo popup is painted in paint_dropdown() via the chrome batch.
}

void ExportChoiceDialog::paint_modal(wds::interaction::UiPainter& painter) const {
  if (!open_ || !visible_) return;
  const wds::interaction::Rect abs = absolute_bounds();
  painter.fill_rect(abs, {0.0f, 0.0f, 0.0f, 0.55f}, 0.0f, 0.985f);

  wds::interaction::Rect content = content_bounds_;
  content.x += abs.x;
  content.y += abs.y;
  painter.fill_rect(content, th::kSurface, th::kCornerRadiusMd, 0.986f);

  const float pad = th::kUiPad * 1.5f;
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  painter.label({content.x + pad, content.y + pad, content.w - pad * 2.0f, ctrl_h}, "导出",
                th::kOnSurface, 0.987f);

  // Same row / height as the combo so "格式" shares its vertical center line.
  const float row_y = content.y + pad + ctrl_h + gap;
  painter.label({content.x + pad, row_y, format_label_w(), ctrl_h}, "格式", th::kOnSurfaceMuted,
                0.987f, false, 0.0f, true);

  // Modal panel sits at ~0.986; paint controls above it (same as WidthSlotsDialog).
  constexpr float kFieldZ = 0.988f;
  static_cast<const wds::interaction::ComboBox*>(format_combo_)->paint_at(painter, kFieldZ);
  static_cast<const wds::interaction::Button*>(project_button_)->paint_at(painter, kFieldZ);
  static_cast<const wds::interaction::Button*>(chart_button_)->paint_at(painter, kFieldZ);
  static_cast<const wds::interaction::Button*>(cancel_button_)->paint_at(painter, kFieldZ);
}

void ExportChoiceDialog::paint_dropdown(wds::interaction::UiPainter& painter) const {
  if (!open_ || !visible_ || format_combo_ == nullptr) return;
  format_combo_->paint_popup_layer(painter);
}

wds::interaction::Widget* ExportChoiceDialog::hit_test(wds::interaction::Vec2 point) {
  if (!open_ || !visible_ || !enabled_) return nullptr;
  if (!absolute_bounds().contains(point)) return nullptr;
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test(point)) return hit;
  }
  return this;
}

void ExportChoiceDialog::on_click(const wds::interaction::ClickEvent& event) {
  if (!open_ || event.button != wds::interaction::PointerButton::Left) return;
  wds::interaction::Rect content = content_bounds_;
  const wds::interaction::Rect abs = absolute_bounds();
  content.x += abs.x;
  content.y += abs.y;
  if (!content.contains(event.position)) {
    close();
  }
}

}  // namespace wds::ui

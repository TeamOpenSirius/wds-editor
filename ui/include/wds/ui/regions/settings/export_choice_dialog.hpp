#pragma once

#include "wds/ui/editor_session.hpp"

#include <wds/interaction/widget.hpp>

#include <functional>

namespace wds::ui {

// Modal: choose export scope + format (official CSV / SUS).
class ExportChoiceDialog final : public wds::interaction::Widget {
 public:
  using ChoiceHandler = std::function<void(ExportFormat format)>;

  ExportChoiceDialog();

  bool is_open() const noexcept { return open_; }
  void open();
  void close();

  ExportFormat selected_format() const noexcept { return format_; }

  void on_export_project(ChoiceHandler handler) { on_export_project_ = std::move(handler); }
  void on_export_chart(ChoiceHandler handler) { on_export_chart_ = std::move(handler); }

  void layout(const wds::interaction::Rect& parent_bounds) override;
  void paint(wds::interaction::UiPainter& painter) const override;
  // Skip default popup walk — format combo is painted in paint_dropdown() (chrome pass)
  // so button glyphs from the modal batch cannot cover the menu panel.
  void paint_popup_layers(wds::interaction::UiPainter& painter) const override;
  void paint_modal(wds::interaction::UiPainter& painter) const;
  // Open format dropdown; must be flushed in a later draw pass than paint_modal.
  void paint_dropdown(wds::interaction::UiPainter& painter) const;
  Widget* hit_test(wds::interaction::Vec2 point) override;
  void on_click(const wds::interaction::ClickEvent& event) override;

 private:
  void layout_content(const wds::interaction::Rect& host);
  ExportFormat format_from_combo() const;

  bool open_ = false;
  ExportFormat format_ = ExportFormat::OfficialCsv;
  wds::interaction::Rect content_bounds_{};
  wds::interaction::Widget* format_combo_ = nullptr;
  wds::interaction::Widget* project_button_ = nullptr;
  wds::interaction::Widget* chart_button_ = nullptr;
  wds::interaction::Widget* cancel_button_ = nullptr;
  ChoiceHandler on_export_project_;
  ChoiceHandler on_export_chart_;
};

}  // namespace wds::ui

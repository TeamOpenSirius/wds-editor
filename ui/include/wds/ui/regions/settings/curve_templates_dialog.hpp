#pragma once

#include "wds/ui/curve_template.hpp"

#include <wds/interaction/ui_painter.hpp>
#include <wds/interaction/widget.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wds::ui {

// Transactional working copy for the curve-template editor. All edits stay here
// until commit(); begin() copies live state, discard() drops pending edits.
class CurveTemplateDialogSession {
 public:
  void begin(const CurveTemplateUiState& live);
  void discard();
  void commit(CurveTemplateUiState& live) const;

  const std::vector<CurveTemplate>& templates() const noexcept { return templates_; }
  std::uint64_t selected_id() const noexcept { return selected_id_; }
  const CurveTemplate* selected() const;
  CurveTemplate* selected();

  bool can_add() const noexcept;
  void add();
  void remove_at(std::size_t index);
  void select_id(std::uint64_t id);
  void select_index(std::size_t index);
  void set_selected_name(std::string name);
  void set_selected_algorithm(wds::chart_editor::EasingAlgorithm algorithm);
  void set_selected_parameter(double parameter);

  static std::string next_default_name(const std::vector<CurveTemplate>& templates);

 private:
  std::vector<CurveTemplate> templates_;
  std::uint64_t selected_id_ = 0;
};

void paint_easing_preview_polyline(wds::interaction::UiPainter& painter,
                                   wds::interaction::Rect inner,
                                   wds::chart_editor::EasingAlgorithm algorithm,
                                   wds::chart_editor::EasingDirection direction, double parameter,
                                   float z);

class CurveTemplatesDialog final : public wds::interaction::Widget {
 public:
  CurveTemplatesDialog();

  bool is_open() const noexcept { return open_; }
  bool is_interaction_modal() const override { return open_; }
  bool captures_keys() const override { return open_; }
  bool is_focusable() const override { return true; }
  bool wants_focus() const override { return open_; }
  bool intercept_modal_key_down(const wds::interaction::KeyDownEvent& event) override;

  void open(const CurveTemplateUiState& live);
  void close();

  void set_on_confirmed(std::function<void(const CurveTemplateUiState&)> cb) {
    on_confirmed_ = std::move(cb);
  }

  const CurveTemplateDialogSession& session() const noexcept { return session_; }

  void layout(const wds::interaction::Rect& parent_bounds) override;
  void update(float delta_seconds) override;
  void paint(wds::interaction::UiPainter& painter) const override;
  void paint_modal(wds::interaction::UiPainter& painter) const;
  Widget* hit_test(wds::interaction::Vec2 point) override;
  void on_click(const wds::interaction::ClickEvent& event) override;
  void on_scroll(const wds::interaction::ScrollEvent& event) override;
  void on_key_down(const wds::interaction::KeyDownEvent& event) override;

 private:
  void layout_content(const wds::interaction::Rect& host);
  void sync_fields_from_session();
  void apply_fields_to_session();
  void update_control_enabled();
  void clamp_list_scroll();
  void try_confirm();
  void paint_preview(wds::interaction::UiPainter& painter, const wds::interaction::Rect& pane,
                     wds::chart_editor::EasingDirection direction, float z) const;

  CurveTemplateDialogSession session_;
  std::uint64_t fill_id_at_open_ = 0;
  wds::chart_editor::EasingDirection direction_at_open_ =
      wds::chart_editor::EasingDirection::In;
  bool open_ = false;
  std::function<void(const CurveTemplateUiState&)> on_confirmed_;
  wds::interaction::Rect content_bounds_{};
  wds::interaction::Rect list_bounds_{};
  wds::interaction::Rect add_row_bounds_{};
  std::array<wds::interaction::Rect, 4> algo_bounds_{};
  std::array<wds::interaction::Rect, 4> preview_bounds_{};
  float list_scroll_ = 0.0f;

  wds::interaction::Widget* add_button_ = nullptr;
  wds::interaction::Widget* confirm_button_ = nullptr;
  wds::interaction::Widget* cancel_button_ = nullptr;
  wds::interaction::Widget* name_field_ = nullptr;
  wds::interaction::Widget* parameter_field_ = nullptr;
  std::array<wds::interaction::Widget*, kMaxCurveTemplates> delete_buttons_{};
};

}  // namespace wds::ui

#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>

namespace wds::ui {

// Modal: create a new chart vs add an existing .wdschart into the project.
class ChartAddDialog final : public wds::interaction::Widget {
 public:
  ChartAddDialog();

  bool is_open() const noexcept { return open_; }
  void open();
  void close();

  void on_create_new(std::function<void()> handler) { on_create_new_ = std::move(handler); }
  void on_add_existing(std::function<void()> handler) { on_add_existing_ = std::move(handler); }

  void layout(const wds::interaction::Rect& parent_bounds) override;
  void paint(wds::interaction::UiPainter& painter) const override;
  void paint_modal(wds::interaction::UiPainter& painter) const;
  Widget* hit_test(wds::interaction::Vec2 point) override;
  void on_click(const wds::interaction::ClickEvent& event) override;

 private:
  void layout_content(const wds::interaction::Rect& host);

  bool open_ = false;
  wds::interaction::Rect content_bounds_{};
  wds::interaction::Widget* create_button_ = nullptr;
  wds::interaction::Widget* add_button_ = nullptr;
  wds::interaction::Widget* cancel_button_ = nullptr;
  std::function<void()> on_create_new_;
  std::function<void()> on_add_existing_;
};

}  // namespace wds::ui

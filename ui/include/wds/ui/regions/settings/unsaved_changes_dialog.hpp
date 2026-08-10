#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>

namespace wds::ui {

// In-app modal: 保存 / 不保存 / 取消 for unsaved project changes.
class UnsavedChangesDialog final : public wds::interaction::Widget {
 public:
  UnsavedChangesDialog();

  bool is_open() const noexcept { return open_; }
  bool is_interaction_modal() const override { return open_; }
  void open();
  void close();

  void on_save(std::function<void()> handler) { on_save_ = std::move(handler); }
  void on_discard(std::function<void()> handler) { on_discard_ = std::move(handler); }
  void on_cancel(std::function<void()> handler) { on_cancel_ = std::move(handler); }

  void layout(const wds::interaction::Rect& parent_bounds) override;
  void paint(wds::interaction::UiPainter& painter) const override;
  void paint_modal(wds::interaction::UiPainter& painter) const;
  Widget* hit_test(wds::interaction::Vec2 point) override;
  void on_click(const wds::interaction::ClickEvent& event) override;

 private:
  void layout_content(const wds::interaction::Rect& host);
  void choose_cancel();

  bool open_ = false;
  wds::interaction::Rect content_bounds_{};
  wds::interaction::Widget* save_button_ = nullptr;
  wds::interaction::Widget* discard_button_ = nullptr;
  wds::interaction::Widget* cancel_button_ = nullptr;
  std::function<void()> on_save_;
  std::function<void()> on_discard_;
  std::function<void()> on_cancel_;
};

}  // namespace wds::ui

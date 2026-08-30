#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>
#include <string>
#include <vector>

namespace wds::interaction {

class Dropdown : public Widget {
 public:
  using SelectionHandler = std::function<void(int, const std::string&)>;

  Dropdown();

  void set_items(std::vector<std::string> items);
  const std::vector<std::string>& items() const noexcept { return items_; }

  void set_placeholder(std::string text) { placeholder_ = std::move(text); }
  const std::string& placeholder() const noexcept { return placeholder_; }

  void set_selected_index(int index) noexcept;
  int selected_index() const noexcept { return selected_index_; }
  const std::string& selected_label() const;

  void on_select(SelectionHandler handler) { on_select_ = std::move(handler); }

  // Open the item menu above the field (toolbar / bottom panels).
  void set_opens_upward(bool enabled) noexcept { opens_upward_ = enabled; }
  bool opens_upward() const noexcept { return opens_upward_; }
  bool is_open() const noexcept { return open_; }

  void paint(UiPainter& painter) const override;
  void paint_popup_layer(UiPainter& painter) const override;
  Widget* hit_test(Vec2 point) override;
  Widget* hit_test_popup(Vec2 point) override;
  Widget* hit_test_popup_host(Vec2 point) override;
  bool dismiss_popups(Vec2 point) override;
  void close_own_popup() override;
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_pointer_move(const PointerMoveEvent& event) override;
  void on_click(const ClickEvent& event) override;
  void on_scroll(const ScrollEvent& event) override;

  const char* trace_name() const override { return "Dropdown"; }
  void trace_snapshot(wds::common::CrashTraceSnap& snap) const override {
    snap.mask = wds::common::kCrashSnapPopupScroll | wds::common::kCrashSnapComboOpen;
    snap.popup_scroll = menu_scroll_;
    snap.combo_open = open_ ? 1 : 0;
  }

 private:
  std::vector<std::string> items_;
  std::string placeholder_;
  int selected_index_ = -1;
  bool open_ = false;
  bool opens_upward_ = false;
  float menu_scroll_ = 0.0f;
  int hover_index_ = -1;
  SelectionHandler on_select_;
};

}  // namespace wds::interaction

#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>
#include <string>
#include <vector>

namespace wds::interaction {

class ComboBox : public Widget {
 public:
  using CommitHandler = std::function<void(const std::string&)>;

  ComboBox();

  void set_items(std::vector<std::string> items);
  const std::vector<std::string>& items() const noexcept { return items_; }

  // Updates both the editable text and the last-committed fallback value.
  void set_text(std::string text);
  const std::string& text() const noexcept { return text_; }

  void on_commit(CommitHandler handler) { on_commit_ = std::move(handler); }
  void set_validator(std::function<bool(const std::string&)> validator) {
    validator_ = std::move(validator);
  }

  // When true, the field is selection-only: click opens the menu, typing is ignored.
  void set_dropdown_only(bool enabled) noexcept { dropdown_only_ = enabled; }
  bool dropdown_only() const noexcept { return dropdown_only_; }

  // Open the item menu above the field (toolbar / bottom panels).
  void set_opens_upward(bool enabled) noexcept { opens_upward_ = enabled; }
  bool opens_upward() const noexcept { return opens_upward_; }

  bool wants_focus() const override { return true; }
  bool is_focusable() const override { return true; }
  // Editable fields always capture; dropdown-only captures while the menu is open.
  bool captures_keys() const override { return visible() && (!dropdown_only_ || open_); }

  void update(float delta_seconds) override;
  void paint(UiPainter& painter) const override;
  // Same as paint(), with an explicit base z (modal panels sit near 0.986).
  void paint_at(UiPainter& painter, float z) const;
  void paint_popup_layer(UiPainter& painter) const override;
  Widget* hit_test(Vec2 point) override;
  Widget* hit_test_popup(Vec2 point) override;
  bool dismiss_popups(Vec2 point) override;
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_pointer_move(const PointerMoveEvent& event) override;
  void on_key_down(const KeyDownEvent& event) override;
  void on_text_input(const TextInputEvent& event) override;
  void on_scroll(const ScrollEvent& event) override;
  void on_focus() override;
  void on_blur() override;

 private:
  int selected_item_index() const noexcept;
  void commit_or_revert();
  void accept_text(std::string text);
  void reset_caret_blink() noexcept { caret_blink_t_ = 0.0f; }

  std::vector<std::string> items_;
  std::string text_;
  std::string committed_text_;
  bool open_ = false;
  bool dropdown_only_ = false;
  bool opens_upward_ = false;
  float menu_scroll_ = 0.0f;
  int hover_index_ = -1;
  float caret_blink_t_ = 0.0f;
  CommitHandler on_commit_;
  std::function<bool(const std::string&)> validator_;
};

}  // namespace wds::interaction

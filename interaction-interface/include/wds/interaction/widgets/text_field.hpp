#pragma once

#include <wds/interaction/widget.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>

namespace wds::interaction {

class TextField : public Widget {
 public:
  using ChangeHandler = std::function<void(const std::string&)>;
  using CommitHandler = std::function<void(const std::string&)>;

  explicit TextField(std::string placeholder = {});

  // Updates both the editable text and the last-committed fallback value.
  void set_text(std::string text);
  const std::string& text() const noexcept { return text_; }
  void set_placeholder(std::string placeholder) { placeholder_ = std::move(placeholder); }

  void on_change(ChangeHandler handler) { on_change_ = std::move(handler); }
  void on_commit(CommitHandler handler) { on_commit_ = std::move(handler); }
  void set_validator(std::function<bool(const std::string&)> validator) {
    validator_ = std::move(validator);
  }
  // True while the editable text fails the validator (red outline).
  bool text_invalid() const noexcept {
    return static_cast<bool>(validator_) && !validator_(text_);
  }
  // Paint the same red outline as text_invalid() for `seconds`, then expire.
  void flash_error(float seconds) noexcept {
    error_flash_remaining_ = std::max(0.0f, seconds);
  }
  bool error_flashing() const noexcept { return error_flash_remaining_ > 0.0f; }

  bool wants_focus() const override { return true; }
  bool is_focusable() const override { return true; }
  // Suppress chart-edit chords (Left/Right nudge, Delete selection, …) only while focused.
  bool captures_keys() const override {
    return visible() && visual_state() == WidgetState::Focused;
  }

  void paint(UiPainter& painter) const override;
  // Same as paint(), with an explicit base z (modal dialogs need z above the panel).
  void paint_at(UiPainter& painter, float z) const;
  void update(float delta_seconds) override;
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_key_down(const KeyDownEvent& event) override;
  void on_text_input(const TextInputEvent& event) override;
  void on_focus() override;
  void on_blur() override;

 private:
  void commit_or_revert();
  void reset_caret_blink() noexcept { caret_blink_t_ = 0.0f; }

  std::string text_;
  std::string committed_text_;
  std::string placeholder_;
  ChangeHandler on_change_;
  CommitHandler on_commit_;
  std::function<bool(const std::string&)> validator_;
  float caret_blink_t_ = 0.0f;
  float error_flash_remaining_ = 0.0f;
  std::size_t caret_ = 0;
};

}  // namespace wds::interaction

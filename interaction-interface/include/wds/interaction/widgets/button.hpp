#pragma once

#include <wds/interaction/widget.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace wds::interaction {

enum class ButtonTone : uint8_t {
  Default,
  Confirm,  // green — matches split/timing modal confirm
  Cancel,   // gray — matches split/timing modal cancel
};

class Button : public Widget {
 public:
  using ClickHandler = std::function<void()>;

  explicit Button(std::string label = {});

  void set_label(std::string label) { label_ = std::move(label); }
  const std::string& label() const noexcept { return label_; }
  void set_tone(ButtonTone tone) noexcept { tone_ = tone; }
  ButtonTone tone() const noexcept { return tone_; }

  void on_click(ClickHandler handler) { on_click_ = std::move(handler); }

  void paint(UiPainter& painter) const override;
  // Same as paint(), with an explicit base z (modal panels sit near 0.986).
  void paint_at(UiPainter& painter, float z) const;
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_pointer_up(const PointerUpEvent& event) override;
  void on_pointer_move(const PointerMoveEvent& event) override;
  void on_click(const ClickEvent& event) override;

 private:
  std::string label_;
  ClickHandler on_click_;
  ButtonTone tone_ = ButtonTone::Default;
  bool pointer_inside_ = false;
  bool pressed_ = false;
};

}  // namespace wds::interaction

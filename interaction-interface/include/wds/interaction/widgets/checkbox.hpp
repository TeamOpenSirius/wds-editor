#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>
#include <string>

namespace wds::interaction {

class Checkbox : public Widget {
 public:
  using ChangeHandler = std::function<void(bool)>;

  explicit Checkbox(std::string label = {});

  void set_label(std::string label) { label_ = std::move(label); }
  const std::string& label() const noexcept { return label_; }
  void set_checked(bool checked) noexcept { checked_ = checked; }
  bool checked() const noexcept { return checked_; }
  void on_change(ChangeHandler handler) { on_change_ = std::move(handler); }

  void paint(UiPainter& painter) const override;
  void paint_at(UiPainter& painter, float z) const;
  void on_click(const ClickEvent& event) override;

 private:
  std::string label_;
  bool checked_ = false;
  ChangeHandler on_change_;
};

}  // namespace wds::interaction

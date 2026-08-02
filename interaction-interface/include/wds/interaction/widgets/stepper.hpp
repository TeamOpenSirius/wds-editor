#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>

namespace wds::interaction {

class Stepper : public Widget {
 public:
  using ValueHandler = std::function<void(int)>;

  Stepper();

  void set_range(int min_value, int max_value) noexcept;
  void set_step(int step) noexcept;
  void set_value(int value) noexcept;
  int value() const noexcept { return value_; }

  void on_change(ValueHandler handler) { on_change_ = std::move(handler); }

  void layout(const Rect& parent_bounds) override;
  void paint(UiPainter& painter) const override;
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_click(const ClickEvent& event) override;

 private:
  void bump(int delta);

  int min_ = 0;
  int max_ = 100;
  int step_ = 1;
  int value_ = 0;
  Rect minus_bounds_{};
  Rect plus_bounds_{};
  ValueHandler on_change_;
};

class FloatStepper : public Widget {
 public:
  using ValueHandler = std::function<void(double)>;

  void set_range(double min_value, double max_value) noexcept;
  void set_step(double step) noexcept;
  void set_value(double value) noexcept;
  double value() const noexcept { return value_; }
  void on_change(ValueHandler handler) { on_change_ = std::move(handler); }

  void layout(const Rect& parent_bounds) override;
  void paint(UiPainter& painter) const override;
  void on_pointer_down(const PointerDownEvent& event) override;

 private:
  void bump(double delta);

  double min_ = 0.0;
  double max_ = 100.0;
  double step_ = 0.5;
  double value_ = 0.0;
  Rect minus_bounds_{};
  Rect plus_bounds_{};
  ValueHandler on_change_;
};

}  // namespace wds::interaction

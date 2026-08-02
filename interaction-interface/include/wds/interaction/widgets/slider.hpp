#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>

namespace wds::interaction {

enum class SliderMode { Interactive, ReadOnly };

class Slider : public Widget {
 public:
  using ValueHandler = std::function<void(float)>;

  Slider();

  void set_mode(SliderMode mode) noexcept { mode_ = mode; }
  SliderMode mode() const noexcept { return mode_; }

  void set_range(float min_value, float max_value) noexcept;
  void set_value(float value) noexcept;
  float value() const noexcept { return value_; }
  float min_value() const noexcept { return min_; }
  float max_value() const noexcept { return max_; }
  bool is_dragging() const noexcept { return dragging_; }

  using DragHandler = std::function<void()>;

  void on_change(ValueHandler handler) { on_change_ = std::move(handler); }
  void on_drag_begin(DragHandler handler) { on_drag_begin_ = std::move(handler); }
  void on_drag_end(DragHandler handler) { on_drag_end_ = std::move(handler); }

  void paint(UiPainter& painter) const override;
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_pointer_up(const PointerUpEvent& event) override;
  void on_pointer_move(const PointerMoveEvent& event) override;

 private:
  void set_from_position(Vec2 position);

  SliderMode mode_ = SliderMode::Interactive;
  float min_ = 0.0f;
  float max_ = 1.0f;
  float value_ = 0.0f;
  bool dragging_ = false;
  ValueHandler on_change_;
  DragHandler on_drag_begin_;
  DragHandler on_drag_end_;
};

}  // namespace wds::interaction

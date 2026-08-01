#pragma once

#include "events.hpp"

namespace wds::interaction {

enum class SwipeDirection { None, Up, Down, Left, Right };

// Default matches editor placement (kEditorSwipeMinDistancePx).
inline constexpr float kDefaultSwipeMinDistancePx = 16.0f;

SwipeDirection classify_swipe(Vec2 start, Vec2 end,
                              float min_distance_px = kDefaultSwipeMinDistancePx,
                              float max_angle_deg_from_axis = 35.0f);

// Axis lock for chart place gestures: once horizontal (L/R) or vertical-up is
// acquired, only that axis feeds classification. The same per-axis threshold
// both arms the gesture and clears the lock (editor: horizontal ≥ half a lane,
// vertical = kEditorSwipeMinDistancePx). After unlock, classification runs in
// the same update so another axis can arm immediately.
enum class PlaceAxisLock { None, Horizontal, Vertical };

class PlaceSwipeTracker {
 public:
  void reset() noexcept;
  PlaceAxisLock axis_lock() const noexcept { return lock_; }
  SwipeDirection last_swipe() const noexcept { return last_; }

  // dx = current.x - origin_x (press point); dy = current.y - origin_y
  // (placed note's time → screen Y). Axes may use different references.
  SwipeDirection update(float origin_x, float origin_y, Vec2 current,
                        float horizontal_min_px = kDefaultSwipeMinDistancePx,
                        float vertical_min_px = kDefaultSwipeMinDistancePx) noexcept;

 private:
  PlaceAxisLock lock_ = PlaceAxisLock::None;
  SwipeDirection last_ = SwipeDirection::None;
};

class GestureTracker {
 public:
  void set_min_drag_distance(float px) noexcept { min_drag_distance_px_ = px; }

  void on_pointer_down(const PointerDownEvent& event) noexcept;
  void on_pointer_move(const PointerMoveEvent& event) noexcept;
  void on_pointer_up(const PointerUpEvent& event) noexcept;

  bool is_dragging() const noexcept { return dragging_; }
  bool is_click() const noexcept { return released_ && !dragging_; }
  SwipeDirection swipe() const noexcept { return swipe_; }
  Vec2 start() const noexcept { return start_; }
  Vec2 current() const noexcept { return current_; }

 private:
  bool pressed_ = false;
  bool dragging_ = false;
  bool released_ = false;
  float min_drag_distance_px_ = kDefaultSwipeMinDistancePx;
  Vec2 start_{};
  Vec2 current_{};
  SwipeDirection swipe_ = SwipeDirection::None;
};

}  // namespace wds::interaction

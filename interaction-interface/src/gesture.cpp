#include "wds/interaction/gesture.hpp"

#include <cmath>

namespace wds::interaction {

SwipeDirection classify_swipe(Vec2 start, Vec2 end, float min_distance_px,
                              float max_angle_deg_from_axis) {
  const Vec2 delta = end - start;
  const float distance = std::hypot(delta.x, delta.y);
  if (distance < min_distance_px) {
    return SwipeDirection::None;
  }
  const float angle_limit = std::tan(max_angle_deg_from_axis * 3.14159265358979323846f / 180.0f);
  if (std::abs(delta.y) <= std::abs(delta.x) * angle_limit) {
    return delta.x < 0.0f ? SwipeDirection::Left : SwipeDirection::Right;
  }
  if (std::abs(delta.x) <= std::abs(delta.y) * angle_limit) {
    return delta.y < 0.0f ? SwipeDirection::Up : SwipeDirection::Down;
  }
  return SwipeDirection::None;
}

void PlaceSwipeTracker::reset() noexcept {
  lock_ = PlaceAxisLock::None;
  last_ = SwipeDirection::None;
}

SwipeDirection PlaceSwipeTracker::update(float origin_x, float origin_y, Vec2 current,
                                         float horizontal_min_px,
                                         float vertical_min_px) noexcept {
  if (horizontal_min_px < 1.0f) horizontal_min_px = 1.0f;
  if (vertical_min_px < 1.0f) vertical_min_px = 1.0f;
  const Vec2 delta{current.x - origin_x, current.y - origin_y};
  // One rule: drop lock when the locked axis falls below its own threshold.
  if (lock_ == PlaceAxisLock::Horizontal && std::abs(delta.x) < horizontal_min_px) {
    lock_ = PlaceAxisLock::None;
  } else if (lock_ == PlaceAxisLock::Vertical && std::abs(delta.y) < vertical_min_px) {
    lock_ = PlaceAxisLock::None;
  }

  if (lock_ == PlaceAxisLock::Horizontal) {
    last_ = delta.x < 0.0f ? SwipeDirection::Left : SwipeDirection::Right;
    return last_;
  }
  if (lock_ == PlaceAxisLock::Vertical) {
    last_ = delta.y < 0.0f ? SwipeDirection::Up : SwipeDirection::Down;
    return last_;
  }

  // Unlocked: arm with per-axis thresholds (horizontal is typically half a lane).
  constexpr float kAngleTan = 0.7002075382f;  // tan(35°)
  const bool horizontal_ok =
      std::abs(delta.x) >= horizontal_min_px && std::abs(delta.y) <= std::abs(delta.x) * kAngleTan;
  const bool vertical_ok =
      std::abs(delta.y) >= vertical_min_px && std::abs(delta.x) <= std::abs(delta.y) * kAngleTan;
  if (horizontal_ok && (!vertical_ok || std::abs(delta.x) >= std::abs(delta.y))) {
    last_ = delta.x < 0.0f ? SwipeDirection::Left : SwipeDirection::Right;
    lock_ = PlaceAxisLock::Horizontal;
    return last_;
  }
  if (vertical_ok) {
    last_ = delta.y < 0.0f ? SwipeDirection::Up : SwipeDirection::Down;
    if (last_ == SwipeDirection::Up) {
      lock_ = PlaceAxisLock::Vertical;
    }
    return last_;
  }
  last_ = SwipeDirection::None;
  return last_;
}

void GestureTracker::on_pointer_down(const PointerDownEvent& event) noexcept {
  pressed_ = true;
  dragging_ = false;
  released_ = false;
  swipe_ = SwipeDirection::None;
  start_ = current_ = event.position;
}

void GestureTracker::on_pointer_move(const PointerMoveEvent& event) noexcept {
  if (!pressed_) {
    return;
  }
  current_ = event.position;
  const Vec2 delta = current_ - start_;
  dragging_ = dragging_ || std::hypot(delta.x, delta.y) >= min_drag_distance_px_;
}

void GestureTracker::on_pointer_up(const PointerUpEvent& event) noexcept {
  if (!pressed_) {
    return;
  }
  current_ = event.position;
  swipe_ = classify_swipe(start_, current_, min_drag_distance_px_);
  const Vec2 delta = current_ - start_;
  dragging_ = dragging_ || std::hypot(delta.x, delta.y) >= min_drag_distance_px_;
  pressed_ = false;
  released_ = true;
}

}  // namespace wds::interaction

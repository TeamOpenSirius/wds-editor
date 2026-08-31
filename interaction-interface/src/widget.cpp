#include "wds/interaction/widget.hpp"

#include "wds/interaction/theme.hpp"
#include "wds/interaction/widget_root.hpp"

#include <wds/common/crash_input_journal.hpp>

#include <algorithm>
#include <cmath>

namespace wds::interaction {

namespace {
constexpr float kPressLerpSpeed = 12.0f;
}

void Widget::set_visible(bool v) noexcept {
  if (visible_ == v) {
    return;
  }
  visible_ = v;
  if (!v) {
    // Hidden focusables must not keep capturing keys / showing tooltips.
    for (Widget* p = this; p != nullptr; p = p->parent_) {
      if (WidgetRoot* root = p->as_root()) {
        root->clear_focus_if(this);
        break;
      }
    }
  }
}

Widget& Widget::add_child(std::unique_ptr<Widget> child) {
  child->parent_ = this;
  children_.push_back(std::move(child));
  return *children_.back();
}

WidgetRoot* Widget::find_root() noexcept {
  Widget* p = this;
  while (p->parent_ != nullptr) {
    p = p->parent_;
  }
  return p->as_root();
}

Rect Widget::absolute_bounds() const {
  float x = bounds_.x;
  float y = bounds_.y;
  for (const Widget* p = parent_; p != nullptr; p = p->parent_) {
    x += p->bounds_.x;
    y += p->bounds_.y;
  }
  return {x, y, bounds_.w, bounds_.h};
}

void Widget::layout(const Rect& /*parent_bounds*/) {
  for (auto& child : children_) {
    child->layout(absolute_bounds());
  }
}

void Widget::update(float delta_seconds) {
  const float target = (visual_state_ == WidgetState::Pressed) ? 1.0f : 0.0f;
  press_anim_ += (target - press_anim_) * std::min(1.0f, delta_seconds * kPressLerpSpeed);
  for (auto& child : children_) {
    if (child->visible()) {
      child->update(delta_seconds);
    }
  }
}

void Widget::paint(UiPainter& painter) const {
  if (!visible_) {
    return;
  }
  for (const auto& child : children_) {
    child->paint(painter);
  }
}

void Widget::paint_popup_layer(UiPainter& /*painter*/) const {}

void Widget::paint_popup_layers(UiPainter& painter) const {
  if (!visible_) {
    return;
  }
  for (const auto& child : children_) {
    child->paint_popup_layers(painter);
  }
  paint_popup_layer(painter);
}

bool Widget::blocks_interaction_behind(Vec2 point) const {
  return is_interaction_modal() && visible_ && enabled_ && absolute_bounds().contains(point);
}

Widget* Widget::hit_test_popup(Vec2 point) {
  if (!visible_) {
    return nullptr;
  }
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test_popup(point)) {
      return hit;
    }
    // Modal scrim / panel owns this point — do not hit menus on widgets behind it.
    if ((*it)->blocks_interaction_behind(point)) {
      return nullptr;
    }
  }
  return nullptr;
}

Widget* Widget::hit_test_popup_host(Vec2 point) {
  if (!visible_ || !enabled_) {
    return nullptr;
  }
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test_popup_host(point)) {
      return hit;
    }
    if ((*it)->blocks_interaction_behind(point)) {
      return nullptr;
    }
  }
  return nullptr;
}

Widget* Widget::hit_test(Vec2 point) {
  if (!visible_ || !enabled_) {
    return nullptr;
  }
  // Open menus capture input even when they overlap later siblings.
  if (Widget* hit = hit_test_popup(point)) {
    return hit;
  }
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test(point)) {
      return hit;
    }
  }
  return absolute_bounds().contains(point) ? this : nullptr;
}

bool Widget::dismiss_popups(Vec2 point) {
  bool closed = false;
  for (auto& child : children_) {
    closed = child->dismiss_popups(point) || closed;
  }
  return closed;
}

void Widget::close_popups() {
  close_own_popup();
  for (auto& child : children_) {
    child->close_popups();
  }
}

void Widget::close_popups_except(Widget* keep) {
  if (this == keep) {
    return;
  }
  close_own_popup();
  for (auto& child : children_) {
    child->close_popups_except(keep);
  }
}

void Widget::close_sibling_popups() {
  Widget* root = this;
  while (root->parent_ != nullptr) {
    root = root->parent_;
  }
  root->close_popups_except(this);
}

float Widget::interaction_scale() const noexcept {
  const float t = press_anim_;
  return 1.0f - (1.0f - theme::kPressedScale) * t;
}

void Widget::on_pointer_down(const PointerDownEvent&) {}
void Widget::on_pointer_up(const PointerUpEvent&) {}
void Widget::on_pointer_move(const PointerMoveEvent&) {}
void Widget::on_click(const ClickEvent&) {}
void Widget::on_double_click(const DoubleClickEvent&) {}
void Widget::on_scroll(const ScrollEvent&) {}
void Widget::on_key_down(const KeyDownEvent&) {}
void Widget::on_key_up(const KeyUpEvent&) {}
void Widget::on_text_input(const TextInputEvent&) {}

namespace {

template <typename Fn>
void dispatch_with_snap(Widget& widget, wds::common::CrashHandlerId handler, Fn&& invoke) {
  using wds::common::journal_diff_snap;
  using wds::common::journal_set_handler;
  journal_set_handler(handler);
  wds::common::CrashTraceSnap before;
  widget.trace_snapshot(before);
  invoke();
  wds::common::CrashTraceSnap after;
  widget.trace_snapshot(after);
  journal_diff_snap(before, after);
}

}  // namespace

void Widget::dispatch_on_pointer_down(const PointerDownEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::PointerDown,
                     [&] { on_pointer_down(event); });
}

void Widget::dispatch_on_pointer_up(const PointerUpEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::PointerUp,
                     [&] { on_pointer_up(event); });
}

void Widget::dispatch_on_pointer_move(const PointerMoveEvent& event) {
  wds::common::journal_note_move(event.position.x, event.position.y, trace_drag_mode());
  on_pointer_move(event);
}

void Widget::dispatch_on_click(const ClickEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::Click, [&] { on_click(event); });
}

void Widget::dispatch_on_double_click(const DoubleClickEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::DoubleClick,
                     [&] { on_double_click(event); });
}

void Widget::dispatch_on_scroll(const ScrollEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::Scroll, [&] { on_scroll(event); });
}

void Widget::dispatch_on_key_down(const KeyDownEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::KeyDown, [&] { on_key_down(event); });
}

void Widget::dispatch_on_key_up(const KeyUpEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::KeyUp, [&] { on_key_up(event); });
}

void Widget::dispatch_on_text_input(const TextInputEvent& event) {
  dispatch_with_snap(*this, wds::common::CrashHandlerId::TextInput,
                     [&] { on_text_input(event); });
}

}  // namespace wds::interaction

#include "wds/interaction/widget_root.hpp"

#include "wds/interaction/theme.hpp"

namespace wds::interaction {

WidgetRoot::WidgetRoot() = default;

void WidgetRoot::set_focus(Widget* widget) {
  if (focused_ == widget) {
    return;
  }
  if (focused_ != nullptr) {
    focused_->on_blur();
    focused_->set_visual_state(WidgetState::Normal);
  }
  focused_ = widget;
  if (focused_ != nullptr) {
    focused_->set_visual_state(WidgetState::Focused);
    focused_->on_focus();
  }
}

void WidgetRoot::clear_focus() { set_focus(nullptr); }

void WidgetRoot::clear_focus_if(Widget* widget) {
  if (focused_ == widget) {
    clear_focus();
  }
}

Widget* WidgetRoot::hit_test(Vec2 point) {
  if (!visible_) {
    return nullptr;
  }
  if (!bounds_.contains(point)) {
    return nullptr;
  }
  if (Widget* hit = hit_test_popup(point)) {
    return hit;
  }
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test(point)) {
      return hit;
    }
  }
  return this;
}

void WidgetRoot::update_hover(Vec2 point) {
  Widget* hit = hit_test(point);
  if (hover_ != hit) {
    // Notify leave before clearing hover chrome (skip while pointer-captured —
    // the captured widget still receives moves outside its bounds).
    if (hover_ != nullptr && hover_ != capture_) {
      hover_->on_hover_leave();
      // Never clobber keyboard focus just because the pointer left the field.
      if (hover_ != focused_) {
        hover_->set_visual_state(WidgetState::Normal);
      }
    }
    hover_ = hit;
    if (hover_ != nullptr && hover_ != capture_ && hover_ != focused_) {
      hover_->set_visual_state(WidgetState::Hovered);
    }
  }
  active_tooltip_ = (hover_ != nullptr && hover_->visible() && !hover_->tooltip().empty())
                        ? hover_->tooltip()
                        : "";
}

void WidgetRoot::dispatch_event(const InputEvent& event, ShortcutManager* shortcuts) {
  if (focused_ != nullptr && !focused_->visible()) {
    clear_focus();
  }
  if (std::holds_alternative<KeyDownEvent>(event) && shortcuts != nullptr) {
    // Focused shortcut/text capture widgets must see keys before global chords.
    const bool capture_keys =
        focused_ != nullptr && focused_->visible() && focused_->captures_keys();
    if (!capture_keys && shortcuts->dispatch(std::get<KeyDownEvent>(event))) {
      return;
    }
  }

  Widget* target = capture_ != nullptr ? capture_ : nullptr;

  if (std::holds_alternative<PointerDownEvent>(event)) {
    const auto& e = std::get<PointerDownEvent>(event);
    // Resolve popup hit before dismiss so menu-item clicks still target the owner.
    Widget* popup_hit = hit_test_popup(e.position);
    for (auto& child : children_) {
      child->dismiss_popups(e.position);
    }

    // Outside dismiss: fall through to whatever is under the cursor.
    // Menu-item press: keep targeting the popup owner so Click does not hit-through.
    Widget* target = popup_hit != nullptr ? popup_hit : hit_test(e.position);

    // Focus sticks until Enter blur or an explicit click elsewhere.
    if (target != nullptr && target->wants_focus()) {
      set_focus(target);
    } else {
      clear_focus();
    }
    capture_ = target;
    press_target_ = target;
    update_hover(e.position);
    if (target != nullptr) {
      target->on_pointer_down(e);
    }
    return;
  }

  if (std::holds_alternative<PointerUpEvent>(event)) {
    const auto& e = std::get<PointerUpEvent>(event);
    target = capture_ != nullptr ? capture_ : hit_test(e.position);
    if (target != nullptr) {
      target->on_pointer_up(e);
    }
    capture_ = nullptr;
    update_hover(e.position);
    return;
  }

  if (std::holds_alternative<PointerMoveEvent>(event)) {
    const auto& e = std::get<PointerMoveEvent>(event);
    update_hover(e.position);
    target = capture_ != nullptr ? capture_ : hover_;
    if (target != nullptr) {
      target->on_pointer_move(e);
    }
    return;
  }

  if (std::holds_alternative<ClickEvent>(event)) {
    const auto& e = std::get<ClickEvent>(event);
    // Deliver to the press target — fresh hit-test would click through after a
    // menu-item selection closed the popup on pointer-down.
    target = press_target_;
    press_target_ = nullptr;
    if (target != nullptr) {
      target->on_click(e);
    }
    return;
  }

  if (std::holds_alternative<DoubleClickEvent>(event)) {
    const auto& e = std::get<DoubleClickEvent>(event);
    // Do not clear press_target_: GlfwInputAdapter emits DoubleClick then Click
    // on the same release. Clearing here would drop the Click (buttons only
    // handle on_click), so rapid re-clicks appeared dead.
    target = press_target_;
    if (target != nullptr) {
      target->on_double_click(e);
    }
    return;
  }

  if (std::holds_alternative<ScrollEvent>(event)) {
    const auto& e = std::get<ScrollEvent>(event);
    target = hit_test(e.position);
    if (target != nullptr) {
      target->on_scroll(e);
    }
    return;
  }

  if (std::holds_alternative<KeyDownEvent>(event)) {
    const auto& e = std::get<KeyDownEvent>(event);
    if (focused_ != nullptr) {
      focused_->on_key_down(e);
      // Enter / Esc drop Focused — run the same blur path as clicking away.
      if (focused_ != nullptr && focused_->visual_state() != WidgetState::Focused) {
        Widget* was = focused_;
        focused_ = nullptr;
        was->on_blur();
      }
    }
    return;
  }

  if (std::holds_alternative<KeyUpEvent>(event)) {
    const auto& e = std::get<KeyUpEvent>(event);
    if (focused_ != nullptr) {
      focused_->on_key_up(e);
    }
    return;
  }

  if (std::holds_alternative<TextInputEvent>(event)) {
    const auto& e = std::get<TextInputEvent>(event);
    if (focused_ != nullptr) {
      focused_->on_text_input(e);
    }
  }
}

void WidgetRoot::process_frame(float delta_seconds, const std::vector<InputEvent>& events,
                               ShortcutManager* shortcuts) {
  if (focused_ != nullptr && !focused_->visible()) {
    clear_focus();
  }
  if (hover_ != nullptr && !hover_->visible()) {
    hover_ = nullptr;
    active_tooltip_.clear();
  }
  for (const auto& event : events) {
    dispatch_event(event, shortcuts);
  }
  Widget::update(delta_seconds);
}

void WidgetRoot::paint(UiPainter& painter) const {
  if (!visible_) {
    return;
  }
  // Do not fill the full window: the preview stage shares this framebuffer, and an
  // opaque root rect would cover all skin sprites (depth write is off).
  // Popup menus are painted in a separate post-overlay batch so they stay above
  // skinned note sprites (UiPainter rects would otherwise lose to later sprites).
  Widget::paint(painter);

  if (!active_tooltip_.empty() && hover_ != nullptr && hover_->visible() &&
      !hover_->paints_inline_tooltip()) {
    const float tip_px = theme::kFontSizeTooltip;
    const Vec2 text_size = painter.measure_text(active_tooltip_, tip_px);
    const float tip_h = tip_px + 14.0f;
    const Rect tip_bounds{hover_->absolute_bounds().x,
                          hover_->absolute_bounds().y - tip_h - 6.0f, text_size.x + 16.0f, tip_h};
    painter.fill_rect(tip_bounds, theme::kSurfaceVariant, theme::kCornerRadiusSm, 0.95f);
    painter.text({tip_bounds.x + 8.0f, tip_bounds.y + 7.0f, tip_bounds.w - 8.0f, tip_bounds.h},
                 active_tooltip_, theme::kOnSurface, 0.96f, tip_px);
  }
}

}  // namespace wds::interaction

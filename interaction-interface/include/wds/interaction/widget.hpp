#pragma once

#include "events.hpp"
#include "types.hpp"
#include "ui_painter.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wds::interaction {

enum class WidgetState : uint8_t {
  Normal,
  Hovered,
  Pressed,
  Focused,
  Disabled,
};

class Widget {
 public:
  virtual ~Widget() = default;

  const std::string& id() const noexcept { return id_; }
  void set_id(std::string id) { id_ = std::move(id); }

  Rect& bounds() noexcept { return bounds_; }
  const Rect& bounds() const noexcept { return bounds_; }
  void set_bounds(Rect bounds) noexcept { bounds_ = bounds; }

  bool visible() const noexcept { return visible_; }
  void set_visible(bool v) noexcept { visible_ = v; }

  bool enabled() const noexcept { return enabled_; }
  void set_enabled(bool e) noexcept { enabled_ = e; }

  const std::string& tooltip() const noexcept { return tooltip_; }
  void set_tooltip(std::string tip) { tooltip_ = std::move(tip); }

  WidgetState visual_state() const noexcept { return visual_state_; }

  Widget* parent() noexcept { return parent_; }
  const Widget* parent() const noexcept { return parent_; }

  const std::vector<std::unique_ptr<Widget>>& children() const noexcept { return children_; }

  Widget& add_child(std::unique_ptr<Widget> child);
  Rect absolute_bounds() const;

  virtual void layout(const Rect& parent_bounds);
  virtual void update(float delta_seconds);
  virtual void paint(UiPainter& painter) const;
  // Drawn after the full widget tree so open menus stay opaque on top of siblings.
  virtual void paint_popup_layer(UiPainter& painter) const;
  virtual void paint_popup_layers(UiPainter& painter) const;
  virtual Widget* hit_test(Vec2 point);
  // Open popup menus (dropdown lists) — tested before normal hit targets.
  virtual Widget* hit_test_popup(Vec2 point);
  // Close popups that do not contain `point`. Returns true if any popup closed.
  virtual bool dismiss_popups(Vec2 point);
  virtual bool wants_focus() const { return false; }
  virtual bool is_focusable() const { return false; }
  // When true, WidgetRoot routes KeyDown to this widget before ShortcutManager.
  virtual bool captures_keys() const { return false; }
  // When true, WidgetRoot skips the floating tooltip bubble (widget paints tip itself).
  virtual bool paints_inline_tooltip() const { return false; }

  virtual void on_pointer_down(const PointerDownEvent& event);
  virtual void on_pointer_up(const PointerUpEvent& event);
  virtual void on_pointer_move(const PointerMoveEvent& event);
  virtual void on_click(const ClickEvent& event);
  virtual void on_double_click(const DoubleClickEvent& event);
  virtual void on_scroll(const ScrollEvent& event);
  virtual void on_key_down(const KeyDownEvent& event);
  virtual void on_key_up(const KeyUpEvent& event);
  virtual void on_text_input(const TextInputEvent& event);
  // Called by WidgetRoot when keyboard focus is gained/lost.
  virtual void on_focus() {}
  virtual void on_blur() {}
  // Called by WidgetRoot when the hover target leaves this widget (not while captured).
  virtual void on_hover_leave() {}

  void set_visual_state(WidgetState state) noexcept { visual_state_ = state; }

 protected:
  float interaction_scale() const noexcept;

  std::string id_;
  Rect bounds_{};
  bool visible_ = true;
  bool enabled_ = true;
  WidgetState visual_state_ = WidgetState::Normal;
  std::string tooltip_;
  Widget* parent_ = nullptr;
  std::vector<std::unique_ptr<Widget>> children_;
  float press_anim_ = 0.0f;
};

}  // namespace wds::interaction

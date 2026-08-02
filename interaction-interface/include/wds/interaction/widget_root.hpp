#pragma once

#include "events.hpp"
#include "shortcuts.hpp"
#include "types.hpp"
#include <wds/interaction/widget.hpp>

#include <memory>
#include <string>
#include <vector>

namespace wds::interaction {

class WidgetRoot : public Widget {
 public:
  WidgetRoot();

  void set_bounds(const Rect& bounds) { bounds_ = bounds; }

  void process_frame(float delta_seconds, const std::vector<InputEvent>& events,
                     ShortcutManager* shortcuts = nullptr);
  void paint(UiPainter& painter) const override;

  Widget* widget_at(Vec2 point) { return hit_test(point); }

  Widget* focused_widget() noexcept { return focused_; }
  void set_focus(Widget* widget);
  void clear_focus();

  const std::string& active_tooltip() const noexcept { return active_tooltip_; }

 private:
  Widget* hit_test(Vec2 point) override;
  void dispatch_event(const InputEvent& event, ShortcutManager* shortcuts);
  void update_hover(Vec2 point);

  Widget* focused_ = nullptr;
  Widget* capture_ = nullptr;
  Widget* hover_ = nullptr;
  // Widget that received the current press; Click/DoubleClick go here to avoid
  // click-through after a popup closes on pointer-down (menu item selection).
  // Cleared only by Click — DoubleClick may precede Click on the same release.
  Widget* press_target_ = nullptr;
  std::string active_tooltip_;
};

}  // namespace wds::interaction

#pragma once
#include <QWindow>
#include <array>
#include "wds/interaction/events.hpp"

namespace wds::ui {

wds::interaction::KeyCode qt_key_code(int qt_key) noexcept;
wds::interaction::Modifiers qt_modifiers(Qt::KeyboardModifiers mods) noexcept;
// Host-side invert for timeline scrub. Option+wheel later undoes this so
// invert_visible_range_scroll stays independent.
void apply_scroll_invert(float& dx, float& dy) noexcept;
// OS-queried modifiers. Use this for mouse/wheel: macOS often drops Command
// from QMouseEvent::modifiers().
wds::interaction::Modifiers qt_live_modifiers() noexcept;
class QtInputAdapter final : public QObject {
 public:
  explicit QtInputAdapter(QWindow* window, wds::interaction::InputQueue& queue);
  bool eventFilter(QObject* watched, QEvent* event) override;
  void set_cursor(wds::interaction::CursorKind kind);
  wds::interaction::Vec2 pointer_logical() const noexcept { return pointer_; }
 private:
  wds::interaction::Modifiers mods(Qt::KeyboardModifiers) const noexcept;
  wds::interaction::PointerButton button(Qt::MouseButton) const noexcept;
  wds::interaction::KeyCode key(int) const noexcept;
  int button_index(wds::interaction::PointerButton) const noexcept;
  QWindow* window_;
  wds::interaction::InputQueue& queue_;
  wds::interaction::Vec2 pointer_;
  std::array<bool, 3> pending_double_click_{{false, false, false}};
};
}

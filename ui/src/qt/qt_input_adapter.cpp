#include "wds/ui/qt/qt_input_adapter.hpp"

#include "wds/interaction/editor_input.hpp"

#include <QEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace wds::ui {

QtInputAdapter::QtInputAdapter(QWindow* window, wds::interaction::InputQueue& queue)
    : window_(window), queue_(queue) {
  if (window_ != nullptr) window_->installEventFilter(this);
}

void QtInputAdapter::set_cursor(wds::interaction::CursorKind kind) {
  if (window_ == nullptr) return;
  switch (kind) {
    case wds::interaction::CursorKind::ResizeHorizontal: window_->setCursor(QCursor(Qt::SizeHorCursor)); break;
    case wds::interaction::CursorKind::ResizeVertical: window_->setCursor(QCursor(Qt::SizeVerCursor)); break;
    case wds::interaction::CursorKind::Default:
    default: window_->unsetCursor(); break;
  }
}

wds::interaction::Modifiers qt_modifiers(Qt::KeyboardModifiers value) noexcept {
  // Qt already unifies platforms: Command and Ctrl are ControlModifier, Option
  // and Alt are AltModifier, physical Control / Win is MetaModifier.
  return {value.testFlag(Qt::ShiftModifier), value.testFlag(Qt::ControlModifier),
          value.testFlag(Qt::AltModifier), value.testFlag(Qt::MetaModifier)};
}

wds::interaction::Modifiers qt_live_modifiers() noexcept {
  return qt_modifiers(QGuiApplication::queryKeyboardModifiers());
}

void apply_scroll_invert(float& dx, float& dy) noexcept {
  if (wds::interaction::invert_scroll_wheel()) {
    dx = -dx;
    dy = -dy;
  }
}

wds::interaction::KeyCode qt_key_code(int value) noexcept {
  using wds::interaction::KeyCode;
  if (value >= 0x20 && value <= 0x7e) return static_cast<KeyCode>(value);
  if (value >= Qt::Key_F1 && value <= Qt::Key_F12) {
    return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (value - Qt::Key_F1));
  }
  switch (value) {
    case Qt::Key_Escape: return KeyCode::Escape;
    case Qt::Key_Return:
    case Qt::Key_Enter: return KeyCode::Enter;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return KeyCode::Tab;
    case Qt::Key_Backspace: return KeyCode::Backspace;
    case Qt::Key_Insert: return KeyCode::Insert;
    case Qt::Key_Delete: return KeyCode::Delete;
    case Qt::Key_Left: return KeyCode::Left;
    case Qt::Key_Right: return KeyCode::Right;
    case Qt::Key_Up: return KeyCode::Up;
    case Qt::Key_Down: return KeyCode::Down;
    case Qt::Key_PageUp: return KeyCode::PageUp;
    case Qt::Key_PageDown: return KeyCode::PageDown;
    case Qt::Key_Home: return KeyCode::Home;
    case Qt::Key_End: return KeyCode::End;
    default: return KeyCode::Unknown;
  }
}

int key_code_to_qt(wds::interaction::KeyCode key) noexcept {
  using wds::interaction::KeyCode;
  const int v = static_cast<int>(key);
  if (v >= 0x20 && v <= 0x7e) return v;
  if (v >= static_cast<int>(KeyCode::F1) && v <= static_cast<int>(KeyCode::F12)) {
    return Qt::Key_F1 + (v - static_cast<int>(KeyCode::F1));
  }
  switch (key) {
    case KeyCode::Escape: return Qt::Key_Escape;
    case KeyCode::Enter: return Qt::Key_Return;
    case KeyCode::Tab: return Qt::Key_Tab;
    case KeyCode::Backspace: return Qt::Key_Backspace;
    case KeyCode::Insert: return Qt::Key_Insert;
    case KeyCode::Delete: return Qt::Key_Delete;
    case KeyCode::Left: return Qt::Key_Left;
    case KeyCode::Right: return Qt::Key_Right;
    case KeyCode::Up: return Qt::Key_Up;
    case KeyCode::Down: return Qt::Key_Down;
    case KeyCode::PageUp: return Qt::Key_PageUp;
    case KeyCode::PageDown: return Qt::Key_PageDown;
    case KeyCode::Home: return Qt::Key_Home;
    case KeyCode::End: return Qt::Key_End;
    default: return 0;
  }
}

wds::interaction::Modifiers QtInputAdapter::mods(Qt::KeyboardModifiers value) const noexcept {
  return qt_modifiers(value);
}

wds::interaction::PointerButton QtInputAdapter::button(Qt::MouseButton value) const noexcept {
  if (value == Qt::RightButton) return wds::interaction::PointerButton::Right;
  if (value == Qt::MiddleButton) return wds::interaction::PointerButton::Middle;
  return wds::interaction::PointerButton::Left;
}

int QtInputAdapter::button_index(wds::interaction::PointerButton value) const noexcept {
  if (value == wds::interaction::PointerButton::Right) return 1;
  if (value == wds::interaction::PointerButton::Middle) return 2;
  return 0;
}

wds::interaction::KeyCode QtInputAdapter::key(int value) const noexcept {
  return qt_key_code(value);
}

bool QtInputAdapter::eventFilter(QObject* watched, QEvent* event) {
  if (watched != window_ || event == nullptr) return QObject::eventFilter(watched, event);
  switch (event->type()) {
    case QEvent::MouseMove: {
      auto* mouse = static_cast<QMouseEvent*>(event);
      pointer_ = {static_cast<float>(mouse->position().x()), static_cast<float>(mouse->position().y())};
      queue_.push(wds::interaction::PointerMoveEvent{pointer_, mods(mouse->modifiers())});
      break;
    }
    case QEvent::MouseButtonPress: {
      auto* mouse = static_cast<QMouseEvent*>(event);
      pointer_ = {static_cast<float>(mouse->position().x()), static_cast<float>(mouse->position().y())};
      queue_.push(wds::interaction::PointerDownEvent{pointer_, button(mouse->button()), mods(mouse->modifiers())});
      break;
    }
    case QEvent::MouseButtonDblClick: {
      auto* mouse = static_cast<QMouseEvent*>(event);
      pointer_ = {static_cast<float>(mouse->position().x()), static_cast<float>(mouse->position().y())};
      const auto pointer_button = button(mouse->button());
      pending_double_click_[static_cast<std::size_t>(button_index(pointer_button))] = true;
      queue_.push(wds::interaction::DoubleClickEvent{pointer_, pointer_button, mods(mouse->modifiers())});
      break;
    }
    case QEvent::MouseButtonRelease: {
      auto* mouse = static_cast<QMouseEvent*>(event);
      pointer_ = {static_cast<float>(mouse->position().x()), static_cast<float>(mouse->position().y())};
      const auto pointer_button = button(mouse->button());
      const int index = button_index(pointer_button);
      const int click_count = pending_double_click_[static_cast<std::size_t>(index)] ? 2 : 1;
      pending_double_click_[static_cast<std::size_t>(index)] = false;
      const auto modifiers = mods(mouse->modifiers());
      queue_.push(wds::interaction::PointerUpEvent{pointer_, pointer_button, modifiers});
      queue_.push(wds::interaction::ClickEvent{pointer_, pointer_button, modifiers, click_count});
      break;
    }
    case QEvent::Wheel: {
      auto* wheel = static_cast<QWheelEvent*>(event);
      pointer_ = {static_cast<float>(wheel->position().x()), static_cast<float>(wheel->position().y())};
      float dx = static_cast<float>(wheel->angleDelta().x()) / 120.0f;
      float dy = static_cast<float>(wheel->angleDelta().y()) / 120.0f;
      apply_scroll_invert(dx, dy);
      queue_.push(wds::interaction::ScrollEvent{pointer_, dx, dy, mods(wheel->modifiers())});
      break;
    }
    case QEvent::KeyPress: {
      auto* key_event = static_cast<QKeyEvent*>(event);
      queue_.push(wds::interaction::KeyDownEvent{key(key_event->key()), mods(key_event->modifiers()), key_event->isAutoRepeat()});
      if (!key_event->isAutoRepeat() && !key_event->text().isEmpty())
        queue_.push(wds::interaction::TextInputEvent{key_event->text().toUtf8().toStdString()});
      break;
    }
    case QEvent::InputMethod: {
      auto* input = static_cast<QInputMethodEvent*>(event);
      if (!input->commitString().isEmpty())
        queue_.push(wds::interaction::TextInputEvent{input->commitString().toUtf8().toStdString()});
      break;
    }
    case QEvent::KeyRelease: {
      auto* key_event = static_cast<QKeyEvent*>(event);
      queue_.push(wds::interaction::KeyUpEvent{key(key_event->key()), mods(key_event->modifiers())});
      break;
    }
    case QEvent::FocusOut:
      pointer_ = {};
      pending_double_click_.fill(false);
      if (window_ != nullptr) window_->unsetCursor();
      break;
    default: break;
  }
  return false;
}

}  // namespace wds::ui

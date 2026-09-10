#include "wds/ui/qt/qt_input_adapter.hpp"

#include "wds/interaction/editor_input.hpp"

#include <QEvent>
#include <QCursor>
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

wds::interaction::Modifiers QtInputAdapter::mods(Qt::KeyboardModifiers value) const noexcept {
  return {value.testFlag(Qt::ShiftModifier), value.testFlag(Qt::ControlModifier),
          value.testFlag(Qt::AltModifier), value.testFlag(Qt::MetaModifier)};
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
  if (value >= Qt::Key_0 && value <= Qt::Key_9)
    return static_cast<wds::interaction::KeyCode>(48 + value - Qt::Key_0);
  if (value >= Qt::Key_A && value <= Qt::Key_Z)
    return static_cast<wds::interaction::KeyCode>(65 + value - Qt::Key_A);
  switch (value) {
    case Qt::Key_Space: return wds::interaction::KeyCode::Space;
    case Qt::Key_Escape: return wds::interaction::KeyCode::Escape;
    case Qt::Key_Return:
    case Qt::Key_Enter: return wds::interaction::KeyCode::Enter;
    case Qt::Key_Tab: return wds::interaction::KeyCode::Tab;
    case Qt::Key_Backspace: return wds::interaction::KeyCode::Backspace;
    case Qt::Key_Delete: return wds::interaction::KeyCode::Delete;
    case Qt::Key_Left: return wds::interaction::KeyCode::Left;
    case Qt::Key_Right: return wds::interaction::KeyCode::Right;
    case Qt::Key_Up: return wds::interaction::KeyCode::Up;
    case Qt::Key_Down: return wds::interaction::KeyCode::Down;
    case Qt::Key_F1: return wds::interaction::KeyCode::F1;
    case Qt::Key_F2: return wds::interaction::KeyCode::F2;
    case Qt::Key_F3: return wds::interaction::KeyCode::F3;
    case Qt::Key_F4: return wds::interaction::KeyCode::F4;
    case Qt::Key_F11: return wds::interaction::KeyCode::F11;
    case Qt::Key_Period: return static_cast<wds::interaction::KeyCode>('.');
    default: return wds::interaction::KeyCode::Unknown;
  }
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
      if (wds::interaction::invert_scroll_wheel()) { dx = -dx; dy = -dy; }
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

#include "wds/interaction/glfw_input_adapter.hpp"
#include "wds/interaction/editor_input.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// GLFW 3.4 renamed/added resize cursors; map to 3.3 equivalents when missing.
#ifndef GLFW_RESIZE_EW_CURSOR
#define GLFW_RESIZE_EW_CURSOR GLFW_HRESIZE_CURSOR
#endif
#ifndef GLFW_RESIZE_NS_CURSOR
#define GLFW_RESIZE_NS_CURSOR GLFW_VRESIZE_CURSOR
#endif

#include <cmath>

namespace wds::interaction {

namespace {

Modifiers from_glfw_mods(int mods) {
  Modifiers m;
  m.shift = (mods & GLFW_MOD_SHIFT) != 0;
  m.control = (mods & GLFW_MOD_CONTROL) != 0;
  m.alt = (mods & GLFW_MOD_ALT) != 0;
  m.super = (mods & GLFW_MOD_SUPER) != 0;
  return m;
}

Modifiers mods_from_glfw_keys(GLFWwindow* window) {
  Modifiers m;
  m.shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
  m.control = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
              glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
  m.alt = glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
          glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
  m.super = glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
  return m;
}

int click_track_index(PointerButton button) noexcept {
  switch (button) {
    case PointerButton::Right:
      return 1;
    case PointerButton::Middle:
      return 2;
    case PointerButton::Left:
    default:
      return 0;
  }
}

KeyCode from_glfw_key(int key) {
  if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
    return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + (key - GLFW_KEY_0));
  }
  if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
    return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (key - GLFW_KEY_A));
  }
  switch (key) {
    case GLFW_KEY_SPACE:
      return KeyCode::Space;
    case GLFW_KEY_ESCAPE:
      return KeyCode::Escape;
    case GLFW_KEY_ENTER:
      return KeyCode::Enter;
    case GLFW_KEY_TAB:
      return KeyCode::Tab;
    case GLFW_KEY_BACKSPACE:
      return KeyCode::Backspace;
    case GLFW_KEY_DELETE:
      return KeyCode::Delete;
    case GLFW_KEY_LEFT:
      return KeyCode::Left;
    case GLFW_KEY_RIGHT:
      return KeyCode::Right;
    case GLFW_KEY_UP:
      return KeyCode::Up;
    case GLFW_KEY_DOWN:
      return KeyCode::Down;
    case GLFW_KEY_F1:
      return KeyCode::F1;
    case GLFW_KEY_F2:
      return KeyCode::F2;
    case GLFW_KEY_F3:
      return KeyCode::F3;
    case GLFW_KEY_F4:
      return KeyCode::F4;
    case GLFW_KEY_F11:
      return KeyCode::F11;
    case GLFW_KEY_PERIOD:
      return static_cast<KeyCode>(46);  // '.' — forbidden as shortcut key
    default:
      return KeyCode::Unknown;
  }
}

PointerButton from_glfw_button(int button) {
  switch (button) {
    case GLFW_MOUSE_BUTTON_RIGHT:
      return PointerButton::Right;
    case GLFW_MOUSE_BUTTON_MIDDLE:
      return PointerButton::Middle;
    default:
      return PointerButton::Left;
  }
}

GlfwInputAdapter* adapter_from(GLFWwindow* window) {
  return static_cast<GlfwInputAdapter*>(glfwGetWindowUserPointer(window));
}

}  // namespace

GlfwInputAdapter::GlfwInputAdapter(GLFWwindow* window) : window_(window) {}

GlfwInputAdapter::~GlfwInputAdapter() {
  // Prefer destroy via detach() before glfwTerminate(); do not call GLFW here.
  cursor_ew_ = nullptr;
  cursor_ns_ = nullptr;
}

void GlfwInputAdapter::destroy_cursors() {
  if (cursor_ew_ != nullptr) {
    glfwDestroyCursor(static_cast<GLFWcursor*>(cursor_ew_));
    cursor_ew_ = nullptr;
  }
  if (cursor_ns_ != nullptr) {
    glfwDestroyCursor(static_cast<GLFWcursor*>(cursor_ns_));
    cursor_ns_ = nullptr;
  }
  cursor_ = CursorKind::Default;
}

void GlfwInputAdapter::set_cursor(CursorKind kind) {
  if (window_ == nullptr || kind == cursor_) {
    return;
  }
  cursor_ = kind;
  switch (kind) {
    case CursorKind::ResizeHorizontal:
      if (cursor_ew_ == nullptr) {
        cursor_ew_ = glfwCreateStandardCursor(GLFW_RESIZE_EW_CURSOR);
      }
      glfwSetCursor(window_, static_cast<GLFWcursor*>(cursor_ew_));
      break;
    case CursorKind::ResizeVertical:
      if (cursor_ns_ == nullptr) {
        cursor_ns_ = glfwCreateStandardCursor(GLFW_RESIZE_NS_CURSOR);
      }
      glfwSetCursor(window_, static_cast<GLFWcursor*>(cursor_ns_));
      break;
    case CursorKind::Default:
    default:
      glfwSetCursor(window_, nullptr);
      break;
  }
}

void GlfwInputAdapter::attach() {
  if (window_ == nullptr) {
    return;
  }
  glfwSetWindowUserPointer(window_, this);
  // Seed pointer in logical (window) coordinates — matches WidgetRoot layout.
  double cx = 0.0;
  double cy = 0.0;
  glfwGetCursorPos(window_, &cx, &cy);
  pointer_ = to_logical(cx, cy);
  glfwSetCursorPosCallback(window_, &GlfwInputAdapter::pointer_pos_callback);
  glfwSetMouseButtonCallback(window_, &GlfwInputAdapter::mouse_button_callback);
  glfwSetScrollCallback(window_, &GlfwInputAdapter::scroll_callback);
  glfwSetKeyCallback(window_, &GlfwInputAdapter::key_callback);
  glfwSetCharCallback(window_, &GlfwInputAdapter::char_callback);
}

void GlfwInputAdapter::detach() {
  if (window_ == nullptr) {
    return;
  }
  glfwSetCursor(window_, nullptr);
  cursor_ = CursorKind::Default;
  destroy_cursors();
  glfwSetWindowUserPointer(window_, nullptr);
  glfwSetCursorPosCallback(window_, nullptr);
  glfwSetMouseButtonCallback(window_, nullptr);
  glfwSetScrollCallback(window_, nullptr);
  glfwSetKeyCallback(window_, nullptr);
  glfwSetCharCallback(window_, nullptr);
}

void GlfwInputAdapter::pointer_pos_callback(GLFWwindow* window, double x, double y) {
  auto* self = adapter_from(window);
  if (self == nullptr) {
    return;
  }
  self->pointer_ = self->to_logical(x, y);
  self->queue_.push(PointerMoveEvent{self->pointer_, self->mods_});
}

void GlfwInputAdapter::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
  auto* self = adapter_from(window);
  if (self == nullptr) {
    return;
  }
  self->mods_ = from_glfw_mods(mods);
  // Refresh from GLFW in case press arrives before a move callback.
  double cx = 0.0;
  double cy = 0.0;
  glfwGetCursorPos(window, &cx, &cy);
  self->pointer_ = self->to_logical(cx, cy);
  const auto pb = from_glfw_button(button);
  if (action == GLFW_PRESS) {
    self->queue_.push(PointerDownEvent{self->pointer_, pb, self->mods_});
  } else if (action == GLFW_RELEASE) {
    self->queue_.push(PointerUpEvent{self->pointer_, pb, self->mods_});

    const double now = glfwGetTime();
    // Fixed slop in logical pixels (matches layout space on all DPI).
    constexpr float kClickSlopLogical = 6.0f;
    ClickTrack& track = self->click_track_[click_track_index(pb)];
    const bool double_click =
        (now - track.last_click_time) < 0.35 &&
        std::hypot(self->pointer_.x - track.last_click_pos.x,
                   self->pointer_.y - track.last_click_pos.y) < kClickSlopLogical;
    if (double_click) {
      track.click_count = 2;
      self->queue_.push(DoubleClickEvent{self->pointer_, pb, self->mods_});
    } else {
      track.click_count = 1;
    }
    self->queue_.push(ClickEvent{self->pointer_, pb, self->mods_, track.click_count});
    track.last_click_time = now;
    track.last_click_pos = self->pointer_;
  }
}

void GlfwInputAdapter::scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
  auto* self = adapter_from(window);
  if (self == nullptr) {
    return;
  }
  double cx = 0.0;
  double cy = 0.0;
  glfwGetCursorPos(window, &cx, &cy);
  self->pointer_ = to_logical(cx, cy);
  // Scroll callbacks do not carry modifier bits — sample keys so Option+wheel etc. work.
  self->mods_ = mods_from_glfw_keys(window);
  float dx = static_cast<float>(xoffset);
  float dy = static_cast<float>(yoffset);
  if (invert_scroll_wheel()) {
    dx = -dx;
    dy = -dy;
  }
  self->queue_.push(ScrollEvent{self->pointer_, dx, dy, self->mods_});
}

void GlfwInputAdapter::key_callback(GLFWwindow* window, int key, int, int action, int mods) {
  auto* self = adapter_from(window);
  if (self == nullptr) {
    return;
  }
  self->mods_ = from_glfw_mods(mods);
  const auto code = from_glfw_key(key);
  if (action == GLFW_PRESS || action == GLFW_REPEAT) {
    self->queue_.push(KeyDownEvent{code, self->mods_, action == GLFW_REPEAT});
  } else if (action == GLFW_RELEASE) {
    self->queue_.push(KeyUpEvent{code, self->mods_});
  }
}

void GlfwInputAdapter::char_callback(GLFWwindow* window, unsigned int codepoint) {
  auto* self = adapter_from(window);
  if (self == nullptr) {
    return;
  }
  std::string text;
  if (codepoint <= 0x7f) {
    text.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    text = {static_cast<char>(0xc0 | (codepoint >> 6)),
            static_cast<char>(0x80 | (codepoint & 0x3f))};
  } else if (codepoint <= 0xffff) {
    text = {static_cast<char>(0xe0 | (codepoint >> 12)),
            static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)),
            static_cast<char>(0x80 | (codepoint & 0x3f))};
  } else if (codepoint <= 0x10ffff) {
    text = {static_cast<char>(0xf0 | (codepoint >> 18)),
            static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)),
            static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)),
            static_cast<char>(0x80 | (codepoint & 0x3f))};
  }
  if (!text.empty()) self->queue_.push(TextInputEvent{std::move(text)});
}

}  // namespace wds::interaction

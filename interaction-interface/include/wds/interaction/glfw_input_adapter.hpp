#pragma once

#include "events.hpp"
#include "types.hpp"

#include <vector>

struct GLFWwindow;

namespace wds::interaction {

// GLFW → InputQueue bridge. Lives in wds_interaction_glfw so the core widget
// library stays free of a hard GLFW link requirement.
class GlfwInputAdapter {
 public:
  explicit GlfwInputAdapter(GLFWwindow* window);
  ~GlfwInputAdapter();

  void attach();
  void detach();

  InputQueue& queue() noexcept { return queue_; }
  const InputQueue& queue() const noexcept { return queue_; }

  Modifiers current_modifiers() const noexcept { return mods_; }

  // Cursor position in the same space as WidgetRoot layout / UiPainter (logical /
  // GLFW window pixels). Physical scaling is applied only at GPU flush.
  Vec2 pointer_logical() const noexcept { return pointer_; }
  // Deprecated alias — same as pointer_logical().
  Vec2 pointer_framebuffer() const noexcept { return pointer_; }

  void set_cursor(CursorKind kind);

 private:
  static Vec2 to_logical(double window_x, double window_y) {
    return {static_cast<float>(window_x), static_cast<float>(window_y)};
  }
  void destroy_cursors();

  static void pointer_pos_callback(GLFWwindow* window, double x, double y);
  static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
  static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
  static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
  static void char_callback(GLFWwindow* window, unsigned int codepoint);

  struct ClickTrack {
    double last_click_time = 0.0;
    Vec2 last_click_pos{};
    int click_count = 0;
  };

  GLFWwindow* window_ = nullptr;
  InputQueue queue_;
  Modifiers mods_;
  Vec2 pointer_{};  // logical / window pixels
  // Per-button double-click state (left/right/middle) — do not share across keys.
  ClickTrack click_track_[3]{};
  CursorKind cursor_ = CursorKind::Default;
  void* cursor_ew_ = nullptr;  // GLFWcursor*
  void* cursor_ns_ = nullptr;
};

}  // namespace wds::interaction

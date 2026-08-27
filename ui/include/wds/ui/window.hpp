#pragma once

#include "wds/interaction/types.hpp"

#include <functional>
#include <string>

struct GLFWwindow;

namespace wds::ui {

class UiWindow {
 public:
  // Return false to abort the close (GLFW close flag is cleared).
  using CloseHandler = std::function<bool()>;

  UiWindow() = default;
  ~UiWindow();

  UiWindow(const UiWindow&) = delete;
  UiWindow& operator=(const UiWindow&) = delete;

  bool create(int width, int height, const char* title);
  void destroy();

  // Win/Linux taskbar/window icon from RGBA PNG. No-op on macOS (uses .icns).
  void set_app_icon_png(const std::string& png_path);

  // Invoked from the GLFW window-close callback before the flag sticks.
  void set_close_handler(CloseHandler handler);

  GLFWwindow* handle() noexcept { return window_; }
  const GLFWwindow* handle() const noexcept { return window_; }

  bool should_close() const;
  void poll_events();
  void request_close();

  // Fullscreen: macOS uses native Spaces (toggleFullScreen); Win/Linux use
  // glfwSetWindowMonitor exclusive mode.
  bool is_fullscreen() const;
  void set_fullscreen(bool enable);
  void toggle_fullscreen() { set_fullscreen(!is_fullscreen()); }

  // Leave exclusive fullscreen / maximized / iconified before tearing down Vulkan.
  // Destroying the swapchain while the HWND still owns the display mode races the
  // driver (Windows TDR / “图形输出错误”, macOS native-fullscreen Spaces glitches).
  // Call VulkanRenderer::release_fullscreen_exclusive() before this when FSE was acquired.
  void prepare_for_teardown();

  // GLFW window size = logical / DIP pixels (UI layout space).
  wds::interaction::Vec2 window_size() const;
  // Vulkan swapchain size = physical framebuffer pixels.
  wds::interaction::Vec2 framebuffer_size() const;

 private:
  static void on_window_close(GLFWwindow* window);
  void save_windowed_rect();
  void apply_aspect_ratio(bool enable);

  GLFWwindow* window_ = nullptr;
  CloseHandler close_handler_;

  // Last known windowed placement (restored on leave-fullscreen).
  bool have_windowed_rect_ = false;
  int windowed_x_ = 80;
  int windowed_y_ = 80;
  int windowed_w_ = 1280;
  int windowed_h_ = 720;
};

}  // namespace wds::ui

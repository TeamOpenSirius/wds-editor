#include "wds/ui/window.hpp"

#include "wds/ui/macos_menu.hpp"

#include <wds/interaction/theme.hpp>
#include <wds/renderer/texture.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace wds::ui {
namespace {

// Close callback must not use glfwGetWindowUserPointer — GlfwInputAdapter owns that slot.
UiWindow* g_close_window = nullptr;

void apply_window_content_scale(GLFWwindow* window) {
  if (window == nullptr) return;
  // ui_content_scale maps layout (window / screen coords) → Vulkan framebuffer pixels.
  // Use ONLY fb/window — never glfwGetWindowContentScale (OS DPI %).
  //
  // macOS Retina: window in points, fb = points × DPI → scale 2.0 (correct).
  // Windows / X11: window size == framebuffer size always (pixels map 1:1), while
  // glfwGetWindowContentScale still returns e.g. 1.25 at 125% DPI. Mixing that into
  // the painter scale drew UI at 1.25× into a 1× framebuffer → clipped / partial UI.
  int fb_w = 0, fb_h = 0, win_w = 0, win_h = 0;
  glfwGetFramebufferSize(window, &fb_w, &fb_h);
  glfwGetWindowSize(window, &win_w, &win_h);
  float scale = 1.0f;
  if (win_w > 0 && fb_w > 0) {
    scale = static_cast<float>(fb_w) / static_cast<float>(win_w);
  }
  if (win_h > 0 && fb_h > 0) {
    scale = std::max(scale, static_cast<float>(fb_h) / static_cast<float>(win_h));
  }
  wds::interaction::theme::apply_content_scale(scale);
}

void on_content_scale(GLFWwindow* window, float /*xscale*/, float /*yscale*/) {
  apply_window_content_scale(window);
}

#if !defined(__APPLE__)
GLFWmonitor* monitor_for_window(GLFWwindow* window) {
  if (window == nullptr) {
    return glfwGetPrimaryMonitor();
  }
  if (GLFWmonitor* exclusive = glfwGetWindowMonitor(window)) {
    return exclusive;
  }
  int wx = 0, wy = 0, ww = 0, wh = 0;
  glfwGetWindowPos(window, &wx, &wy);
  glfwGetWindowSize(window, &ww, &wh);
  const int cx = wx + ww / 2;
  const int cy = wy + wh / 2;

  int count = 0;
  GLFWmonitor** monitors = glfwGetMonitors(&count);
  for (int i = 0; i < count; ++i) {
    int mx = 0, my = 0;
    glfwGetMonitorPos(monitors[i], &mx, &my);
    const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
    if (mode == nullptr) {
      continue;
    }
    if (cx >= mx && cx < mx + mode->width && cy >= my && cy < my + mode->height) {
      return monitors[i];
    }
  }
  return glfwGetPrimaryMonitor();
}
#endif

}  // namespace

UiWindow::~UiWindow() { destroy(); }

bool UiWindow::create(int width, int height, const char* title) {
  if (window_ != nullptr) {
    return true;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
  if (window_ != nullptr) {
    apply_window_content_scale(window_);
    glfwSetWindowContentScaleCallback(window_, on_content_scale);
    g_close_window = this;
    glfwSetWindowCloseCallback(window_, on_window_close);
    apply_aspect_ratio(true);
    windowed_w_ = width;
    windowed_h_ = height;
    save_windowed_rect();
    // After the first window, GLFW's Cocoa menubar exists — free Cmd+M.
    reclaim_cmd_m_from_menubar();
  }
  return window_ != nullptr;
}

void UiWindow::set_close_handler(CloseHandler handler) { close_handler_ = std::move(handler); }

void UiWindow::on_window_close(GLFWwindow* window) {
  UiWindow* self = g_close_window;
  if (self == nullptr || self->window_ != window) return;
  // GLFW already marked should-close; clear it if the handler aborts.
  if (self->close_handler_ && !self->close_handler_()) {
    glfwSetWindowShouldClose(window, GLFW_FALSE);
  }
}

void UiWindow::set_app_icon_png(const std::string& png_path) {
  if (window_ == nullptr || png_path.empty()) {
    return;
  }
#if defined(__APPLE__)
  // Dock / Finder use CFBundleIconFile (.icns); glfwSetWindowIcon is a no-op on Cocoa.
  (void)png_path;
#else
  std::vector<unsigned char> rgba;
  int width = 0;
  int height = 0;
  if (!wds::renderer::load_png_rgba8(png_path, rgba, width, height) || width <= 0 || height <= 0) {
    return;
  }
  // load_png_rgba8 stores bottom-up for Vulkan uploads; GLFW icons need top-down
  // (first pixel = top-left). Without this flip the Win32 title-bar icon is inverted.
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  std::vector<unsigned char> top_down(rgba.size());
  for (int y = 0; y < height; ++y) {
    const unsigned char* src =
        rgba.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
    unsigned char* dst = top_down.data() + static_cast<size_t>(y) * row_bytes;
    std::memcpy(dst, src, row_bytes);
  }
  GLFWimage image{};
  image.width = width;
  image.height = height;
  image.pixels = top_down.data();
  glfwSetWindowIcon(window_, 1, &image);
#endif
}

void UiWindow::destroy() {
  if (window_ != nullptr) {
    if (g_close_window == this) g_close_window = nullptr;
    glfwSetWindowCloseCallback(window_, nullptr);
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
}

bool UiWindow::should_close() const {
  return window_ == nullptr || glfwWindowShouldClose(window_);
}

void UiWindow::poll_events() {
  if (window_ != nullptr) {
    glfwPollEvents();
    // Keep fb/window ratio current (monitor moves, Retina toggles, DPI changes).
    apply_window_content_scale(window_);
  }
}

void UiWindow::request_close() {
  if (window_ != nullptr) {
    // Route through the same confirm path as the window chrome close button.
    if (close_handler_ && !close_handler_()) {
      return;
    }
    glfwSetWindowShouldClose(window_, 1);
  }
}

void UiWindow::save_windowed_rect() {
  if (window_ == nullptr || is_fullscreen()) {
    return;
  }
  glfwGetWindowPos(window_, &windowed_x_, &windowed_y_);
  glfwGetWindowSize(window_, &windowed_w_, &windowed_h_);
  if (windowed_w_ > 0 && windowed_h_ > 0) {
    have_windowed_rect_ = true;
  }
}

void UiWindow::apply_aspect_ratio(bool enable) {
  if (window_ == nullptr) {
    return;
  }
  if (enable) {
    glfwSetWindowAspectRatio(window_, 16, 9);
  } else {
    glfwSetWindowAspectRatio(window_, GLFW_DONT_CARE, GLFW_DONT_CARE);
  }
}

bool UiWindow::is_fullscreen() const {
  if (window_ == nullptr) {
    return false;
  }
#if defined(__APPLE__)
  return is_native_fullscreen(window_);
#else
  return glfwGetWindowMonitor(window_) != nullptr;
#endif
}

void UiWindow::set_fullscreen(bool enable) {
  if (window_ == nullptr || enable == is_fullscreen()) {
    return;
  }
#if defined(__APPLE__)
  // Native Spaces fullscreen — glfwSetWindowMonitor exclusive mode is a poor fit
  // on Cocoa.
  set_native_fullscreen(window_, enable);
#else
  if (enable) {
    save_windowed_rect();
    GLFWmonitor* monitor = monitor_for_window(window_);
    if (monitor == nullptr) {
      return;
    }
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if (mode == nullptr) {
      return;
    }
    apply_aspect_ratio(false);
    glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
  } else {
    const int w = have_windowed_rect_ ? windowed_w_ : 1280;
    const int h = have_windowed_rect_ ? windowed_h_ : 734;
    const int x = have_windowed_rect_ ? windowed_x_ : 80;
    const int y = have_windowed_rect_ ? windowed_y_ : 80;
    glfwSetWindowMonitor(window_, nullptr, x, y, w, h, 0);
    apply_aspect_ratio(true);
  }
#endif
  apply_window_content_scale(window_);
}

void UiWindow::prepare_for_teardown() {
  if (window_ == nullptr) {
    return;
  }
#if defined(__APPLE__)
  leave_native_fullscreen(window_);
#else
  // Exclusive fullscreen (glfwSetWindowMonitor) — leave video mode before Vulkan dies.
  if (is_fullscreen()) {
    const int w = have_windowed_rect_ ? windowed_w_ : 1280;
    const int h = have_windowed_rect_ ? windowed_h_ : 734;
    const int x = have_windowed_rect_ ? windowed_x_ : 80;
    const int y = have_windowed_rect_ ? windowed_y_ : 80;
    glfwSetWindowMonitor(window_, nullptr, x, y, w, h, 0);
  }
#endif
  if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) {
    glfwRestoreWindow(window_);
  }
  if (glfwGetWindowAttrib(window_, GLFW_MAXIMIZED)) {
    glfwRestoreWindow(window_);
  }
  // Hide before Vulkan surface teardown so DWM/compositor stops sampling the
  // swapchain (Win32 TDR / “系统图形输出错误” when destroying a live present).
  glfwHideWindow(window_);
  for (int i = 0; i < 8; ++i) {
    glfwPollEvents();
  }
}

wds::interaction::Vec2 UiWindow::window_size() const {
  if (window_ == nullptr) {
    return {};
  }
  int w = 0;
  int h = 0;
  glfwGetWindowSize(window_, &w, &h);
  return {static_cast<float>(w), static_cast<float>(h)};
}

wds::interaction::Vec2 UiWindow::framebuffer_size() const {
  if (window_ == nullptr) {
    return {};
  }
  int w = 0;
  int h = 0;
  glfwGetFramebufferSize(window_, &w, &h);
  return {static_cast<float>(w), static_cast<float>(h)};
}

}  // namespace wds::ui

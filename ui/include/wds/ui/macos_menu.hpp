#pragma once

#include <functional>

struct GLFWwindow;

namespace wds::ui {

// GLFW's Cocoa menubar binds Cmd+M to Window → Minimize, which swallows the
// key before our mirror shortcut. Clear that menu keyEquivalent so Cmd+M
// reaches the app. No-op on non-Apple platforms.
// Also: native Spaces fullscreen (preferred over glfwSetWindowMonitor on macOS).
#ifdef __APPLE__
void reclaim_cmd_m_from_menubar();
bool is_native_fullscreen(GLFWwindow* window);
void set_native_fullscreen(GLFWwindow* window, bool enable);
void leave_native_fullscreen(GLFWwindow* window);
// Window-menu item for Cmd+Shift+F11 fullscreen (matches chord_toggle_fullscreen).
void install_fullscreen_menu_shortcut(std::function<void()> handler);
// Drop the handler before Window/UiManager teardown (avoids UAF if menu fires).
void clear_fullscreen_menu_shortcut();
#else
inline void reclaim_cmd_m_from_menubar() {}
inline bool is_native_fullscreen(GLFWwindow*) { return false; }
inline void set_native_fullscreen(GLFWwindow*, bool) {}
inline void leave_native_fullscreen(GLFWwindow*) {}
inline void install_fullscreen_menu_shortcut(std::function<void()>) {}
inline void clear_fullscreen_menu_shortcut() {}
#endif

}  // namespace wds::ui

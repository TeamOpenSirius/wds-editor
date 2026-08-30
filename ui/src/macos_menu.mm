#include "wds/ui/macos_menu.hpp"

#include <wds/common/crash_input_journal.hpp>

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <functional>

@interface WdsFullscreenMenuTarget : NSObject
- (void)toggleFullscreen:(id)sender;
@end

namespace wds::ui {
namespace {

std::function<void()> g_fullscreen_handler;
WdsFullscreenMenuTarget* g_fs_target = nil;

NSWindow* cocoa_window(GLFWwindow* window) {
  if (window == nullptr) return nil;
  return glfwGetCocoaWindow(window);
}

}  // namespace

void invoke_fullscreen_menu_handler() {
  wds::common::journal_begin_event(wds::common::CrashInputKind::Menu, 0, 0, 0, 0, 0, 0, 0, 0);
  wds::common::journal_set_route("MacosMenu", wds::common::CrashRouteVia::Menu);
  if (g_fullscreen_handler) {
    g_fullscreen_handler();
  }
  wds::common::journal_end_event();
}

void clear_fullscreen_menu_shortcut() {
  g_fullscreen_handler = nullptr;
}

void reclaim_cmd_m_from_menubar() {
  NSMenu* main_menu = NSApp.mainMenu;
  if (main_menu == nil) return;

  for (NSMenuItem* top in main_menu.itemArray) {
    NSMenu* submenu = top.submenu;
    if (submenu == nil) continue;
    for (NSMenuItem* item in submenu.itemArray) {
      if (item.action != @selector(performMiniaturize:)) continue;
      // Keep the menu item; only free Cmd+M for the editor shortcut.
      item.keyEquivalent = @"";
    }
  }
}

bool is_native_fullscreen(GLFWwindow* window) {
  NSWindow* nsw = cocoa_window(window);
  if (nsw == nil) return false;
  return (nsw.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

void set_native_fullscreen(GLFWwindow* window, bool enable) {
  NSWindow* nsw = cocoa_window(window);
  if (nsw == nil) return;
  const bool now = (nsw.styleMask & NSWindowStyleMaskFullScreen) != 0;
  if (now == enable) return;
  [nsw toggleFullScreen:nil];
  // Pump briefly so the Space transition / framebuffer size update starts.
  for (int i = 0; i < 20; ++i) {
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.016]];
    glfwPollEvents();
    const bool cur = (nsw.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (cur == enable) break;
  }
}

void leave_native_fullscreen(GLFWwindow* window) {
  set_native_fullscreen(window, false);
}

void install_fullscreen_menu_shortcut(std::function<void()> handler) {
  g_fullscreen_handler = std::move(handler);
  if (g_fs_target == nil) {
    g_fs_target = [[WdsFullscreenMenuTarget alloc] init];
  }

  NSMenu* main_menu = NSApp.mainMenu;
  if (main_menu == nil) return;

  // Prefer the existing Window menu; fall back to creating one.
  NSMenu* window_menu = nil;
  for (NSMenuItem* top in main_menu.itemArray) {
    if (top.submenu != nil &&
        (top.submenu == NSApp.windowsMenu ||
         [top.title isEqualToString:@"Window"] ||
         [top.title isEqualToString:@"窗口"])) {
      window_menu = top.submenu;
      break;
    }
  }
  if (window_menu == nil) {
    NSMenuItem* top = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
    window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
    top.submenu = window_menu;
    [main_menu addItem:top];
    NSApp.windowsMenu = window_menu;
  }

  // Cmd+Shift+F11 — same chord as chord_toggle_fullscreen on macOS.
  unichar f11 = NSF11FunctionKey;
  NSString* f11_key = [NSString stringWithCharacters:&f11 length:1];

  // Reuse / retarget any existing toggleFullScreen: item; otherwise add one.
  NSMenuItem* fs_item = nil;
  for (NSMenuItem* item in window_menu.itemArray) {
    if (item.action == @selector(toggleFullScreen:) ||
        item.action == @selector(toggleFullscreen:)) {
      fs_item = item;
      break;
    }
  }
  if (fs_item == nil) {
    fs_item = [[NSMenuItem alloc] initWithTitle:@"Toggle Full Screen"
                                         action:@selector(toggleFullscreen:)
                                  keyEquivalent:@""];
    [window_menu addItem:[NSMenuItem separatorItem]];
    [window_menu addItem:fs_item];
  }
  fs_item.target = g_fs_target;
  fs_item.action = @selector(toggleFullscreen:);
  fs_item.keyEquivalent = f11_key;
  fs_item.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
}

}  // namespace wds::ui

@implementation WdsFullscreenMenuTarget
- (void)toggleFullscreen:(id)sender {
  (void)sender;
  wds::ui::invoke_fullscreen_menu_handler();
}
@end

#endif  // __APPLE__

#include "wds/ui/macos_file_dialog.hpp"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cctype>
#include <string>

namespace wds::ui::macos_file_dialog {
namespace {

std::string strip_leading_dot(std::string ext) {
  while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
  return ext;
}

bool ends_with_ci(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i) {
    const unsigned char a = static_cast<unsigned char>(value[value.size() - suffix.size() + i]);
    const unsigned char b = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

// NSSavePanel appends an allowed extension when allowedContentTypes is set. If the suggested
// name already includes that extension (e.g. "1.sus"), the field becomes "1.sus.sus".
std::string name_without_allowed_extension(std::string name,
                                           const std::vector<std::string>& extensions) {
  for (const auto& raw : extensions) {
    const std::string ext = strip_leading_dot(raw);
    if (ext.empty()) continue;
    const std::string suffix = "." + ext;
    if (ends_with_ci(name, suffix)) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name;
}

NSArray<UTType*>* content_types(const std::vector<std::string>& extensions) {
  if (extensions.empty()) return nil;
  NSMutableArray<UTType*>* types = [NSMutableArray arrayWithCapacity:extensions.size()];
  for (const auto& ext : extensions) {
    const std::string e = strip_leading_dot(ext);
    if (e.empty()) continue;
    NSString* ext_ns = [[NSString alloc] initWithBytes:e.data()
                                                length:e.size()
                                              encoding:NSUTF8StringEncoding];
    if (ext_ns == nil) continue;
    UTType* type = [UTType typeWithFilenameExtension:ext_ns];
    if (type != nil) [types addObject:type];
  }
  return types.count > 0 ? types : nil;
}

NSString* ns_string(const std::string& value) {
  NSString* s = [[NSString alloc] initWithBytes:value.data()
                                         length:value.size()
                                       encoding:NSUTF8StringEncoding];
  return s != nil ? s : @"";
}

void activate_app() {
  [NSApp activateIgnoringOtherApps:YES];
}

void restore_glfw_key_window(void* glfw_window) {
  [NSApp activateIgnoringOtherApps:YES];
  if (glfw_window == nullptr) return;
  GLFWwindow* gw = static_cast<GLFWwindow*>(glfw_window);
  NSWindow* nsw = glfwGetCocoaWindow(gw);
  if (nsw != nil) {
    [nsw makeKeyAndOrderFront:nil];
    NSView* view = nsw.contentView;
    if (view != nil) {
      [nsw makeFirstResponder:view];
    }
  }
  glfwFocusWindow(gw);
}

// Prefer fileSystemRepresentation for paths passed to POSIX / libstdc++-style APIs.
// url.path.UTF8String can diverge from the on-disk form for some Unicode names.
std::optional<std::string> path_from_url(NSURL* url) {
  if (url == nil || !url.isFileURL) return std::nullopt;
  const char* fs = url.fileSystemRepresentation;
  if (fs == nullptr || fs[0] == '\0') return std::nullopt;
  return std::string(fs);
}

void apply_types(NSSavePanel* panel, const std::vector<std::string>& extensions) {
  NSArray<UTType*>* types = content_types(extensions);
  if (types == nil) return;
  panel.allowedContentTypes = types;
  panel.allowsOtherFileTypes = NO;
}

}  // namespace

std::optional<std::string> open_file(const std::string& title,
                                     const std::vector<std::string>& extensions) {
  @autoreleasepool {
    activate_app();
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = ns_string(title);
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.canCreateDirectories = NO;
    apply_types(panel, extensions);
    if ([panel runModal] != NSModalResponseOK) return std::nullopt;
    return path_from_url(panel.URL);
  }
}

std::optional<std::string> save_file(const std::string& title, const std::string& default_name,
                                     const std::vector<std::string>& extensions) {
  @autoreleasepool {
    activate_app();
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.message = ns_string(title);
    panel.canCreateDirectories = YES;
    panel.extensionHidden = NO;
    // Set types before the name so the panel owns a single extension append.
    apply_types(panel, extensions);
    if (!default_name.empty()) {
      panel.nameFieldStringValue =
          ns_string(name_without_allowed_extension(default_name, extensions));
    }
    if ([panel runModal] != NSModalResponseOK) return std::nullopt;
    return path_from_url(panel.URL);
  }
}

std::optional<std::string> choose_directory(const std::string& title) {
  @autoreleasepool {
    activate_app();
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = ns_string(title);
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.canCreateDirectories = YES;
    if ([panel runModal] != NSModalResponseOK) return std::nullopt;
    return path_from_url(panel.URL);
  }
}

bool confirm(const std::string& title, const std::string& message) {
  @autoreleasepool {
    activate_app();
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleInformational;
    alert.messageText = ns_string(title);
    alert.informativeText = ns_string(message);
    // First button is the default (Return) — keep prior osascript default of No.
    [alert addButtonWithTitle:@"No"];
    [alert addButtonWithTitle:@"Yes"];
    return [alert runModal] == NSAlertSecondButtonReturn;
  }
}

void alert_error(const std::string& title, const std::string& message) {
  @autoreleasepool {
    activate_app();
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = ns_string(title);
    alert.informativeText = ns_string(message);
    [alert addButtonWithTitle:@"确定"];
    (void)[alert runModal];
  }
}

native_file_dialog::SaveDiscardCancel confirm_save_discard_cancel(const std::string& title,
                                                                  const std::string& message) {
  @autoreleasepool {
    activate_app();
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = ns_string(title);
    alert.informativeText = ns_string(message);
    // Order: first = default (保存), second = 不保存, third = 取消.
    [alert addButtonWithTitle:@"保存"];
    [alert addButtonWithTitle:@"不保存"];
    [alert addButtonWithTitle:@"取消"];
    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) {
      return native_file_dialog::SaveDiscardCancel::Save;
    }
    if (response == NSAlertSecondButtonReturn) {
      return native_file_dialog::SaveDiscardCancel::Discard;
    }
    return native_file_dialog::SaveDiscardCancel::Cancel;
  }
}

void restore_owner_focus(void* glfw_window) {
  restore_glfw_key_window(glfw_window);
  // Panel teardown can steal key status after runModal returns.
  GLFWwindow* gw = static_cast<GLFWwindow*>(glfw_window);
  dispatch_async(dispatch_get_main_queue(), ^{
    restore_glfw_key_window(gw);
  });
}

}  // namespace wds::ui::macos_file_dialog

#endif  // __APPLE__

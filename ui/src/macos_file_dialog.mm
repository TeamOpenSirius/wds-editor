#include "wds/ui/macos_file_dialog.hpp"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

namespace wds::ui::macos_file_dialog {
namespace {

NSArray<UTType*>* content_types(const std::vector<std::string>& extensions) {
  if (extensions.empty()) return nil;
  NSMutableArray<UTType*>* types = [NSMutableArray arrayWithCapacity:extensions.size()];
  for (const auto& ext : extensions) {
    std::string e = ext;
    while (!e.empty() && e.front() == '.') e.erase(e.begin());
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
    if (!default_name.empty()) {
      panel.nameFieldStringValue = ns_string(default_name);
    }
    apply_types(panel, extensions);
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

}  // namespace wds::ui::macos_file_dialog

#endif  // __APPLE__

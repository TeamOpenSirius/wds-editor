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
    UTType* type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:e.c_str()]];
    if (type != nil) [types addObject:type];
  }
  return types.count > 0 ? types : nil;
}

NSString* ns_string(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()];
}

void activate_app() {
  [NSApp activateIgnoringOtherApps:YES];
}

std::optional<std::string> path_from_url(NSURL* url) {
  if (url == nil || url.path == nil) return std::nullopt;
  const char* utf8 = url.path.UTF8String;
  if (utf8 == nullptr || utf8[0] == '\0') return std::nullopt;
  return std::string(utf8);
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

}  // namespace wds::ui::macos_file_dialog

#endif  // __APPLE__

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace wds::ui::macos_file_dialog {

// In-process AppKit panels (not osascript). Must run on the main thread.
std::optional<std::string> open_file(const std::string& title,
                                     const std::vector<std::string>& extensions);
std::optional<std::string> save_file(const std::string& title, const std::string& default_name,
                                     const std::vector<std::string>& extensions);
std::optional<std::string> choose_directory(const std::string& title);

}  // namespace wds::ui::macos_file_dialog

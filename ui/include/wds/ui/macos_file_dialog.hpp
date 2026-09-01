#pragma once

#include "wds/ui/native_file_dialog.hpp"

#include <optional>
#include <string>
#include <vector>

namespace wds::ui::macos_file_dialog {

// In-process AppKit panels / alerts (not osascript). Must run on the main thread.
// Paths are UTF-8 filesystem representations suitable for POSIX / std::fstream.
std::optional<std::string> open_file(const std::string& title,
                                     const std::vector<std::string>& extensions);
std::optional<std::string> save_file(const std::string& title, const std::string& default_name,
                                     const std::vector<std::string>& extensions);
std::optional<std::string> choose_directory(const std::string& title);

bool confirm(const std::string& title, const std::string& message);
void alert_error(const std::string& title, const std::string& message);
native_file_dialog::SaveDiscardCancel confirm_save_discard_cancel(const std::string& title,
                                                                  const std::string& message);

// After a modal panel, make the GLFW / NSWindow key again (AppKit does not).
void restore_owner_focus(void* glfw_window);

}  // namespace wds::ui::macos_file_dialog

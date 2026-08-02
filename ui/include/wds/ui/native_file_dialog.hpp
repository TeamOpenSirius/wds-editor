#pragma once

#include <optional>
#include <string>
#include <vector>

namespace wds::ui::native_file_dialog {
using Filters = std::vector<std::string>;

// Filters are bare extensions without dots, e.g. {"wdsproject"} or {"csv","sus"}.
// Open dialogs restrict the file list to these types (platform support varies).
// Save dialogs set a default extension and normalize the returned path so a missing
// or wrong suffix is replaced with the preferred filter extension.
std::optional<std::string> open_file(const std::string& title, const Filters& filters = {});
std::optional<std::string> save_file(const std::string& title, const std::string& default_name = {},
                                     const Filters& filters = {});
std::vector<std::string> open_files(const std::string& title, const Filters& filters = {});

// Folder picker. Returns a POSIX/native directory path, or nullopt on cancel.
std::optional<std::string> choose_directory(const std::string& title);

// Blocking yes/no prompt. Returns true for Yes, false for No/cancel.
bool confirm(const std::string& title, const std::string& message);

// Blocking error alert (single OK). Use for fatal startup dependency failures.
void alert_error(const std::string& title, const std::string& message);

// On Windows, set the owner HWND for file dialogs / MessageBox so they stay modal to
// the Vulkan window. Pass nullptr to clear. No-op on other platforms.
void set_owner_window(void* hwnd);

// Three-way prompt for unsaved changes: 保存 / 不保存 / 取消.
// Cancel (and Escape) aborts the pending action; Discard continues without saving.
enum class SaveDiscardCancel { Save, Discard, Cancel };
SaveDiscardCancel confirm_save_discard_cancel(const std::string& title, const std::string& message);

}  // namespace wds::ui::native_file_dialog

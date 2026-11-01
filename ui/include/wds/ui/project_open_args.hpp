#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace wds::ui {

// Project files double-clicked in a file manager reach the process as a bare
// argument: Windows shell associations run `wds_editor.exe "%1"` and Linux
// desktop entries use %f / %U. Only a bare argument naming a `.wdsproject` file
// is a project request — flags, their values and any other path must keep the
// normal argument handling (automation flags such as --capture-preview rely on
// it). macOS does not pass the document in argv at all; LaunchServices sends a
// QFileOpenEvent instead (see file_open_router.hpp).
inline bool is_wdsproject_path(std::string_view path) noexcept {
  constexpr std::string_view kExt = ".wdsproject";
  if (path.size() <= kExt.size()) {
    return false;
  }
  const std::size_t offset = path.size() - kExt.size();
  for (std::size_t i = 0; i < kExt.size(); ++i) {
    char c = path[offset + i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != kExt[i]) {
      return false;
    }
  }
  return true;
}

// First bare argument naming a project, or -1. The caller still has to check
// that the file exists: the extension alone never opens anything.
inline std::ptrdiff_t first_project_argument(const char* const* args, std::size_t count) noexcept {
  if (args == nullptr) {
    return -1;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (args[i] == nullptr) {
      continue;
    }
    const std::string_view arg(args[i]);
    if (arg.empty() || arg.front() == '-') {
      continue;
    }
    if (is_wdsproject_path(arg)) {
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

}  // namespace wds::ui

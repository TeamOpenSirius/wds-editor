#pragma once

#include <wds/core/chart_serializer.hpp>

#include <string>

namespace wds::chart_editor {

// Paths are UTF-8 on all platforms. On Windows, narrow `std::fstream` / many
// libstdc++ filesystem operations use the ACP and break on non-ASCII paths —
// prefer these helpers (and Win32 wide APIs underneath) for user-facing I/O.

// Read entire file contents. On failure returns {} and fills `status`.
std::string read_text_file(const std::string& path, SerializeResult& status);

// Write `text` to `path` via a same-directory temp file, then rename/replace.
// On failure the original file (if any) is left intact.
SerializeResult write_text_atomic(const std::string& path, const std::string& text);

// Atomically replace `path` with an already-written temp file at `temp_path`.
// Removes `temp_path` on failure after a best-effort cleanup.
SerializeResult replace_file_atomic(const std::string& path, const std::string& temp_path);

}  // namespace wds::chart_editor

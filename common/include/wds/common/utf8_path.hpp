#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace wds::common {

// Project-wide path convention: std::string paths are UTF-8 on every platform.
// On Windows, narrow filesystem / CRT APIs use the active code page (ACP, e.g.
// GBK) — never pass UTF-8 through path::string(), fopen, or argv-derived
// narrow paths without these helpers.

std::filesystem::path path_from_utf8(const std::string& utf8);
std::string path_to_utf8(const std::filesystem::path& path);

// Directory containing the running executable. Prefers OS APIs
// (GetModuleFileNameW / _NSGetExecutablePath /proc) over argv0.
std::filesystem::path executable_dir(const char* argv0 = nullptr);

bool read_file_bytes(const std::string& utf8_path, std::vector<char>& out);
bool read_file_bytes(const std::string& utf8_path, std::vector<unsigned char>& out);

// Existence probes that stay on Win32 wide APIs under MinGW (libstdc++ filesystem
// often still routes u8path through the ACP).
bool path_exists_utf8(const std::string& utf8_path);
bool is_directory_utf8(const std::string& utf8_path);
bool is_regular_file_utf8(const std::string& utf8_path);

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& utf8);
std::string wide_to_utf8(const std::wstring& wide);
// CRT fopen replacement that accepts UTF-8 paths (uses _wfopen).
FILE* fopen_utf8(const std::string& utf8_path, const char* mode);
// Regular files only (skips directories / `.` / `..`).
std::vector<std::string> list_regular_files_utf8(const std::string& dir_utf8);
#else
inline FILE* fopen_utf8(const std::string& utf8_path, const char* mode) {
  return std::fopen(utf8_path.c_str(), mode);
}
#endif

}  // namespace wds::common

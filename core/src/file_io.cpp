#include <wds/core/file_io.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wds::chart_editor {
namespace {

namespace fs = std::filesystem;

std::string make_temp_path(const std::string& path) {
  return path + ".wds-tmp";
}

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}

bool win_read_bytes(const std::string& utf8_path, std::string& out) {
  const std::wstring wide = utf8_to_wide(utf8_path);
  if (wide.empty() && !utf8_path.empty()) return false;
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    ::CloseHandle(file);
    return false;
  }
  if (static_cast<std::uint64_t>(size.QuadPart) > (std::uint64_t{1} << 30)) {
    ::CloseHandle(file);
    return false;  // refuse >1GiB chart/project text
  }
  out.assign(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  bool ok = true;
  while (offset < out.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, std::size_t{1} << 20));
    DWORD chunk_read = 0;
    if (!::ReadFile(file, out.data() + offset, chunk, &chunk_read, nullptr) || chunk_read == 0) {
      ok = false;
      break;
    }
    offset += chunk_read;
  }
  ::CloseHandle(file);
  return ok && offset == out.size();
}

// libstdc++ filesystem on MinGW often routes through the narrow ACP even for
// u8path — non-ASCII project/chart paths then fail at rename/create. Use Win32
// wide APIs for all mutation.

bool win_create_directories(const std::wstring& wide) {
  if (wide.empty()) return true;
  const DWORD attr = GetFileAttributesW(wide.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES) {
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }
  std::size_t pos = wide.find_last_of(L"\\/");
  // Skip drive root like "C:" or "\\server\share"
  if (pos != std::wstring::npos && pos > 0) {
    // "C:\foo" → parent "C:" — CreateDirectoryW("C:") fails; treat as ok.
    if (!(pos == 2 && wide[1] == L':')) {
      if (!win_create_directories(wide.substr(0, pos))) return false;
    }
  }
  if (CreateDirectoryW(wide.c_str(), nullptr)) return true;
  const DWORD err = GetLastError();
  return err == ERROR_ALREADY_EXISTS;
}

bool win_delete_file(const std::string& utf8) {
  const std::wstring wide = utf8_to_wide(utf8);
  if (wide.empty() && !utf8.empty()) return false;
  if (DeleteFileW(wide.c_str())) return true;
  const DWORD err = GetLastError();
  return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

bool win_write_bytes(const std::string& utf8_path, const std::string& bytes) {
  const std::wstring wide = utf8_to_wide(utf8_path);
  if (wide.empty() && !utf8_path.empty()) return false;
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  if (!bytes.empty()) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const DWORD chunk =
          static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, std::size_t{1} << 20));
      DWORD chunk_written = 0;
      if (!::WriteFile(file, bytes.data() + offset, chunk, &chunk_written, nullptr) ||
          chunk_written != chunk) {
        ok = false;
        break;
      }
      offset += chunk_written;
    }
    ok = ok && offset == bytes.size();
  }
  ::CloseHandle(file);
  return ok;
}

bool win_rename_replace(const std::string& from_utf8, const std::string& to_utf8) {
  const std::wstring from = utf8_to_wide(from_utf8);
  const std::wstring to = utf8_to_wide(to_utf8);
  if ((from.empty() && !from_utf8.empty()) || (to.empty() && !to_utf8.empty())) return false;
  if (MoveFileExW(from.c_str(), to.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  // Some volumes reject replace-in-place; delete destination then retry.
  win_delete_file(to_utf8);
  return MoveFileExW(from.c_str(), to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) !=
         0;
}

std::wstring parent_dir_wide(const std::wstring& wide) {
  const std::size_t pos = wide.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return {};
  if (pos == 2 && wide.size() >= 3 && wide[1] == L':') {
    // "C:\file" → "C:\"
    return wide.substr(0, 3);
  }
  return wide.substr(0, pos);
}
#endif

}  // namespace

std::string read_text_file(const std::string& path, SerializeResult& status) {
#if defined(_WIN32)
  std::string bytes;
  if (!win_read_bytes(path, bytes)) {
    status = {SerializeError::IoError, "failed to open file for reading: " + path};
    return {};
  }
  status = {SerializeError::Ok, {}};
  return bytes;
#else
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    status = {SerializeError::IoError, "failed to open file for reading: " + path};
    return {};
  }
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (!file && !file.eof()) {
    status = {SerializeError::IoError, "failed while reading: " + path};
    return {};
  }
  status = {SerializeError::Ok, {}};
  return bytes;
#endif
}

SerializeResult replace_file_atomic(const std::string& path, const std::string& temp_path) {
#if defined(_WIN32)
  if (win_rename_replace(temp_path, path)) {
    return {SerializeError::Ok, {}};
  }
  win_delete_file(temp_path);
  return {SerializeError::IoError, "failed to replace file: " + path};
#else
  const fs::path target(path);
  const fs::path temp(temp_path);
  std::error_code ec;
  fs::rename(temp, target, ec);
  if (ec) {
    fs::remove(target, ec);
    ec.clear();
    fs::rename(temp, target, ec);
  }
  if (ec) {
    std::error_code cleanup;
    fs::remove(temp, cleanup);
    return {SerializeError::IoError, "failed to replace file: " + path + " (" + ec.message() + ")"};
  }
  return {SerializeError::Ok, {}};
#endif
}

SerializeResult write_text_atomic(const std::string& path, const std::string& text) {
  const std::string temp_path = make_temp_path(path);
#if defined(_WIN32)
  {
    const std::wstring wide = utf8_to_wide(path);
    const std::wstring parent = parent_dir_wide(wide);
    if (!parent.empty() && !win_create_directories(parent)) {
      return {SerializeError::IoError, "failed to create directory for: " + path};
    }
  }
  if (!win_write_bytes(temp_path, text)) {
    win_delete_file(temp_path);
    return {SerializeError::IoError, "failed to open temp file for writing: " + temp_path};
  }
  return replace_file_atomic(path, temp_path);
#else
  {
    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return {SerializeError::IoError,
                "failed to create directory: " + parent.string() + " (" + ec.message() + ")"};
      }
    }
  }
  {
    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      return {SerializeError::IoError, "failed to open temp file for writing: " + temp_path};
    }
    file << text;
    file.flush();
    if (!file) {
      std::error_code cleanup;
      fs::remove(temp_path, cleanup);
      return {SerializeError::IoError, "failed while writing: " + temp_path};
    }
  }
  return replace_file_atomic(path, temp_path);
#endif
}

}  // namespace wds::chart_editor

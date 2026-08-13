#include "wds/common/utf8_path.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace wds::common {
namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
bool win_read_bytes(const std::wstring& wide, std::vector<char>& out) {
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    ::CloseHandle(file);
    return false;
  }
  if (static_cast<unsigned long long>(size.QuadPart) > (1ull << 30)) {
    ::CloseHandle(file);
    return false;
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

std::wstring mode_to_wide(const char* mode) {
  std::wstring wide;
  if (mode == nullptr) return wide;
  for (const char* p = mode; *p != '\0'; ++p) {
    wide.push_back(static_cast<unsigned char>(*p));
  }
  return wide;
}
#endif

}  // namespace

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

std::string wide_to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  if (needed <= 0) return {};
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed,
                      nullptr, nullptr);
  return utf8;
}

FILE* fopen_utf8(const std::string& utf8_path, const char* mode) {
  const std::wstring wide = utf8_to_wide(utf8_path);
  if ((wide.empty() && !utf8_path.empty()) || mode == nullptr) return nullptr;
  return _wfopen(wide.c_str(), mode_to_wide(mode).c_str());
}
#endif

fs::path path_from_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return fs::u8path(utf8);
#else
  return fs::path(utf8);
#endif
}

std::string path_to_utf8(const fs::path& path) {
#if defined(_WIN32)
  const auto u8 = path.generic_u8string();
  return std::string(u8.begin(), u8.end());
#else
  return path.generic_string();
#endif
}

fs::path executable_dir(const char* argv0) {
  std::error_code ec;
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
      buf.resize(std::strlen(buf.c_str()));
      const fs::path resolved = fs::weakly_canonical(fs::path(buf), ec);
      if (!ec && !resolved.empty()) {
        return resolved.parent_path();
      }
      return fs::path(buf).lexically_normal().parent_path();
    }
  }
#elif defined(_WIN32)
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    const fs::path resolved = fs::weakly_canonical(fs::path(buf), ec);
    if (!ec && !resolved.empty()) {
      return resolved.parent_path();
    }
    return fs::path(buf).lexically_normal().parent_path();
  }
#elif defined(__linux__)
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    const fs::path resolved = fs::weakly_canonical(fs::path(buf), ec);
    if (!ec && !resolved.empty()) {
      return resolved.parent_path();
    }
    return fs::path(buf).lexically_normal().parent_path();
  }
#endif

  if (argv0 != nullptr && argv0[0] != '\0') {
#if defined(_WIN32)
    // argv0 is ACP on Windows — convert via path construction (ACP→wide), not u8path.
    fs::path exe = fs::path(argv0);
#else
    fs::path exe = path_from_utf8(argv0);
#endif
    if (!exe.is_absolute()) {
      exe = fs::current_path(ec) / exe;
    }
    if (!ec) {
      const fs::path resolved = fs::weakly_canonical(exe, ec);
      if (!ec && !resolved.empty()) {
        return resolved.parent_path();
      }
      return exe.lexically_normal().parent_path();
    }
  }

  return fs::current_path(ec);
}

bool read_file_bytes(const std::string& utf8_path, std::vector<char>& out) {
  out.clear();
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(utf8_path);
  if (wide.empty() && !utf8_path.empty()) return false;
  return win_read_bytes(wide, out);
#else
  std::ifstream in(path_from_utf8(utf8_path), std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return static_cast<bool>(in) || in.eof();
#endif
}

bool read_file_bytes(const std::string& utf8_path, std::vector<unsigned char>& out) {
  std::vector<char> bytes;
  if (!read_file_bytes(utf8_path, bytes)) return false;
  out.assign(bytes.begin(), bytes.end());
  return true;
}

bool path_exists_utf8(const std::string& utf8_path) {
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(utf8_path);
  if (wide.empty() && !utf8_path.empty()) return false;
  return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  std::error_code ec;
  return fs::exists(path_from_utf8(utf8_path), ec) && !ec;
#endif
}

bool is_directory_utf8(const std::string& utf8_path) {
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(utf8_path);
  if (wide.empty() && !utf8_path.empty()) return false;
  const DWORD attr = GetFileAttributesW(wide.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return false;
  return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  std::error_code ec;
  return fs::is_directory(path_from_utf8(utf8_path), ec) && !ec;
#endif
}

bool is_regular_file_utf8(const std::string& utf8_path) {
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(utf8_path);
  if (wide.empty() && !utf8_path.empty()) return false;
  const DWORD attr = GetFileAttributesW(wide.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return false;
  return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
  std::error_code ec;
  return fs::is_regular_file(path_from_utf8(utf8_path), ec) && !ec;
#endif
}

#if defined(_WIN32)
std::vector<std::string> list_regular_files_utf8(const std::string& dir_utf8) {
  std::vector<std::string> out;
  std::wstring pattern = utf8_to_wide(dir_utf8);
  if (pattern.empty()) return out;
  if (pattern.back() != L'\\' && pattern.back() != L'/') pattern.push_back(L'\\');
  pattern.push_back(L'*');
  WIN32_FIND_DATAW fd{};
  const HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
  if (handle == INVALID_HANDLE_VALUE) return out;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::string name = wide_to_utf8(fd.cFileName);
    if (name.empty() || name == "." || name == "..") continue;
    if (!dir_utf8.empty() && (dir_utf8.back() == '/' || dir_utf8.back() == '\\')) {
      out.push_back(dir_utf8 + name);
    } else {
      out.push_back(dir_utf8 + "/" + name);
    }
  } while (FindNextFileW(handle, &fd));
  FindClose(handle);
  return out;
}
#endif

}  // namespace wds::common

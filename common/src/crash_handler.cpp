#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "wds/common/crash_handler.hpp"
#include "wds/common/crash_input_journal.hpp"
#include "wds/common/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <typeinfo>

#ifndef WDS_APP_VERSION
#define WDS_APP_VERSION "dev"
#endif
#ifndef WDS_GIT_HASH
#define WDS_GIT_HASH "unknown"
#endif
#ifndef WDS_BUILD_TYPE
#define WDS_BUILD_TYPE "unknown"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <csignal>
#include "wds/common/utf8_path.hpp"
#if defined(_MSC_VER)
#include <crtdbg.h>
#else
#include <cxxabi.h>
#endif
#ifndef MiniDumpWithIndirectlyReferencedMemory
#define MiniDumpWithIndirectlyReferencedMemory static_cast<MINIDUMP_TYPE>(0x0040)
#endif
#ifndef MiniDumpScanMemory
#define MiniDumpScanMemory static_cast<MINIDUMP_TYPE>(0x0010)
#endif
#ifndef MiniDumpWithThreadInfo
#define MiniDumpWithThreadInfo static_cast<MINIDUMP_TYPE>(0x1000)
#endif
#ifndef MiniDumpNormal
#define MiniDumpNormal static_cast<MINIDUMP_TYPE>(0x0000)
#endif
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
#else
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

namespace wds::common {
namespace {

constexpr std::size_t kPathCap = 1024;
constexpr std::size_t kMsgCap = 4096;
constexpr std::size_t kHeaderCap = 2048;
constexpr std::size_t kCtxCap = 512;
constexpr int kCtxCount = 6;
constexpr int kRotateKeep = 20;
constexpr char kSessionStartLine[] = "WDS Editor session start\n";

char g_log_dir[kPathCap] = {};
char g_crash_path[kPathCap] = {};
char g_install_header[kHeaderCap] = {};
char g_ctx[kCtxCount][kCtxCap] = {};
char g_prev_crash_path[kPathCap] = {};
char g_os_version[256] = {};
char g_install_stamp[32] = {};

std::atomic<std::uint64_t> g_handling_tid{0};
std::atomic<bool> g_reported_by_terminate{false};
std::atomic<bool> g_reported_full{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_prev_crashed{false};
bool g_checked_prev = false;
bool g_no_dialog = false;
bool g_symbolize_in_handler = true;
std::uint64_t g_main_slide = 0;
std::mutex g_report_mu;

#if defined(_WIN32)
HANDLE g_crash_file = INVALID_HANDLE_VALUE;
char g_dump_path[kPathCap] = {};
ULONGLONG g_install_tick = 0;
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
using SymInitFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using StackWalkFn = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
                                  PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                  PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
using SymFnTableFn = PVOID(WINAPI*)(HANDLE, DWORD64);
using SymBaseFn = DWORD64(WINAPI*)(HANDLE, DWORD64);
HMODULE g_dbghelp = nullptr;
MiniDumpWriteDumpFn g_minidump_write = nullptr;
SymInitFn g_sym_init = nullptr;
StackWalkFn g_stack_walk = nullptr;
SymFnTableFn g_fn_table = nullptr;
SymBaseFn g_mod_base = nullptr;
LONG g_header_only_size = 0;
#else
int g_crash_fd = -1;
off_t g_header_only_size = 0;
timespec g_install_mono{};
alignas(16) char g_main_altstack[128 * 1024];
#endif

const char* ctx_field_name(int i) {
  switch (i) {
    case 0:
      return "project_path";
    case 1:
      return "music_path";
    case 2:
      return "vulkan_device";
    case 3:
      return "audio_backend";
    case 4:
      return "custom0";
    case 5:
      return "custom1";
    default:
      return "unknown";
  }
}

void copy_trunc(char* dst, std::size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) return;
  dst[0] = '\0';
  if (src == nullptr) return;
  std::size_t i = 0;
  while (src[i] != '\0' && i + 1 < cap) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

void append_cstr(char* dst, std::size_t cap, const char* src) {
  if (dst == nullptr || cap == 0 || src == nullptr) return;
  const std::size_t used = std::strlen(dst);
  if (used + 1 >= cap) return;
  std::strncat(dst, src, cap - used - 1);
}

std::uint64_t current_tid() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
  const pthread_t self = ::pthread_self();
  std::uint64_t tid = 0;
  std::memcpy(&tid, &self, sizeof(self) < sizeof(tid) ? sizeof(self) : sizeof(tid));
  return tid;
#endif
}

[[noreturn]] void die_hard() {
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(), 1);
#else
  ::_exit(1);
#endif
}

[[noreturn]] void wait_other_crash_writer() {
  for (;;) {
#if defined(_WIN32)
    ::Sleep(1000);
#else
    ::pause();
#endif
  }
}

void acquire_crash_lock() {
  const std::uint64_t self = current_tid();
  std::uint64_t expected = 0;
  if (g_handling_tid.compare_exchange_strong(expected, self, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    return;
  }
  if (expected == self) {
    die_hard();
  }
  wait_other_crash_writer();
}

void release_crash_lock() {
  g_handling_tid.store(0, std::memory_order_release);
}

bool dialogs_suppressed() {
  if (g_no_dialog) return true;
  const char* e = std::getenv("WDS_CRASH_NO_DIALOG");
  return e != nullptr && std::strcmp(e, "1") == 0;
}

void format_timestamp(char* out, std::size_t cap) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  std::snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
}

bool is_crash_artifact_name(const char* name) {
  if (name == nullptr) return false;
  if (std::strncmp(name, "crash-", 6) != 0) return false;
  const std::size_t n = std::strlen(name);
  if (n >= 4 && std::strcmp(name + n - 4, ".txt") == 0) return true;
  if (n >= 4 && std::strcmp(name + n - 4, ".dmp") == 0) return true;
  return false;
}

void join_log_path(char* out, std::size_t cap, const char* name) {
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  if (g_log_dir[0] == '\0' || name == nullptr) return;
#if defined(_WIN32)
  std::snprintf(out, cap, "%s\\%s", g_log_dir, name);
#else
  std::snprintf(out, cap, "%s/%s", g_log_dir, name);
#endif
}

void sentinel_path(char* out, std::size_t cap) {
  join_log_path(out, cap, "running.sentinel");
}

void capture_os_version() {
  g_os_version[0] = '\0';
#if defined(_WIN32)
  OSVERSIONINFOW vi{};
  vi.dwOSVersionInfoSize = sizeof(vi);
  bool ok = false;
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll != nullptr) {
    auto rtl = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl != nullptr) {
      ok = (rtl(&vi) == 0);
    }
  }
  if (!ok) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    ok = ::GetVersionExW(&vi) != 0;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  }
  if (ok) {
    std::snprintf(g_os_version, sizeof(g_os_version), "Windows %lu.%lu.%lu",
                  static_cast<unsigned long>(vi.dwMajorVersion),
                  static_cast<unsigned long>(vi.dwMinorVersion),
                  static_cast<unsigned long>(vi.dwBuildNumber));
  } else {
    copy_trunc(g_os_version, sizeof(g_os_version), "Windows");
  }
#else
  utsname u{};
  if (::uname(&u) == 0) {
    std::snprintf(g_os_version, sizeof(g_os_version), "%s %s %s", u.sysname, u.release, u.machine);
  } else {
    copy_trunc(g_os_version, sizeof(g_os_version), "POSIX");
  }
#endif
}

void capture_main_slide() {
  g_main_slide = 0;
#if defined(_WIN32)
  g_main_slide = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)));
#elif defined(__APPLE__)
  g_main_slide = static_cast<std::uint64_t>(_dyld_get_image_vmaddr_slide(0));
#else
  Dl_info info{};
  if (::dladdr(reinterpret_cast<const void*>(&install_crash_handlers), &info) != 0 &&
      info.dli_fbase != nullptr) {
    g_main_slide = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.dli_fbase));
  }
#endif
}

void build_install_header() {
  std::snprintf(g_install_header, sizeof(g_install_header),
                "WDS Editor crash report\n"
                "version: %s\n"
                "git: %s\n"
                "build: %s\n"
                "os: %s\n"
                "main_slide=0x%llx\n"
                "install_time: %s\n",
                WDS_APP_VERSION, WDS_GIT_HASH, WDS_BUILD_TYPE,
                g_os_version[0] != '\0' ? g_os_version : "unknown",
                static_cast<unsigned long long>(g_main_slide),
                g_install_stamp[0] != '\0' ? g_install_stamp : "unknown");
}

#if defined(_WIN32)
std::uint64_t crash_uptime_ms() {
  const ULONGLONG now = ::GetTickCount64();
  return now >= g_install_tick ? static_cast<std::uint64_t>(now - g_install_tick) : 0;
}
#else
std::uint64_t crash_uptime_ms() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  const std::uint64_t start_ms = static_cast<std::uint64_t>(g_install_mono.tv_sec) * 1000ull +
                                 static_cast<std::uint64_t>(g_install_mono.tv_nsec) / 1000000ull;
  const std::uint64_t now_ms = static_cast<std::uint64_t>(now.tv_sec) * 1000ull +
                               static_cast<std::uint64_t>(now.tv_nsec) / 1000000ull;
  return now_ms >= start_ms ? now_ms - start_ms : 0;
}
#endif

bool ensure_log_dir() {
  if (g_log_dir[0] == '\0') return false;
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(std::string(g_log_dir));
  if (wide.empty()) return false;
  std::wstring wds = wide;
  const auto pos = wds.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    const std::wstring parent = wds.substr(0, pos);
    ::CreateDirectoryW(parent.c_str(), nullptr);
  }
  return ::CreateDirectoryW(wide.c_str(), nullptr) != 0 || ::GetLastError() == ERROR_ALREADY_EXISTS;
#else
  char tmp[kPathCap];
  std::snprintf(tmp, sizeof(tmp), "%s", g_log_dir);
  const std::size_t n = std::strlen(tmp);
  for (std::size_t i = 1; i < n; ++i) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      ::mkdir(tmp, 0755);
      tmp[i] = '/';
    }
  }
  return ::mkdir(g_log_dir, 0755) == 0 || errno == EEXIST;
#endif
}

void resolve_log_dir() {
  g_log_dir[0] = '\0';
#if defined(_WIN32)
  wchar_t wover[kPathCap] = {};
  const DWORD nover = ::GetEnvironmentVariableW(L"WDS_CRASH_LOG_DIR", wover, kPathCap);
  if (nover > 0 && nover < kPathCap) {
    const std::string utf8 = wide_to_utf8(std::wstring(wover));
    copy_trunc(g_log_dir, sizeof(g_log_dir), utf8.c_str());
    return;
  }
  wchar_t base[kPathCap] = {};
  const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, kPathCap);
  if (n > 0 && n < kPathCap) {
    std::wstring wide(base);
    wide += L"\\WDS\\logs";
    const std::string utf8 = wide_to_utf8(wide);
    copy_trunc(g_log_dir, sizeof(g_log_dir), utf8.c_str());
    return;
  }
  const DWORD n2 = ::GetEnvironmentVariableW(L"USERPROFILE", base, kPathCap);
  if (n2 > 0 && n2 < kPathCap) {
    std::wstring wide(base);
    wide += L"\\AppData\\Local\\WDS\\logs";
    const std::string utf8 = wide_to_utf8(wide);
    copy_trunc(g_log_dir, sizeof(g_log_dir), utf8.c_str());
  }
#elif defined(__APPLE__)
  const char* over = std::getenv("WDS_CRASH_LOG_DIR");
  if (over != nullptr && over[0] != '\0') {
    copy_trunc(g_log_dir, sizeof(g_log_dir), over);
    return;
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    std::snprintf(g_log_dir, sizeof(g_log_dir), "%s/Library/Application Support/WDS/logs", home);
  }
#else
  const char* over = std::getenv("WDS_CRASH_LOG_DIR");
  if (over != nullptr && over[0] != '\0') {
    copy_trunc(g_log_dir, sizeof(g_log_dir), over);
    return;
  }
  const char* xdg = std::getenv("XDG_STATE_HOME");
  if (xdg != nullptr && xdg[0] != '\0') {
    std::snprintf(g_log_dir, sizeof(g_log_dir), "%s/WDS/logs", xdg);
  } else {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      std::snprintf(g_log_dir, sizeof(g_log_dir), "%s/.local/share/WDS/logs", home);
    }
  }
#endif
}

bool pid_alive(std::int64_t pid) {
  if (pid <= 0) return false;
#if defined(_WIN32)
  const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) {
    return ::GetLastError() == ERROR_ACCESS_DENIED;
  }
  DWORD code = 0;
  const bool alive = ::GetExitCodeProcess(h, &code) != 0 && code == STILL_ACTIVE;
  ::CloseHandle(h);
  return alive;
#else
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno != ESRCH;
#endif
}

void delete_path_utf8(const char* path) {
  if (path == nullptr || path[0] == '\0') return;
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(std::string(path));
  if (!wide.empty()) {
    ::DeleteFileW(wide.c_str());
  }
#else
  ::unlink(path);
#endif
}

// Size in bytes, or -1 when the file does not exist / cannot be read.
std::int64_t file_size_utf8(const char* path) {
  if (path == nullptr || path[0] == '\0') return -1;
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(std::string(path));
  if (wide.empty()) return -1;
  const HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER got{};
  const bool ok = ::GetFileSizeEx(file, &got) != 0;
  ::CloseHandle(file);
  return ok ? static_cast<std::int64_t>(got.QuadPart) : -1;
#else
  struct stat st {};
  if (::stat(path, &st) != 0) return -1;
  return static_cast<std::int64_t>(st.st_size);
#endif
}

struct CrashName {
  char name[256];
};

int crash_name_cmp_desc(const void* a, const void* b) {
  return std::strcmp(static_cast<const CrashName*>(b)->name, static_cast<const CrashName*>(a)->name);
}

void rotate_old_crashes() {
  if (g_log_dir[0] == '\0') return;
  CrashName names[64] = {};
  int count = 0;
#if defined(_WIN32)
  const std::wstring dirw = utf8_to_wide(std::string(g_log_dir));
  if (dirw.empty()) return;
  const std::wstring pattern = dirw + L"\\crash-*";
  WIN32_FIND_DATAW fd{};
  const HANDLE find = ::FindFirstFileW(pattern.c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::string name = wide_to_utf8(fd.cFileName);
    if (!is_crash_artifact_name(name.c_str())) continue;
    if (count < 64) {
      copy_trunc(names[count].name, sizeof(names[count].name), name.c_str());
      ++count;
    } else {
      char path[kPathCap];
      join_log_path(path, sizeof(path), name.c_str());
      delete_path_utf8(path);
    }
  } while (::FindNextFileW(find, &fd));
  ::FindClose(find);
#else
  DIR* dir = ::opendir(g_log_dir);
  if (dir == nullptr) return;
  while (const dirent* ent = ::readdir(dir)) {
    if (!is_crash_artifact_name(ent->d_name)) continue;
    if (count < 64) {
      copy_trunc(names[count].name, sizeof(names[count].name), ent->d_name);
      ++count;
    } else {
      char path[kPathCap];
      join_log_path(path, sizeof(path), ent->d_name);
      ::unlink(path);
    }
  }
  ::closedir(dir);
#endif
  if (count <= kRotateKeep) return;
  std::qsort(names, static_cast<std::size_t>(count), sizeof(CrashName), crash_name_cmp_desc);
  for (int i = kRotateKeep; i < count; ++i) {
    char path[kPathCap];
    join_log_path(path, sizeof(path), names[i].name);
#if defined(_WIN32)
    delete_path_utf8(path);
#else
    ::unlink(path);
#endif
  }
}

bool inspect_stale_sentinel(char* out, std::size_t cap) {
  if (out != nullptr && cap > 0) out[0] = '\0';
  if (g_log_dir[0] == '\0') return false;
  char sent[kPathCap];
  sentinel_path(sent, sizeof(sent));
  char buf[kPathCap * 2] = {};
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(std::string(sent));
  if (wide.empty()) return false;
  const HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  (void)::ReadFile(file, buf, static_cast<DWORD>(sizeof(buf) - 1), &n, nullptr);
  ::CloseHandle(file);
#else
  const int fd = ::open(sent, O_RDONLY);
  if (fd < 0) return false;
  const ssize_t nread = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (nread < 0) return false;
#endif
  std::int64_t pid = 0;
  char crash[kPathCap] = {};
  const char* p = buf;
  while (*p != '\0') {
    if (std::strncmp(p, "pid=", 4) == 0) {
      pid = std::strtoll(p + 4, nullptr, 10);
    } else if (std::strncmp(p, "crash=", 6) == 0) {
      const char* s = p + 6;
      std::size_t i = 0;
      while (s[i] != '\0' && s[i] != '\n' && s[i] != '\r' && i + 1 < sizeof(crash)) {
        crash[i] = s[i];
        ++i;
      }
      crash[i] = '\0';
    }
    while (*p != '\0' && *p != '\n') ++p;
    if (*p == '\n') ++p;
  }
  if (pid_alive(pid)) return false;
  // A stale sentinel alone is not a crash: SIGKILL / force quit / power loss
  // also leave it behind. Only report when the session's crash file actually
  // received a report body (more than the "session start" header line).
  if (crash[0] != '\0') {
    const std::int64_t size = file_size_utf8(crash);
    if (size > static_cast<std::int64_t>(sizeof(kSessionStartLine) - 1)) {
      if (out != nullptr && cap > 0) copy_trunc(out, cap, crash);
      return true;
    }
    // Unclean exit without a report: clean up quietly.
    if (size >= 0) delete_path_utf8(crash);
    delete_path_utf8(sent);
    return false;
  }
  delete_path_utf8(sent);
  return false;
}

void write_sentinel_file() {
  char sent[kPathCap];
  sentinel_path(sent, sizeof(sent));
  char body[kPathCap + 64];
#if defined(_WIN32)
  std::snprintf(body, sizeof(body), "pid=%lu\ncrash=%s\n",
                static_cast<unsigned long>(::GetCurrentProcessId()), g_crash_path);
  const std::wstring wide = utf8_to_wide(std::string(sent));
  if (wide.empty()) return;
  const HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(file, body, static_cast<DWORD>(std::strlen(body)), &written, nullptr);
  ::CloseHandle(file);
#else
  std::snprintf(body, sizeof(body), "pid=%ld\ncrash=%s\n", static_cast<long>(::getpid()),
                g_crash_path);
  const int fd = ::open(sent, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  (void)::write(fd, body, std::strlen(body));
  ::close(fd);
#endif
}

void delete_header_only_crash_file() {
  if (g_crash_path[0] == '\0') return;
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(std::string(g_crash_path));
  if (wide.empty()) return;
  const HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER got{};
  const bool ok = ::GetFileSizeEx(file, &got) != 0;
  ::CloseHandle(file);
  if (ok && got.QuadPart <= static_cast<LONGLONG>(g_header_only_size)) {
    ::DeleteFileW(wide.c_str());
  }
#else
  struct stat st {};
  if (::stat(g_crash_path, &st) == 0 && st.st_size <= g_header_only_size) {
    ::unlink(g_crash_path);
  }
#endif
}

#if defined(_WIN32)
void write_win_text(HANDLE file, const char* text) {
  if (file == INVALID_HANDLE_VALUE || text == nullptr) return;
  const DWORD n = static_cast<DWORD>(std::strlen(text));
  if (n == 0) return;
  DWORD written = 0;
  ::WriteFile(file, text, n, &written, nullptr);
}

void write_win_bytes(HANDLE file, const char* data, std::size_t n) {
  if (file == INVALID_HANDLE_VALUE || data == nullptr || n == 0) return;
  DWORD written = 0;
  ::WriteFile(file, data, static_cast<DWORD>(n), &written, nullptr);
}

void write_win_fmt(HANDLE file, const char* fmt, ...) {
  char buf[768];
  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n > 0) {
    write_win_text(file, buf);
  }
}

void emit_journal_win(void* ctx, const char* data, std::size_t n) {
  write_win_bytes(static_cast<HANDLE>(ctx), data, n);
}

void write_ctx_win(HANDLE file) {
  for (int i = 0; i < kCtxCount; ++i) {
    if (g_ctx[i][0] == '\0') continue;
    write_win_fmt(file, "ctx.%s: %s\n", ctx_field_name(i), g_ctx[i]);
  }
}

void append_module_for_addr(HANDLE file, const char* label, const void* addr) {
  if (addr == nullptr) {
    write_win_fmt(file, "%s: (null)\n", label);
    return;
  }
  HMODULE hm = nullptr;
  if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &hm) ||
      hm == nullptr) {
    write_win_fmt(file, "%s: %p (module unknown)\n", label, addr);
    return;
  }
  char name[MAX_PATH] = {};
  ::GetModuleFileNameA(hm, name, MAX_PATH);
  char shown[MAX_PATH] = {};
  journal_copy_path(shown, sizeof(shown), name);
  const auto base = reinterpret_cast<uintptr_t>(hm);
  const auto off = reinterpret_cast<uintptr_t>(addr) - base;
  write_win_fmt(file, "%s: %p  %s+0x%llx\n", label, addr, shown,
                static_cast<unsigned long long>(off));
}

void append_modules_win(HANDLE file) {
  write_win_text(file, "--- modules ---\n");
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           ::GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) {
    write_win_text(file, "  (CreateToolhelp32Snapshot failed)\n");
    return;
  }
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  int count = 0;
  if (::Module32FirstW(snap, &me)) {
    do {
      char name[MAX_PATH] = {};
      ::WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, MAX_PATH, nullptr, nullptr);
      write_win_fmt(file, "  %p-%p  %s\n", static_cast<void*>(me.modBaseAddr),
                    static_cast<void*>(me.modBaseAddr + me.modBaseSize), name);
      ++count;
    } while (count < 64 && ::Module32NextW(snap, &me));
  }
  ::CloseHandle(snap);
}

void append_exception_stack_win(HANDLE file, CONTEXT* ctx) {
  write_win_text(file, "--- stack ---\n");
  if (ctx == nullptr) {
    write_win_text(file, "  (no context)\n");
    return;
  }
  if (g_stack_walk == nullptr || g_fn_table == nullptr || g_mod_base == nullptr) {
    write_win_text(file, "  (dbghelp stack walk unavailable)\n");
    return;
  }
  const HANDLE process = ::GetCurrentProcess();
  const HANDLE thread = ::GetCurrentThread();
  if (g_sym_init != nullptr) {
    (void)g_sym_init(process, nullptr, TRUE);
  }
  CONTEXT copy = *ctx;
  STACKFRAME64 frame{};
#if defined(_M_X64) || defined(__x86_64__)
  const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = copy.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrStack.Offset = copy.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = copy.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
#else
  const DWORD machine = IMAGE_FILE_MACHINE_I386;
  frame.AddrPC.Offset = copy.Eip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrStack.Offset = copy.Esp;
  frame.AddrStack.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = copy.Ebp;
  frame.AddrFrame.Mode = AddrModeFlat;
#endif
  for (unsigned i = 0; i < 48; ++i) {
    if (!g_stack_walk(machine, process, thread, &frame, &copy, nullptr, g_fn_table, g_mod_base,
                      nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) break;
    const void* pc = reinterpret_cast<void*>(static_cast<uintptr_t>(frame.AddrPC.Offset));
    write_win_fmt(file, "  #%u ", i);
    append_module_for_addr(file, "pc", pc);
  }
}

void append_exception_win(HANDLE file, EXCEPTION_POINTERS* info) {
  write_win_text(file, "--- exception ---\n");
  if (info == nullptr || info->ExceptionRecord == nullptr) {
    write_win_text(file, "  (no EXCEPTION_POINTERS)\n");
    return;
  }
  EXCEPTION_RECORD* rec = info->ExceptionRecord;
  write_win_fmt(file, "code=0x%08lX address=%p params=%lu\n",
                static_cast<unsigned long>(rec->ExceptionCode), rec->ExceptionAddress,
                static_cast<unsigned long>(rec->NumberParameters));
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const ULONG_PTR op = rec->ExceptionInformation[0];
    const char* op_name = op == 0 ? "read" : (op == 1 ? "write" : (op == 8 ? "dep" : "other"));
    write_win_fmt(file, "access=%s fault=%p\n", op_name,
                  reinterpret_cast<void*>(rec->ExceptionInformation[1]));
  }
  append_module_for_addr(file, "fault_pc", rec->ExceptionAddress);
  if (info->ContextRecord != nullptr) {
#if defined(_M_X64) || defined(__x86_64__)
    write_win_fmt(file, "rip=%p rsp=%p rbp=%p\n",
                  reinterpret_cast<void*>(info->ContextRecord->Rip),
                  reinterpret_cast<void*>(info->ContextRecord->Rsp),
                  reinterpret_cast<void*>(info->ContextRecord->Rbp));
#endif
  }
}

void write_minidump(EXCEPTION_POINTERS* info) {
  if (g_minidump_write == nullptr || g_dump_path[0] == '\0') return;
  const std::wstring wide = utf8_to_wide(std::string(g_dump_path));
  if (wide.empty()) return;
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei{};
  PMINIDUMP_EXCEPTION_INFORMATION pmei = nullptr;
  if (info != nullptr) {
    mei.ThreadId = ::GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    pmei = &mei;
  }
  const DWORD rich = static_cast<DWORD>(MiniDumpWithIndirectlyReferencedMemory) |
                     static_cast<DWORD>(MiniDumpScanMemory) |
                     static_cast<DWORD>(MiniDumpWithThreadInfo);
  BOOL ok = g_minidump_write(::GetCurrentProcess(), ::GetCurrentProcessId(), file,
                             static_cast<MINIDUMP_TYPE>(rich), pmei, nullptr, nullptr);
  if (!ok) {
    ::SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    ::SetEndOfFile(file);
    (void)g_minidump_write(::GetCurrentProcess(), ::GetCurrentProcessId(), file, MiniDumpNormal,
                           pmei, nullptr, nullptr);
  }
  ::CloseHandle(file);
}

HANDLE crash_file_handle() {
  if (g_crash_file != INVALID_HANDLE_VALUE) return g_crash_file;
  if (g_crash_path[0] == '\0') return INVALID_HANDLE_VALUE;
  const std::wstring wide = utf8_to_wide(std::string(g_crash_path));
  if (wide.empty()) return INVALID_HANDLE_VALUE;
  return ::CreateFileW(wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void write_win_report(HANDLE file, const char* kind, const char* detail, const char* exception_type,
                      EXCEPTION_POINTERS* info) {
  if (file == INVALID_HANDLE_VALUE) return;
  write_win_text(file, g_install_header);
  write_win_fmt(file, "kind: %s\n", kind != nullptr ? kind : "(unknown)");
  write_win_fmt(file, "detail: %s\n", detail != nullptr ? detail : "(none)");
  if (exception_type != nullptr && exception_type[0] != '\0') {
    write_win_fmt(file, "exception_type: %s\n", exception_type);
  }
  write_win_fmt(file, "privacy_sensitive: %d\n", journal_allow_sensitive() ? 1 : 0);
  write_win_fmt(file, "uptime_ms: %llu\n", static_cast<unsigned long long>(crash_uptime_ms()));
  write_ctx_win(file);
  append_exception_win(file, info);
  if (info != nullptr) {
    append_exception_stack_win(file, info->ContextRecord);
  } else {
    write_win_text(file, "--- stack ---\n");
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    ::RtlCaptureContext(&ctx);
    append_exception_stack_win(file, &ctx);
  }
  journal_write_text(emit_journal_win, file);
  log_ring_write_text(emit_journal_win, file);
  append_modules_win(file);
}

void preload_dbghelp() {
  g_dbghelp = ::LoadLibraryW(L"dbghelp.dll");
  if (g_dbghelp == nullptr) return;
  g_minidump_write =
      reinterpret_cast<MiniDumpWriteDumpFn>(::GetProcAddress(g_dbghelp, "MiniDumpWriteDump"));
  g_sym_init = reinterpret_cast<SymInitFn>(::GetProcAddress(g_dbghelp, "SymInitialize"));
  g_stack_walk = reinterpret_cast<StackWalkFn>(::GetProcAddress(g_dbghelp, "StackWalk64"));
  g_fn_table =
      reinterpret_cast<SymFnTableFn>(::GetProcAddress(g_dbghelp, "SymFunctionTableAccess64"));
  g_mod_base = reinterpret_cast<SymBaseFn>(::GetProcAddress(g_dbghelp, "SymGetModuleBase64"));
}

void open_session_crash_file() {
  format_timestamp(g_install_stamp, sizeof(g_install_stamp));
  std::snprintf(g_crash_path, sizeof(g_crash_path), "%s\\crash-%s-%lu.txt", g_log_dir,
                g_install_stamp, static_cast<unsigned long>(::GetCurrentProcessId()));
  std::snprintf(g_dump_path, sizeof(g_dump_path), "%s\\crash-%s-%lu.dmp", g_log_dir, g_install_stamp,
                static_cast<unsigned long>(::GetCurrentProcessId()));
  const std::wstring wide = utf8_to_wide(std::string(g_crash_path));
  if (wide.empty()) return;
  g_crash_file = ::CreateFileW(wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_crash_file == INVALID_HANDLE_VALUE) return;
  write_win_text(g_crash_file, kSessionStartLine);
  LARGE_INTEGER pos{};
  if (::GetFileSizeEx(g_crash_file, &pos)) {
    g_header_only_size = static_cast<LONG>(pos.QuadPart);
  } else {
    g_header_only_size = static_cast<LONG>(sizeof(kSessionStartLine) - 1);
  }
}
#else
const char* signal_name(int sig);

void asafe_write_bytes(int fd, const char* data, std::size_t n) {
  if (fd < 0 || data == nullptr || n == 0) return;
  (void)::write(fd, data, n);
}

void asafe_write_cstr_fd(int fd, const char* s) {
  if (fd < 0 || s == nullptr) return;
  std::size_t n = 0;
  while (s[n] != '\0' && n < kMsgCap) {
    ++n;
  }
  asafe_write_bytes(fd, s, n);
}

void asafe_write_uint_fd(int fd, std::uint64_t v) {
  char buf[24];
  int i = 23;
  buf[i] = '\0';
  if (v == 0) {
    buf[--i] = '0';
  } else {
    while (v > 0 && i > 0) {
      buf[--i] = static_cast<char>('0' + static_cast<int>(v % 10));
      v /= 10;
    }
  }
  asafe_write_cstr_fd(fd, buf + i);
}

void asafe_write_int_fd(int fd, std::int64_t v) {
  if (v < 0) {
    asafe_write_cstr_fd(fd, "-");
    asafe_write_uint_fd(fd, static_cast<std::uint64_t>(-v));
    return;
  }
  asafe_write_uint_fd(fd, static_cast<std::uint64_t>(v));
}

void asafe_write_hex_fd(int fd, std::uint64_t v) {
  char buf[18];
  buf[0] = '0';
  buf[1] = 'x';
  constexpr char kHex[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    buf[2 + i] = kHex[v & 0xf];
    v >>= 4;
  }
  asafe_write_bytes(fd, buf, 18);
}

void emit_journal_fd(void* ctx, const char* data, std::size_t n) {
  asafe_write_bytes(static_cast<int>(reinterpret_cast<std::intptr_t>(ctx)), data, n);
}

void write_ctx_fd(int fd) {
  for (int i = 0; i < kCtxCount; ++i) {
    if (g_ctx[i][0] == '\0') continue;
    asafe_write_cstr_fd(fd, "ctx.");
    asafe_write_cstr_fd(fd, ctx_field_name(i));
    asafe_write_cstr_fd(fd, ": ");
    asafe_write_cstr_fd(fd, g_ctx[i]);
    asafe_write_cstr_fd(fd, "\n");
  }
}

void write_privacy_and_uptime_fd(int fd) {
  asafe_write_cstr_fd(fd, "privacy_sensitive: ");
  asafe_write_cstr_fd(fd, journal_allow_sensitive() ? "1" : "0");
  asafe_write_cstr_fd(fd, "\nuptime_ms: ");
  asafe_write_uint_fd(fd, crash_uptime_ms());
  asafe_write_cstr_fd(fd, "\n");
}

void write_dladdr_block(int fd, void* const* frames, int n) {
  asafe_write_cstr_fd(fd, "--- images ---\n");
  if (frames == nullptr || n <= 0) return;
  for (int i = 0; i < n; ++i) {
    Dl_info info{};
    asafe_write_cstr_fd(fd, "  #");
    asafe_write_int_fd(fd, i);
    if (frames[i] == nullptr || ::dladdr(frames[i], &info) == 0 || info.dli_fbase == nullptr) {
      asafe_write_cstr_fd(fd, " (unresolved)\n");
      continue;
    }
    int image_index = -1;
#if defined(__APPLE__)
    const std::uint32_t count = _dyld_image_count();
    for (std::uint32_t img = 0; img < count; ++img) {
      if (_dyld_get_image_header(img) == static_cast<const mach_header*>(info.dli_fbase)) {
        image_index = static_cast<int>(img);
        break;
      }
    }
#endif
    const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
    const auto off = addr >= base ? addr - base : 0;
    asafe_write_cstr_fd(fd, " image_index=");
    asafe_write_int_fd(fd, image_index);
    asafe_write_cstr_fd(fd, " base=");
    asafe_write_hex_fd(fd, static_cast<std::uint64_t>(base));
    asafe_write_cstr_fd(fd, "+");
    asafe_write_hex_fd(fd, static_cast<std::uint64_t>(off));
    asafe_write_cstr_fd(fd, "\n");
  }
}

void write_report_tail(int fd, void* const* frames, int nframes) {
  journal_write_text(emit_journal_fd, reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
  log_ring_write_text(emit_journal_fd, reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
  if (g_symbolize_in_handler) {
    write_dladdr_block(fd, frames, nframes);
  }
}

void read_pc_sp(const void* uctx, std::uint64_t* pc, std::uint64_t* sp) {
  if (pc != nullptr) *pc = 0;
  if (sp != nullptr) *sp = 0;
  if (uctx == nullptr) return;
  const auto* uc = static_cast<const ucontext_t*>(uctx);
#if defined(__APPLE__) && defined(__arm64__)
  if (uc->uc_mcontext == nullptr) return;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#endif
  if (pc != nullptr) {
    *pc = static_cast<std::uint64_t>(
        __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
  }
  if (sp != nullptr) {
    *sp = static_cast<std::uint64_t>(
        __darwin_arm_thread_state64_get_sp(uc->uc_mcontext->__ss));
  }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#elif defined(__APPLE__) && defined(__x86_64__)
  if (uc->uc_mcontext == nullptr) return;
  if (pc != nullptr) *pc = static_cast<std::uint64_t>(uc->uc_mcontext->__ss.__rip);
  if (sp != nullptr) *sp = static_cast<std::uint64_t>(uc->uc_mcontext->__ss.__rsp);
#elif defined(__linux__) && defined(__x86_64__)
  if (pc != nullptr) *pc = static_cast<std::uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  if (sp != nullptr) *sp = static_cast<std::uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(__aarch64__)
  if (pc != nullptr) *pc = static_cast<std::uint64_t>(uc->uc_mcontext.pc);
  if (sp != nullptr) *sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);
#else
  (void)uc;
#endif
}

int open_crash_fd_for_write(bool* opened_here) {
  if (opened_here != nullptr) *opened_here = false;
  if (g_crash_fd >= 0) return g_crash_fd;
  if (g_crash_path[0] == '\0') return -1;
  const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0 && opened_here != nullptr) *opened_here = true;
  return fd;
}

void write_posix_signal_report(int fd, int sig, const siginfo_t* info, void* uctx) {
  asafe_write_cstr_fd(fd, g_install_header);
  asafe_write_cstr_fd(fd, "kind: signal\n");
  asafe_write_cstr_fd(fd, "signal: ");
  asafe_write_int_fd(fd, sig);
  asafe_write_cstr_fd(fd, " ");
  asafe_write_cstr_fd(fd, signal_name(sig));
  asafe_write_cstr_fd(fd, "\n");
  if (info != nullptr) {
    asafe_write_cstr_fd(fd, "si_code: ");
    asafe_write_int_fd(fd, info->si_code);
    asafe_write_cstr_fd(fd, "\nsi_addr: ");
    asafe_write_hex_fd(fd, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info->si_addr)));
    asafe_write_cstr_fd(fd, "\n");
  }
  std::uint64_t pc = 0;
  std::uint64_t sp = 0;
  read_pc_sp(uctx, &pc, &sp);
  asafe_write_cstr_fd(fd, "pc: ");
  asafe_write_hex_fd(fd, pc);
  asafe_write_cstr_fd(fd, "\nsp: ");
  asafe_write_hex_fd(fd, sp);
  asafe_write_cstr_fd(fd, "\nthread: ");
  asafe_write_hex_fd(fd, current_tid());
  asafe_write_cstr_fd(fd, "\n");
  write_privacy_and_uptime_fd(fd);
  write_ctx_fd(fd);
  asafe_write_cstr_fd(fd, "--- stack ---\n");
  void* frames[64] = {};
  const int n = ::backtrace(frames, 64);
  if (n > 0) {
    ::backtrace_symbols_fd(frames, n, fd);
  }
  write_report_tail(fd, frames, n);
}

void write_posix_text_report(int fd, const char* kind, const char* detail,
                             const char* exception_type) {
  asafe_write_cstr_fd(fd, g_install_header);
  asafe_write_cstr_fd(fd, "kind: ");
  asafe_write_cstr_fd(fd, kind != nullptr ? kind : "(unknown)");
  asafe_write_cstr_fd(fd, "\ndetail: ");
  asafe_write_cstr_fd(fd, detail != nullptr ? detail : "(none)");
  asafe_write_cstr_fd(fd, "\n");
  if (exception_type != nullptr && exception_type[0] != '\0') {
    asafe_write_cstr_fd(fd, "exception_type: ");
    asafe_write_cstr_fd(fd, exception_type);
    asafe_write_cstr_fd(fd, "\n");
  }
  write_privacy_and_uptime_fd(fd);
  write_ctx_fd(fd);
  asafe_write_cstr_fd(fd, "--- stack ---\n");
  void* frames[64] = {};
  const int n = ::backtrace(frames, 64);
  if (n > 0) {
    ::backtrace_symbols_fd(frames, n, fd);
  }
  write_report_tail(fd, frames, n);
}

void open_session_crash_file() {
  format_timestamp(g_install_stamp, sizeof(g_install_stamp));
  std::snprintf(g_crash_path, sizeof(g_crash_path), "%s/crash-%s-%ld.txt", g_log_dir, g_install_stamp,
                static_cast<long>(::getpid()));
  g_crash_fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (g_crash_fd < 0) return;
  asafe_write_bytes(g_crash_fd, kSessionStartLine, sizeof(kSessionStartLine) - 1);
  const off_t pos = ::lseek(g_crash_fd, 0, SEEK_CUR);
  g_header_only_size = pos >= 0 ? pos : static_cast<off_t>(sizeof(kSessionStartLine) - 1);
}

void install_main_altstack() {
  stack_t ss{};
  ss.ss_sp = g_main_altstack;
  ss.ss_size = sizeof(g_main_altstack);
  ss.ss_flags = 0;
  (void)::sigaltstack(&ss, nullptr);
}

void prewarm_backtrace() {
  void* scratch[16] = {};
  (void)::backtrace(scratch, 16);
}

const char* signal_name(int sig);

void handle_crash_from_signal(int sig, const siginfo_t* info, void* uctx) {
  acquire_crash_lock();
  // terminate_handler / report_fatal already wrote a full report and then
  // abort()ed to reach the OS crash reporter: only append a marker line.
  if (g_reported_by_terminate.load(std::memory_order_acquire) ||
      g_reported_full.load(std::memory_order_acquire)) {
    bool opened = false;
    const int fd = open_crash_fd_for_write(&opened);
    if (fd >= 0) {
      asafe_write_cstr_fd(fd, "(re-raised from terminate)\n");
      if (opened) ::close(fd);
    }
    return;
  }

  asafe_write_bytes(STDERR_FILENO, "[wds] FATAL signal: ", 20);
  asafe_write_cstr_fd(STDERR_FILENO, signal_name(sig));
  asafe_write_bytes(STDERR_FILENO, "\n", 1);
  if (g_crash_path[0] != '\0') {
    asafe_write_bytes(STDERR_FILENO, "[wds] crash log: ", 17);
    asafe_write_cstr_fd(STDERR_FILENO, g_crash_path);
    asafe_write_bytes(STDERR_FILENO, "\n", 1);
  }

  bool opened = false;
  const int fd = open_crash_fd_for_write(&opened);
  if (fd >= 0) {
    write_posix_signal_report(fd, sig, info, uctx);
    if (opened) ::close(fd);
  }
  g_reported_full.store(true, std::memory_order_release);
}

void fatal_signal_handler(int sig, siginfo_t* info, void* uctx) {
  handle_crash_from_signal(sig, info, uctx);
  ::raise(sig);
}

void install_fatal_sigaction(int sig) {
  struct sigaction sa {};
  sa.sa_sigaction = fatal_signal_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  (void)::sigaction(sig, &sa, nullptr);
}

// SIGTERM / SIGINT / SIGHUP are requested shutdowns, not crashes: drop the
// sentinel and the header-only session file (unlink/fstat are AS-safe), then
// let the default action terminate the process as usual.
char g_sentinel_path_asafe[kPathCap] = {};

void termination_signal_handler(int sig) {
  if (g_sentinel_path_asafe[0] != '\0') {
    ::unlink(g_sentinel_path_asafe);
  }
  if (g_crash_fd >= 0 && g_crash_path[0] != '\0') {
    struct stat st {};
    if (::fstat(g_crash_fd, &st) == 0 && st.st_size <= g_header_only_size) {
      ::unlink(g_crash_path);
    }
  }
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

void install_termination_sigaction(int sig) {
  struct sigaction prev {};
  if (::sigaction(sig, nullptr, &prev) == 0 && prev.sa_handler != SIG_DFL) {
    return;  // The host already handles it (e.g. an embedder); leave it alone.
  }
  struct sigaction sa {};
  sa.sa_handler = termination_signal_handler;
  sa.sa_flags = SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  (void)::sigaction(sig, &sa, nullptr);
}
#endif

#if !defined(_WIN32)
const char* signal_name(int sig) {
  switch (sig) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGABRT:
      return "SIGABRT";
    case SIGFPE:
      return "SIGFPE";
    case SIGILL:
      return "SIGILL";
#ifdef SIGBUS
    case SIGBUS:
      return "SIGBUS";
#endif
#ifdef SIGTRAP
    case SIGTRAP:
      return "SIGTRAP";
#endif
#ifdef SIGSYS
    case SIGSYS:
      return "SIGSYS";
#endif
    default:
      return "signal";
  }
}
#endif

void build_user_message(char* out, std::size_t cap, const char* kind, const char* detail,
                        const char* log_path, bool log_written) {
  out[0] = '\0';
  append_cstr(out, cap, "WDS Editor 遇到致命错误并已退出。\n\n");
  append_cstr(out, cap, "错误日志地址：\n");
  if (log_path != nullptr && log_path[0] != '\0') {
    append_cstr(out, cap, log_path);
    if (!log_written) {
      append_cstr(out, cap, "\n（写入失败，请检查该目录权限）");
    }
  } else {
    append_cstr(out, cap, "（未知，未能确定日志目录）");
  }
  append_cstr(out, cap, "\n\n");
  if (kind != nullptr && kind[0] != '\0') {
    append_cstr(out, cap, "类型：");
    append_cstr(out, cap, kind);
    append_cstr(out, cap, "\n");
  }
  if (detail != nullptr && detail[0] != '\0') {
    append_cstr(out, cap, "详情：");
    append_cstr(out, cap, detail);
  }
}

void show_crash_dialog(const char* title, const char* message) {
  if (dialogs_suppressed()) return;
#if defined(_WIN32)
  (void)title;
  (void)message;
  // MessageBoxW can deadlock under the loader lock; next-launch UI informs the user.
  return;
#elif defined(__APPLE__)
  const pid_t pid = ::fork();
  if (pid == 0) {
    char script[kMsgCap * 3];
    std::size_t o = 0;
    auto append = [&](const char* s) {
      if (s == nullptr) return;
      for (const char* p = s; *p && o + 1 < sizeof(script); ++p) {
        script[o++] = *p;
      }
    };
    auto append_as_literal = [&](const char* s) {
      append("\"");
      if (s != nullptr) {
        for (const char* p = s; *p && o + 8 < sizeof(script); ++p) {
          if (*p == '\n' || *p == '\r') {
            append("\" & return & \"");
            continue;
          }
          if (*p == '\\' || *p == '"') {
            script[o++] = '\\';
          }
          script[o++] = *p;
        }
      }
      append("\"");
    };
    append("display dialog ");
    append_as_literal(message);
    append(" with title ");
    append_as_literal(title != nullptr ? title : "WDS Editor");
    append(" with icon stop buttons {\"确定\"} default button \"确定\"");
    script[o] = '\0';
    ::execlp("osascript", "osascript", "-e", script, static_cast<char*>(nullptr));
    ::_exit(127);
  }
  if (pid > 0) {
    int status = 0;
    (void)::waitpid(pid, &status, 0);
  }
#else
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::execlp("zenity", "zenity", "--error", "--title", title != nullptr ? title : "WDS Editor",
             "--text", message != nullptr ? message : "", "--width=480",
             static_cast<char*>(nullptr));
    if (title) (void)::write(STDERR_FILENO, title, std::strlen(title));
    (void)::write(STDERR_FILENO, "\n", 1);
    if (message) (void)::write(STDERR_FILENO, message, std::strlen(message));
    (void)::write(STDERR_FILENO, "\n", 1);
    ::_exit(127);
  }
  if (pid > 0) {
    int status = 0;
    (void)::waitpid(pid, &status, 0);
  }
#endif
}

#if defined(_WIN32)
const char* exception_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:
      return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    default:
      return "Unhandled SEH exception";
  }
}

void handle_crash(const char* kind, const char* detail, const char* exception_type,
                  EXCEPTION_POINTERS* info, bool show_dialog) {
  acquire_crash_lock();
  ensure_log_dir();
  HANDLE file = crash_file_handle();
  const bool wrote = file != INVALID_HANDLE_VALUE;
  if (wrote) {
    write_win_report(file, kind, detail, exception_type, info);
    if (file != g_crash_file) {
      ::CloseHandle(file);
    }
  }
  write_minidump(info);
  g_reported_full.store(true, std::memory_order_release);

  std::fprintf(stderr, "[wds] FATAL %s: %s\n", kind != nullptr ? kind : "?",
               detail != nullptr ? detail : "");
  if (wrote) {
    std::fprintf(stderr, "[wds] crash log: %s\n", g_crash_path);
  }
  if (show_dialog) {
    char msg[kMsgCap];
    build_user_message(msg, sizeof(msg), kind, detail, g_crash_path, wrote);
    show_crash_dialog("WDS Editor — 致命错误", msg);
  }
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
  char detail[128];
  const DWORD code =
      info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
  std::snprintf(detail, sizeof(detail), "%s (0x%08lX)", exception_name(code),
                static_cast<unsigned long>(code));
  handle_crash("Windows 异常", detail, nullptr, info, false);
  return EXCEPTION_EXECUTE_HANDLER;
}

LONG CALLBACK vectored_exception_handler(EXCEPTION_POINTERS* info) {
  if (info == nullptr || info->ExceptionRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_reported_full.load(std::memory_order_acquire)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  HANDLE file = crash_file_handle();
  if (file != INVALID_HANDLE_VALUE) {
    write_win_fmt(file, "veh: code=0x%08lX address=%p\n", static_cast<unsigned long>(code),
                  info->ExceptionRecord->ExceptionAddress);
    if (file != g_crash_file) {
      ::CloseHandle(file);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void win_sigabrt_handler(int) {
  if (g_reported_by_terminate.load(std::memory_order_acquire) ||
      g_reported_full.load(std::memory_order_acquire)) {
    HANDLE file = crash_file_handle();
    if (file != INVALID_HANDLE_VALUE) {
      write_win_text(file, "(re-raised from terminate)\n");
      if (file != g_crash_file) {
        ::CloseHandle(file);
      }
    }
  } else {
    handle_crash("signal", "SIGABRT", nullptr, nullptr, false);
  }
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

#if defined(_MSC_VER)
void __cdecl invalid_parameter_handler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                       uintptr_t) {
  handle_crash("invalid_parameter", "CRT invalid parameter", nullptr, nullptr, false);
  release_crash_lock();
  std::abort();
}

void __cdecl purecall_handler() {
  handle_crash("purecall", "pure virtual call", nullptr, nullptr, false);
  release_crash_lock();
  std::abort();
}
#endif
#else
void handle_crash(const char* kind, const char* detail, const char* exception_type,
                  bool show_dialog) {
  acquire_crash_lock();
  ensure_log_dir();
  bool opened = false;
  const int fd = open_crash_fd_for_write(&opened);
  const bool wrote = fd >= 0;
  if (wrote) {
    write_posix_text_report(fd, kind, detail, exception_type);
    if (opened) ::close(fd);
  }
  g_reported_full.store(true, std::memory_order_release);

  std::fprintf(stderr, "[wds] FATAL %s: %s\n", kind != nullptr ? kind : "?",
               detail != nullptr ? detail : "");
  if (wrote) {
    std::fprintf(stderr, "[wds] crash log: %s\n", g_crash_path);
  }
  if (show_dialog) {
    char msg[kMsgCap];
    build_user_message(msg, sizeof(msg), kind, detail, g_crash_path, wrote);
    show_crash_dialog("WDS Editor — 致命错误", msg);
  }
}
#endif

void fill_exception_strings(char* type_buf, std::size_t type_cap, char* what_buf,
                            std::size_t what_cap, const char** detail) {
  if (type_buf != nullptr && type_cap > 0) type_buf[0] = '\0';
  if (what_buf != nullptr && what_cap > 0) what_buf[0] = '\0';
#if !defined(_MSC_VER)
  if (const std::type_info* ti = abi::__cxa_current_exception_type()) {
    int status = 0;
    char* dem = abi::__cxa_demangle(ti->name(), nullptr, nullptr, &status);
    copy_trunc(type_buf, type_cap, (status == 0 && dem != nullptr) ? dem : ti->name());
    std::free(dem);
  }
#endif
  try {
    if (const std::exception_ptr eptr = std::current_exception()) {
      try {
        std::rethrow_exception(eptr);
      } catch (const std::exception& ex) {
        copy_trunc(what_buf, what_cap, ex.what());
        if (detail != nullptr) *detail = what_buf;
      } catch (...) {
        if (detail != nullptr) *detail = "non-std exception";
      }
    }
  } catch (...) {
    if (detail != nullptr) *detail = "exception while inspecting terminate state";
  }
}

void terminate_handler() {
  char type_buf[256];
  char what_buf[512];
  const char* detail = "std::terminate()";
  fill_exception_strings(type_buf, sizeof(type_buf), what_buf, sizeof(what_buf), &detail);
#if defined(_WIN32)
  handle_crash("terminate", detail, type_buf, nullptr, true);
#else
  handle_crash("terminate", detail, type_buf, true);
#endif
  g_reported_by_terminate.store(true, std::memory_order_release);
  release_crash_lock();
  std::abort();
}

void do_install() {
  resolve_log_dir();
  ensure_log_dir();
  g_no_dialog = dialogs_suppressed();
  capture_os_version();
  capture_main_slide();
#if defined(_WIN32)
  g_install_tick = ::GetTickCount64();
#else
  (void)::clock_gettime(CLOCK_MONOTONIC, &g_install_mono);
#endif

  g_prev_crashed.store(inspect_stale_sentinel(g_prev_crash_path, sizeof(g_prev_crash_path)),
                       std::memory_order_release);
  g_checked_prev = true;

  rotate_old_crashes();
  open_session_crash_file();
  build_install_header();
  write_sentinel_file();

  std::set_terminate(terminate_handler);

#if defined(_WIN32)
  preload_dbghelp();
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  ::SetUnhandledExceptionFilter(unhandled_exception_filter);
  (void)::AddVectoredExceptionHandler(0, vectored_exception_handler);
  std::signal(SIGABRT, win_sigabrt_handler);
#if defined(_MSC_VER)
  ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  ::_set_invalid_parameter_handler(invalid_parameter_handler);
  ::_set_purecall_handler(purecall_handler);
#endif
#else
  install_main_altstack();
  prewarm_backtrace();
  install_fatal_sigaction(SIGSEGV);
  install_fatal_sigaction(SIGABRT);
  install_fatal_sigaction(SIGFPE);
  install_fatal_sigaction(SIGILL);
#ifdef SIGBUS
  install_fatal_sigaction(SIGBUS);
#endif
#ifdef SIGTRAP
  install_fatal_sigaction(SIGTRAP);
#endif
#ifdef SIGSYS
  install_fatal_sigaction(SIGSYS);
#endif
  sentinel_path(g_sentinel_path_asafe, sizeof(g_sentinel_path_asafe));
  install_termination_sigaction(SIGTERM);
  install_termination_sigaction(SIGINT);
  install_termination_sigaction(SIGHUP);
#endif
  g_installed.store(true, std::memory_order_release);
}

}  // namespace

void install_crash_handlers() {
  static std::once_flag once;
  std::call_once(once, [] { do_install(); });
}

void install_thread_crash_stack() {
#if !defined(_WIN32)
  alignas(16) thread_local char buf[64 * 1024];
  thread_local bool done = false;
  if (done) return;
  stack_t ss{};
  ss.ss_sp = buf;
  ss.ss_size = sizeof(buf);
  ss.ss_flags = 0;
  if (::sigaltstack(&ss, nullptr) == 0) {
    done = true;
  }
#endif
}

void report_fatal(const char* kind, const char* detail) {
  std::lock_guard<std::mutex> lock(g_report_mu);
#if defined(_WIN32)
  handle_crash(kind != nullptr ? kind : "fatal", detail != nullptr ? detail : "", nullptr, nullptr,
               true);
#else
  handle_crash(kind != nullptr ? kind : "fatal", detail != nullptr ? detail : "", nullptr, true);
#endif
  release_crash_lock();
  // report_fatal returns to the caller (e.g. Vulkan device lost keeps the app
  // alive). A later real crash is a new event and must get a full report, so
  // do not leave the "already reported" marker set. Callers that abort() right
  // after may produce a second (signal) report in the same file; acceptable.
  g_reported_full.store(false, std::memory_order_release);
}

void crash_set_context(CrashContextField field, const char* utf8) {
  const int i = static_cast<int>(field);
  if (i < 0 || i >= kCtxCount) return;
  copy_trunc(g_ctx[i], kCtxCap, utf8);
}

void mark_clean_exit() {
  if (g_log_dir[0] == '\0') {
    resolve_log_dir();
  }
  char sent[kPathCap];
  sentinel_path(sent, sizeof(sent));
#if defined(_WIN32)
  delete_path_utf8(sent);
  if (g_crash_file != INVALID_HANDLE_VALUE) {
    ::CloseHandle(g_crash_file);
    g_crash_file = INVALID_HANDLE_VALUE;
  }
#else
  ::unlink(sent);
  if (g_crash_fd >= 0) {
    ::close(g_crash_fd);
    g_crash_fd = -1;
  }
#endif
  delete_header_only_crash_file();
}

bool previous_session_crashed(char* crash_path_out, std::size_t cap) {
  if (g_installed.load(std::memory_order_acquire) && g_checked_prev) {
    if (crash_path_out != nullptr && cap > 0) {
      copy_trunc(crash_path_out, cap, g_prev_crash_path);
    }
    return g_prev_crashed.load(std::memory_order_acquire);
  }
  if (g_log_dir[0] == '\0') {
    resolve_log_dir();
  }
  return inspect_stale_sentinel(crash_path_out, cap);
}

const char* crash_log_directory() {
  if (g_log_dir[0] == '\0') {
    resolve_log_dir();
  }
  return g_log_dir;
}

}  // namespace wds::common

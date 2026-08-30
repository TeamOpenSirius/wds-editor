#include "wds/common/crash_handler.hpp"
#include "wds/common/crash_input_journal.hpp"

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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

namespace wds::common {
namespace {

constexpr std::size_t kPathCap = 1024;
constexpr std::size_t kMsgCap = 4096;

char g_log_dir[kPathCap] = {};
#if !defined(_WIN32)
// Prebuilt at install time so the signal path never formats paths (not AS-safe).
char g_signal_crash_path[kPathCap] = {};
constexpr char kSignalCrashHeader[] =
    "WDS Editor crash report\n"
    "kind: signal\n"
    "detail: ";
#endif
std::atomic_flag g_handling = ATOMIC_FLAG_INIT;
std::mutex g_report_mu;

#if !defined(_WIN32)
using SignalHandler = void (*)(int);
SignalHandler g_prev_segv = SIG_DFL;
SignalHandler g_prev_abrt = SIG_DFL;
SignalHandler g_prev_fpe = SIG_DFL;
SignalHandler g_prev_ill = SIG_DFL;
SignalHandler g_prev_bus = SIG_DFL;

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
      buf[--i] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
  }
  asafe_write_cstr_fd(fd, buf + i);
}

void emit_journal_fd(void* ctx, const char* data, std::size_t n) {
  asafe_write_bytes(static_cast<int>(reinterpret_cast<std::intptr_t>(ctx)), data, n);
}

void write_privacy_and_uptime_fd(int fd) {
  asafe_write_cstr_fd(fd, "privacy_sensitive: ");
  asafe_write_cstr_fd(fd, journal_allow_sensitive() ? "1" : "0");
  asafe_write_cstr_fd(fd, "\nuptime_ms: ");
  asafe_write_uint_fd(fd, journal_uptime_ms());
  asafe_write_cstr_fd(fd, "\n");
}
#endif

void append_cstr(char* dst, std::size_t cap, const char* src) {
  if (dst == nullptr || cap == 0 || src == nullptr) return;
  const std::size_t used = std::strlen(dst);
  if (used + 1 >= cap) return;
  std::strncat(dst, src, cap - used - 1);
}

#if defined(_WIN32)
std::wstring utf8_to_wide(const char* utf8) {
  if (utf8 == nullptr || utf8[0] == '\0') return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
  return out;
}
#endif

bool ensure_log_dir() {
  if (g_log_dir[0] == '\0') return false;
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(g_log_dir);
  if (wide.empty()) return false;
  // CreateDirectoryW only creates one level; create WDS then logs.
  std::wstring wds = wide;
  const auto pos = wds.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    const std::wstring parent = wds.substr(0, pos);
    ::CreateDirectoryW(parent.c_str(), nullptr);
  }
  return ::CreateDirectoryW(wide.c_str(), nullptr) != 0 ||
         ::GetLastError() == ERROR_ALREADY_EXISTS;
#else
  // mkdir -p style for .../WDS/logs
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
  char base[kPathCap] = {};
  const DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
  if (n > 0 && n < sizeof(base)) {
    std::snprintf(g_log_dir, sizeof(g_log_dir), "%s\\WDS\\logs", base);
  } else {
    const DWORD n2 = ::GetEnvironmentVariableA("USERPROFILE", base, sizeof(base));
    if (n2 > 0 && n2 < sizeof(base)) {
      std::snprintf(g_log_dir, sizeof(g_log_dir), "%s\\AppData\\Local\\WDS\\logs", base);
    }
  }
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    std::snprintf(g_log_dir, sizeof(g_log_dir),
                  "%s/Library/Application Support/WDS/logs", home);
  }
#else
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

void make_crash_path(char* out, std::size_t cap, const char* stamp) {
  if (g_log_dir[0] == '\0') {
    out[0] = '\0';
    return;
  }
#if defined(_WIN32)
  std::snprintf(out, cap, "%s\\crash-%s.txt", g_log_dir, stamp);
#else
  std::snprintf(out, cap, "%s/crash-%s.txt", g_log_dir, stamp);
#endif
}

void format_timestamp(char* out, std::size_t cap) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  std::snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

#if defined(_WIN32)
void write_win_text(HANDLE file, const char* text) {
  if (file == INVALID_HANDLE_VALUE || text == nullptr) return;
  const DWORD n = static_cast<DWORD>(std::strlen(text));
  if (n == 0) return;
  DWORD written = 0;
  ::WriteFile(file, text, n, &written, nullptr);
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
  HMODULE dbg = ::LoadLibraryW(L"dbghelp.dll");
  if (dbg == nullptr) {
    write_win_text(file, "  (dbghelp.dll not loaded)\n");
    return;
  }
  using SymInitFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
  using StackWalkFn = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                    PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64,
                                    PTRANSLATE_ADDRESS_ROUTINE64);
  using SymFnTableFn = PVOID(WINAPI*)(HANDLE, DWORD64);
  using SymBaseFn = DWORD64(WINAPI*)(HANDLE, DWORD64);
  auto sym_init = reinterpret_cast<SymInitFn>(::GetProcAddress(dbg, "SymInitialize"));
  auto walk = reinterpret_cast<StackWalkFn>(::GetProcAddress(dbg, "StackWalk64"));
  auto fn_table = reinterpret_cast<SymFnTableFn>(::GetProcAddress(dbg, "SymFunctionTableAccess64"));
  auto mod_base = reinterpret_cast<SymBaseFn>(::GetProcAddress(dbg, "SymGetModuleBase64"));
  if (sym_init == nullptr || walk == nullptr || fn_table == nullptr || mod_base == nullptr) {
    write_win_text(file, "  (dbghelp exports missing)\n");
    ::FreeLibrary(dbg);
    return;
  }
  const HANDLE process = ::GetCurrentProcess();
  const HANDLE thread = ::GetCurrentThread();
  (void)sym_init(process, nullptr, TRUE);
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
    if (!walk(machine, process, thread, &frame, &copy, nullptr, fn_table, mod_base, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }
    const void* pc = reinterpret_cast<void*>(static_cast<uintptr_t>(frame.AddrPC.Offset));
    write_win_fmt(file, "  #%u ", i);
    append_module_for_addr(file, "pc", pc);
  }
  ::FreeLibrary(dbg);
}

void emit_journal_win(void* ctx, const char* data, std::size_t n) {
  if (ctx == nullptr || data == nullptr || n == 0) return;
  DWORD written = 0;
  ::WriteFile(static_cast<HANDLE>(ctx), data, static_cast<DWORD>(n), &written, nullptr);
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

void append_stack_win(HANDLE file, EXCEPTION_POINTERS* info) {
  append_exception_win(file, info);
  if (info != nullptr) {
    append_exception_stack_win(file, info->ContextRecord);
  }
  append_modules_win(file);
  journal_write_text(emit_journal_win, file);
}
#else
void append_stack_posix(int fd) {
  void* frames[64] = {};
  const int n = ::backtrace(frames, 64);
  if (n > 0) {
    // backtrace_symbols_fd is async-signal-safe.
    ::backtrace_symbols_fd(frames, n, fd);
  }
}
#endif

#if defined(_WIN32)
bool write_crash_file(const char* path, const char* kind, const char* detail,
                      EXCEPTION_POINTERS* info) {
#else
bool write_crash_file(const char* path, const char* kind, const char* detail) {
#endif
  if (path == nullptr || path[0] == '\0') return false;
#if defined(_WIN32)
  const std::wstring wide = utf8_to_wide(path);
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  char header[kMsgCap];
  const int hlen = std::snprintf(header, sizeof(header),
                                 "WDS Editor crash report\n"
                                 "kind: %s\n"
                                 "detail: %s\n"
                                 "privacy_sensitive: %d\n"
                                 "uptime_ms: %llu\n",
                                 kind != nullptr ? kind : "(unknown)",
                                 detail != nullptr ? detail : "(none)",
                                 journal_allow_sensitive() ? 1 : 0,
                                 static_cast<unsigned long long>(journal_uptime_ms()));
  if (hlen > 0) {
    DWORD written = 0;
    ::WriteFile(file, header, static_cast<DWORD>(hlen), &written, nullptr);
  }
  append_stack_win(file, info);
  ::CloseHandle(file);
  return true;
#else
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  char header[kMsgCap];
  const int hlen = std::snprintf(header, sizeof(header),
                                 "WDS Editor crash report\n"
                                 "kind: %s\n"
                                 "detail: %s\n"
                                 "privacy_sensitive: %d\n"
                                 "uptime_ms: %llu\n"
                                 "--- stack ---\n",
                                 kind != nullptr ? kind : "(unknown)",
                                 detail != nullptr ? detail : "(none)",
                                 journal_allow_sensitive() ? 1 : 0,
                                 static_cast<unsigned long long>(journal_uptime_ms()));
  if (hlen > 0) {
    (void)::write(fd, header, static_cast<std::size_t>(hlen));
  }
  append_stack_posix(fd);
  journal_write_text(emit_journal_fd, reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
  ::close(fd);
  return true;
#endif
}

void show_crash_dialog(const char* title, const char* message) {
#if defined(_WIN32)
  const std::wstring title_w = utf8_to_wide(title);
  const std::wstring message_w = utf8_to_wide(message);
  ::MessageBoxW(nullptr, message_w.c_str(), title_w.c_str(),
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
#elif defined(__APPLE__)
  // Prefer a child process so signal contexts stay mostly async-safe.
  const pid_t pid = ::fork();
  if (pid == 0) {
    // Build: display dialog ("a" & return & "b") … so newlines never break -e parsing.
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
    ::execlp("zenity", "zenity", "--error", "--title",
             title != nullptr ? title : "WDS Editor", "--text",
             message != nullptr ? message : "", "--width=480",
             static_cast<char*>(nullptr));
    // Fallback: write to stderr if zenity is missing.
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

// Non-signal path only: may use heap, snprintf, mkdir, dialogs.
#if defined(_WIN32)
void handle_crash(const char* kind, const char* detail, EXCEPTION_POINTERS* info = nullptr) {
#else
void handle_crash(const char* kind, const char* detail) {
#endif
  if (g_handling.test_and_set(std::memory_order_acq_rel)) {
    // Nested crash — bail hard.
#if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), 1);
#else
    ::_exit(1);
#endif
  }

  // Log dir is created at install; retry here for late permission fixes.
  ensure_log_dir();
  char stamp[32];
  format_timestamp(stamp, sizeof(stamp));
  char path[kPathCap];
  make_crash_path(path, sizeof(path), stamp);
#if defined(_WIN32)
  const bool wrote = write_crash_file(path, kind, detail, info);
#else
  const bool wrote = write_crash_file(path, kind, detail);
#endif

  char msg[kMsgCap];
  build_user_message(msg, sizeof(msg), kind, detail, path, wrote);

  std::fprintf(stderr, "[wds] FATAL %s: %s\n", kind != nullptr ? kind : "?",
               detail != nullptr ? detail : "");
  if (wrote) {
    std::fprintf(stderr, "[wds] crash log: %s\n", path);
  }

  show_crash_dialog("WDS Editor — 致命错误", msg);
}

#if !defined(_WIN32)
// Signal path: preallocated path + static headers only — no snprintf/malloc/dialog.
void handle_crash_from_signal(int sig, const char* kind) {
  if (g_handling.test_and_set(std::memory_order_acq_rel)) {
    ::_exit(1);
  }

  // File-scope literals only — avoid function-local static init in signal context.
  asafe_write_bytes(STDERR_FILENO, "[wds] FATAL signal: ", 20);
  asafe_write_cstr_fd(STDERR_FILENO, kind != nullptr ? kind : "signal");
  asafe_write_bytes(STDERR_FILENO, "\n", 1);
  if (g_signal_crash_path[0] != '\0') {
    asafe_write_bytes(STDERR_FILENO, "[wds] crash log: ", 17);
    asafe_write_cstr_fd(STDERR_FILENO, g_signal_crash_path);
    asafe_write_bytes(STDERR_FILENO, "\n", 1);
  }

  if (g_signal_crash_path[0] != '\0') {
    const int fd = ::open(g_signal_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      asafe_write_bytes(fd, kSignalCrashHeader, sizeof(kSignalCrashHeader) - 1);
      asafe_write_cstr_fd(fd, kind != nullptr ? kind : "signal");
      asafe_write_bytes(fd, "\n", 1);
      write_privacy_and_uptime_fd(fd);
      asafe_write_bytes(fd, "--- stack ---\n", 14);
      append_stack_posix(fd);
      journal_write_text(emit_journal_fd, reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
      ::close(fd);
    }
  }

  (void)sig;  // re-raise done by caller
}
#endif

#if defined(_WIN32)
const char* exception_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "EXCEPTION_ACCESS_VIOLATION (访问违例)";
    case EXCEPTION_STACK_OVERFLOW:
      return "EXCEPTION_STACK_OVERFLOW (栈溢出)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "EXCEPTION_INT_DIVIDE_BY_ZERO (除零)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "EXCEPTION_ILLEGAL_INSTRUCTION (非法指令)";
    default:
      return "Unhandled SEH exception";
  }
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
  char detail[128];
  const DWORD code =
      info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
  std::snprintf(detail, sizeof(detail), "%s (0x%08lX)", exception_name(code),
                static_cast<unsigned long>(code));
  handle_crash("Windows 异常", detail, info);
  return EXCEPTION_EXECUTE_HANDLER;
}
#else
const char* signal_name(int sig) {
  switch (sig) {
    case SIGSEGV:
      return "SIGSEGV (段错误)";
    case SIGABRT:
      return "SIGABRT (abort)";
    case SIGFPE:
      return "SIGFPE (算术异常)";
    case SIGILL:
      return "SIGILL (非法指令)";
#ifdef SIGBUS
    case SIGBUS:
      return "SIGBUS (总线错误)";
#endif
    default:
      return "signal";
  }
}

void fatal_signal_handler(int sig) {
  // Must stay async-signal-safe: no snprintf, malloc, locale, or UI dialogs.
  handle_crash_from_signal(sig, signal_name(sig));
  // Restore default and re-raise so the OS records the real crash status.
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}
#endif

void terminate_handler() {
  const char* detail = "std::terminate()";
  try {
    if (const auto eptr = std::current_exception()) {
      try {
        std::rethrow_exception(eptr);
      } catch (const std::exception& ex) {
        detail = ex.what();
      } catch (...) {
        detail = "non-std exception";
      }
    }
  } catch (...) {
    detail = "exception while inspecting terminate state";
  }
  handle_crash("未捕获的 C++ 异常 / terminate", detail);
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(), 1);
#else
  ::_exit(1);
#endif
}

}  // namespace

void install_crash_handlers() {
  static std::once_flag once;
  std::call_once(once, [] {
    resolve_log_dir();
    ensure_log_dir();
#if !defined(_WIN32)
    // Fixed path for signal crashes (no timestamp formatting in the handler).
    make_crash_path(g_signal_crash_path, sizeof(g_signal_crash_path), "signal");
#endif

    std::set_terminate(terminate_handler);

#if defined(_WIN32)
    ::SetUnhandledExceptionFilter(unhandled_exception_filter);
#if defined(_MSC_VER)
    // Keep abort() from skipping our filter in some CRT builds.
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#else
    g_prev_segv = ::signal(SIGSEGV, fatal_signal_handler);
    g_prev_abrt = ::signal(SIGABRT, fatal_signal_handler);
    g_prev_fpe = ::signal(SIGFPE, fatal_signal_handler);
    g_prev_ill = ::signal(SIGILL, fatal_signal_handler);
#ifdef SIGBUS
    g_prev_bus = ::signal(SIGBUS, fatal_signal_handler);
#endif
    (void)g_prev_segv;
    (void)g_prev_abrt;
    (void)g_prev_fpe;
    (void)g_prev_ill;
    (void)g_prev_bus;
#endif
  });
}

void report_fatal(const char* kind, const char* detail) {
  std::lock_guard<std::mutex> lock(g_report_mu);
  handle_crash(kind != nullptr ? kind : "fatal", detail != nullptr ? detail : "");
}

}  // namespace wds::common

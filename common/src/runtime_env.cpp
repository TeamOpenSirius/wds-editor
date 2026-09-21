#include "wds/common/runtime_env.hpp"

#include "wds/common/log.hpp"
#include "wds/common/utf8_path.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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
#else
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#endif

namespace wds::common {
namespace {

void copy_trunc(char* dst, std::size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::snprintf(dst, cap, "%s", src);
}

void log_env_var(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return;
  }
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    WDS_LOG("env %s=unset\n", name);
    return;
  }
  WDS_LOG("env %s=%s\n", name, value);
}

#if defined(_WIN32)
unsigned long current_pid() {
  return static_cast<unsigned long>(::GetCurrentProcessId());
}
#else
long current_pid() {
  return static_cast<long>(::getpid());
}
#endif

void log_cpu_memory() {
  const unsigned hw = std::thread::hardware_concurrency();
#if defined(_WIN32)
  SYSTEM_INFO si{};
  ::GetSystemInfo(&si);
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  const int mem_ok = ::GlobalMemoryStatusEx(&ms) ? 1 : 0;
  const unsigned long long ram_mib =
      mem_ok ? static_cast<unsigned long long>(ms.ullTotalPhys / (1024ull * 1024ull)) : 0;
  WDS_LOG("cpu cores=%u sys_cores=%lu ram_mib=%llu\n", hw,
          static_cast<unsigned long>(si.dwNumberOfProcessors), ram_mib);
#elif defined(__APPLE__)
  int ncpu = 0;
  std::size_t ncpu_sz = sizeof(ncpu);
  if (::sysctlbyname("hw.ncpu", &ncpu, &ncpu_sz, nullptr, 0) != 0) {
    ncpu = static_cast<int>(hw);
  }
  std::uint64_t mem = 0;
  std::size_t mem_sz = sizeof(mem);
  if (::sysctlbyname("hw.memsize", &mem, &mem_sz, nullptr, 0) != 0) {
    mem = 0;
  }
  char brand[96] = {};
  std::size_t brand_sz = sizeof(brand);
  if (::sysctlbyname("machdep.cpu.brand_string", brand, &brand_sz, nullptr, 0) != 0) {
    copy_trunc(brand, sizeof(brand), "-");
  }
  WDS_LOG("cpu '%s' cores=%d ram_mib=%llu\n", brand, ncpu,
          static_cast<unsigned long long>(mem / (1024ull * 1024ull)));
#else
  const long ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page = ::sysconf(_SC_PAGE_SIZE);
  unsigned long long ram_mib = 0;
  if (pages > 0 && page > 0) {
    ram_mib = (static_cast<unsigned long long>(pages) *
               static_cast<unsigned long long>(page)) /
              (1024ull * 1024ull);
  }
  WDS_LOG("cpu cores=%ld hw=%u ram_mib=%llu\n", ncpu > 0 ? ncpu : static_cast<long>(hw), hw,
          ram_mib);
#endif
}

void log_paths(const char* argv0) {
  const std::string exe = path_to_utf8(executable_dir(argv0));
  WDS_LOG("exe_dir=%s\n", exe.empty() ? "-" : exe.c_str());

#if defined(_WIN32)
  wchar_t wide[1024];
  const DWORD n = ::GetCurrentDirectoryW(1024, wide);
  if (n > 0 && n < 1024) {
    const std::string cwd = wide_to_utf8(std::wstring(wide));
    WDS_LOG("cwd=%s\n", cwd.c_str());
  } else {
    WDS_LOG("cwd=-\n");
  }
#else
  char cwd[1024];
  if (::getcwd(cwd, sizeof(cwd)) != nullptr) {
    WDS_LOG("cwd=%s\n", cwd);
  } else {
    WDS_LOG("cwd=-\n");
  }
#endif

  const char* debug_path = log_debug_session_path();
  WDS_LOG("debug_log=%s\n", debug_path[0] != '\0' ? debug_path : "none");
}

}  // namespace

void format_os_version(char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) {
    return;
  }
  out[0] = '\0';
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
    std::snprintf(out, cap, "Windows %lu.%lu.%lu", static_cast<unsigned long>(vi.dwMajorVersion),
                  static_cast<unsigned long>(vi.dwMinorVersion),
                  static_cast<unsigned long>(vi.dwBuildNumber));
  } else {
    copy_trunc(out, cap, "Windows");
  }
#else
  utsname u{};
  if (::uname(&u) == 0) {
    std::snprintf(out, cap, "%s %s %s", u.sysname, u.release, u.machine);
  } else {
    copy_trunc(out, cap, "POSIX");
  }
#endif
}

void log_runtime_environment(const char* argv0) {
  char os[256] = {};
  format_os_version(os, sizeof(os));
#if defined(_WIN32)
  WDS_LOG("app version=%s git=%s build=%s pid=%lu logging=%d\n", WDS_APP_VERSION, WDS_GIT_HASH,
          WDS_BUILD_TYPE, current_pid(), WDS_ENABLE_LOGGING);
#else
  WDS_LOG("app version=%s git=%s build=%s pid=%ld logging=%d\n", WDS_APP_VERSION, WDS_GIT_HASH,
          WDS_BUILD_TYPE, current_pid(), WDS_ENABLE_LOGGING);
#endif
  WDS_LOG("os %s\n", os[0] != '\0' ? os : "unknown");
  log_cpu_memory();
  log_paths(argv0);
  log_env_var("VK_ICD_FILENAMES");
  log_env_var("VK_DRIVER_FILES");
  log_env_var("VK_LAYER_PATH");
  log_env_var("VK_INSTANCE_LAYERS");
  log_env_var("QT_PLUGIN_PATH");
  log_env_var("QT_MTL_NO_TRANSACTION");
  log_env_var("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS");
  log_env_var("WDS_CRASH_LOG_DIR");
  log_env_var("WDS_FRAME_DIAG");
  log_env_var("WDS_CRASH_NO_DIALOG");
}

}  // namespace wds::common

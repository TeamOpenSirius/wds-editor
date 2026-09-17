// Uninstall helper shipped next to wds_editor.exe.
// Finds related MSI products by UpgradeCode and removes them via Windows Installer.
//
// The helper lives in INSTALLDIR, so msiexec cannot delete it while this process
// still runs. We copy a worker + watcher into %TEMP%, write a delete-record,
// then exit the installed copy immediately. msiexec is told not to reboot
// (REBOOT=ReallySuppress); the watcher waits for the worker to exit and deletes
// any leftovers (installed Uninstall.exe, empty folder, temp worker) at once.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <msi.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

constexpr wchar_t kUpgradeCode[] = L"{A7E3C2B1-9F4D-4E8A-9C6B-1D2E3F4A5B6C}";
constexpr wchar_t kAppName[] = L"WDS Editor";

std::wstring FormatMsiError(UINT err) {
  wchar_t buf[256];
  const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr,
                                 err,
                                 0,
                                 buf,
                                 static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
                                 nullptr);
  if (n == 0) {
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"MSI error %u", err);
  }
  return buf;
}

std::wstring ModulePath() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) {
      return {};
    }
    if (n < buf.size() - 1) {
      buf.resize(n);
      return buf;
    }
    buf.resize(buf.size() * 2);
  }
}

std::wstring DirName(const std::wstring& path) {
  const auto pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) {
    return {};
  }
  return path.substr(0, pos);
}

std::wstring Quote(const std::wstring& s) {
  return L"\"" + s + L"\"";
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return {};
  }
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) {
    return {};
  }
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::wstring TempNamed(const wchar_t* prefix, const wchar_t* ext) {
  wchar_t dir[MAX_PATH];
  const DWORD n = GetTempPathW(MAX_PATH, dir);
  if (n == 0 || n >= MAX_PATH) {
    return {};
  }
  wchar_t path[MAX_PATH];
  if (swprintf(path,
               MAX_PATH,
               L"%s%s%lu-%lu%s",
               dir,
               prefix,
               GetCurrentProcessId(),
               GetTickCount(),
               ext) < 0) {
    return {};
  }
  return path;
}

bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD sz = sizeof(elev);
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz);
  CloseHandle(token);
  return ok && elev.TokenIsElevated;
}

bool ConfirmUninstall() {
  const int answer = MessageBoxW(
      nullptr,
      L"确定要卸载 WDS Editor 吗？\n\n"
      L"程序文件将被删除。用户配置（%LOCALAPPDATA%\\WDS）会保留。",
      kAppName,
      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
  return answer == IDYES;
}

bool WriteRecord(const std::wstring& path,
                 const std::vector<std::wstring>& files,
                 const std::vector<std::wstring>& dirs) {
  const HANDLE h = CreateFileW(path.c_str(),
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::string out;
  for (const auto& f : files) {
    out += "file\t";
    out += Narrow(f);
    out += '\n';
  }
  for (const auto& d : dirs) {
    out += "dir\t";
    out += Narrow(d);
    out += '\n';
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
  CloseHandle(h);
  return ok && written == out.size();
}

void ApplyRecord(const std::wstring& path) {
  const HANDLE h = CreateFileW(path.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1 << 20) {
    CloseHandle(h);
    return;
  }
  std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
  DWORD got = 0;
  const BOOL ok = ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr);
  CloseHandle(h);
  if (!ok) {
    return;
  }
  raw.resize(got);

  std::vector<std::wstring> files;
  std::vector<std::wstring> dirs;
  size_t i = 0;
  while (i < raw.size()) {
    size_t nl = raw.find('\n', i);
    if (nl == std::string::npos) {
      nl = raw.size();
    }
    std::string line = raw.substr(i, nl - i);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    i = nl + 1;
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) {
      continue;
    }
    const std::string kind = line.substr(0, tab);
    const std::wstring value = Widen(line.substr(tab + 1));
    if (value.empty()) {
      continue;
    }
    if (kind == "file") {
      files.push_back(value);
    } else if (kind == "dir") {
      dirs.push_back(value);
    }
  }
  for (const auto& f : files) {
    SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(f.c_str());
  }
  for (const auto& d : dirs) {
    RemoveDirectoryW(d.c_str());
  }
}

bool LaunchProcess(const std::wstring& exe,
                   const std::wstring& params,
                   bool hide,
                   bool wait,
                   DWORD* exit_code) {
  std::wstring cmd = Quote(exe);
  if (!params.empty()) {
    cmd += L' ';
    cmd += params;
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = hide ? SW_HIDE : SW_SHOWNORMAL;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  DWORD flags = CREATE_NEW_PROCESS_GROUP;
  if (hide) {
    flags |= CREATE_NO_WINDOW | DETACHED_PROCESS;
  }
  if (!CreateProcessW(exe.c_str(),
                      buf.data(),
                      nullptr,
                      nullptr,
                      FALSE,
                      flags,
                      nullptr,
                      nullptr,
                      &si,
                      &pi)) {
    return false;
  }
  if (wait) {
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (exit_code) {
      GetExitCodeProcess(pi.hProcess, exit_code);
    }
  } else if (exit_code) {
    *exit_code = 0;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

bool RelaunchElevated(const std::wstring& params) {
  const std::wstring self = ModulePath();
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.lpVerb = L"runas";
  sei.lpFile = self.c_str();
  sei.lpParameters = params.c_str();
  sei.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&sei);
}

bool UninstallProduct(const wchar_t* product_code) {
  // ReallySuppress: do not schedule a reboot when a file is still in use.
  // The temp watcher deletes leftovers after this process exits.
  std::wstring args = L"/x ";
  args += product_code;
  args += L" /qb REBOOT=ReallySuppress";

  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = IsElevated() ? L"open" : L"runas";
  sei.lpFile = L"msiexec.exe";
  sei.lpParameters = args.c_str();
  sei.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&sei)) {
    const DWORD err = GetLastError();
    if (IsElevated() || wcscmp(sei.lpVerb, L"open") == 0) {
      std::wstring msg = L"无法启动卸载程序：\n";
      msg += FormatMsiError(err);
      MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONERROR);
      return false;
    }
    sei.lpVerb = L"open";
    if (!ShellExecuteExW(&sei)) {
      std::wstring msg = L"无法启动卸载程序：\n";
      msg += FormatMsiError(err);
      MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONERROR);
      return false;
    }
  }

  if (sei.hProcess) {
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    // 3010 = reboot requested; we suppressed the prompt and clean up after exit.
    if (code != 0 && code != ERROR_SUCCESS_REBOOT_REQUIRED) {
      std::wstring msg = L"卸载未成功完成。\n";
      msg += FormatMsiError(static_cast<UINT>(code));
      MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
      return false;
    }
  }
  return true;
}

bool UninstallRelatedProducts() {
  wchar_t product_code[39];
  UINT rc = MsiEnumRelatedProductsW(kUpgradeCode, 0, 0, product_code);
  if (rc == ERROR_NO_MORE_ITEMS) {
    MessageBoxW(nullptr,
                L"未找到已安装的 WDS Editor（可能已卸载，或安装方式不是 MSI）。",
                kAppName,
                MB_OK | MB_ICONINFORMATION);
    return false;
  }
  if (rc != ERROR_SUCCESS) {
    std::wstring msg = L"查询已安装产品失败：\n";
    msg += FormatMsiError(rc);
    MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONERROR);
    return false;
  }

  if (!UninstallProduct(product_code)) {
    return false;
  }
  for (DWORD index = 1;; ++index) {
    rc = MsiEnumRelatedProductsW(kUpgradeCode, 0, index, product_code);
    if (rc != ERROR_SUCCESS) {
      break;
    }
    UninstallProduct(product_code);
  }
  return true;
}

void SelfDelete() {
  const std::wstring self = ModulePath();
  if (self.empty()) {
    return;
  }
  std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 2 >nul & del /f /q ";
  cmd += Quote(self);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (CreateProcessW(nullptr,
                     buf.data(),
                     nullptr,
                     nullptr,
                     FALSE,
                     CREATE_NO_WINDOW | DETACHED_PROCESS,
                     nullptr,
                     nullptr,
                     &si,
                     &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

int RunCleanup(DWORD pid, const std::wstring& record) {
  if (pid != 0) {
    const HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) {
      WaitForSingleObject(h, INFINITE);
      CloseHandle(h);
    }
  }
  Sleep(400);
  ApplyRecord(record);
  DeleteFileW(record.c_str());
  SelfDelete();
  return 0;
}

std::wstring WorkParams(const std::wstring& record, const std::wstring& watch) {
  std::wstring p = L"--work --confirmed --record ";
  p += Quote(record);
  p += L" --watch ";
  p += Quote(watch);
  return p;
}

int RunWork(const std::wstring& record, const std::wstring& watch) {
  if (!IsElevated()) {
    if (!RelaunchElevated(WorkParams(record, watch))) {
      MessageBoxW(nullptr,
                  L"需要管理员权限才能卸载。",
                  kAppName,
                  MB_OK | MB_ICONERROR);
      return 1;
    }
    return 0;
  }

  if (!watch.empty()) {
    wchar_t pid_buf[32];
    swprintf(pid_buf, 32, L"%lu", GetCurrentProcessId());
    std::wstring params = L"--cleanup ";
    params += pid_buf;
    params += L' ';
    params += Quote(record);
    LaunchProcess(watch, params, true, false, nullptr);
  }

  return UninstallRelatedProducts() ? 0 : 1;
}

bool StartDeferredUninstall(const std::wstring& original) {
  const std::wstring work = TempNamed(L"wds-uninst-work-", L".exe");
  const std::wstring watch = TempNamed(L"wds-uninst-watch-", L".exe");
  const std::wstring record = TempNamed(L"wds-uninst-", L".todo");
  if (work.empty() || watch.empty() || record.empty()) {
    return false;
  }
  if (!CopyFileW(original.c_str(), work.c_str(), FALSE)) {
    return false;
  }
  if (!CopyFileW(original.c_str(), watch.c_str(), FALSE)) {
    DeleteFileW(work.c_str());
    return false;
  }

  std::vector<std::wstring> files = {original, work};
  std::vector<std::wstring> dirs;
  const std::wstring dir = DirName(original);
  if (!dir.empty()) {
    dirs.push_back(dir);
  }
  if (!WriteRecord(record, files, dirs)) {
    DeleteFileW(work.c_str());
    DeleteFileW(watch.c_str());
    return false;
  }

  if (!LaunchProcess(work, WorkParams(record, watch), false, false, nullptr)) {
    DeleteFileW(work.c_str());
    DeleteFileW(watch.c_str());
    DeleteFileW(record.c_str());
    return false;
  }
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool cleanup = false;
  bool work = false;
  DWORD wait_pid = 0;
  std::wstring record;
  std::wstring watch;
  if (argv) {
    for (int i = 1; i < argc; ++i) {
      if (wcscmp(argv[i], L"--cleanup") == 0 && i + 2 < argc) {
        cleanup = true;
        wait_pid = static_cast<DWORD>(wcstoul(argv[++i], nullptr, 10));
        record = argv[++i];
      } else if (wcscmp(argv[i], L"--work") == 0) {
        work = true;
      } else if (wcscmp(argv[i], L"--record") == 0 && i + 1 < argc) {
        record = argv[++i];
      } else if (wcscmp(argv[i], L"--watch") == 0 && i + 1 < argc) {
        watch = argv[++i];
      }
    }
    LocalFree(argv);
  }

  if (cleanup) {
    return RunCleanup(wait_pid, record);
  }
  if (work) {
    return RunWork(record, watch);
  }

  if (!ConfirmUninstall()) {
    return 0;
  }

  const std::wstring self = ModulePath();
  if (!self.empty() && StartDeferredUninstall(self)) {
    // Installed Uninstall.exe exits now so msiexec can delete it without a reboot.
    return 0;
  }

  // Copy/launch failed: still uninstall, and try a watcher from %TEMP% if possible.
  if (!self.empty()) {
    const std::wstring fallback_watch = TempNamed(L"wds-uninst-watch-", L".exe");
    const std::wstring fallback_record = TempNamed(L"wds-uninst-", L".todo");
    if (!fallback_watch.empty() && !fallback_record.empty() &&
        CopyFileW(self.c_str(), fallback_watch.c_str(), FALSE)) {
      std::vector<std::wstring> files = {self};
      std::vector<std::wstring> dirs;
      const std::wstring dir = DirName(self);
      if (!dir.empty()) {
        dirs.push_back(dir);
      }
      if (WriteRecord(fallback_record, files, dirs)) {
        wchar_t pid_buf[32];
        swprintf(pid_buf, 32, L"%lu", GetCurrentProcessId());
        std::wstring params = L"--cleanup ";
        params += pid_buf;
        params += L' ';
        params += Quote(fallback_record);
        LaunchProcess(fallback_watch, params, true, false, nullptr);
      }
    }
  }
  return UninstallRelatedProducts() ? 0 : 1;
}

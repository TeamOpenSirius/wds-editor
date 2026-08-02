// Uninstall helper shipped next to wds_editor.exe.
// Finds related MSI products by UpgradeCode and removes them via Windows Installer.

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

#include <cstdio>
#include <string>

namespace {

// Must match UpgradeCode in scripts/win-msi-product.wxs
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

bool ConfirmUninstall() {
  const int answer = MessageBoxW(
      nullptr,
      L"确定要卸载 WDS Editor 吗？\n\n"
      L"程序文件将被删除。用户配置（%LOCALAPPDATA%\\WDS）会保留。",
      kAppName,
      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
  return answer == IDYES;
}

bool UninstallProduct(const wchar_t* product_code) {
  // Prefer msiexec UI so the user sees progress / reboot prompts.
  std::wstring args = L"/x ";
  args += product_code;
  args += L" /qb";

  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";  // elevation for per-machine product
  sei.lpFile = L"msiexec.exe";
  sei.lpParameters = args.c_str();
  sei.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&sei)) {
    const DWORD err = GetLastError();
    // Fallback without explicit elevation verb (UAC may still prompt).
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
    if (code != 0 && code != ERROR_SUCCESS_REBOOT_REQUIRED) {
      std::wstring msg = L"卸载未成功完成。\n";
      msg += FormatMsiError(static_cast<UINT>(code));
      MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
      return false;
    }
  }
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  if (!ConfirmUninstall()) {
    return 0;
  }

  wchar_t product_code[39];
  UINT rc = MsiEnumRelatedProductsW(kUpgradeCode, 0, 0, product_code);
  if (rc == ERROR_NO_MORE_ITEMS) {
    MessageBoxW(nullptr,
                L"未找到已安装的 WDS Editor（可能已卸载，或安装方式不是 MSI）。",
                kAppName,
                MB_OK | MB_ICONINFORMATION);
    return 1;
  }
  if (rc != ERROR_SUCCESS) {
    std::wstring msg = L"查询已安装产品失败：\n";
    msg += FormatMsiError(rc);
    MessageBoxW(nullptr, msg.c_str(), kAppName, MB_OK | MB_ICONERROR);
    return 1;
  }

  // Uninstall the first related product; MajorUpgrade keeps a single install.
  if (!UninstallProduct(product_code)) {
    return 1;
  }

  // Remove any additional related products (rare, e.g. interrupted upgrades).
  for (DWORD index = 1;; ++index) {
    rc = MsiEnumRelatedProductsW(kUpgradeCode, 0, index, product_code);
    if (rc != ERROR_SUCCESS) {
      break;
    }
    UninstallProduct(product_code);
  }

  return 0;
}

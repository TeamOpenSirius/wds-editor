/* MSI custom actions for WDS Editor (MinGW DLL, Binary table — not installed).
 *
 * Checkbox memory is AppSearch + WriteRegistryValues in the .wxs, not a
 * UI-sequence DLL (LoadLibrary failure there aborted first install; Repair
 * skipped the CA and looked "successful").
 *
 * ApplyUserShortcuts (deferred, after InstallFiles):
 *   Create or delete per-machine .lnk files from CustomActionData, then also
 *   write HKLM Create*Shortcut as a belt-and-suspenders persist. Must run
 *   after early RemoveExistingProducts so an upgrade cannot delete the values.
 *
 * LoadShortcutPrefs remains exported for diagnostics; it is not scheduled.
 *
 * Built with -static-libgcc, static winpthread, --kill-at so msiexec
 * LoadLibrary works from a temp dir with no MinGW runtimes on PATH.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <objbase.h>
#include <shlobj.h>

#include <stdio.h>
#include <string.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define WDS_REG_KEY L"Software\\WDS\\Editor"
#define WDS_REG_DESKTOP L"CreateDesktopShortcut"
#define WDS_REG_STARTMENU L"CreateStartMenuShortcut"
#define WDS_LNK_NAME L"WDS Editor.lnk"
#define WDS_STARTMENU_FOLDER L"WDS Editor"

static void log_info(MSIHANDLE hInstall, const WCHAR* msg) {
  MSIHANDLE rec = MsiCreateRecord(1);
  if (!rec) return;
  MsiRecordSetStringW(rec, 0, L"[1]");
  MsiRecordSetStringW(rec, 1, msg);
  MsiProcessMessage(hInstall, INSTALLMESSAGE_INFO, rec);
  MsiCloseHandle(rec);
}

static BOOL get_prop(MSIHANDLE hInstall, const WCHAR* name, WCHAR* buf, DWORD cch) {
  DWORD n = cch;
  if (!buf || cch == 0) return FALSE;
  buf[0] = L'\0';
  if (MsiGetPropertyW(hInstall, name, buf, &n) != ERROR_SUCCESS) {
    buf[0] = L'\0';
    return FALSE;
  }
  return buf[0] != L'\0';
}

static BOOL prop_is_one(MSIHANDLE hInstall, const WCHAR* name) {
  WCHAR buf[16];
  if (!get_prop(hInstall, name, buf, ARRAYSIZE(buf))) return FALSE;
  return buf[0] == L'1' && buf[1] == L'\0';
}

static BOOL prop_equals(MSIHANDLE hInstall, const WCHAR* name, const WCHAR* want) {
  WCHAR buf[64];
  if (!get_prop(hInstall, name, buf, ARRAYSIZE(buf))) return FALSE;
  return lstrcmpiW(buf, want) == 0;
}

static BOOL read_reg_sz(HKEY root, REGSAM wow, const WCHAR* value_name, WCHAR* buf, DWORD cch) {
  HKEY key = NULL;
  DWORD type = 0;
  BYTE raw[256];
  DWORD bytes = sizeof(raw);
  LONG rc;
  buf[0] = L'\0';
  rc = RegOpenKeyExW(root, WDS_REG_KEY, 0, KEY_QUERY_VALUE | wow, &key);
  if (rc != ERROR_SUCCESS) return FALSE;
  rc = RegQueryValueExW(key, value_name, NULL, &type, raw, &bytes);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS) return FALSE;
  if (type == REG_DWORD && bytes >= sizeof(DWORD)) {
    DWORD v = 0;
    memcpy(&v, raw, sizeof(DWORD));
    buf[0] = v ? L'1' : L'0';
    buf[1] = L'\0';
    return TRUE;
  }
  if (type != REG_SZ && type != REG_EXPAND_SZ) return FALSE;
  if (bytes >= sizeof(raw)) bytes = sizeof(raw) - sizeof(WCHAR);
  memcpy(buf, raw, bytes);
  buf[bytes / sizeof(WCHAR)] = L'\0';
  buf[cch - 1] = L'\0';
  return buf[0] != L'\0';
}

static BOOL read_pref(const WCHAR* value_name, WCHAR* buf, DWORD cch) {
  if (read_reg_sz(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, value_name, buf, cch)) return TRUE;
  if (read_reg_sz(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, value_name, buf, cch)) return TRUE;
  if (read_reg_sz(HKEY_CURRENT_USER, 0, value_name, buf, cch)) return TRUE;
  /* Legacy KeyPath markers from older per-user MSIs. */
  if (lstrcmpW(value_name, WDS_REG_DESKTOP) == 0) {
    return read_reg_sz(HKEY_CURRENT_USER, 0, L"DesktopShortcut", buf, cch);
  }
  if (lstrcmpW(value_name, WDS_REG_STARTMENU) == 0) {
    return read_reg_sz(HKEY_CURRENT_USER, 0, L"StartMenuShortcut", buf, cch);
  }
  return FALSE;
}

static BOOL write_pref(const WCHAR* value_name, const WCHAR* value) {
  HKEY key = NULL;
  DWORD disp = 0;
  LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, WDS_REG_KEY, 0, NULL, 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, &disp);
  if (rc != ERROR_SUCCESS) return FALSE;
  rc = RegSetValueExW(key, value_name, 0, REG_SZ, (const BYTE*)value,
                      (DWORD)((lstrlenW(value) + 1) * sizeof(WCHAR)));
  RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

static void apply_pref_property(MSIHANDLE hInstall, const WCHAR* prop, const WCHAR* value_name) {
  WCHAR buf[16];
  if (!read_pref(value_name, buf, ARRAYSIZE(buf))) return;
  if (buf[0] == L'0' || buf[0] == L'1') {
    buf[1] = L'\0';
    MsiSetPropertyW(hInstall, prop, buf);
  }
}

static void join_exe(const WCHAR* installdir, WCHAR* exe, DWORD cch) {
  size_t n = lstrlenW(installdir);
  if (n && (installdir[n - 1] == L'\\' || installdir[n - 1] == L'/')) {
    _snwprintf(exe, cch, L"%swds_editor.exe", installdir);
  } else {
    _snwprintf(exe, cch, L"%s\\wds_editor.exe", installdir);
  }
  exe[cch - 1] = L'\0';
}

static void ensure_parent_dir(const WCHAR* path) {
  WCHAR dir[MAX_PATH];
  WCHAR* slash;
  lstrcpynW(dir, path, MAX_PATH);
  slash = wcsrchr(dir, L'\\');
  if (!slash) return;
  *slash = L'\0';
  CreateDirectoryW(dir, NULL);
}

static BOOL create_shortcut(const WCHAR* lnk, const WCHAR* exe, const WCHAR* workdir) {
  IShellLinkW* psl = NULL;
  IPersistFile* ppf = NULL;
  HRESULT hr;
  BOOL ok = FALSE;

  ensure_parent_dir(lnk);
  hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW,
                        (void**)&psl);
  if (FAILED(hr) || !psl) return FALSE;
  psl->lpVtbl->SetPath(psl, exe);
  psl->lpVtbl->SetWorkingDirectory(psl, workdir);
  psl->lpVtbl->SetIconLocation(psl, exe, 0);
  psl->lpVtbl->SetDescription(psl, L"World Dai Star chart editor");
  hr = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void**)&ppf);
  if (SUCCEEDED(hr) && ppf) {
    hr = ppf->lpVtbl->Save(ppf, lnk, TRUE);
    ok = SUCCEEDED(hr);
    ppf->lpVtbl->Release(ppf);
  }
  psl->lpVtbl->Release(psl);
  return ok;
}

static void delete_shortcut(const WCHAR* lnk) {
  DeleteFileW(lnk);
}

static BOOL desktop_lnk_path(WCHAR* out, DWORD cch) {
  WCHAR folder[MAX_PATH];
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT,
                              folder))) {
    return FALSE;
  }
  _snwprintf(out, cch, L"%s\\%s", folder, WDS_LNK_NAME);
  out[cch - 1] = L'\0';
  return TRUE;
}

static BOOL startmenu_paths(WCHAR* lnk, DWORD lnk_cch, WCHAR* folder, DWORD folder_cch) {
  WCHAR programs[MAX_PATH];
  if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, programs))) {
    return FALSE;
  }
  _snwprintf(folder, folder_cch, L"%s\\%s", programs, WDS_STARTMENU_FOLDER);
  folder[folder_cch - 1] = L'\0';
  _snwprintf(lnk, lnk_cch, L"%s\\%s", folder, WDS_LNK_NAME);
  lnk[lnk_cch - 1] = L'\0';
  return TRUE;
}

static void apply_desktop(MSIHANDLE hInstall, const WCHAR* exe, const WCHAR* workdir, BOOL enabled) {
  WCHAR lnk[MAX_PATH];
  if (!desktop_lnk_path(lnk, ARRAYSIZE(lnk))) {
    log_info(hInstall, L"WDS: cannot resolve common desktop folder");
    return;
  }
  if (enabled) {
    if (!create_shortcut(lnk, exe, workdir)) {
      log_info(hInstall, L"WDS: failed to create desktop shortcut");
    }
  } else {
    delete_shortcut(lnk);
  }
}

static void apply_startmenu(MSIHANDLE hInstall, const WCHAR* exe, const WCHAR* workdir, BOOL enabled) {
  WCHAR lnk[MAX_PATH];
  WCHAR folder[MAX_PATH];
  if (!startmenu_paths(lnk, ARRAYSIZE(lnk), folder, ARRAYSIZE(folder))) {
    log_info(hInstall, L"WDS: cannot resolve common Start Menu folder");
    return;
  }
  if (enabled) {
    CreateDirectoryW(folder, NULL);
    if (!create_shortcut(lnk, exe, workdir)) {
      log_info(hInstall, L"WDS: failed to create Start Menu shortcut");
    }
  } else {
    delete_shortcut(lnk);
    RemoveDirectoryW(folder);
  }
}

__declspec(dllexport) UINT __stdcall LoadShortcutPrefs(MSIHANDLE hInstall) {
  apply_pref_property(hInstall, L"CREATE_DESKTOP_SHORTCUT", WDS_REG_DESKTOP);
  apply_pref_property(hInstall, L"CREATE_STARTMENU_SHORTCUT", WDS_REG_STARTMENU);
  return ERROR_SUCCESS;
}

static void split_data(WCHAR* data, WCHAR** parts, int want) {
  int i;
  WCHAR* p = data;
  for (i = 0; i < want; ++i) {
    parts[i] = p;
    while (*p && *p != L'|') ++p;
    if (*p == L'|') {
      *p = L'\0';
      ++p;
    }
  }
}

__declspec(dllexport) UINT __stdcall ApplyUserShortcuts(MSIHANDLE hInstall) {
  WCHAR data[1024];
  WCHAR installdir[512];
  WCHAR exe[512];
  WCHAR* parts[4];
  WCHAR empty[] = L"";
  HRESULT co;
  BOOL removing;
  BOOL desktop_on;
  BOOL startmenu_on;

  parts[0] = parts[1] = parts[2] = parts[3] = empty;
  installdir[0] = L'\0';

  /* Deferred: only CustomActionData is available (installdir|desktop|startmenu|remove). */
  if (get_prop(hInstall, L"CustomActionData", data, ARRAYSIZE(data))) {
    split_data(data, parts, 4);
    lstrcpynW(installdir, parts[0], ARRAYSIZE(installdir));
    removing = lstrcmpiW(parts[3], L"ALL") == 0;
    desktop_on = !removing && parts[1][0] == L'1';
    startmenu_on = !removing && parts[2][0] == L'1';
  } else {
    removing = prop_equals(hInstall, L"REMOVE", L"ALL");
    desktop_on = !removing && prop_is_one(hInstall, L"CREATE_DESKTOP_SHORTCUT");
    startmenu_on = !removing && prop_is_one(hInstall, L"CREATE_STARTMENU_SHORTCUT");
    get_prop(hInstall, L"INSTALLDIR", installdir, ARRAYSIZE(installdir));
  }

  join_exe(installdir, exe, ARRAYSIZE(exe));

  co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  apply_desktop(hInstall, exe, installdir, desktop_on);
  apply_startmenu(hInstall, exe, installdir, startmenu_on);
  if (SUCCEEDED(co)) CoUninitialize();

  if (!removing) {
    write_pref(WDS_REG_DESKTOP, desktop_on ? L"1" : L"0");
    write_pref(WDS_REG_STARTMENU, startmenu_on ? L"1" : L"0");
  }
  return ERROR_SUCCESS;
}

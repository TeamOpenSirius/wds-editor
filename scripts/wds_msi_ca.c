/* Immediate MSI custom action: apply shortcut feature states from CREATE_* props.
 *
 * Built as a MinGW DLL for wixl Binary/DllEntry. Runs after MigrateFeatureStates so
 * UI AddLocal/Remove (or registry prefs on silent installs) win over migrated state.
 *
 * Entry must be stdcall and listed in the .def / -Wl,--kill-at for undecorated names.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>
#include <msiquery.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static int prop_is_one(MSIHANDLE hInstall, const WCHAR* name) {
  WCHAR buf[16];
  DWORD cch = ARRAYSIZE(buf);
  buf[0] = L'\0';
  if (MsiGetPropertyW(hInstall, name, buf, &cch) != ERROR_SUCCESS) return 0;
  return buf[0] == L'1';
}

__declspec(dllexport) UINT __stdcall ApplyShortcutFeatureStates(MSIHANDLE hInstall) {
  const INSTALLSTATE desk =
      prop_is_one(hInstall, L"CREATE_DESKTOP_SHORTCUT") ? INSTALLSTATE_LOCAL
                                                         : INSTALLSTATE_ABSENT;
  const INSTALLSTATE start =
      prop_is_one(hInstall, L"CREATE_STARTMENU_SHORTCUT") ? INSTALLSTATE_LOCAL
                                                           : INSTALLSTATE_ABSENT;
  MsiSetFeatureStateW(hInstall, L"DesktopFeature", desk);
  MsiSetFeatureStateW(hInstall, L"StartMenuFeature", start);
  return ERROR_SUCCESS;
}

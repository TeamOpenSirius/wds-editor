/* Immediate MSI custom action: apply shortcut feature/component states from CREATE_*.
 *
 * Built as a MinGW DLL for wixl Binary/DllEntry.
 *
 * Scheduled twice in InstallExecuteSequence:
 *   1) After FileCost / before CostFinalize — so ABSENT↔LOCAL is costed correctly
 *   2) After MigrateFeatureStates slot — so upgrade feature migration cannot discard
 *      the choice (package-target.sh also removes MigrateFeatureStates entirely)
 *
 * Sets both Feature and Component action states: post-CostFinalize MsiSetFeatureState
 * alone often fails to install a previously-absent shortcut component.
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

static void apply_shortcut(MSIHANDLE hInstall, const WCHAR* feature,
                           const WCHAR* component, int enabled) {
  const INSTALLSTATE state =
      enabled ? INSTALLSTATE_LOCAL : INSTALLSTATE_ABSENT;
  MsiSetFeatureStateW(hInstall, feature, state);
  MsiSetComponentStateW(hInstall, component, state);
}

__declspec(dllexport) UINT __stdcall ApplyShortcutFeatureStates(MSIHANDLE hInstall) {
  apply_shortcut(hInstall, L"DesktopFeature", L"DesktopShortcut",
                 prop_is_one(hInstall, L"CREATE_DESKTOP_SHORTCUT"));
  apply_shortcut(hInstall, L"StartMenuFeature", L"ApplicationShortcut",
                 prop_is_one(hInstall, L"CREATE_STARTMENU_SHORTCUT"));
  return ERROR_SUCCESS;
}

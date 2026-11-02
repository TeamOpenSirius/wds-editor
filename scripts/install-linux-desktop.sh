#!/usr/bin/env bash
# Register/unregister WDS Editor as the handler for *.wdsproject and
# *.wdschart on freedesktop desktops (Linux, BSD). Linux builds are development
# builds — there is no distro package — so this writes the user-level desktop
# entry and MIME definitions that make double-clicking a project or chart open
# the editor, and makes the entry the default handler through xdg-mime.
#
# Windows and macOS do not use this script: the MSI writes the ProgId / shell
# open command and the .app declares CFBundleDocumentTypes, so their normal
# install (and uninstall) already registers the association.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/install-linux-desktop.sh [--uninstall] [--editor PATH]

Installs, under ${XDG_DATA_HOME:-$HOME/.local/share}:
  applications/wds-editor.desktop   desktop entry (Exec=<editor> %f)
  mime/packages/wds-editor.xml      x-wdsproject + x-wdschart definitions
  icons/hicolor/256x256/apps/wds-editor.png  (when icons/wds.png exists)
then refreshes the desktop/MIME databases and makes the entry the default
handler for .wdsproject and .wdschart.

--editor PATH  editor binary the entry launches (default: $WDS_EDITOR_BIN,
               otherwise the build produced by scripts/build-linux.sh)
--uninstall    remove the files again and refresh the databases; the MIME types
               and the associations disappear with the desktop entry
EOF
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
editor="${WDS_EDITOR_BIN:-}"
uninstall=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --uninstall) uninstall=1; shift ;;
    --editor) editor="${2:?--editor needs a path}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

desktop_dir="${data_home}/applications"
mime_dir="${data_home}/mime/packages"
icon_dir="${data_home}/icons/hicolor/256x256/apps"
desktop_file="${desktop_dir}/wds-editor.desktop"
mime_file="${mime_dir}/wds-editor.xml"
icon_file="${icon_dir}/wds-editor.png"
mime_type_project="application/x-wdsproject"
mime_type_chart="application/x-wdschart"
desktop_id="wds-editor.desktop"

refresh_databases() {
  if [[ -d "${data_home}/mime" ]] && command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "${data_home}/mime" >/dev/null 2>&1 || true
  fi
  if [[ -d "${desktop_dir}" ]] && command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${desktop_dir}" >/dev/null 2>&1 || true
  fi
}

if [[ "${uninstall}" == 1 ]]; then
  rm -f "${desktop_file}" "${mime_file}" "${icon_file}"
  refresh_databases
  echo "Removed WDS Editor document associations (${desktop_file})."
  exit 0
fi

if [[ -z "${editor}" ]]; then
  # shellcheck source=lib/wds-linux.sh
  source "${ROOT}/scripts/lib/wds-linux.sh"
  wds_linux_init
  editor="${wds_linux_build}/ui/wds_editor"
fi
if [[ ! -x "${editor}" ]]; then
  echo "error: editor not found or not executable: ${editor}" >&2
  echo "Build it with scripts/build-linux.sh or pass --editor PATH." >&2
  exit 1
fi
# The desktop entry stores an absolute path: file managers do not inherit our cwd.
editor_dir="$(cd "$(dirname "${editor}")" && pwd)"
editor="${editor_dir}/$(basename "${editor}")"

mkdir -p "${desktop_dir}" "${mime_dir}" "${icon_dir}"

# Exec=%f: the shell hands the double-clicked file to argv[1], which is exactly
# what the editor's file-association argument handling expects. Quotes keep
# paths with spaces working.
cat >"${desktop_file}" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=WDS Editor
GenericName=WDS Chart Editor
Comment=Edit World Dai Star chart projects
Exec="${editor}" %f
TryExec=${editor}
Icon=wds-editor
Terminal=false
Categories=AudioVideo;Audio;Editor;
MimeType=${mime_type_project};${mime_type_chart};
StartupNotify=true
EOF

cat >"${mime_file}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="${mime_type_project}">
    <comment>WDS chart project</comment>
    <comment xml:lang="zh_CN">WDS 音游谱面工程</comment>
    <icon name="wds-editor"/>
    <glob pattern="*.wdsproject"/>
    <magic priority="50">
      <match value="WDSPROJECT " type="string" offset="0"/>
    </magic>
  </mime-type>
  <mime-type type="${mime_type_chart}">
    <comment>WDS chart</comment>
    <comment xml:lang="zh_CN">WDS 音游谱面</comment>
    <icon name="wds-editor"/>
    <glob pattern="*.wdschart"/>
    <magic priority="50">
      <match value="WDSCHART " type="string" offset="0"/>
    </magic>
  </mime-type>
</mime-info>
EOF

if [[ -f "${ROOT}/icons/wds.png" ]]; then
  cp -f "${ROOT}/icons/wds.png" "${icon_file}"
fi

refresh_databases
if command -v xdg-mime >/dev/null 2>&1; then
  if xdg-mime default "${desktop_id}" "${mime_type_project}" >/dev/null 2>&1 \
     && xdg-mime default "${desktop_id}" "${mime_type_chart}" >/dev/null 2>&1; then
    echo "WDS Editor is now the default handler for .wdsproject and .wdschart."
  else
    echo "warning: could not set the default handler; pick it in the file manager's Open With dialog." >&2
  fi
else
  echo "warning: xdg-mime not found; pick WDS Editor in the file manager's Open With dialog." >&2
fi
echo "Installed ${desktop_file} (editor: ${editor})."

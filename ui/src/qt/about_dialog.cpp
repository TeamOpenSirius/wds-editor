#include "wds/ui/qt/about_dialog.hpp"

#include "wds/ui/resource_paths.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/common/crash_handler.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace wds::ui {
namespace {

constexpr const char* kVersion = WDS_APP_VERSION;

const char* kLicenseText = R"(WDS Editor — a chart editor for World Dai Star rhythm charts.

Copyright (C) 2026 softmanmaker.
Copyright (C) 2026 The OpenSirius team.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.

Full texts ship in the app's licenses/ folder (COPYING.LESSER, COPYING).
Online: <https://www.gnu.org/licenses/>.

This program bundles or downloads third-party components under their own
licenses. Open “打开许可文件文件夹” for the complete set.

────────────────────────────────────────
Qt 6 (LGPL v3)
  The Qt Company Ltd. https://www.qt.io
  Dynamically linked. You may replace the bundled Qt shared libraries with a
  compatible build of the same major version. Corresponding Qt source for the
  version shown above is available from https://code.qt.io/ and
  https://download.qt.io/. See licenses/Qt-NOTICE.txt and COPYING.LESSER.
  https://doc.qt.io/qt-6/lgpl.html

────────────────────────────────────────
OBS Studio theme "Yami" (GPL v2 or later)
  Copyright (C) The OBS Project.
  The editor's Qt stylesheet and theme icons are derived from OBS Studio's
  theme assets. See licenses/OBS-Yami-NOTICE.txt and GPL-2.0.txt.
  https://github.com/obsproject/obs-studio

────────────────────────────────────────
FFmpeg (LGPL v2.1 or later)
  Optional helper downloaded for video export; run as a separate process.
  Windows/Linux: BtbN FFmpeg-Builds LGPL builds.
  macOS: serversideup/ffmpeg-lgpl-builds (--disable-gpl --disable-nonfree).
  Binaries with --enable-nonfree are rejected. Source:
  https://ffmpeg.org/releases/ — see licenses/FFmpeg-NOTICE.txt.

────────────────────────────────────────
Noto Sans SC (SIL Open Font License 1.1)
  Copyright The Noto Project Authors. See licenses/NotoSansSC-OFL.txt.

────────────────────────────────────────
Fluent UI System Icons (MIT)
  Copyright (c) 2020 Microsoft Corporation.
  Toolbar and playback icons. See licenses/Fluent-UI-System-Icons-LICENSE.txt.
  https://github.com/microsoft/fluentui-system-icons

────────────────────────────────────────
BASS / BASSmix (un4seen developments)
  Free for non-commercial use; a commercial licence is required otherwise.
  See licenses/BASS-NOTICE.txt and https://www.un4seen.com

────────────────────────────────────────
Other bundled libraries (MoltenVK, Vulkan Loader, libpng, OpenSSL, HarfBuzz,
ICU, …)
  See licenses/THIRD_PARTY_NOTICES.txt and the individual LICENSE files in
  licenses/.

Game-related preview assets (skins, SFX, split-effect resources) are used for
chart authoring and playback preview. Rights follow the applicable game-asset
terms for this project.)";

}  // namespace

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("关于 WDS Editor"));
  setFixedSize(560, 460);

  auto* root = new QVBoxLayout(this);

  auto* header = new QLabel(this);
  header->setTextFormat(Qt::RichText);
  header->setText(tr("<div style='font-size:18pt; font-weight:bold;'>WDS Editor</div>"
                     "<div style='color:#969696;'>版本 %1</div>"
                     "<div style='margin-top:6px;'>音游谱面编辑器 · Qt %2</div>")
                      .arg(QString::fromLatin1(kVersion), QStringLiteral(QT_VERSION_STR)));
  root->addWidget(header);

  auto* license = new QPlainTextEdit(this);
  license->setReadOnly(true);
  license->setPlainText(QString::fromUtf8(kLicenseText));
  root->addWidget(license, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto* open_logs = buttons->addButton(tr("打开崩溃日志文件夹"), QDialogButtonBox::ActionRole);
  connect(open_logs, &QPushButton::clicked, this, [] {
    journal_menu_action("about.open_logs");
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromUtf8(wds::common::crash_log_directory())));
  });
  auto* open_licenses = buttons->addButton(tr("打开许可文件文件夹"), QDialogButtonBox::ActionRole);
  connect(open_licenses, &QPushButton::clicked, this, [] {
    journal_menu_action("about.open_licenses");
    const auto argv0 = QCoreApplication::applicationFilePath();
    const QString dir =
        QString::fromStdString(resolve_licenses_dir(argv0.toUtf8().constData()));
    if (!dir.isEmpty()) {
      QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
  });
  connect(buttons, &QDialogButtonBox::rejected, this, [this] {
    journal_menu_action("about.close");
    accept();
  });
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    journal_menu_action("about.close");
    accept();
  });
  root->addWidget(buttons);
}

}  // namespace wds::ui

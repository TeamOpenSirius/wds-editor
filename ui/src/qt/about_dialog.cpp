#include "wds/ui/qt/about_dialog.hpp"

#include "wds/ui/qt/fluent_icons.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

namespace wds::ui {
namespace {

constexpr const char* kVersion = WDS_APP_VERSION;

const char* kLicenseText = R"(WDS Editor — a chart editor for World Dai Star rhythm charts.

Copyright (C) 2026 softmanmaker.
Copyright (C) 2026 The OpenSirius team.

This program bundles third-party components under their own licenses:

────────────────────────────────────────
Qt 6 (LGPL v3)
  The Qt Company Ltd. https://www.qt.io
  Dynamically linked. See https://doc.qt.io/qt-6/lgpl.html

────────────────────────────────────────
OBS Studio theme "Yami" (GPL v2 or later)
  Copyright (C) The OBS Project.
  The editor's Qt stylesheet and theme icons are derived from OBS Studio's
  theme assets. https://github.com/obsproject/obs-studio

────────────────────────────────────────
Noto Sans SC (SIL Open Font License 1.1)
  Copyright The Noto Project Authors.

────────────────────────────────────────
Segoe Fluent Icons (Microsoft — restricted)
  The Segoe Fluent Icons font is used under Microsoft's font EULA solely for
  UI on a Microsoft platform; it may not be redistributed to third parties.
  Ship your own icon font when distributing outside that scope.

────────────────────────────────────────
BASS / BASSmix (un4seen developments)
  Free for non-commercial use; a license is required for commercial use.
  https://www.un4seen.com

Skin / chart assets belonging to the original game are the property of their
respective rights holders and are used here for authoring/preview only.)";

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
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  root->addWidget(buttons);
}

}  // namespace wds::ui

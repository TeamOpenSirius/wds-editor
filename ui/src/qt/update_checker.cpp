#include "wds/ui/qt/update_checker.hpp"

#include "wds/common/app_version.hpp"

#include <QAbstractButton>
#include <QByteArray>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <optional>
#include <string>

namespace wds::ui {
namespace {

constexpr auto kAutoCheckKey = "updates/auto_check";
constexpr auto kVersionHost = "wds-editor-version.ginojin.top";
constexpr auto kReleasesUrl = "https://github.com/TeamOpenSirius/wds-editor/releases/latest";
constexpr int kLookupTimeoutMs = 8000;

QString cleaned_txt(const QDnsTextRecord& record) {
  QByteArray joined;
  for (const QByteArray& chunk : record.values()) joined += chunk;
  QString text = QString::fromUtf8(joined).trimmed();
  if (text.size() >= 2) {
    const QChar first = text.front();
    const QChar last = text.back();
    if ((first == QLatin1Char('"') && last == QLatin1Char('"')) ||
        (first == QLatin1Char('\'') && last == QLatin1Char('\''))) {
      text = text.mid(1, text.size() - 2).trimmed();
    }
  }
  return text;
}

}  // namespace

bool auto_check_updates_enabled() {
  return QSettings(QStringLiteral("WDS"), QStringLiteral("WDS Editor"))
      .value(QLatin1String(kAutoCheckKey), true)
      .toBool();
}

void set_auto_check_updates_enabled(bool enabled) {
  QSettings(QStringLiteral("WDS"), QStringLiteral("WDS Editor"))
      .setValue(QLatin1String(kAutoCheckKey), enabled);
}

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {
  lookup_.setType(QDnsLookup::TXT);
  lookup_.setName(QLatin1String(kVersionHost));
  timeout_.setSingleShot(true);
  timeout_.setInterval(kLookupTimeoutMs);
  connect(&lookup_, &QDnsLookup::finished, this, [this] { on_lookup_finished(); });
  connect(&timeout_, &QTimer::timeout, this, [this] { on_timeout(); });
}

void UpdateChecker::check(QWidget* parent, bool user_initiated) {
  if (busy_) {
    if (user_initiated) {
      user_initiated_ = true;
      dialog_parent_ = parent;
    }
    return;
  }
  dialog_parent_ = parent;
  user_initiated_ = user_initiated;
  start_lookup();
}

void UpdateChecker::start_lookup() {
  busy_ = true;
  timeout_.start();
  lookup_.lookup();
}

void UpdateChecker::on_timeout() {
  if (!busy_) return;
  lookup_.abort();
}

void UpdateChecker::on_lookup_finished() {
  if (!busy_) return;
  timeout_.stop();
  const bool user = user_initiated_;
  if (lookup_.error() != QDnsLookup::NoError) {
    finish_idle();
    if (user) show_failure();
    return;
  }
  handle_records();
}

void UpdateChecker::finish_idle() {
  busy_ = false;
  user_initiated_ = false;
}

void UpdateChecker::handle_records() {
  const bool user = user_initiated_;
  std::optional<wds::common::AppVersion> remote;
  QString remote_text;
  for (const QDnsTextRecord& record : lookup_.textRecords()) {
    const QString text = cleaned_txt(record);
    const auto parsed = wds::common::parse_app_version(text.toStdString());
    if (!parsed.has_value()) continue;
    if (!remote.has_value() || wds::common::compare_app_version(*parsed, *remote) > 0) {
      remote = parsed;
      remote_text = QString::fromStdString(wds::common::format_app_version(*parsed));
    }
  }

  const QString local_text = QStringLiteral(WDS_APP_VERSION);
  const auto local = wds::common::parse_app_version(local_text.toStdString());
  finish_idle();
  if (!remote.has_value() || !local.has_value()) {
    if (user) show_failure();
    return;
  }
  if (wds::common::compare_app_version(*remote, *local) > 0) {
    show_update(remote_text, local_text);
    return;
  }
  if (user) show_latest();
}

void UpdateChecker::show_update(const QString& remote, const QString& local) {
  QMessageBox box(QMessageBox::Information,
                  QCoreApplication::translate("wds::ui", "检查更新"),
                  QCoreApplication::translate("wds::ui", "检测到新版本 %1（当前 %2）。")
                      .arg(remote, local),
                  QMessageBox::NoButton, dialog_parent());
  auto* download =
      box.addButton(QCoreApplication::translate("wds::ui", "前往下载"), QMessageBox::AcceptRole);
  box.addButton(QCoreApplication::translate("wds::ui", "取消"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == static_cast<QAbstractButton*>(download)) {
    QDesktopServices::openUrl(QUrl(QLatin1String(kReleasesUrl)));
  }
}

void UpdateChecker::show_latest() {
  QMessageBox::information(dialog_parent(), QCoreApplication::translate("wds::ui", "检查更新"),
                           QCoreApplication::translate("wds::ui", "当前已是最新版本。"));
}

void UpdateChecker::show_failure() {
  QMessageBox::warning(dialog_parent(), QCoreApplication::translate("wds::ui", "检查更新"),
                       QCoreApplication::translate("wds::ui", "更新检查失败"));
}

QWidget* UpdateChecker::dialog_parent() const { return dialog_parent_.data(); }

}  // namespace wds::ui

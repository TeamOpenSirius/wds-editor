#pragma once

#include <QDnsLookup>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QWidget;

namespace wds::ui {

bool auto_check_updates_enabled();
void set_auto_check_updates_enabled(bool enabled);

class UpdateChecker final : public QObject {
 public:
  explicit UpdateChecker(QObject* parent = nullptr);
  void check(QWidget* dialog_parent, bool user_initiated);

 private:
  void start_lookup();
  void on_lookup_finished();
  void on_timeout();
  void finish_idle();
  void handle_records();
  void show_update(const QString& remote, const QString& local);
  void show_latest();
  void show_failure();
  QWidget* dialog_parent() const;

  QDnsLookup lookup_;
  QTimer timeout_;
  QPointer<QWidget> dialog_parent_;
  bool user_initiated_ = false;
  bool busy_ = false;
};

}  // namespace wds::ui

#pragma once

#include <QString>

class QWidget;
class QDialog;

namespace wds::ui {

// Modal "please wait" overlay for synchronous, UI-blocking operations
// (project open / save). Shown for the lifetime of the scope; because the
// work runs on the UI thread the bar is indeterminate and does not animate,
// but it blocks input and gives the user feedback that work is in progress.
class BusyScope {
 public:
  explicit BusyScope(QWidget* parent, const QString& message);
  ~BusyScope();
  BusyScope(const BusyScope&) = delete;
  BusyScope& operator=(const BusyScope&) = delete;

 private:
  QDialog* dialog_ = nullptr;
};

}  // namespace wds::ui

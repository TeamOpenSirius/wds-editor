#pragma once

#include <QString>

class QWidget;
class QDialog;

namespace wds::ui {

// Modal "please wait" overlay for UI-blocking operations (project open / save).
// Shown for the lifetime of the scope. Callers should display this, return to
// the event loop so Qt can paint it, then run the blocking work (see
// EditorMainWindow::run_with_busy). The bar is indeterminate.
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

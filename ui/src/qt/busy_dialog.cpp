#include "wds/ui/qt/busy_dialog.hpp"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

namespace wds::ui {

BusyScope::BusyScope(QWidget* parent, const QString& message) {
  dialog_ = new QDialog(parent, Qt::FramelessWindowHint | Qt::Dialog);
  dialog_->setModal(true);
  dialog_->setAttribute(Qt::WA_DeleteOnClose, false);
  dialog_->setMinimumWidth(320);

  auto* layout = new QVBoxLayout(dialog_);
  layout->setContentsMargins(24, 20, 24, 20);
  auto* label = new QLabel(message, dialog_);
  label->setAlignment(Qt::AlignCenter);
  layout->addWidget(label);
  auto* bar = new QProgressBar(dialog_);
  bar->setRange(0, 0);  // indeterminate
  bar->setTextVisible(false);
  layout->addWidget(bar);
  dialog_->setFixedSize(360, 120);

  dialog_->show();
  dialog_->raise();
  // Force one paint pass so the overlay is visible before the blocking call.
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

BusyScope::~BusyScope() {
  if (dialog_ != nullptr) {
    dialog_->close();
    dialog_->deleteLater();
  }
}

}  // namespace wds::ui

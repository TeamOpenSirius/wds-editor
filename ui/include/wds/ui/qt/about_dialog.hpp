#pragma once

#include <QDialog>

namespace wds::ui {

// About page: app name/version, copyright, and third-party license notices.
class AboutDialog final : public QDialog {
 public:
  explicit AboutDialog(QWidget* parent = nullptr);
};

}  // namespace wds::ui

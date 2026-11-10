#pragma once

#include <QDialog>

class QLineEdit;
class QShowEvent;

namespace wds::ui {

class SpeedEditDialog final : public QDialog {
 public:
  explicit SpeedEditDialog(double multiplier, QWidget* parent = nullptr);
  double multiplier() const;

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  void commit_field();
  void select_all_text();

  QLineEdit* field_ = nullptr;
  double value_ = 1.0;
  bool committing_ = false;
};

}  // namespace wds::ui

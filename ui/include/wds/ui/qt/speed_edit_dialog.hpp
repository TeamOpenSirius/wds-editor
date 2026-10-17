#pragma once

#include <QDialog>

class QDoubleSpinBox;

namespace wds::ui {

class SpeedEditDialog final : public QDialog {
 public:
  explicit SpeedEditDialog(double multiplier, QWidget* parent = nullptr);
  double multiplier() const;

 private:
  QDoubleSpinBox* multiplier_ = nullptr;
};

}  // namespace wds::ui

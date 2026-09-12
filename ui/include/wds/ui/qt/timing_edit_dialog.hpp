#pragma once

#include <QDialog>
#include <cstdint>

class QDoubleSpinBox;
class QSpinBox;

namespace wds::ui {

// Qt stand-in for the old painted BPM / meter popup.
class TimingEditDialog final : public QDialog {
 public:
  TimingEditDialog(bool bpm_mode, double bpm, int32_t numerator, int32_t denominator,
                   QWidget* parent = nullptr);

  double bpm() const;
  int32_t numerator() const;
  int32_t denominator() const;

 private:
  bool bpm_mode_ = true;
  QDoubleSpinBox* bpm_ = nullptr;
  QSpinBox* numerator_ = nullptr;
  QSpinBox* denominator_ = nullptr;
};

}  // namespace wds::ui

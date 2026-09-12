#include "wds/ui/qt/timing_edit_dialog.hpp"

#include "wds/ui/qt/fluent_icons.hpp"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace wds::ui {

TimingEditDialog::TimingEditDialog(bool bpm_mode, double bpm, int32_t numerator,
                                   int32_t denominator, QWidget* parent)
    : QDialog(parent), bpm_mode_(bpm_mode) {
  setWindowTitle(bpm_mode_ ? tr("BPM 编辑") : tr("拍号编辑"));
  setMinimumWidth(280);

  auto* root = new QVBoxLayout(this);
  auto* form = new QFormLayout;

  if (bpm_mode_) {
    bpm_ = new QDoubleSpinBox(this);
    bpm_->setDecimals(3);
    bpm_->setRange(0.001, 9999.0);
    bpm_->setSingleStep(1.0);
    bpm_->setValue(std::isfinite(bpm) && bpm > 0.0 ? bpm : 120.0);
    form->addRow(tr("BPM"), bpm_);
  } else {
    auto* row = new QWidget(this);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    numerator_ = new QSpinBox(row);
    denominator_ = new QSpinBox(row);
    numerator_->setRange(1, 64);
    denominator_->setRange(1, 64);
    numerator_->setValue(std::max(1, numerator));
    denominator_->setValue(std::max(1, denominator));
    row_layout->addWidget(numerator_, 1);
    row_layout->addWidget(new QLabel(QStringLiteral("/"), row));
    row_layout->addWidget(denominator_, 1);
    form->addRow(tr("拍号"), row);
  }
  root->addLayout(form);

  auto* buttons = new QDialogButtonBox(this);
  auto* confirm = buttons->addButton(tr("确认"), QDialogButtonBox::AcceptRole);
  confirm->setIcon(fluent_icon(fluent::Accept));
  auto* cancel = buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
  cancel->setIcon(fluent_icon(fluent::Clear));
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

double TimingEditDialog::bpm() const { return bpm_ != nullptr ? bpm_->value() : 120.0; }

int32_t TimingEditDialog::numerator() const {
  return numerator_ != nullptr ? numerator_->value() : 4;
}

int32_t TimingEditDialog::denominator() const {
  return denominator_ != nullptr ? denominator_->value() : 4;
}

}  // namespace wds::ui

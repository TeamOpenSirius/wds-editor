#include "wds/ui/qt/speed_edit_dialog.hpp"

#include "wds/ui/ui_manager.hpp"

#include <wds/core/notation.hpp>

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>

#include <cmath>

namespace wds::ui {

SpeedEditDialog::SpeedEditDialog(double multiplier, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("速度编辑"));
  setMinimumWidth(280);

  auto* root = new QVBoxLayout(this);
  auto* form = new QFormLayout;
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

  multiplier_ = new QDoubleSpinBox(this);
  multiplier_->setDecimals(6);
  multiplier_->setRange(0.0, 1000.0);
  multiplier_->setSingleStep(0.1);
  multiplier_->setValue(wds::chart_editor::is_valid_scroll_multiplier(multiplier) ? multiplier
                                                                                  : 1.0);
  form->addRow(tr("变速"), multiplier_);
  root->addLayout(form);

  auto* buttons = new QDialogButtonBox(this);
  buttons->addButton(tr("确认"), QDialogButtonBox::AcceptRole);
  buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    journal_menu_action("speed.ok");
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, [this] {
    journal_menu_action("speed.cancel");
    reject();
  });
}

double SpeedEditDialog::multiplier() const {
  return multiplier_ != nullptr ? multiplier_->value() : 1.0;
}

}  // namespace wds::ui

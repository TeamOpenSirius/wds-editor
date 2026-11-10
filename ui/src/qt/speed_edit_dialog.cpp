#include "wds/ui/qt/speed_edit_dialog.hpp"

#include "wds/ui/constant_expr.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/core/notation.hpp>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <string_view>

namespace wds::ui {

SpeedEditDialog::SpeedEditDialog(double multiplier, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("速度编辑"));
  setMinimumWidth(280);

  value_ = wds::chart_editor::is_valid_scroll_multiplier(multiplier) ? multiplier : 1.0;
  if (const auto rounded = eval_constant_expr(format_trimmed_decimal(value_))) {
    value_ = *rounded;
  }

  auto* root = new QVBoxLayout(this);
  auto* form = new QFormLayout;
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

  field_ = new QLineEdit(this);
  field_->setText(QString::fromStdString(format_trimmed_decimal(value_)));
  form->addRow(tr("变速"), field_);
  root->addLayout(form);

  connect(field_, &QLineEdit::editingFinished, this, [this] { commit_field(); });

  auto* buttons = new QDialogButtonBox(this);
  buttons->addButton(tr("确认"), QDialogButtonBox::AcceptRole);
  buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    journal_menu_action("speed.ok");
    commit_field();
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, [this] {
    journal_menu_action("speed.cancel");
    reject();
  });

  QTimer::singleShot(0, this, [this] { select_all_text(); });
}

double SpeedEditDialog::multiplier() const { return value_; }

void SpeedEditDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  select_all_text();
  QTimer::singleShot(0, this, [this] { select_all_text(); });
}

void SpeedEditDialog::select_all_text() {
  if (field_ == nullptr) return;
  field_->setFocus(Qt::OtherFocusReason);
  field_->selectAll();
}

void SpeedEditDialog::commit_field() {
  if (committing_ || field_ == nullptr) return;
  committing_ = true;
  const QByteArray utf8 = field_->text().toUtf8();
  const auto parsed =
      eval_constant_expr(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
  if (parsed && wds::chart_editor::is_valid_scroll_multiplier(*parsed)) {
    value_ = *parsed;
  }
  const QString shown = QString::fromStdString(format_trimmed_decimal(value_));
  if (field_->text() != shown) field_->setText(shown);
  committing_ = false;
}

}  // namespace wds::ui

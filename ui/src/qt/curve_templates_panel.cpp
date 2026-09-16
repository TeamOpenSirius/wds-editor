#include "wds/ui/qt/curve_templates_panel.hpp"

#include "wds/ui/curve_template.hpp"
#include "wds/ui/regions/settings/curve_templates_dialog.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/core/easing.hpp>

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QDoubleSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace wds::ui {
namespace {

constexpr wds::chart_editor::EasingAlgorithm kAlgorithms[] = {
    wds::chart_editor::EasingAlgorithm::Linear,
    wds::chart_editor::EasingAlgorithm::Poly,
    wds::chart_editor::EasingAlgorithm::Exp,
    wds::chart_editor::EasingAlgorithm::Sine,
};

constexpr const char* kAlgorithmLabels[] = {"Linear", "Poly", "Exp", "Sine"};

constexpr wds::chart_editor::EasingDirection kPreviewDirections[] = {
    wds::chart_editor::EasingDirection::In,
    wds::chart_editor::EasingDirection::Out,
    wds::chart_editor::EasingDirection::InOut,
    wds::chart_editor::EasingDirection::OutIn,
};

constexpr const char* kPreviewLabels[] = {"In", "Out", "InOut", "OutIn"};

bool algorithm_uses_parameter(wds::chart_editor::EasingAlgorithm algorithm) noexcept {
  return algorithm == wds::chart_editor::EasingAlgorithm::Poly ||
         algorithm == wds::chart_editor::EasingAlgorithm::Exp;
}

class EasingPreviewWidget final : public QWidget {
 public:
  explicit EasingPreviewWidget(wds::chart_editor::EasingDirection direction, QWidget* parent)
      : QWidget(parent), direction_(direction) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumSize(64, 64);
  }

  QSize sizeHint() const override { return {80, 80}; }
  QSize minimumSizeHint() const override { return {64, 64}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override { return std::max(64, w); }

  void set_curve(wds::chart_editor::EasingAlgorithm algorithm, double parameter) {
    algorithm_ = algorithm;
    parameter_ = parameter;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(6, 16, -6, -6);
    painter.fillRect(rect(), palette().base());
    painter.setPen(QPen(palette().mid(), 1));
    painter.drawRect(box);
    if (box.width() < 2.0 || box.height() < 2.0) return;
    QPainterPath path;
    const int cols = std::max(2, static_cast<int>(std::floor(box.width())));
    for (int x = 0; x <= cols; ++x) {
      const double t = static_cast<double>(x) / static_cast<double>(cols);
      const double y =
          wds::chart_editor::apply_easing(t, algorithm_, direction_, parameter_);
      const QPointF p(box.left() + static_cast<qreal>(x) / cols * box.width(),
                      box.bottom() - y * box.height());
      if (x == 0)
        path.moveTo(p);
      else
        path.lineTo(p);
    }
    painter.setPen(QPen(palette().highlight(), 1.6));
    painter.drawPath(path);
    painter.setPen(palette().text().color());
    painter.drawText(QRect(0, 0, width(), 16), Qt::AlignHCenter | Qt::AlignVCenter,
                     QString::fromUtf8(kPreviewLabels[static_cast<int>(direction_)]));
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    const int side = std::max(64, width());
    if (height() != side) setFixedHeight(side);
  }

 private:
  wds::chart_editor::EasingDirection direction_;
  wds::chart_editor::EasingAlgorithm algorithm_ = wds::chart_editor::EasingAlgorithm::Linear;
  double parameter_ = 0.0;
};

}  // namespace

CurveTemplatesPanel::CurveTemplatesPanel(UiManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  setMinimumWidth(0);
  build_ui();
  reload_from_manager();
}

void CurveTemplatesPanel::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 6);
  root->setSpacing(6);

  auto* list_row = new QHBoxLayout;
  list_ = new QListWidget(this);
  list_->setMinimumHeight(80);
  list_row->addWidget(list_, 1);
  auto* list_btns = new QVBoxLayout;
  add_ = new QPushButton(tr("添加"), this);
  remove_ = new QPushButton(tr("删除"), this);
  list_btns->addWidget(add_);
  list_btns->addWidget(remove_);
  list_btns->addStretch(1);
  list_row->addLayout(list_btns);
  root->addLayout(list_row, 1);

  auto add_labeled_row = [&](const QString& title, QWidget* field) {
    auto* row = new QHBoxLayout;
    auto* label = new QLabel(title, this);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    row->addWidget(label, 0, Qt::AlignVCenter);
    row->addWidget(field, 1, Qt::AlignVCenter);
    root->addLayout(row);
  };

  name_ = new QLineEdit(this);
  add_labeled_row(tr("名称"), name_);

  algorithm_ = new QComboBox(this);
  algorithm_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  for (int i = 0; i < 4; ++i)
    algorithm_->addItem(QString::fromUtf8(kAlgorithmLabels[i]));
  add_labeled_row(tr("算法"), algorithm_);

  parameter_ = new QDoubleSpinBox(this);
  parameter_->setRange(0.0, 20.0);
  parameter_->setDecimals(4);
  parameter_->setSingleStep(0.25);
  add_labeled_row(tr("参数"), parameter_);

  auto* preview_grid = new QGridLayout;
  preview_grid->setSpacing(4);
  for (int i = 0; i < 4; ++i) {
    auto* preview = new EasingPreviewWidget(kPreviewDirections[i], this);
    previews_[static_cast<std::size_t>(i)] = preview;
    preview_grid->addWidget(preview, i / 2, i % 2);
  }
  root->addLayout(preview_grid, 0);

  connect(list_, &QListWidget::currentRowChanged, this, [this](int) {
    if (syncing_ || manager_ == nullptr) return;
    journal_menu_action("curve_templates.select");
    auto state = manager_->curve_template_state();
    const int row = list_->currentRow();
    if (row >= 0 && static_cast<std::size_t>(row) < state.templates.size()) {
      state.selected_id = state.templates[static_cast<std::size_t>(row)].id;
      manager_->set_curve_template_state_from_qt(std::move(state));
    }
    sync_fields_from_state();
    if (on_changed_) on_changed_();
  });
  connect(add_, &QPushButton::clicked, this, [this] {
    journal_menu_action("curve_templates.add");
    if (manager_ == nullptr) return;
    auto state = manager_->curve_template_state();
    if (state.templates.size() >= kMaxCurveTemplates) return;
    CurveTemplate tmpl;
    tmpl.id = allocate_curve_template_id(state.templates);
    tmpl.name = CurveTemplateDialogSession::next_default_name(state.templates);
    tmpl.algorithm = wds::chart_editor::EasingAlgorithm::Linear;
    tmpl.parameter = 0.0;
    state.templates.push_back(std::move(tmpl));
    state.selected_id = state.templates.back().id;
    manager_->set_curve_template_state_from_qt(std::move(state));
    reload_from_manager();
    if (on_changed_) on_changed_();
  });
  connect(remove_, &QPushButton::clicked, this, [this] {
    journal_menu_action("curve_templates.delete");
    if (manager_ == nullptr) return;
    auto state = manager_->curve_template_state();
    const int row = list_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= state.templates.size()) return;
    const auto idx = static_cast<std::size_t>(row);
    const bool removing_selected = state.templates[idx].id == state.selected_id;
    state.templates.erase(state.templates.begin() + static_cast<std::ptrdiff_t>(idx));
    if (removing_selected) {
      if (state.templates.empty())
        state.selected_id = 0;
      else
        state.selected_id = state.templates[std::min(idx, state.templates.size() - 1)].id;
    }
    manager_->set_curve_template_state_from_qt(std::move(state));
    reload_from_manager();
    if (on_changed_) on_changed_();
  });
  connect(name_, &QLineEdit::editingFinished, this, [this] {
    journal_menu_action("curve_templates.rename");
    apply_live();
  });
  connect(algorithm_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (!syncing_) journal_menu_action("curve_templates.algorithm");
    apply_live();
  });
  connect(parameter_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) { apply_live(); });
}

void CurveTemplatesPanel::reload_from_manager() {
  if (manager_ == nullptr) return;
  syncing_ = true;
  const auto& state = manager_->curve_template_state();
  list_->clear();
  int select = -1;
  for (std::size_t i = 0; i < state.templates.size(); ++i) {
    const auto& tmpl = state.templates[i];
    const QString label =
        tmpl.name.empty() ? tr("曲线 %1").arg(static_cast<int>(i + 1))
                          : QString::fromStdString(tmpl.name);
    list_->addItem(label);
    if (tmpl.id == state.selected_id) select = static_cast<int>(i);
  }
  if (select < 0 && !state.templates.empty()) select = 0;
  if (select >= 0) list_->setCurrentRow(select);
  syncing_ = false;
  sync_fields_from_state();
  update_enabled();
}

void CurveTemplatesPanel::sync_fields_from_state() {
  if (manager_ == nullptr) return;
  syncing_ = true;
  const auto& state = manager_->curve_template_state();
  const CurveTemplate* tmpl = find_curve_template_by_id(state.templates, state.selected_id);
  if (tmpl == nullptr && !state.templates.empty() && list_->currentRow() >= 0 &&
      static_cast<std::size_t>(list_->currentRow()) < state.templates.size()) {
    tmpl = &state.templates[static_cast<std::size_t>(list_->currentRow())];
  }
  const bool has = tmpl != nullptr;
  name_->setEnabled(has);
  algorithm_->setEnabled(has);
  parameter_->setEnabled(has && tmpl != nullptr && algorithm_uses_parameter(tmpl->algorithm));
  if (has) {
    name_->setText(QString::fromStdString(tmpl->name));
    int algo = 0;
    for (int i = 0; i < 4; ++i) {
      if (kAlgorithms[i] == tmpl->algorithm) algo = i;
    }
    algorithm_->setCurrentIndex(algo);
    parameter_->setValue(tmpl->parameter);
    for (auto* widget : previews_) {
      if (auto* preview = static_cast<EasingPreviewWidget*>(widget))
        preview->set_curve(tmpl->algorithm, tmpl->parameter);
    }
  } else {
    name_->clear();
    algorithm_->setCurrentIndex(0);
    parameter_->setValue(0.0);
    for (auto* widget : previews_) {
      if (auto* preview = static_cast<EasingPreviewWidget*>(widget))
        preview->set_curve(wds::chart_editor::EasingAlgorithm::Linear, 0.0);
    }
  }
  add_->setEnabled(state.templates.size() < kMaxCurveTemplates);
  remove_->setEnabled(has);
  syncing_ = false;
}

void CurveTemplatesPanel::apply_live() {
  if (syncing_ || manager_ == nullptr) return;
  auto state = manager_->curve_template_state();
  const int row = list_->currentRow();
  if (row < 0 || static_cast<std::size_t>(row) >= state.templates.size()) return;
  auto& tmpl = state.templates[static_cast<std::size_t>(row)];
  tmpl.name = name_->text().toStdString();
  const int algo = algorithm_->currentIndex();
  if (algo >= 0 && algo < 4) tmpl.algorithm = kAlgorithms[algo];
  tmpl.parameter = parameter_->value();
  normalize_curve_template(tmpl);
  state.selected_id = tmpl.id;
  manager_->set_curve_template_state_from_qt(std::move(state));
  {
    const QSignalBlocker blocker(list_);
    if (row >= 0 && row < list_->count()) {
      list_->item(row)->setText(name_->text().isEmpty() ? tr("曲线 %1").arg(row + 1)
                                                        : name_->text());
    }
  }
  sync_fields_from_state();
  if (on_changed_) on_changed_();
}

void CurveTemplatesPanel::update_enabled() {
  if (manager_ == nullptr) return;
  const auto& state = manager_->curve_template_state();
  add_->setEnabled(state.templates.size() < kMaxCurveTemplates);
  remove_->setEnabled(list_->currentRow() >= 0);
}

}  // namespace wds::ui

#include "wds/ui/qt/split_picker_dialog.hpp"

#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace wds::ui {
namespace {

QColor qcolor(const wds::interaction::Color& c) {
  return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f),
                          std::clamp(c.b, 0.0f, 1.0f), std::clamp(c.a, 0.0f, 1.0f));
}

QPixmap split_preview_pixmap(int32_t split_count, int32_t color_id, const QSize& size) {
  QPixmap pm(size);
  pm.fill(QColor(5, 8, 13));
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, false);
  constexpr int32_t kLanes = 12;
  std::vector<int32_t> mids;
  split_boundaries_12(split_count, mids);
  const float line_w =
      std::clamp(static_cast<float>(size.width()) / static_cast<float>(kLanes) * 0.14f, 1.5f, 2.5f);
  auto draw_edge = [&](int32_t edge_lane, int32_t slot) {
    const float x = std::floor(static_cast<float>(edge_lane) * static_cast<float>(size.width()) /
                                   static_cast<float>(kLanes) +
                               0.5f);
    auto c = split_slot_color(color_id, slot, split_count, nullptr);
    if (c.a < 0.02f) return;
    apply_official_split_rgb_opacity(c);
    p.fillRect(QRectF(x - line_w * 0.5f, 2.0, static_cast<qreal>(line_w), size.height() - 4.0),
               qcolor(c));
  };
  draw_edge(0, 0);
  int32_t slot = 1;
  for (int32_t mid : mids) draw_edge(mid + 1, slot++);
  draw_edge(kLanes, std::max(1, split_count));
  return pm;
}

}  // namespace

SplitPickerDialog::SplitPickerDialog(int32_t count, int32_t color_id, QWidget* parent)
    : QDialog(parent), count_(std::clamp(count, 1, 6)), color_id_(color_id) {
  setWindowTitle(tr("分割线"));
  setMinimumSize(420, 520);
  resize(440, 560);

  auto* root = new QVBoxLayout(this);

  root->addWidget(new QLabel(tr("分割轨道数"), this));
  auto* count_row = new QHBoxLayout;
  counts_ = new QButtonGroup(this);
  counts_->setExclusive(true);
  for (int i = 1; i <= 6; ++i) {
    auto* btn = new QPushButton(QString::number(i), this);
    btn->setCheckable(true);
    btn->setChecked(i == count_);
    counts_->addButton(btn, i);
    count_row->addWidget(btn);
  }
  root->addLayout(count_row);

  root->addWidget(new QLabel(tr("分割线编号"), this));
  search_ = new QLineEdit(this);
  search_->setPlaceholderText(tr("数字筛选"));
  search_->setMaxLength(8);
  search_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("^[0-9]{0,8}$")),
                                                        search_));
  root->addWidget(search_);

  root->addWidget(new QLabel(tr("分割线外观"), this));
  colors_ = new QListWidget(this);
  colors_->setViewMode(QListView::IconMode);
  colors_->setResizeMode(QListView::Adjust);
  colors_->setMovement(QListView::Static);
  colors_->setWrapping(true);
  colors_->setUniformItemSizes(true);
  colors_->setIconSize(QSize(110, 32));
  colors_->setGridSize(QSize(128, 60));
  colors_->setSpacing(4);
  colors_->setSelectionMode(QAbstractItemView::SingleSelection);
  root->addWidget(colors_, 1);

  auto* buttons = new QDialogButtonBox(this);
  auto* confirm = buttons->addButton(tr("确认"), QDialogButtonBox::AcceptRole);
  confirm->setIcon(fluent_icon(fluent::Accept));
  auto* cancel = buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
  cancel->setIcon(fluent_icon(fluent::Clear));
  root->addWidget(buttons);

  connect(counts_, &QButtonGroup::idClicked, this, [this](int id) {
    count_ = std::clamp(id, 1, 6);
    rebuild_colors();
  });
  connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { rebuild_colors(); });
  connect(colors_, &QListWidget::currentItemChanged, this,
          [this](QListWidgetItem* cur, QListWidgetItem*) {
            if (cur != nullptr) color_id_ = cur->data(Qt::UserRole).toInt();
          });
  connect(colors_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { accept(); });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  rebuild_colors();
}

void SplitPickerDialog::rebuild_colors() {
  const QString query = search_ != nullptr ? search_->text() : QString();
  const auto ids = filter_split_picker_color_ids(query.toUtf8().toStdString());
  colors_->clear();
  QListWidgetItem* selected = nullptr;
  for (int32_t id : ids) {
    auto* item = new QListWidgetItem(QString::number(id), colors_);
    item->setData(Qt::UserRole, id);
    item->setIcon(QIcon(split_preview_pixmap(count_, id, colors_->iconSize())));
    item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    if (id == color_id_) selected = item;
  }
  if (selected != nullptr) {
    colors_->setCurrentItem(selected);
    colors_->scrollToItem(selected);
  }
}

}  // namespace wds::ui

#include "wds/ui/qt/spread_layout.hpp"

#include <QStyle>
#include <QWidget>
#include <QWidgetItem>

#include <algorithm>

namespace wds::ui {
namespace {

int item_hint_w(const QLayoutItem* item) {
  return std::max(item->minimumSize().width(), item->sizeHint().width());
}

int item_hint_h(const QLayoutItem* item) {
  return std::max(item->minimumSize().height(), item->sizeHint().height());
}

}  // namespace

SpreadLayout::SpreadLayout(QWidget* parent) : QLayout(parent) {
  setContentsMargins(kSpreadMinMargin, kSpreadMinVMargin, kSpreadMinMargin, kSpreadMinVMargin);
}

SpreadLayout::~SpreadLayout() {
  while (QLayoutItem* item = takeAt(0)) delete item;
}

void SpreadLayout::addRow() {
  if (rows_.isEmpty() || !rows_.back().items.isEmpty()) rows_.push_back({});
}

void SpreadLayout::addItem(QLayoutItem* item) {
  if (rows_.isEmpty()) rows_.push_back({});
  rows_.back().items.push_back({item, 0});
}

void SpreadLayout::addWidget(QWidget* widget, int stretch) {
  if (widget == nullptr) return;
  addChildWidget(widget);
  if (rows_.isEmpty()) rows_.push_back({});
  rows_.back().items.push_back({new QWidgetItem(widget), std::max(0, stretch)});
}

int SpreadLayout::count() const {
  int n = 0;
  for (const auto& row : rows_) n += row.items.size();
  return n;
}

QLayoutItem* SpreadLayout::itemAt(int index) const {
  int i = 0;
  for (const auto& row : rows_) {
    for (const auto& item : row.items) {
      if (i == index) return item.layout_item;
      ++i;
    }
  }
  return nullptr;
}

QLayoutItem* SpreadLayout::takeAt(int index) {
  int i = 0;
  for (auto& row : rows_) {
    for (int j = 0; j < row.items.size(); ++j) {
      if (i == index) {
        QLayoutItem* item = row.items[j].layout_item;
        row.items.removeAt(j);
        return item;
      }
      ++i;
    }
  }
  return nullptr;
}

QSize SpreadLayout::row_min_size(const Row& row) const {
  int w = 0;
  int h = 0;
  for (int i = 0; i < row.items.size(); ++i) {
    if (i > 0) w += kSpreadMinGap;
    w += item_hint_w(row.items[i].layout_item);
    h = std::max(h, item_hint_h(row.items[i].layout_item));
  }
  return {w, h};
}

QSize SpreadLayout::stacked_min_size() const {
  const QMargins m = contentsMargins();
  int w = 0;
  int h = m.top() + m.bottom();
  for (int i = 0; i < rows_.size(); ++i) {
    if (rows_[i].items.isEmpty()) continue;
    if (i > 0) h += kSpreadMinVGap;
    const QSize row = row_min_size(rows_[i]);
    w = std::max(w, row.width());
    h += row.height();
  }
  return {w + m.left() + m.right(), h};
}

QSize SpreadLayout::sizeHint() const { return stacked_min_size(); }

QSize SpreadLayout::minimumSize() const { return stacked_min_size(); }

Qt::Orientations SpreadLayout::expandingDirections() const {
  return Qt::Horizontal | Qt::Vertical;
}

void SpreadLayout::layout_row(const Row& row, const QRect& slot) const {
  if (row.items.isEmpty()) return;
  const int n = row.items.size();
  if (n == 2 && row.items[0].stretch > 0 &&
      row.items[0].stretch == row.items[1].stretch) {
    const int min_w = item_hint_w(row.items[0].layout_item) + kSpreadMinGap +
                      item_hint_w(row.items[1].layout_item);
    const int extra_w = std::max(0, slot.width() - min_w);
    const int gap = kSpreadMinGap + std::min(extra_w, kSpreadMaxGap - kSpreadMinGap);
    const int w0 = std::max(0, (slot.width() - gap) / 2);
    const int w1 = std::max(0, slot.width() - gap - w0);
    auto place = [&](int i, int x, int w) {
      const int h = item_hint_h(row.items[i].layout_item);
      const int y = slot.y() + std::max(0, (slot.height() - h) / 2);
      row.items[i].layout_item->setGeometry(QRect(x, y, w, h));
    };
    place(0, slot.x(), w0);
    place(1, slot.x() + w0 + gap, w1);
    return;
  }
  int min_w = 0;
  int stretch_sum = 0;
  for (int i = 0; i < n; ++i) {
    if (i > 0) min_w += kSpreadMinGap;
    min_w += item_hint_w(row.items[i].layout_item);
    stretch_sum += row.items[i].stretch;
  }
  const int extra_w = std::max(0, slot.width() - min_w);
  const int gaps = std::max(0, n - 1);
  const int gap_room = gaps * (kSpreadMaxGap - kSpreadMinGap);
  const int gap_extra = std::min(extra_w, gap_room);
  const int gap = gaps > 0 ? kSpreadMinGap + gap_extra / gaps : kSpreadMinGap;
  int gap_rem = gaps > 0 ? gap_extra % gaps : 0;
  int ctrl_extra = extra_w - gap_extra;
  const int grow_denom = stretch_sum > 0 ? stretch_sum : n;
  int x = slot.x();
  int leftover = ctrl_extra;
  for (int i = 0; i < n; ++i) {
    const int share = stretch_sum > 0 ? row.items[i].stretch : 1;
    int grow = 0;
    if (ctrl_extra > 0 && grow_denom > 0) {
      grow = ctrl_extra * share / grow_denom;
      leftover -= grow;
      if (i == n - 1) grow += leftover;
    }
    const int w = item_hint_w(row.items[i].layout_item) + grow;
    const int h = item_hint_h(row.items[i].layout_item);
    const int y = slot.y() + std::max(0, (slot.height() - h) / 2);
    row.items[i].layout_item->setGeometry(QRect(x, y, w, h));
    x += w;
    if (i + 1 < n) {
      x += gap + (gap_rem > 0 ? 1 : 0);
      if (gap_rem > 0) --gap_rem;
    }
  }
}

void SpreadLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  const QMargins m = contentsMargins();
  const QRect inner = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
  int row_count = 0;
  for (const auto& row : rows_) {
    if (!row.items.isEmpty()) ++row_count;
  }
  if (row_count == 0) return;
  const QSize min = stacked_min_size();
  const int extra_h = std::max(0, rect.height() - min.height());
  const int slots = row_count + 2;
  const int pad = extra_h / slots;
  int rem = extra_h % slots;
  auto take = [&]() {
    const int n = pad + (rem > 0 ? 1 : 0);
    if (rem > 0) --rem;
    return n;
  };
  int y = inner.y() + take();
  int seen = 0;
  for (const auto& row : rows_) {
    if (row.items.isEmpty()) continue;
    const int row_h = row_min_size(row).height() + take();
    layout_row(row, QRect(inner.x(), y, inner.width(), row_h));
    y += row_h;
    ++seen;
    if (seen < row_count) y += kSpreadMinVGap;
  }
}

TwoColumnThreeGapLayout::TwoColumnThreeGapLayout(QWidget* parent) : QLayout(parent) {
  setContentsMargins(0, 0, 0, 0);
}

TwoColumnThreeGapLayout::~TwoColumnThreeGapLayout() {
  while (QLayoutItem* item = takeAt(0)) delete item;
}

void TwoColumnThreeGapLayout::addItem(QLayoutItem* item) {
  if (item != nullptr) items_.push_back(item);
}

void TwoColumnThreeGapLayout::addWidget(QWidget* widget) {
  if (widget == nullptr) return;
  addChildWidget(widget);
  items_.push_back(new QWidgetItem(widget));
}

int TwoColumnThreeGapLayout::count() const { return items_.size(); }

QLayoutItem* TwoColumnThreeGapLayout::itemAt(int index) const { return items_.value(index); }

QLayoutItem* TwoColumnThreeGapLayout::takeAt(int index) {
  if (index < 0 || index >= items_.size()) return nullptr;
  return items_.takeAt(index);
}

QSize TwoColumnThreeGapLayout::sizeHint() const {
  int col_w = 0;
  int col_h = 0;
  for (const QLayoutItem* item : items_) {
    col_w = std::max(col_w, item_hint_w(item));
    col_h = std::max(col_h, item_hint_h(item));
  }
  const QMargins m = contentsMargins();
  const int n = std::max(1, static_cast<int>(items_.size()));
  return { (n + 1) * kSpreadMinGap + n * col_w + m.left() + m.right(),
           col_h + m.top() + m.bottom() };
}

QSize TwoColumnThreeGapLayout::minimumSize() const { return sizeHint(); }

Qt::Orientations TwoColumnThreeGapLayout::expandingDirections() const {
  return Qt::Horizontal | Qt::Vertical;
}

void TwoColumnThreeGapLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  const QMargins m = contentsMargins();
  const QRect inner = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
  const int n = items_.size();
  if (n == 0) return;
  int col = 0;
  for (const QLayoutItem* item : items_) col = std::max(col, item_hint_w(item));
  const int gaps = n + 1;
  int rest = inner.width() - n * col;
  int gap = rest / gaps;
  int rem = rest % gaps;
  if (gap < kSpreadMinGap) {
    gap = kSpreadMinGap;
    col = std::max(0, (inner.width() - gaps * gap) / n);
    rem = inner.width() - n * col - gaps * gap;
  }
  auto take_gap = [&]() {
    const int g = gap + (rem > 0 ? 1 : 0);
    if (rem > 0) --rem;
    return g;
  };
  int x = inner.x() + take_gap();
  for (int i = 0; i < n; ++i) {
    items_[i]->setGeometry(QRect(x, inner.y(), col, inner.height()));
    x += col + take_gap();
  }
}

EvenGapStackLayout::EvenGapStackLayout(QWidget* parent) : QLayout(parent) {
  setContentsMargins(0, 0, 0, 0);
}

EvenGapStackLayout::~EvenGapStackLayout() {
  while (QLayoutItem* item = takeAt(0)) delete item;
}

void EvenGapStackLayout::addItem(QLayoutItem* item) {
  if (item != nullptr) items_.push_back(item);
}

void EvenGapStackLayout::addWidget(QWidget* widget) {
  if (widget == nullptr) return;
  addChildWidget(widget);
  items_.push_back(new QWidgetItem(widget));
}

int EvenGapStackLayout::count() const { return items_.size(); }

QLayoutItem* EvenGapStackLayout::itemAt(int index) const { return items_.value(index); }

QLayoutItem* EvenGapStackLayout::takeAt(int index) {
  if (index < 0 || index >= items_.size()) return nullptr;
  return items_.takeAt(index);
}

QSize EvenGapStackLayout::sizeHint() const {
  int w = 0;
  int h = 0;
  for (const QLayoutItem* item : items_) {
    w = std::max(w, item_hint_w(item));
    h += item_hint_h(item);
  }
  const QMargins m = contentsMargins();
  const int n = static_cast<int>(items_.size());
  const int gaps = n + 1;
  return {w + m.left() + m.right(), h + gaps * kSpreadMinGap + m.top() + m.bottom()};
}

QSize EvenGapStackLayout::minimumSize() const { return sizeHint(); }

Qt::Orientations EvenGapStackLayout::expandingDirections() const {
  return Qt::Horizontal | Qt::Vertical;
}

void EvenGapStackLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  const QMargins m = contentsMargins();
  const QRect inner = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
  const int n = items_.size();
  if (n == 0) return;
  int content_h = 0;
  for (const QLayoutItem* item : items_) content_h += item_hint_h(item);
  const int gaps = n + 1;
  int rest = inner.height() - content_h;
  int gap = rest / gaps;
  int rem = rest % gaps;
  if (gap < kSpreadMinGap) {
    gap = kSpreadMinGap;
    rem = 0;
  }
  auto take_gap = [&]() {
    const int g = gap + (rem > 0 ? 1 : 0);
    if (rem > 0) --rem;
    return g;
  };
  int y = inner.y() + take_gap();
  for (int i = 0; i < n; ++i) {
    const int h = item_hint_h(items_[i]);
    items_[i]->setGeometry(QRect(inner.x(), y, inner.width(), h));
    y += h + take_gap();
  }
}

FillScrollArea::FillScrollArea(QWidget* parent) : QScrollArea(parent) {
  setFrameShape(QFrame::NoFrame);
  setWidgetResizable(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize FillScrollArea::sizeHint() const {
  if (QWidget* w = widget()) {
    QSize s = w->minimumSizeHint();
    if (!s.isValid() || s.isEmpty()) s = w->sizeHint();
    const int fw = 2 * frameWidth();
    return s + QSize(fw, fw);
  }
  return {200, 72};
}

QSize FillScrollArea::minimumSizeHint() const {
  const int sb = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
  const int fw = 2 * frameWidth();
  return {sb + fw + 24, sb + fw + 24};
}

}  // namespace wds::ui

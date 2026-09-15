#include "wds/ui/qt/flow_layout.hpp"

#include "wds/ui/qt/spread_layout.hpp"

#include <QWidget>

#include <algorithm>
#include <vector>

namespace wds::ui {
namespace {

int item_hint_w(const QLayoutItem* item) {
  return std::max(item->minimumSize().width(), item->sizeHint().width());
}

int item_hint_h(const QLayoutItem* item) {
  return std::max(item->minimumSize().height(), item->sizeHint().height());
}

}  // namespace

FlowLayout::FlowLayout(QWidget* parent, int margin, int h_spacing, int v_spacing,
                       int h_spacing_max)
    : QLayout(parent), h_space_(h_spacing), v_space_(v_spacing), h_space_max_(h_spacing_max) {
  setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
  while (QLayoutItem* item = takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

int FlowLayout::horizontalSpacing() const {
  return h_space_ >= 0 ? h_space_ : smart_spacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const {
  return v_space_ >= 0 ? v_space_ : smart_spacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const { return items_.size(); }

QLayoutItem* FlowLayout::itemAt(int index) const { return items_.value(index); }

QLayoutItem* FlowLayout::takeAt(int index) {
  if (index >= 0 && index < items_.size()) return items_.takeAt(index);
  return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const {
  return Qt::Horizontal | Qt::Vertical;
}

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const {
  return do_layout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  do_layout(rect, false);
}

QSize FlowLayout::sizeHint() const {
  int w = 0;
  int h = 0;
  const int gap = std::max(0, horizontalSpacing());
  for (int i = 0; i < items_.size(); ++i) {
    if (i > 0) w += gap;
    w += item_hint_w(items_[i]);
    h = std::max(h, item_hint_h(items_[i]));
  }
  const QMargins margins = contentsMargins();
  return {w + margins.left() + margins.right(), h + margins.top() + margins.bottom()};
}

QSize FlowLayout::minimumSize() const {
  QSize size;
  for (const QLayoutItem* item : items_) {
    size = size.expandedTo(QSize(item_hint_w(item), item_hint_h(item)));
  }
  const QMargins margins = contentsMargins();
  size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
  return size;
}

int FlowLayout::do_layout(const QRect& rect, bool test_only) const {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  getContentsMargins(&left, &top, &right, &bottom);
  const QRect effective = rect.adjusted(left, top, -right, -bottom);
  const int min_gap = std::max(0, horizontalSpacing());
  const int max_gap = h_space_max_ >= 0 ? h_space_max_ : kSpreadMaxGap;
  const int space_y = std::max(0, verticalSpacing());

  struct Line {
    std::vector<QLayoutItem*> items;
    int hint_w = 0;
    int hint_h = 0;
  };
  std::vector<Line> lines;
  Line current;
  for (QLayoutItem* item : items_) {
    const int iw = item_hint_w(item);
    const int ih = item_hint_h(item);
    const int next_w = current.items.empty() ? iw : current.hint_w + min_gap + iw;
    if (!current.items.empty() && next_w > effective.width() && effective.width() > 0) {
      lines.push_back(current);
      current = {};
    }
    if (!current.items.empty()) current.hint_w += min_gap;
    current.items.push_back(item);
    current.hint_w += iw;
    current.hint_h = std::max(current.hint_h, ih);
  }
  if (!current.items.empty()) lines.push_back(current);

  int tight_h = top + bottom;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) tight_h += space_y;
    tight_h += lines[i].hint_h;
  }
  if (test_only) return tight_h;

  const int extra_h = std::max(0, rect.height() - tight_h);
  const int slots = static_cast<int>(lines.size()) + 2;
  const int pad = slots > 0 ? extra_h / slots : 0;
  int rem = slots > 0 ? extra_h % slots : 0;
  auto take = [&]() {
    const int n = pad + (rem > 0 ? 1 : 0);
    if (rem > 0) --rem;
    return n;
  };

  int y = effective.y() + take();
  for (std::size_t li = 0; li < lines.size(); ++li) {
    const Line& line = lines[li];
    const int line_h = line.hint_h + take();
    const int n = static_cast<int>(line.items.size());
    const int extra_w = std::max(0, effective.width() - line.hint_w);
    const int gaps = std::max(0, n - 1);
    const int gap_room = gaps * std::max(0, max_gap - min_gap);
    const int gap_extra = std::min(extra_w, gap_room);
    const int gap = gaps > 0 ? min_gap + gap_extra / gaps : min_gap;
    int gap_rem = gaps > 0 ? gap_extra % gaps : 0;
    const int used_w = line.hint_w + gap_extra;
    int x = effective.x() + std::max(0, (effective.width() - used_w) / 2);
    for (int i = 0; i < n; ++i) {
      const int w = item_hint_w(line.items[i]);
      const int h = item_hint_h(line.items[i]);
      const int iy = y + std::max(0, (line_h - h) / 2);
      line.items[i]->setGeometry(QRect(x, iy, w, h));
      x += w;
      if (i + 1 < n) {
        x += gap + (gap_rem > 0 ? 1 : 0);
        if (gap_rem > 0) --gap_rem;
      }
    }
    y += line_h;
    if (li + 1 < lines.size()) y += space_y;
  }
  return y + bottom - rect.y();
}

int FlowLayout::smart_spacing(QStyle::PixelMetric pm) const {
  QObject* object = parent();
  if (object == nullptr) return -1;
  if (object->isWidgetType()) {
    auto* widget = static_cast<QWidget*>(object);
    return widget->style()->pixelMetric(pm, nullptr, widget);
  }
  return static_cast<QLayout*>(object)->spacing();
}

}  // namespace wds::ui

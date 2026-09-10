#include "wds/ui/qt/flow_layout.hpp"

#include <QWidget>

namespace wds::ui {

FlowLayout::FlowLayout(QWidget* parent, int margin, int h_spacing, int v_spacing)
    : QLayout(parent), h_space_(h_spacing), v_space_(v_spacing) {
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

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const {
  return do_layout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  do_layout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const {
  QSize size;
  for (const QLayoutItem* item : items_) size = size.expandedTo(item->minimumSize());
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
  int x = effective.x();
  int y = effective.y();
  int line_height = 0;

  for (QLayoutItem* item : items_) {
    const QWidget* widget = item->widget();
    int space_x = horizontalSpacing();
    if (space_x == -1 && widget != nullptr) {
      space_x = widget->style()->layoutSpacing(QSizePolicy::PushButton, QSizePolicy::PushButton,
                                               Qt::Horizontal);
    }
    int space_y = verticalSpacing();
    if (space_y == -1 && widget != nullptr) {
      space_y = widget->style()->layoutSpacing(QSizePolicy::PushButton, QSizePolicy::PushButton,
                                               Qt::Vertical);
    }
    int next_x = x + item->sizeHint().width() + space_x;
    if (next_x - space_x > effective.right() && line_height > 0) {
      x = effective.x();
      y = y + line_height + space_y;
      next_x = x + item->sizeHint().width() + space_x;
      line_height = 0;
    }
    if (!test_only) item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
    x = next_x;
    line_height = qMax(line_height, item->sizeHint().height());
  }
  return y + line_height - rect.y() + bottom;
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

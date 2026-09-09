#pragma once

#include <QLayout>
#include <QStyle>

namespace wds::ui {

// Standard Qt flow layout (from the Qt layouts example): children wrap to the
// next row when the width runs out, so dock panels stay usable at any aspect.
class FlowLayout final : public QLayout {
 public:
  explicit FlowLayout(QWidget* parent = nullptr, int margin = -1, int h_spacing = -1,
                      int v_spacing = -1);
  ~FlowLayout() override;

  void addItem(QLayoutItem* item) override;
  int horizontalSpacing() const;
  int verticalSpacing() const;
  Qt::Orientations expandingDirections() const override;
  bool hasHeightForWidth() const override;
  int heightForWidth(int width) const override;
  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QSize minimumSize() const override;
  void setGeometry(const QRect& rect) override;
  QSize sizeHint() const override;
  QLayoutItem* takeAt(int index) override;

 private:
  int do_layout(const QRect& rect, bool test_only) const;
  int smart_spacing(QStyle::PixelMetric pm) const;

  QList<QLayoutItem*> items_;
  int h_space_;
  int v_space_;
};

}  // namespace wds::ui

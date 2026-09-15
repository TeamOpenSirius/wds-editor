#pragma once

#include <QLayout>
#include <QScrollArea>
#include <QVector>

namespace wds::ui {

constexpr int kSpreadMinGap = 16;
constexpr int kSpreadMaxGap = 36;
constexpr int kSpreadMinMargin = 16;
constexpr int kSpreadMinVMargin = 10;
constexpr int kSpreadMinVGap = 6;

// Vertical stack of rows. Extra height is split evenly across each row plus the
// top and bottom gaps. Two-item rows split into equal columns. Other rows grow
// inter-item gaps up to kSpreadMaxGap, then stretch widgets.
class SpreadLayout final : public QLayout {
 public:
  explicit SpreadLayout(QWidget* parent = nullptr);
  ~SpreadLayout() override;

  void addItem(QLayoutItem* item) override;
  void addWidget(QWidget* widget, int stretch = 0);
  void addRow();

  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QLayoutItem* takeAt(int index) override;
  QSize sizeHint() const override;
  QSize minimumSize() const override;
  void setGeometry(const QRect& rect) override;
  Qt::Orientations expandingDirections() const override;

 private:
  struct Item {
    QLayoutItem* layout_item = nullptr;
    int stretch = 0;
  };
  struct Row {
    QVector<Item> items;
  };

  QSize row_min_size(const Row& row) const;
  QSize stacked_min_size() const;
  void layout_row(const Row& row, const QRect& slot) const;

  QVector<Row> rows_;
};

// [gap][column][gap][column][gap]: columns share one width, gaps share one width.
class TwoColumnThreeGapLayout final : public QLayout {
 public:
  explicit TwoColumnThreeGapLayout(QWidget* parent = nullptr);
  ~TwoColumnThreeGapLayout() override;

  void addItem(QLayoutItem* item) override;
  void addWidget(QWidget* widget);

  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QLayoutItem* takeAt(int index) override;
  QSize sizeHint() const override;
  QSize minimumSize() const override;
  void setGeometry(const QRect& rect) override;
  Qt::Orientations expandingDirections() const override;

 private:
  QVector<QLayoutItem*> items_;
};

// [gap][row][gap][row]... : rows keep their height, leftover is split into equal gaps.
class EvenGapStackLayout final : public QLayout {
 public:
  explicit EvenGapStackLayout(QWidget* parent = nullptr);
  ~EvenGapStackLayout() override;

  void addItem(QLayoutItem* item) override;
  void addWidget(QWidget* widget);

  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QLayoutItem* takeAt(int index) override;
  QSize sizeHint() const override;
  QSize minimumSize() const override;
  void setGeometry(const QRect& rect) override;
  Qt::Orientations expandingDirections() const override;

 private:
  QVector<QLayoutItem*> items_;
};

// Scroll host whose sizeHint tracks the child, but minimumSizeHint stays small
// so a dock can shrink and show scrollbars instead of compressing controls.
class FillScrollArea final : public QScrollArea {
 public:
  explicit FillScrollArea(QWidget* parent = nullptr);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
};

}  // namespace wds::ui

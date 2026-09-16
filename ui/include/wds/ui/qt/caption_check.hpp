#pragma once

#include <QCheckBox>
#include <QString>
#include <QWidget>

class QEvent;
class QLabel;
class QShowEvent;

namespace wds::ui {

// Empty QCheckBox + QLabel. Aligns first-line ink (tightBoundingRect) to
// the 16px indicator center so Win/Mac line-box differences do not shift
// the caption.
class CaptionCheckRow final : public QWidget {
 public:
  CaptionCheckRow(const QString& text, QWidget* parent = nullptr, bool wrap = true);
  QCheckBox* box() const { return box_; }

  bool hasHeightForWidth() const override { return wrap_; }
  int heightForWidth(int w) const override;
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void sync();

 protected:
  void changeEvent(QEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  int box_width() const;
  int box_band_height() const;
  int text_width() const;
  int label_width_for(int row_w) const;
  void apply_optical();

  bool wrap_ = true;
  QWidget* box_host_ = nullptr;
  QCheckBox* box_ = nullptr;
  QLabel* label_ = nullptr;
};

void sync_caption_check_row(QWidget* row);

}  // namespace wds::ui

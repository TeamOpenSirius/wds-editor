#include "wds/ui/qt/caption_check.hpp"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMargins>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace wds::ui {
namespace {

constexpr int kCaptionGap = 6;
constexpr int kIndicatorSide = 16;

int caption_ink_center_from_top(const QFontMetrics& metrics, const QString& text) {
  QString sample = QStringLiteral("国");
  for (const QChar ch : text) {
    if (!ch.isSpace()) {
      sample = QString(ch);
      break;
    }
  }
  const QRect tight = metrics.tightBoundingRect(sample);
  if (tight.height() <= 0) return std::max(1, metrics.ascent() / 2);
  return metrics.ascent() + (tight.top() + tight.bottom()) / 2;
}

void apply_caption_optical_align(QCheckBox* box, QLabel* label) {
  if (box == nullptr || label == nullptr) return;
  const int side = std::max({box->minimumWidth(), box->minimumHeight(), kIndicatorSide});
  const int ink = caption_ink_center_from_top(label->fontMetrics(), label->text());
  const int delta = side / 2 - ink;
  QWidget* host = box->parentWidget();
  if (host != nullptr && !host->property("wdsBoxHost").toBool()) host = nullptr;
  if (delta >= 0) {
    label->setContentsMargins(0, delta, 0, 0);
    if (host != nullptr) {
      if (QLayout* lay = host->layout()) lay->setContentsMargins(0, 0, 0, 0);
      host->updateGeometry();
    }
  } else {
    label->setContentsMargins(0, 0, 0, 0);
    if (host != nullptr) {
      if (QLayout* lay = host->layout()) lay->setContentsMargins(0, -delta, 0, 0);
      host->updateGeometry();
    }
  }
}

class LabelToggleFilter final : public QObject {
 public:
  LabelToggleFilter(CaptionCheckRow* row, QCheckBox* box, QObject* parent)
      : QObject(parent), row_(row), box_(box) {}

 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::MouseButtonRelease && box_ != nullptr) {
      box_->toggle();
      return true;
    }
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        row_ != nullptr) {
      row_->sync();
    }
    return false;
  }

 private:
  CaptionCheckRow* row_ = nullptr;
  QCheckBox* box_ = nullptr;
};

int wrap_text_height(const QFont& font, const QString& text, int width, const QMargins& margins) {
  const QFontMetrics metrics(font);
  const int inner = std::max(1, width - margins.left() - margins.right());
  const QRect bounds = metrics.boundingRect(QRect(0, 0, inner, 100000),
                                            Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, text);
  return std::max(metrics.height(), bounds.height()) + margins.top() + margins.bottom();
}

int caption_text_width(const QFontMetrics& metrics, const QString& text) {
  return std::max(metrics.horizontalAdvance(text), metrics.boundingRect(text).width());
}

void pin_height(QWidget* widget, int height) {
  if (widget == nullptr || height <= 0) return;
  if (widget->minimumHeight() != height || widget->maximumHeight() != height) {
    widget->setFixedHeight(height);
  }
}

}  // namespace

void sync_caption_check_row(QWidget* row) {
  if (row == nullptr || !row->property("wdsWrapRow").toBool() || row->width() <= 0) return;
  auto* box = row->findChild<QCheckBox*>();
  auto* label = row->findChild<QLabel*>();
  if (box == nullptr || label == nullptr) return;
  apply_caption_optical_align(box, label);
  const QMargins margins = row->contentsMargins();
  const int box_w = std::max(box->minimumWidth(), kIndicatorSide);
  const int label_w = label->width() > 0
                          ? label->width()
                          : std::max(1, row->width() - box_w - kCaptionGap - margins.left() -
                                            margins.right());
  const int text_h = wrap_text_height(label->font(), label->text(), label_w, label->contentsMargins());
  pin_height(label, text_h);
  int box_h = box->minimumHeight();
  if (QWidget* host = box->parentWidget(); host != nullptr && host->property("wdsBoxHost").toBool()) {
    box_h = std::max(box_h, host->sizeHint().height());
  }
  pin_height(row, std::max(box_h, text_h));
}

CaptionCheckRow::CaptionCheckRow(const QString& text, QWidget* parent, bool wrap)
    : QWidget(parent), wrap_(wrap) {
  setProperty("wdsWrapRow", wrap);
  setSizePolicy(wrap ? QSizePolicy::Preferred : QSizePolicy::Minimum, QSizePolicy::Maximum);
  box_host_ = new QWidget(this);
  box_host_->setProperty("wdsBoxHost", true);
  box_host_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Maximum);
  box_ = new QCheckBox(box_host_);
  box_->setObjectName(QStringLiteral("captionCheck"));
  box_->setText(QString());
  box_->setAccessibleName(text);
  const int side =
      std::max({box_->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, box_),
                box_->style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, box_), kIndicatorSide});
  box_->setFixedSize(side, side);
  box_host_->setFixedWidth(side);
  auto* box_lay = new QVBoxLayout(box_host_);
  box_lay->setContentsMargins(0, 0, 0, 0);
  box_lay->setSpacing(0);
  box_lay->addWidget(box_, 0, Qt::AlignTop);
  label_ = new QLabel(text, this);
  label_->setWordWrap(wrap);
  label_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  label_->setMargin(0);
  label_->setIndent(0);
  label_->setSizePolicy(wrap ? QSizePolicy::Expanding : QSizePolicy::Fixed, QSizePolicy::Fixed);
  label_->setCursor(Qt::PointingHandCursor);
  label_->installEventFilter(new LabelToggleFilter(this, box_, label_));
  if (!wrap) label_->setFixedWidth(text_width());
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(kCaptionGap);
  row->addWidget(box_host_, 0, Qt::AlignTop);
  row->addWidget(label_, wrap ? 1 : 0, Qt::AlignTop);
  label_->ensurePolished();
  apply_optical();
}

void CaptionCheckRow::apply_optical() { apply_caption_optical_align(box_, label_); }

int CaptionCheckRow::box_width() const { return std::max(box_->minimumWidth(), kIndicatorSide); }

int CaptionCheckRow::box_band_height() const {
  return box_host_ != nullptr ? std::max(box_->minimumHeight(), box_host_->sizeHint().height())
                              : box_->minimumHeight();
}

int CaptionCheckRow::text_width() const {
  return caption_text_width(label_->fontMetrics(), label_->text());
}

int CaptionCheckRow::label_width_for(int row_w) const {
  const QMargins margins = contentsMargins();
  return std::max(1, row_w - box_width() - kCaptionGap - margins.left() - margins.right());
}

int CaptionCheckRow::heightForWidth(int w) const {
  if (!wrap_) return sizeHint().height();
  return std::max(box_band_height(), wrap_text_height(label_->font(), label_->text(),
                                                      label_width_for(w), label_->contentsMargins()));
}

QSize CaptionCheckRow::sizeHint() const {
  const int w = box_width() + kCaptionGap + text_width();
  const QMargins pad = label_->contentsMargins();
  const int h = wrap_ && width() > 0
                    ? heightForWidth(width())
                    : std::max(box_band_height(),
                               label_->fontMetrics().height() + pad.top() + pad.bottom());
  return QSize(w, h);
}

QSize CaptionCheckRow::minimumSizeHint() const {
  if (!wrap_) return sizeHint();
  return QSize(box_width() + kCaptionGap + label_->fontMetrics().averageCharWidth(),
               std::max(box_band_height(), label_->fontMetrics().height()));
}

void CaptionCheckRow::sync() {
  apply_optical();
  if (!wrap_) label_->setFixedWidth(text_width());
  sync_caption_check_row(this);
}

void CaptionCheckRow::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) sync();
}

void CaptionCheckRow::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  sync();
}

void CaptionCheckRow::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  sync();
}

}  // namespace wds::ui

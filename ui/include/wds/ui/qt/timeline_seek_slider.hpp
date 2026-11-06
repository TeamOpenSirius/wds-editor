#pragma once

#include <wds/interaction/editor_input.hpp>

#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOption>
#include <QWheelEvent>

#include <algorithm>

namespace wds::ui {

// Horizontal timeline scrubber (the playback progress bar). The groove is thin,
// so QSlider only starts a drag on the handle; any press on the control jumps to
// that x and keeps tracking, matching the playback seek bar.
//
// The wheel follows 「反转时间轴滚轮方向」 (invert_scroll_wheel): that is a
// timeline-scrub setting, not an edit-canvas one, so the progress bar must move
// in the same direction as the playfield. The flip goes through Qt's own
// invertedControls contract and is scoped to the wheel event, so the slider's
// arrow / page keys keep their normal meaning.
class TimelineSeekSlider final : public QSlider {
 public:
  using QSlider::QSlider;

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton) {
      QSlider::mousePressEvent(event);
      return;
    }
    seek_drag_ = true;
    setSliderDown(true);
    setValue(value_from_pos(event->position().toPoint()));
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (!seek_drag_) {
      QSlider::mouseMoveEvent(event);
      return;
    }
    setValue(value_from_pos(event->position().toPoint()));
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (!seek_drag_) {
      QSlider::mouseReleaseEvent(event);
      return;
    }
    setValue(value_from_pos(event->position().toPoint()));
    seek_drag_ = false;
    setSliderDown(false);
    event->accept();
  }

  void wheelEvent(QWheelEvent* event) override {
    if (!wds::interaction::invert_scroll_wheel()) {
      QSlider::wheelEvent(event);
      return;
    }
    const bool was_inverted = invertedControls();
    setInvertedControls(true);
    QSlider::wheelEvent(event);
    setInvertedControls(was_inverted);
  }

 private:
  int value_from_pos(const QPoint& pos) const {
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
    const int slider_length = std::max(1, handle.width());
    const int slider_min = groove.isValid() ? groove.x() : 0;
    const int slider_max = groove.isValid() ? groove.right() - slider_length + 1
                                            : std::max(0, width() - slider_length);
    const int span = std::max(1, slider_max - slider_min);
    return QStyle::sliderValueFromPosition(minimum(), maximum(), pos.x() - slider_min, span,
                                           opt.upsideDown);
  }

  bool seek_drag_ = false;
};

}  // namespace wds::ui

// The playback progress bar is a timeline scrubber, so 「反转时间轴滚轮方向」
// (invert_scroll_wheel) must flip its wheel direction the same way it flips the
// edit playfield. Keyboard stepping is not a wheel gesture and must keep its
// normal direction.
#include "wds/ui/qt/timeline_seek_slider.hpp"

#include <wds/interaction/editor_input.hpp>

#include <QApplication>
#include <QKeyEvent>
#include <QSlider>
#include <QWheelEvent>

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool cond, const char* expr, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
    ++failures;
  }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

// Returns the value change caused by one wheel event on the slider.
int send_wheel(QSlider& slider, int delta_y) {
  const int before = slider.value();
  QWheelEvent event(QPointF(5, 5), QPointF(5, 5), QPoint(0, 0), QPoint(0, delta_y), Qt::NoButton,
                    Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&slider, &event);
  return slider.value() - before;
}

int send_key(QSlider& slider, int key) {
  const int before = slider.value();
  QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
  QApplication::sendEvent(&slider, &event);
  return slider.value() - before;
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);

  wds::ui::TimelineSeekSlider slider(Qt::Horizontal);
  slider.setRange(0, 100000);
  slider.setSingleStep(1000);
  slider.setValue(50000);

  // Default: wheel up scrubs forward, wheel down rewinds.
  wds::interaction::set_invert_scroll_wheel(false);
  CHECK(send_wheel(slider, 120) > 0);
  CHECK(send_wheel(slider, -120) < 0);

  // 「反转时间轴滚轮方向」 flips the progress bar exactly like the edit area.
  wds::interaction::set_invert_scroll_wheel(true);
  CHECK(send_wheel(slider, 120) < 0);
  CHECK(send_wheel(slider, -120) > 0);

  // ... and only the wheel: arrow keys keep their direction while inverted.
  CHECK(send_key(slider, Qt::Key_Right) > 0);
  CHECK(send_key(slider, Qt::Key_Left) < 0);

  // The setting is read per event, so turning it back off takes effect at once.
  wds::interaction::set_invert_scroll_wheel(false);
  CHECK(send_wheel(slider, 120) > 0);

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("timeline_seek_slider tests passed.\n");
  return 0;
}

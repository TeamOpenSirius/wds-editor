// Value inputs must not take keyboard focus when the mouse only manipulates their
// value: picking from a dropdown menu or stepping the up/down arrows of a spin
// box has to leave focus on the canvas, or Space (the app-level playback
// shortcut) is routed to the widget instead of toggling playback. Qt only does
// that for non-editable QComboBox on macOS; Windows / Linux leave QComboBox and
// QAbstractSpinBox on WheelFocus.
//
// deliver_with_value_input_click_focus() relaxes ClickFocus for exactly those
// arrow / stepper presses around QApplication::notify — and only there, so the
// text area still focuses its embedded QLineEdit for typing.
#include "wds/ui/qt/value_input_focus.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QVBoxLayout>
#include <QWidget>

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

QPoint combo_sub_center(QComboBox* combo, QStyle::SubControl sub) {
  QStyleOptionComboBox option;
  option.initFrom(combo);
  option.rect = combo->rect();
  option.editable = combo->isEditable();
  option.currentText = combo->currentText();
  option.frame = combo->hasFrame();
  return combo->style()->subControlRect(QStyle::CC_ComboBox, &option, sub, combo).center();
}

QPoint spin_sub_center(QAbstractSpinBox* spin, QStyle::SubControl sub) {
  QStyleOptionSpinBox option;
  option.initFrom(spin);
  option.rect = spin->rect();
  option.buttonSymbols = spin->buttonSymbols();
  option.frame = spin->hasFrame();
  return spin->style()->subControlRect(QStyle::CC_SpinBox, &option, sub, spin).center();
}

// Runs the delivery wrapper and reports the focus policy Qt's click-focus logic
// would have seen (i.e. the policy inside `deliver`), plus the restored policy.
struct RelaxResult {
  Qt::FocusPolicy inside = Qt::NoFocus;
  bool delivered = false;
};

RelaxResult deliver(QWidget* receiver, const QPoint& pos, QEvent::Type type = QEvent::MouseButtonPress) {
  QMouseEvent event(type, QPointF(pos), receiver->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  RelaxResult out;
  out.delivered = wds::ui::deliver_with_value_input_click_focus(receiver, &event, [&] {
    out.inside = receiver->focusPolicy();
    return true;
  });
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);

  QWidget window;
  auto* column = new QVBoxLayout(&window);
  auto* combo = new QComboBox(&window);
  combo->addItems({"A", "B", "C"});
  auto* editable = new QComboBox(&window);
  editable->setEditable(true);
  editable->addItems({"A", "B", "C"});
  auto* spin = new QSpinBox(&window);
  spin->setRange(0, 100);
  auto* dspin = new QDoubleSpinBox(&window);
  dspin->setRange(0.0, 100.0);
  auto* no_buttons = new QSpinBox(&window);
  no_buttons->setButtonSymbols(QAbstractSpinBox::NoButtons);
  auto* plain = new QLineEdit(&window);
  column->addWidget(combo);
  column->addWidget(editable);
  column->addWidget(spin);
  column->addWidget(dspin);
  column->addWidget(no_buttons);
  column->addWidget(plain);
  window.show();
  app.processEvents();

  // Windows / Linux defaults: click focus on the value input.
  for (QWidget* input : {static_cast<QWidget*>(combo), static_cast<QWidget*>(editable),
                         static_cast<QWidget*>(spin), static_cast<QWidget*>(dspin)}) {
    input->setFocusPolicy(Qt::WheelFocus);
  }
  CHECK(combo->focusPolicy() == Qt::WheelFocus);

  // --- which presses only manipulate the value? ---
  CHECK(wds::ui::is_value_input_arrow_press(combo, combo_sub_center(combo, QStyle::SC_ComboBoxArrow)));
  CHECK(wds::ui::is_value_input_arrow_press(combo,
                                            combo_sub_center(combo, QStyle::SC_ComboBoxEditField)));
  CHECK(wds::ui::is_value_input_arrow_press(
      editable, combo_sub_center(editable, QStyle::SC_ComboBoxArrow)));
  CHECK(!wds::ui::is_value_input_arrow_press(
      editable, combo_sub_center(editable, QStyle::SC_ComboBoxEditField)));
  CHECK(wds::ui::is_value_input_arrow_press(spin, spin_sub_center(spin, QStyle::SC_SpinBoxUp)));
  CHECK(wds::ui::is_value_input_arrow_press(spin, spin_sub_center(spin, QStyle::SC_SpinBoxDown)));
  CHECK(!wds::ui::is_value_input_arrow_press(spin, spin_sub_center(spin, QStyle::SC_SpinBoxEditField)));
  CHECK(!wds::ui::is_value_input_arrow_press(no_buttons, QPoint(4, 4)));
  CHECK(!wds::ui::is_value_input_arrow_press(plain, plain->rect().center()));

  // --- arrow / stepper presses: Qt sees TabFocus, then the policy is restored ---
  {
    const auto r = deliver(spin, spin_sub_center(spin, QStyle::SC_SpinBoxUp));
    CHECK(r.delivered);
    CHECK(r.inside == Qt::TabFocus);
    CHECK(spin->focusPolicy() == Qt::WheelFocus);
  }
  {
    const auto r = deliver(editable, combo_sub_center(editable, QStyle::SC_ComboBoxArrow));
    CHECK(r.inside == Qt::TabFocus);
    CHECK(editable->focusPolicy() == Qt::WheelFocus);
  }
  {
    // Non-editable combos behave like buttons on macOS: the whole field is
    // value-only, even the label area.
    const auto r = deliver(combo, combo->rect().center());
    CHECK(r.inside == Qt::TabFocus);
    CHECK(combo->focusPolicy() == Qt::WheelFocus);
  }
  {
    // Double clicks focus too in Qt, so they must be relaxed as well.
    const auto r = deliver(spin, spin_sub_center(spin, QStyle::SC_SpinBoxDown),
                           QEvent::MouseButtonDblClick);
    CHECK(r.inside == Qt::TabFocus);
  }

  // --- text areas keep click focus so typing still works ---
  {
    const auto r = deliver(editable, combo_sub_center(editable, QStyle::SC_ComboBoxEditField));
    CHECK(r.inside == Qt::WheelFocus);
    CHECK(editable->focusPolicy() == Qt::WheelFocus);
  }
  {
    const auto r = deliver(spin, spin_sub_center(spin, QStyle::SC_SpinBoxEditField));
    CHECK(r.inside == Qt::WheelFocus);
  }
  {
    const auto r = deliver(plain, plain->rect().center());
    CHECK(r.inside == plain->focusPolicy());
  }

  // Embedded editors are separate children with their own click focus; the
  // wrapper never touches them, so clicking the text focuses the field.
  CHECK(editable->lineEdit() != nullptr);
  CHECK((editable->lineEdit()->focusPolicy() & Qt::ClickFocus) != 0);
  auto* spin_edit = spin->findChild<QLineEdit*>();
  CHECK(spin_edit != nullptr);
  CHECK((spin_edit->focusPolicy() & Qt::ClickFocus) != 0);

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("value_input_focus tests passed.\n");
  return 0;
}

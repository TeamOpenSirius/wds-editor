#pragma once

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionSpinBox>
#include <QWidget>

#include <utility>

namespace wds::ui {

// Value inputs (dropdowns and the spin boxes with up/down arrows) must not take
// keyboard focus when the mouse only manipulates their value. Qt gives QComboBox
// and QAbstractSpinBox Qt::WheelFocus on Windows / Linux, so a picked dropdown or
// a stepped spinner holds focus afterwards; because the app treats those widgets
// as typing fields, Space — the app-level playback shortcut — is delivered to the
// widget instead of toggling playback, and the user had to click the canvas
// first. Qt only avoids this for non-editable combos on macOS
// (QComboBoxPrivate::init: "a combobox looks like a button").
//
// True when a press at `pos` only manipulates the value: the whole non-editable
// combo (Qt turns it into a button), an editable combo's popup arrow, or a spin
// box's up/down buttons. The text area is deliberately excluded — clicking it
// still focuses the embedded QLineEdit so the user can type.
inline bool is_value_input_arrow_press(QWidget* widget, const QPoint& pos) {
  if (auto* combo = qobject_cast<QComboBox*>(widget)) {
    if (!combo->isEditable()) return true;
    QStyleOptionComboBox option;
    option.initFrom(combo);
    option.rect = combo->rect();
    option.editable = true;
    option.currentText = combo->currentText();
    option.frame = combo->hasFrame();
    const QRect arrow = combo->style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                       QStyle::SC_ComboBoxArrow, combo);
    return arrow.contains(pos);
  }
  if (auto* spin = qobject_cast<QAbstractSpinBox*>(widget)) {
    if (spin->buttonSymbols() == QAbstractSpinBox::NoButtons) return false;
    QStyleOptionSpinBox option;
    option.initFrom(spin);
    option.rect = spin->rect();
    option.buttonSymbols = spin->buttonSymbols();
    option.frame = spin->hasFrame();
    for (const QStyle::SubControl sub : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
      if (spin->style()->subControlRect(QStyle::CC_SpinBox, &option, sub, spin).contains(pos)) {
        return true;
      }
    }
  }
  return false;
}

// Delivers one event with ClickFocus temporarily removed when it is a press on a
// value input's arrow / stepper region. Qt applies click focus inside
// QApplication::notify *before* any event filter runs, so the relaxation has to
// wrap that call: QApplicationPrivate::giveFocusAccordingToFocusPolicy() then
// sees Qt::TabFocus and leaves keyboard focus on the canvas. The original policy
// is restored as soon as delivery ends, so Tab navigation, keyboard input and
// the embedded line edit's own click focus are unaffected.
template <typename Deliver>
bool deliver_with_value_input_click_focus(QObject* receiver, QEvent* event, Deliver&& deliver) {
  QPointer<QWidget> relaxed;
  Qt::FocusPolicy saved = Qt::NoFocus;
  if (event != nullptr && (event->type() == QEvent::MouseButtonPress ||
                           event->type() == QEvent::MouseButtonDblClick)) {
    if (auto* widget = qobject_cast<QWidget*>(receiver)) {
      const QPoint pos = static_cast<QMouseEvent*>(event)->position().toPoint();
      if (is_value_input_arrow_press(widget, pos)) {
        const Qt::FocusPolicy policy = widget->focusPolicy();
        if (policy != Qt::NoFocus && policy != Qt::TabFocus) {
          relaxed = widget;
          saved = policy;
          widget->setFocusPolicy(Qt::TabFocus);
        }
      }
    }
  }
  const bool result = deliver();
  // The delivered press may have destroyed the widget (e.g. it closed a dialog).
  if (!relaxed.isNull()) relaxed->setFocusPolicy(saved);
  return result;
}

}  // namespace wds::ui

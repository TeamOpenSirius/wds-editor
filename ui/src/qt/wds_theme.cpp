#include "wds/ui/qt/wds_theme.hpp"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace wds::ui {
namespace {

// Palette pulled from the game's stage backdrop (skins/ingame_bg.png):
// indigo night sky, violet curtains, pink tassels, cyan spotlights.
constexpr const char* kWindow = "#171930";
constexpr const char* kPanel = "#1e2142";
constexpr const char* kField = "#141631";
constexpr const char* kBorder = "#34386b";
constexpr const char* kText = "#e6e8ff";
constexpr const char* kTextMuted = "#9aa0cc";
constexpr const char* kViolet = "#8f6fff";
constexpr const char* kPink = "#ff5fa8";
constexpr const char* kCyan = "#55d6ff";

QString build_qss() {
  return QString(R"QSS(
QMainWindow, QDialog { background: %WINDOW%; }
QWidget { color: %TEXT%; }
QWidget:disabled { color: #5d6188; }

/* --- Menus / status --- */
QMenuBar {
  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #23264e, stop:1 #1a1c3a);
  border-bottom: 1px solid %BORDER%;
  padding: 1px 4px;
}
QMenuBar::item { background: transparent; padding: 4px 10px; border-radius: 5px; }
QMenuBar::item:selected { background: %VIOLET%; color: white; }
QMenu {
  background: %PANEL%;
  border: 1px solid %BORDER%;
  border-radius: 8px;
  padding: 4px;
}
QMenu::item { padding: 5px 26px 5px 14px; border-radius: 5px; }
QMenu::item:selected {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %VIOLET%, stop:1 %PINK%);
  color: white;
}
QMenu::separator { height: 1px; background: %BORDER%; margin: 4px 8px; }
QStatusBar {
  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1a1c3a, stop:1 #14152c);
  border-top: 1px solid %BORDER%;
  color: %MUTED%;
}

/* --- Toolbar --- */
QToolBar {
  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #262a56, stop:1 #1c1f3f);
  border: none;
  border-bottom: 1px solid %BORDER%;
  padding: 3px 6px;
  spacing: 4px;
}
QToolBar QToolButton {
  background: transparent;
  border: 1px solid transparent;
  border-radius: 6px;
  padding: 4px 10px;
}
QToolBar QToolButton:hover { background: #2f3364; border-color: %BORDER%; }
QToolBar QToolButton:pressed { background: %VIOLET%; color: white; }

/* --- Docks --- */
QDockWidget {
  titlebar-close-icon: none;
  titlebar-normal-icon: none;
}
QDockWidget::title {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2b2f61, stop:0.6 #232650, stop:1 #1d2040);
  border-top: 2px solid %VIOLET%;
  padding: 5px 10px;
  font-weight: bold;
}
QDockWidget::close-button, QDockWidget::float-button {
  background: transparent; border: none; padding: 2px;
}
QDockWidget::close-button:hover, QDockWidget::float-button:hover {
  background: %PINK%; border-radius: 4px;
}
QMainWindow::separator { background: #121430; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: %VIOLET%; }

/* --- Buttons --- */
QPushButton {
  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #303467, stop:1 #262a54);
  border: 1px solid %BORDER%;
  border-radius: 7px;
  padding: 5px 14px;
}
QPushButton:hover { border-color: %VIOLET%; background: #363b76; }
QPushButton:pressed {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %VIOLET%, stop:1 %PINK%);
  color: white;
}
QPushButton:disabled { background: #22243f; border-color: #2b2d4e; }
QToolButton {
  background: #262a54;
  border: 1px solid %BORDER%;
  border-radius: 7px;
  padding: 3px;
}
QToolButton:hover { border-color: %CYAN%; background: #2f3364; }
QToolButton:checked {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 %VIOLET%, stop:1 %PINK%);
  border-color: %PINK%;
  color: white;
}

/* --- Inputs --- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QKeySequenceEdit {
  background: %FIELD%;
  border: 1px solid %BORDER%;
  border-radius: 6px;
  padding: 3px 6px;
  selection-background-color: %VIOLET%;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus,
QKeySequenceEdit:focus { border-color: %CYAN%; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
  background: %PANEL%;
  border: 1px solid %BORDER%;
  border-radius: 6px;
  selection-background-color: %VIOLET%;
}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
  background: #2c2f5c; border: none; width: 16px;
}
QSpinBox::up-button:hover, QSpinBox::down-button:hover,
QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: %VIOLET%; }

/* --- Checkbox --- */
QCheckBox { spacing: 6px; }
QCheckBox::indicator {
  width: 15px; height: 15px;
  border: 1px solid %BORDER%;
  border-radius: 4px;
  background: %FIELD%;
}
QCheckBox::indicator:hover { border-color: %CYAN%; }
QCheckBox::indicator:checked {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 %VIOLET%, stop:1 %PINK%);
  border-color: %PINK%;
}

/* --- Slider (seek bar: spotlight handle, violet-pink progress) --- */
QSlider::groove:horizontal {
  height: 6px; border-radius: 3px; background: #23264a;
}
QSlider::sub-page:horizontal {
  border-radius: 3px;
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %VIOLET%, stop:1 %PINK%);
}
QSlider::handle:horizontal {
  width: 14px; height: 14px; margin: -5px 0;
  border-radius: 7px;
  background: qradialgradient(cx:0.5, cy:0.5, radius:0.7, stop:0 white, stop:0.6 %CYAN%, stop:1 #2a7fb8);
  border: 1px solid %CYAN%;
}

/* --- Scrollbars --- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical {
  background: #3a3e74; border-radius: 5px; min-height: 24px;
}
QScrollBar::handle:vertical:hover { background: %VIOLET%; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal {
  background: #3a3e74; border-radius: 5px; min-width: 24px;
}
QScrollBar::handle:horizontal:hover { background: %VIOLET%; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QAbstractScrollArea::corner { background: transparent; }

/* --- Lists / tabs (tabbed docks, settings sidebar) --- */
QListWidget {
  background: %FIELD%;
  border: 1px solid %BORDER%;
  border-radius: 8px;
  padding: 3px;
}
QListWidget::item { padding: 6px 10px; border-radius: 5px; }
QListWidget::item:selected {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %VIOLET%, stop:1 %PINK%);
  color: white;
}
QTabBar::tab {
  background: #22254a;
  border: 1px solid %BORDER%;
  border-bottom: none;
  border-top-left-radius: 6px;
  border-top-right-radius: 6px;
  padding: 4px 12px;
}
QTabBar::tab:selected { background: %PANEL%; border-top: 2px solid %PINK%; }
QToolTip {
  background: %PANEL%;
  color: %TEXT%;
  border: 1px solid %VIOLET%;
  border-radius: 5px;
  padding: 4px 7px;
}
QLabel { background: transparent; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
)QSS");
}

}  // namespace

void apply_wds_theme(QApplication& app) {
  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QPalette palette;
  palette.setColor(QPalette::Window, QColor(kWindow));
  palette.setColor(QPalette::WindowText, QColor(kText));
  palette.setColor(QPalette::Base, QColor(kField));
  palette.setColor(QPalette::AlternateBase, QColor(kPanel));
  palette.setColor(QPalette::Text, QColor(kText));
  palette.setColor(QPalette::Button, QColor(kPanel));
  palette.setColor(QPalette::ButtonText, QColor(kText));
  palette.setColor(QPalette::ToolTipBase, QColor(kPanel));
  palette.setColor(QPalette::ToolTipText, QColor(kText));
  palette.setColor(QPalette::Highlight, QColor(kViolet));
  palette.setColor(QPalette::HighlightedText, Qt::white);
  palette.setColor(QPalette::Link, QColor(kCyan));
  palette.setColor(QPalette::PlaceholderText, QColor(kTextMuted));
  palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#5d6188"));
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#5d6188"));
  palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#5d6188"));
  app.setPalette(palette);

  QString qss = build_qss();
  qss.replace(QStringLiteral("%WINDOW%"), QString::fromLatin1(kWindow));
  qss.replace(QStringLiteral("%PANEL%"), QString::fromLatin1(kPanel));
  qss.replace(QStringLiteral("%FIELD%"), QString::fromLatin1(kField));
  qss.replace(QStringLiteral("%BORDER%"), QString::fromLatin1(kBorder));
  qss.replace(QStringLiteral("%TEXT%"), QString::fromLatin1(kText));
  qss.replace(QStringLiteral("%MUTED%"), QString::fromLatin1(kTextMuted));
  qss.replace(QStringLiteral("%VIOLET%"), QString::fromLatin1(kViolet));
  qss.replace(QStringLiteral("%PINK%"), QString::fromLatin1(kPink));
  qss.replace(QStringLiteral("%CYAN%"), QString::fromLatin1(kCyan));
  app.setStyleSheet(qss);
}

}  // namespace wds::ui

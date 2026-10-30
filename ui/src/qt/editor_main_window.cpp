#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/export_video_dialog.hpp"
#include "wds/ui/qt/stem_panel.hpp"
#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/playback_dock.hpp"
#include "wds/ui/qt/qt_input_adapter.hpp"
#include "wds/ui/qt/settings_dialog.hpp"
#include "wds/ui/qt/curve_templates_panel.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/qt/about_dialog.hpp"
#include "wds/ui/qt/busy_dialog.hpp"
#include <wds/interaction/editor_shortcuts.hpp>
#include <wds/interaction/platform.hpp>
#include <wds/common/time.hpp>
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/resource_paths.hpp"
#include "wds/ui/layout/editor_layout.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/core/chart_editor_engine.hpp"
#include <QCloseEvent>
#include <QEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QDockWidget>
#include <QList>
#include <QPushButton>
#include <QMenu>
#include <QToolBar>
#include <QToolButton>
#include <QLabel>
#include <QEnterEvent>
#include <QCursor>
#include <QKeySequence>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPalette>
#include <QColor>
#include <QSizePolicy>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLayout>
#include <QKeyEvent>
#include <QAbstractSpinBox>
#include <QAbstractItemView>
#include <QKeySequenceEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStyle>
#include <QStyleOptionDockWidget>
#include <QProgressBar>
#include <QMetaObject>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <wds/interaction/editor_input.hpp>
#include <wds/common/crash_handler.hpp>
#include <wds/common/log.hpp>
#include "wds/ui/curve_template.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

// No Ctrl/Alt/Meta (Shift and keypad are ok): letters, digits, punctuation,
// space, and field-edit keys. Used so typing "1"/"Q" cannot become a shortcut.
bool is_plain_text_or_field_edit_key(const QKeyEvent* event) {
  if (event == nullptr) return false;
  const auto chord_mods =
      event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
  if (chord_mods != Qt::NoModifier) return false;

  if (wds::interaction::is_text_field_consumed_key(wds::ui::qt_key_code(event->key()))) {
    return true;
  }

  const QString text = event->text();
  return !text.isEmpty() && text.at(0).isPrint();
}

bool matches_standard_edit_chord(const QKeyEvent* event) {
  return event != nullptr &&
         (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut) ||
          event->matches(QKeySequence::Paste) || event->matches(QKeySequence::Undo) ||
          event->matches(QKeySequence::Redo) || event->matches(QKeySequence::SelectAll));
}

bool focus_records_shortcut(QWidget* focus) {
  for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
    if (qobject_cast<QKeySequenceEdit*>(widget) != nullptr) return true;
  }
  return false;
}

bool focus_is_typing_field(QWidget* focus) {
  if (focus == nullptr) return false;
  if (qobject_cast<QLineEdit*>(focus) != nullptr ||
      qobject_cast<QAbstractSpinBox*>(focus) != nullptr ||
      qobject_cast<QComboBox*>(focus) != nullptr ||
      qobject_cast<QTextEdit*>(focus) != nullptr ||
      qobject_cast<QPlainTextEdit*>(focus) != nullptr ||
      qobject_cast<QKeySequenceEdit*>(focus) != nullptr) {
    return true;
  }
  if (auto* edit = dynamic_cast<wds::ui::ChartEditWidget*>(focus)) return edit->captures_keys();
  return false;
}

// Trailing command-bar items to hide so the rest plus an overflow control fit.
std::size_t command_overflow_hide_count(const std::vector<int>& item_widths, int overflow_w,
                                        int budget) {
  int total = 0;
  for (int width : item_widths) total += std::max(0, width);
  if (total <= budget) return 0;
  int used = total;
  std::size_t hide = 0;
  const int room = budget - std::max(0, overflow_w);
  while (hide < item_widths.size() && used > room) {
    used -= std::max(0, item_widths[item_widths.size() - 1 - hide]);
    ++hide;
  }
  return hide;
}

class HoverOverflowAffordance final : public QLabel {
 public:
  explicit HoverOverflowAffordance(QWidget* parent) : QLabel(parent) {
    setFocusPolicy(Qt::NoFocus);
    setAlignment(Qt::AlignCenter);
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setContentsMargins(0, 0, 0, 0);
    setMargin(0);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setStyleSheet(QStringLiteral("QLabel { border: none; background: transparent; padding: 0px; }"));
    hide_timer_.setSingleShot(true);
    hide_timer_.setInterval(160);
    connect(&hide_timer_, &QTimer::timeout, this, [this] { hide_if_pointer_left(); });
    watch_timer_.setInterval(50);
    connect(&watch_timer_, &QTimer::timeout, this, [this] { watch_pointer(); });
  }

  void set_overflow_menu(QMenu* menu) {
    if (menu_ != nullptr) menu_->removeEventFilter(this);
    menu_ = menu;
    if (menu_ == nullptr) return;
    menu_->installEventFilter(this);
    connect(menu_, &QMenu::aboutToHide, this, [this] {
      hide_timer_.stop();
      watch_timer_.stop();
    });
  }

 protected:
  bool event(QEvent* event) override {
    if (event->type() == QEvent::HoverEnter) {
      hide_timer_.stop();
      show_overflow_menu();
    }
    return QLabel::event(event);
  }

  void enterEvent(QEnterEvent* event) override {
    QLabel::enterEvent(event);
    hide_timer_.stop();
    show_overflow_menu();
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == menu_ && (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter)) {
      hide_timer_.stop();
    }
    return QLabel::eventFilter(watched, event);
  }

 private:
  void show_overflow_menu() {
    if (menu_ == nullptr || menu_->isEmpty() || !isVisible()) return;
    if (!menu_->isVisible()) menu_->popup(mapToGlobal(QPoint(0, height())));
    watch_timer_.start();
  }

  void watch_pointer() {
    if (menu_ == nullptr || !menu_->isVisible()) {
      hide_timer_.stop();
      watch_timer_.stop();
      return;
    }
    if (pointer_over_self_or_menu()) {
      hide_timer_.stop();
      return;
    }
    if (!hide_timer_.isActive()) hide_timer_.start();
  }

  void hide_if_pointer_left() {
    if (menu_ == nullptr || !menu_->isVisible()) {
      watch_timer_.stop();
      return;
    }
    if (pointer_over_self_or_menu()) return;
    menu_->hide();
    watch_timer_.stop();
  }

  bool pointer_over_self_or_menu() const {
    const QPoint global = QCursor::pos();
    const QRect self_global(mapToGlobal(QPoint(0, 0)), size());
    if (self_global.contains(global)) return true;
    if (menu_ == nullptr || !menu_->isVisible()) return false;
    const QRect menu_global(menu_->mapToGlobal(QPoint(0, 0)), menu_->size());
    if (menu_global.contains(global)) return true;
    // Gap between the dots and the list: crossing it is not a leave.
    const int left = std::min(self_global.left(), menu_global.left());
    const int right = std::max(self_global.right(), menu_global.right());
    const int top = std::min(self_global.bottom(), menu_global.top());
    const int bottom = std::max(self_global.bottom(), menu_global.top());
    return QRect(QPoint(left, top), QPoint(right, bottom)).contains(global);
  }

  QMenu* menu_ = nullptr;
  QTimer hide_timer_;
  QTimer watch_timer_;
};

void suppress_qt_toolbar_extension(QToolBar* bar) {
  if (bar == nullptr) return;
  const auto buttons = bar->findChildren<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
  for (QToolButton* ext : buttons) {
    if (ext->isHidden() && ext->maximumWidth() == 0 && ext->maximumHeight() == 0) continue;
    ext->hide();
    ext->setEnabled(false);
    ext->setFixedSize(0, 0);
    ext->setMaximumSize(0, 0);
  }
}

// QToolBar / QMainWindow take a child widget's minimumSizeHint as the window
// floor. The strip must be allowed to shrink to the overflow control; hiding
// trailing buttons happens in relayout, not by locking the window width.
class CommandToolbarStrip final : public QWidget {
 public:
  explicit CommandToolbarStrip(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
  }

  QSize sizeHint() const override {
    if (QLayout* lay = layout()) return lay->sizeHint();
    return QSize(0, 0);
  }

  QSize minimumSizeHint() const override {
    return QSize(0, sizeHint().height());
  }
};

class ShrinkableCommandToolBar final : public QToolBar {
 public:
  using QToolBar::QToolBar;

  QSize minimumSizeHint() const override {
    QSize min = QToolBar::minimumSizeHint();
    if (orientation() == Qt::Horizontal) min.setWidth(0);
    else min.setHeight(0);
    return min;
  }
};

int dock_title_bar_height(const QDockWidget* dock) {
  if (dock == nullptr) return 0;
  if (const QWidget* bar = dock->titleBarWidget()) {
    const int h = bar->height() > 0 ? bar->height() : bar->sizeHint().height();
    return std::max(0, h);
  }
  if (const QWidget* inner = dock->widget()) {
    const int gap = dock->height() - inner->height();
    if (gap > 0 && dock->height() > 0) return gap;
  }
  QStyleOptionDockWidget opt;
  opt.initFrom(dock);
  opt.title = dock->windowTitle();
  opt.closable = dock->features().testFlag(QDockWidget::DockWidgetClosable);
  opt.movable = dock->features().testFlag(QDockWidget::DockWidgetMovable);
  opt.floatable = dock->features().testFlag(QDockWidget::DockWidgetFloatable);
  int h = dock->style()->pixelMetric(QStyle::PM_TitleBarHeight, &opt, dock);
  if (h <= 0) h = dock->fontMetrics().height() + 8;
  return std::max(0, h);
}

}  // namespace

namespace wds::ui {

EditorMainWindow::EditorMainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("WDS Editor");
  resize(1440, 900);
  setDockNestingEnabled(true);
  // Disable animated reparenting: native Vulkan child windows otherwise hitch
  // badly while a dock is being dragged.
  setDockOptions(AllowNestedDocks | AllowTabbedDocks);
  resize_settle_timer_.setSingleShot(true);
  resize_settle_timer_.setInterval(140);
  backup_timer_.setSingleShot(true);
  layout_save_timer_.setSingleShot(true);
  layout_save_timer_.setInterval(400);
  connect(&backup_timer_, &QTimer::timeout, this, [this] { fire_auto_backup(); });
  connect(&layout_save_timer_, &QTimer::timeout, this, [this] { save_chrome_layout(); });
  connect(&resize_settle_timer_, &QTimer::timeout, this, [this] {
    if (native_resizing_) return;
    if (auto* window = static_cast<RealtimeVulkanWindow*>(preview_window_))
      window->set_resize_suspended(false);
    if (auto* window = static_cast<RealtimeVulkanWindow*>(editor_window_))
      window->set_resize_suspended(false);
  });
  menuBar()->setNativeMenuBar(true);
  menuBar()->addMenu(tr("文件"));
  menuBar()->addMenu(tr("编辑"));
  menuBar()->addMenu(tr("视图"));
  auto* fileMenu = menuBar()->actions().at(0)->menu();
  auto* editMenu = menuBar()->actions().at(1)->menu();
  auto* viewMenu = menuBar()->actions().at(2)->menu();
  command_toolbar_ = new ShrinkableCommandToolBar(tr("功能区"), this);
  addToolBar(command_toolbar_);
  command_toolbar_->setObjectName(QStringLiteral("editorToolBar"));
  command_toolbar_->setIconSize(QSize(22, 22));
  command_toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  command_toolbar_->setMovable(true);
  command_toolbar_->setFloatable(true);
  command_toolbar_->setAllowedAreas(Qt::AllToolBarAreas);
  command_toolbar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  command_toolbar_->setMinimumWidth(0);
  // Own the command row: one strip widget so QToolBar never creates per-action
  // buttons or the themed QToolBarExtension (click / bordered / vertical dots).
  command_toolbar_->setStyleSheet(QStringLiteral(
      "QToolBar#editorToolBar { spacing: 0px; }"
      "QToolBar#editorToolBar QToolBarExtension {"
      "  min-width: 0px; max-width: 0px; padding: 0px; margin: 0px; border: none;"
      "}"));
  command_strip_ = new CommandToolbarStrip(command_toolbar_);
  command_strip_->setObjectName(QStringLiteral("commandToolbarStrip"));
  auto* strip_layout = new QHBoxLayout(command_strip_);
  strip_layout->setContentsMargins(0, 0, 0, 0);
  strip_layout->setSpacing(0);
  bool first_command = true;
  auto addCommand = [this, strip_layout, &first_command](QMenu* menu, const QString& text,
                                                         const QKeySequence& shortcut) {
    if (!first_command) {
      auto* gap = new QWidget(command_strip_);
      gap->setFixedWidth(4);
      gap->setAttribute(Qt::WA_TransparentForMouseEvents);
      strip_layout->addWidget(gap);
      command_gaps_.push_back(gap);
    }
    first_command = false;
    auto* action = new QAction(text, this);
    action->setShortcut(shortcut);
    // QKeySequence::NativeText: ⌘ on macOS, Ctrl on Windows.
    action->setToolTip(shortcut.isEmpty()
                           ? text
                           : text + QStringLiteral("（") +
                                 shortcut.toString(QKeySequence::NativeText) +
                                 QStringLiteral("）"));
    if (menu != nullptr) menu->addAction(action);
    auto* button = new QToolButton(command_strip_);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(22, 22));
    button->setFocusPolicy(Qt::NoFocus);
    strip_layout->addWidget(button);
    command_actions_.push_back(action);
    command_buttons_.push_back(button);
    return action;
  };
  open_action_ = addCommand(fileMenu, tr("打开工程"), QKeySequence::Open);
  save_action_ = addCommand(fileMenu, tr("保存工程"), QKeySequence::Save);
  music_action_ = addCommand(fileMenu, tr("导入音乐"), {});
  jacket_action_ = addCommand(fileMenu, tr("导入曲绘"), {});
  auto* stems_action = fileMenu->addAction(tr("本地分轨…"));
  stems_action->setObjectName(QStringLiteral("localStemsAction"));
  connect(stems_action, &QAction::triggered, this, [this] { show_stem_panel(); });
  import_action_ = addCommand(fileMenu, tr("导入谱面"), {});
  export_action_ = addCommand(fileMenu, tr("导出谱面"), {});
  check_action_ = addCommand(fileMenu, tr("检查谱面"), {});
  clear_marks_action_ = addCommand(editMenu, tr("清除记号"), {});
  export_video_action_ = addCommand(nullptr, tr("导出视频"), {});
  if (export_action_ != nullptr && export_video_action_ != nullptr && fileMenu != nullptr) {
    fileMenu->insertAction(check_action_, export_video_action_);
  }
  undo_action_ = addCommand(editMenu, tr("撤销"), QKeySequence::Undo);
  redo_action_ = addCommand(editMenu, tr("重做"), QKeySequence::Redo);
  auto* overflow = new HoverOverflowAffordance(command_strip_);
  overflow->setObjectName(QStringLiteral("commandToolbarOverflow"));
  overflow->setToolTip(tr("更多"));
  overflow->setAccessibleName(tr("更多"));
  command_overflow_menu_ = new QMenu(this);
  overflow->set_overflow_menu(command_overflow_menu_);
  command_overflow_button_ = overflow;
  strip_layout->addWidget(overflow, 0, Qt::AlignVCenter);
  command_toolbar_->addWidget(command_strip_);
  apply_command_icons();
  convert_toolbar_ = addToolBar(tr("转换"));
  convert_toolbar_->setObjectName(QStringLiteral("convertToolBar"));
  convert_toolbar_->setIconSize(QSize(26, 26));
  convert_toolbar_->setMovable(true);
  convert_toolbar_->setFloatable(true);
  convert_toolbar_->setAllowedAreas(Qt::AllToolBarAreas);
  convert_toolbar_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
  convert_toolbar_->setMinimumWidth(0);
  watch_chrome_layout(command_toolbar_);
  watch_chrome_layout(convert_toolbar_);
  command_toolbar_->installEventFilter(this);
  convert_toolbar_->installEventFilter(this);
  connect(command_toolbar_, &QToolBar::orientationChanged, this,
          [this](Qt::Orientation) { schedule_command_toolbar_relayout(); });
  connect(command_toolbar_, &QToolBar::topLevelChanged, this,
          [this](bool) { schedule_command_toolbar_relayout(); });
  connect(convert_toolbar_, &QToolBar::topLevelChanged, this,
          [this](bool) { schedule_command_toolbar_relayout(); });
  connect(convert_toolbar_, &QToolBar::visibilityChanged, this,
          [this](bool) { schedule_command_toolbar_relayout(); });
  schedule_command_toolbar_relayout();
  // AboutRole only takes effect when the action lives in a QMenu.
  // menuBar()->addAction() makes a top-level title and macOS ignores the role.
  about_action_ = new QAction(tr("关于"), this);
  about_action_->setIcon(themed_named_icon("info"));
  auto* help_menu = menuBar()->addMenu(tr("帮助"));
#ifdef Q_OS_MACOS
  about_action_->setText(tr("关于 WDS Editor"));
  about_action_->setMenuRole(QAction::AboutRole);
#else
  about_action_->setMenuRole(QAction::NoRole);
#endif
  help_menu->addAction(about_action_);
  connect(about_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.about");
    AboutDialog(this).exec();
  });
  check_updates_action_ = new QAction(tr("检查更新"), this);
#ifdef Q_OS_MACOS
  check_updates_action_->setMenuRole(QAction::ApplicationSpecificRole);
#else
  check_updates_action_->setMenuRole(QAction::NoRole);
#endif
  help_menu->addAction(check_updates_action_);
  connect(check_updates_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.check_updates");
    update_checker_.check(this, true);
  });

  statusBar()->showMessage(tr("就绪"));

  fullscreen_action_ = viewMenu->addAction(tr("全屏"));
  fullscreen_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F11));
  fullscreen_action_->setToolTip(
      tr("全屏") + QStringLiteral("（") +
      fullscreen_action_->shortcut().toString(QKeySequence::NativeText) + QStringLiteral("）"));
  connect(fullscreen_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.fullscreen");
    isFullScreen() ? showNormal() : showFullScreen();
  });
  auto* reset = viewMenu->addAction(tr("重置布局"));
  connect(reset, &QAction::triggered, this, [this] {
    journal_menu_action("menu.reset_layout");
    QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
    prefs.remove("window/geometry");
    prefs.remove("window/state-v12");
    prefs.remove("window/state-v13");
    prefs.remove("window/state-v14");
    prefs.remove("window/state-v15");
    prefs.remove("window/state-v16");
    reset_default_layout();
  });

  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  const auto geometry = prefs.value("window/geometry").toByteArray();
  if (geometry.isEmpty()) {
    setWindowState(windowState() | Qt::WindowMaximized);
  } else {
    restoreGeometry(geometry);
  }
  // Dock state is restored in bind_ui_manager once every dock exists.
}

std::array<QDockWidget*, 6> EditorMainWindow::chrome_docks() const {
  return {preview_dock_, editor_dock_, playback_dock_, toolbar_dock_, curve_dock_,
          settings_dock_};
}

void EditorMainWindow::add_dock_toggle(QDockWidget* dock) {
  if (dock == nullptr || fullscreen_action_ == nullptr) return;
  watch_chrome_layout(dock);
  auto* viewMenu = menuBar()->actions().at(2)->menu();
  auto* action = dock->toggleViewAction();
  viewMenu->insertAction(fullscreen_action_, action);
  struct DockJournalId {
    const char* object_name;
    const char* route;
  };
  static constexpr DockJournalId kDockJournalIds[] = {
      {"previewViewportDock", "menu.view.preview"},
      {"editorViewportDock", "menu.view.edit"},
      {"playbackAudioDock", "menu.view.playback"},
      {"editorToolbarDock", "menu.view.toolbar"},
      {"curveTemplatesDock", "menu.view.curve"},
      {"settingsDock", "menu.view.settings"},
  };
  const char* id = "menu.view.dock";
  const auto name = dock->objectName();
  for (const auto& entry : kDockJournalIds) {
    if (name == QLatin1String(entry.object_name)) {
      id = entry.route;
      break;
    }
  }
  connect(action, &QAction::toggled, this, [id](bool) { journal_menu_action(id); });
}

void EditorMainWindow::apply_default_dock_sizes() {
  if (preview_dock_ == nullptr || editor_dock_ == nullptr || playback_dock_ == nullptr ||
      toolbar_dock_ == nullptr || curve_dock_ == nullptr || settings_dock_ == nullptr) {
    return;
  }
  // Preview is 0.75× the previous 800px default; height follows 16:9.
  const int right_w = 220;
  const int max_left = std::max(320, width() - right_w - 200);
  const int left_w = std::clamp(600, 320, max_left);
  const int edit_w = std::max(200, std::max(1, width() - left_w - right_w - 24));
  const int curve_h = std::max(220, height() / 2 - 40);
  const int settings_h = std::max(200, height() - curve_h - 80);
  const int play_min = std::max(72, playback_panel_->sizeHint().height() + 16);
  const int tool_min = std::max(96, toolbar_widget_->sizeHint().height() + 16);
  const int preview_h =
      std::max(180, preview_content_height_for_width(left_w) + dock_title_bar_height(preview_dock_));
  const int chrome = 96;
  const int leftover =
      std::max(0, height() - chrome - preview_h - play_min - tool_min);
  const int play_share = leftover * 2 / 5;
  const int play_h = play_min + play_share;
  const int tool_h = tool_min + leftover - play_share;
  resizeDocks({preview_dock_, editor_dock_, curve_dock_}, {left_w, edit_w, right_w},
              Qt::Horizontal);
  resizeDocks({preview_dock_, playback_dock_, toolbar_dock_}, {preview_h, play_h, tool_h},
              Qt::Vertical);
  resizeDocks({curve_dock_, settings_dock_}, {curve_h, settings_h}, Qt::Vertical);
}

void EditorMainWindow::reset_default_layout() {
  if (preview_dock_ == nullptr || editor_dock_ == nullptr) return;
  auto reset_toolbar = [this](QToolBar* bar) {
    if (bar == nullptr) return;
    removeToolBar(bar);
    addToolBar(Qt::TopToolBarArea, bar);
    bar->show();
  };
  reset_toolbar(command_toolbar_);
  reset_toolbar(convert_toolbar_);
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr) removeDockWidget(dock);
  }
  addDockWidget(Qt::LeftDockWidgetArea, preview_dock_);
  splitDockWidget(preview_dock_, editor_dock_, Qt::Horizontal);
  if (curve_dock_ != nullptr) splitDockWidget(editor_dock_, curve_dock_, Qt::Horizontal);
  if (curve_dock_ != nullptr && settings_dock_ != nullptr)
    splitDockWidget(curve_dock_, settings_dock_, Qt::Vertical);
  if (playback_dock_ != nullptr) splitDockWidget(preview_dock_, playback_dock_, Qt::Vertical);
  if (playback_dock_ != nullptr && toolbar_dock_ != nullptr)
    splitDockWidget(playback_dock_, toolbar_dock_, Qt::Vertical);
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr) dock->show();
  }
  QTimer::singleShot(0, this, [this] { apply_default_dock_sizes(); });
  schedule_command_toolbar_relayout();
}

void EditorMainWindow::restore_or_reset_layout() {
  if (preview_dock_ == nullptr || editor_dock_ == nullptr || playback_dock_ == nullptr ||
      toolbar_dock_ == nullptr || curve_dock_ == nullptr || settings_dock_ == nullptr) {
    return;
  }
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  const auto geometry = prefs.value("window/geometry").toByteArray();
  if (!geometry.isEmpty()) restoreGeometry(geometry);
  const auto state = prefs.value("window/state-v16").toByteArray();
  if (state.isEmpty() || !restoreState(state)) {
    reset_default_layout();
    return;
  }
  // Recover the full editor chrome if a saved layout closed the edit / playback
  // docks. Floating docks are hidden on purpose during the startup splash.
  if (editor_dock_->isHidden() && playback_dock_->isHidden() &&
      !editor_dock_->isFloating() && !playback_dock_->isFloating()) {
    reset_default_layout();
  }
  if (preview_dock_->isHidden() && !preview_dock_->isFloating()) {
    preview_dock_->show();
  }
  if (!chrome_revealed_) concealed_floating_chrome_.clear();
  conceal_unrevealed_floating_chrome();
  schedule_command_toolbar_relayout();
}

void EditorMainWindow::save_chrome_layout() {
  layout_save_timer_.stop();
  // CLI / capture mutates docks. The splash conceals floating chrome. Neither
  // snapshot is the user's editor layout.
  if (!persist_chrome_layout_ || !chrome_revealed_) return;
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  prefs.setValue("window/geometry", saveGeometry());
  prefs.setValue("window/state-v16", saveState());
  prefs.sync();
}

void EditorMainWindow::schedule_chrome_layout_save() {
  if (!persist_chrome_layout_ || !chrome_revealed_) return;
  layout_save_timer_.start();
}

void EditorMainWindow::watch_chrome_layout(QWidget* widget) {
  if (widget == nullptr || widget->property("wdsLayoutWatched").toBool()) return;
  widget->setProperty("wdsLayoutWatched", true);
  if (auto* dock = qobject_cast<QDockWidget*>(widget)) {
    connect(dock, &QDockWidget::dockLocationChanged, this,
            &EditorMainWindow::schedule_chrome_layout_save);
    connect(dock, &QDockWidget::topLevelChanged, this,
            &EditorMainWindow::schedule_chrome_layout_save);
    connect(dock, &QDockWidget::visibilityChanged, this,
            &EditorMainWindow::schedule_chrome_layout_save);
    return;
  }
  if (auto* bar = qobject_cast<QToolBar*>(widget)) {
    connect(bar, &QToolBar::topLevelChanged, this,
            &EditorMainWindow::schedule_chrome_layout_save);
    connect(bar, &QToolBar::visibilityChanged, this,
            &EditorMainWindow::schedule_chrome_layout_save);
  }
}

void EditorMainWindow::conceal_unrevealed_floating_chrome() {
  if (chrome_revealed_) return;
  auto conceal = [this](QWidget* widget) {
    if (widget == nullptr || widget->isHidden()) return;
    if (std::find(concealed_floating_chrome_.begin(), concealed_floating_chrome_.end(), widget) ==
        concealed_floating_chrome_.end()) {
      concealed_floating_chrome_.push_back(widget);
    }
    widget->hide();
  };
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr && dock->isFloating()) conceal(dock);
  }
  for (auto* bar : {command_toolbar_, convert_toolbar_}) {
    if (bar != nullptr && bar->isFloating()) conceal(bar);
  }
}

void EditorMainWindow::reveal_concealed_floating_chrome() {
  chrome_revealed_ = true;
  for (auto* widget : concealed_floating_chrome_) {
    if (widget != nullptr) widget->show();
  }
  concealed_floating_chrome_.clear();
}

void EditorMainWindow::ensure_preview_viewport_for_cli() {
  persist_chrome_layout_ = false;
  if (preview_dock_ == nullptr) return;
  preview_dock_->show();
  preview_dock_->raise();
  if (preview_window_ != nullptr) {
    preview_window_->create();
    preview_window_->show();
  }
  apply_default_dock_sizes();
}

void EditorMainWindow::prepare_preview_capture_size(int pixel_w, int pixel_h) {
  // CLI `capture-preview` only. Interactive export keeps the editor chrome.
  persist_chrome_layout_ = false;
  ensure_preview_viewport_for_cli();
  if (preview_dock_ == nullptr || preview_window_ == nullptr || pixel_w <= 0 || pixel_h <= 0) {
    return;
  }
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr && dock != preview_dock_) {
      dock->hide();
    }
  }
  if (editor_dock_ != nullptr) {
    editor_dock_->hide();
  }
  if (menuBar() != nullptr) menuBar()->hide();
  if (statusBar() != nullptr) statusBar()->hide();
  preview_dock_->setFloating(true);
  preview_dock_->setTitleBarWidget(new QWidget(preview_dock_));
  preview_dock_->setContentsMargins(0, 0, 0, 0);
  const qreal dpr = std::max<qreal>(1.0, preview_window_->devicePixelRatio());
  const int logical_w = std::max(1, qRound(static_cast<qreal>(pixel_w) / dpr));
  const int logical_h = std::max(1, qRound(static_cast<qreal>(pixel_h) / dpr));
  if (auto* container = preview_dock_->widget()) {
    container->setContentsMargins(0, 0, 0, 0);
    container->setFixedSize(logical_w, logical_h);
  }
  preview_window_->setMinimumSize(QSize(logical_w, logical_h));
  preview_window_->setMaximumSize(QSize(logical_w, logical_h));
  preview_window_->resize(logical_w, logical_h);
  preview_dock_->adjustSize();
  preview_dock_->show();
  preview_dock_->raise();
  preview_window_->show();
  if (auto* window = dynamic_cast<RealtimeVulkanWindow*>(preview_window_)) {
    window->request_frame();
  }
}

void EditorMainWindow::restore_preview_after_export() {
  persist_chrome_layout_ = true;
  if (menuBar() != nullptr) menuBar()->show();
  if (statusBar() != nullptr) statusBar()->show();
  if (preview_window_ != nullptr) {
    preview_window_->setMinimumSize(QSize(0, 0));
    preview_window_->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
  }
  if (preview_dock_ != nullptr) {
    if (auto* container = preview_dock_->widget()) {
      container->setMinimumSize(QSize(0, 0));
      container->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
    }
    preview_dock_->setTitleBarWidget(nullptr);
  }
  restore_or_reset_layout();
  if (auto* window = dynamic_cast<RealtimeVulkanWindow*>(preview_window_)) {
    window->request_frame();
  }
}

void EditorMainWindow::set_export_session_busy(bool on) { busy_active_ = on; }

void EditorMainWindow::export_video() {
  if (ui_manager_ == nullptr) return;
  ExportVideoDialog dialog(ui_manager_, this, this);
  dialog.exec();
}

void EditorMainWindow::ensure_stem_panel() {
  if (!ui_manager_) return;
  if (!stem_panel_) {
    stem_panel_ = new StemPanel(ui_manager_, this);
    stem_panel_->set_tracks_changed_handler([this] {
      if (playback_panel_)
        playback_panel_->set_stem_tracks(stem_panel_->track_names(), stem_panel_->selected_track());
    });
  }
  // Constructor syncs once; open/import after that must re-scan Stem/ for this song.
  stem_panel_->resync();
}

void EditorMainWindow::show_stem_panel() {
  ensure_stem_panel();
  if (!stem_panel_) return;
  stem_panel_->show();
  stem_panel_->raise();
  stem_panel_->activateWindow();
}

void EditorMainWindow::create_control_docks() {
  playback_dock_ = new QDockWidget(tr("播放"), this);
  playback_dock_->setObjectName(QStringLiteral("playbackAudioDock"));
  playback_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  playback_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
  playback_panel_ = new PlaybackBar(ui_manager_, playback_dock_);
  playback_panel_->set_stem_handlers([this](int index) {
    if (stem_panel_ && !stem_panel_->select_track(index))
      statusBar()->showMessage(tr("无法切换音轨，请检查音源或重新生成分轨。"), 5000);
  }, [this] { show_stem_panel(); });
  playback_dock_->setWidget(playback_panel_);
  playback_dock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  playback_dock_->setMinimumHeight(0);
  playback_dock_->setMaximumHeight(QWIDGETSIZE_MAX);
  addDockWidget(Qt::BottomDockWidgetArea, playback_dock_);
  add_dock_toggle(playback_dock_);

  convert_bar_ = new ConvertBar(ui_manager_, skins_dir_, convert_toolbar_);
  convert_bar_->set_ribbon_mode();
  if (convert_toolbar_ != nullptr) convert_toolbar_->addWidget(convert_bar_);
  schedule_command_toolbar_relayout();

  toolbar_dock_ = new QDockWidget(tr("工具栏"), this);
  toolbar_dock_->setObjectName(QStringLiteral("editorToolbarDock"));
  toolbar_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  toolbar_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
  toolbar_widget_ = new EditorToolbarWidget(ui_manager_, toolbar_dock_);
  toolbar_widget_->set_add_chart_handler([this] { add_chart(); });
  toolbar_dock_->setWidget(toolbar_widget_);
  toolbar_dock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  toolbar_dock_->setMinimumHeight(0);
  toolbar_dock_->setMaximumHeight(QWIDGETSIZE_MAX);
  addDockWidget(Qt::BottomDockWidgetArea, toolbar_dock_);
  add_dock_toggle(toolbar_dock_);

  curve_dock_ = new QDockWidget(tr("曲线模板"), this);
  curve_dock_->setObjectName(QStringLiteral("curveTemplatesDock"));
  curve_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  curve_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
  curve_panel_ = new CurveTemplatesPanel(ui_manager_, curve_dock_);
  curve_panel_->set_on_changed([this] {
    if (toolbar_widget_ != nullptr) toolbar_widget_->refresh_curve_controls();
  });
  curve_dock_->setWidget(curve_panel_);
  addDockWidget(Qt::RightDockWidgetArea, curve_dock_);
  add_dock_toggle(curve_dock_);

  settings_dock_ = new QDockWidget(tr("设置"), this);
  settings_dock_->setObjectName(QStringLiteral("settingsDock"));
  settings_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  settings_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
  settings_panel_ = new SettingsPanel(ui_manager_, theme_dir_, settings_dock_);
  settings_panel_->set_on_applied([this] {
    sync_toolbox_place_checks();
    arm_backup_timer();
  });
  settings_dock_->setWidget(settings_panel_);
  addDockWidget(Qt::RightDockWidgetArea, settings_dock_);
  add_dock_toggle(settings_dock_);
}

void EditorMainWindow::sync_toolbox_place_checks() {
  if (convert_bar_ != nullptr) convert_bar_->sync_place_checks();
}

void EditorMainWindow::set_icons_dir(std::string dir) {
  icons_dir_ = std::move(dir);
  apply_command_icons();
}

void EditorMainWindow::apply_command_icons() {
  if (icons_dir_.empty()) {
    const auto argv0 = QCoreApplication::applicationFilePath();
    icons_dir_ = resolve_icons_dir(argv0.toUtf8().constData());
  }
  const QDir dir(QString::fromStdString(icons_dir_));
  const auto icon_for = [&](const char* stem) {
    return themed_svg_icon(dir.filePath(QString::fromLatin1(stem) + QStringLiteral(".svg")));
  };
  if (open_action_ != nullptr) open_action_->setIcon(icon_for("open"));
  if (save_action_ != nullptr) save_action_->setIcon(icon_for("save"));
  if (undo_action_ != nullptr) undo_action_->setIcon(icon_for("undo"));
  if (redo_action_ != nullptr) redo_action_->setIcon(icon_for("redo"));
  if (import_action_ != nullptr) import_action_->setIcon(icon_for("import"));
  if (export_action_ != nullptr) export_action_->setIcon(icon_for("export"));
  if (music_action_ != nullptr) music_action_->setIcon(icon_for("import-audio"));
  if (jacket_action_ != nullptr) jacket_action_->setIcon(icon_for("import-image"));
  if (check_action_ != nullptr) check_action_->setIcon(icon_for("check"));
  if (clear_marks_action_ != nullptr) clear_marks_action_->setIcon(icon_for("clear-marks"));
  if (export_video_action_ != nullptr) export_video_action_->setIcon(icon_for("export-video"));
  if (about_action_ != nullptr) about_action_->setIcon(icon_for("info"));
  if (command_overflow_button_ != nullptr) {
    const QIcon more = icon_for("more");
    command_overflow_button_->setPixmap(more.pixmap(22, 22));
    command_overflow_button_->setFixedSize(22, 22);
  }
  schedule_command_toolbar_relayout();
}

void EditorMainWindow::schedule_command_toolbar_relayout() {
  if (command_toolbar_relayout_pending_) return;
  command_toolbar_relayout_pending_ = true;
  QTimer::singleShot(0, this, [this] {
    command_toolbar_relayout_pending_ = false;
    relayout_command_toolbar();
  });
}

bool EditorMainWindow::command_shares_row_with_convert() const {
  if (command_toolbar_ == nullptr || convert_toolbar_ == nullptr) return false;
  if (convert_toolbar_->isHidden() || convert_toolbar_->isFloating()) return false;
  if (command_toolbar_->isFloating()) return false;
  if (toolBarArea(command_toolbar_) != toolBarArea(convert_toolbar_)) return false;
  if (command_toolbar_->orientation() != Qt::Horizontal) return false;
  return !toolBarBreak(convert_toolbar_) && !toolBarBreak(command_toolbar_);
}

void EditorMainWindow::relayout_command_toolbar() {
  if (command_toolbar_relayouting_ || command_toolbar_ == nullptr || command_strip_ == nullptr ||
      command_overflow_button_ == nullptr || command_overflow_menu_ == nullptr) {
    return;
  }
  command_toolbar_relayouting_ = true;

  const auto show_all = [this] {
    for (QWidget* gap : command_gaps_) {
      if (gap != nullptr) gap->show();
    }
    for (QToolButton* button : command_buttons_) {
      if (button != nullptr) button->show();
    }
    command_overflow_button_->hide();
    command_overflow_menu_->clear();
  };

  suppress_qt_toolbar_extension(command_toolbar_);

  if (command_toolbar_->orientation() != Qt::Horizontal) {
    show_all();
    command_strip_->setMaximumWidth(QWIDGETSIZE_MAX);
    command_strip_->updateGeometry();
    command_toolbar_->updateGeometry();
    command_toolbar_relayouting_ = false;
    return;
  }

  std::vector<int> widths;
  widths.reserve(command_buttons_.size());
  for (std::size_t i = 0; i < command_buttons_.size(); ++i) {
    QToolButton* button = command_buttons_[i];
    int width = button != nullptr ? std::max(button->sizeHint().width(), button->minimumWidth()) : 0;
    if (i > 0 && i - 1 < command_gaps_.size() && command_gaps_[i - 1] != nullptr) {
      width += command_gaps_[i - 1]->width();
    }
    widths.push_back(width);
  }

  const int overflow_w = std::max(command_overflow_button_->sizeHint().width(), 22);

  int convert_w = 0;
  const bool share_row = command_shares_row_with_convert();
  if (share_row) {
    // Natural ribbon width only. The toolbar sizeHint can grow with the row
    // and would hide command buttons while the window is still wide enough.
    convert_w = convert_bar_ != nullptr ? convert_bar_->sizeHint().width()
                                        : convert_toolbar_->minimumSizeHint().width();
    if (convert_toolbar_->isMovable()) {
      convert_w += convert_toolbar_->style()->pixelMetric(QStyle::PM_ToolBarHandleExtent);
    }
  }

  int budget = command_toolbar_->isFloating() ? command_toolbar_->width() : width() - convert_w;
  if (command_toolbar_->isMovable() && !command_toolbar_->isFloating()) {
    budget -= command_toolbar_->style()->pixelMetric(QStyle::PM_ToolBarHandleExtent);
  }
  budget = std::max(0, budget);

  const std::size_t hide_n = command_overflow_hide_count(widths, overflow_w, budget);
  const std::size_t keep = command_buttons_.size() - hide_n;
  for (std::size_t i = 0; i < command_buttons_.size(); ++i) {
    const bool show = i < keep;
    if (command_buttons_[i] != nullptr) command_buttons_[i]->setVisible(show);
    if (i > 0 && i - 1 < command_gaps_.size() && command_gaps_[i - 1] != nullptr) {
      command_gaps_[i - 1]->setVisible(show);
    }
  }

  command_overflow_menu_->clear();
  int max_width_to_set = QWIDGETSIZE_MAX;
  if (hide_n == 0) {
    command_overflow_button_->hide();
    max_width_to_set = QWIDGETSIZE_MAX;
  } else {
    for (std::size_t i = keep; i < command_actions_.size(); ++i) {
      command_overflow_menu_->addAction(command_actions_[i]);
    }
    command_overflow_button_->show();
    max_width_to_set = std::max(1, budget);
  }

  // Mix the computed layout into a single int so re-layouts that produce an
  // identical result (same visible count, same strip width, same row sharing)
  // short-circuit and stop the self-sustaining LayoutRequest → relayout loop.
  int state = static_cast<int>(keep);
  state = state * 31 + static_cast<int>(hide_n);
  state = state * 31 + max_width_to_set;
  state = state * 31 + (share_row ? 1 : 0);
  if (state == command_layout_state_) {
    command_toolbar_relayouting_ = false;
    suppress_qt_toolbar_extension(command_toolbar_);
    return;
  }
  command_layout_state_ = state;
  command_strip_->setMaximumWidth(max_width_to_set);
  command_strip_->updateGeometry();
  command_toolbar_->updateGeometry();
  suppress_qt_toolbar_extension(command_toolbar_);
  command_toolbar_relayouting_ = false;
}

void EditorMainWindow::refresh_history_actions() {
  if (ui_manager_ == nullptr || undo_action_ == nullptr || redo_action_ == nullptr) return;
  const auto& history = ui_manager_->session().engine().history();
  undo_action_->setEnabled(history.can_undo());
  redo_action_->setEnabled(history.can_redo());
}

double EditorMainWindow::timeline_now_ms() const {
  if (ui_manager_ == nullptr) return 0.0;
  return static_cast<double>(ui_manager_->session().engine().timeline_us()) / 1000.0;
}

void EditorMainWindow::refresh_clear_marks_action() {
  if (clear_marks_action_ == nullptr) return;
  bool has_marks = false;
  if (ui_manager_ != nullptr) {
    if (auto* panel = ui_manager_->edit_panel()) has_marks = panel->has_click_marks();
  }
  clear_marks_action_->setEnabled(has_marks);
}

void EditorMainWindow::clear_click_marks() {
  click_record_held_ = 0;
  if (ui_manager_ != nullptr) {
    if (auto* panel = ui_manager_->edit_panel()) panel->clear_click_marks();
  }
  if (editor_widget_ != nullptr) editor_widget_->update();
  refresh_clear_marks_action();
}

void EditorMainWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  if (event == nullptr) return;
  if (event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::ApplicationPaletteChange) {
    apply_command_icons();
    return;
  }
  if (event->type() == QEvent::WindowStateChange) {
    // While the shell is minimized win32 reports a 0×0 surface for the preview,
    // so the renderer keeps its last swapchain flagged occluded and presents
    // nothing. An embedded native child does not reliably deliver the expose
    // transition when the shell is restored from the taskbar, and the paused
    // preview gate then reports "unchanged" - the preview stayed black until
    // some unrelated input dirtied it. Force one frame as soon as the shell
    // leaves the minimized state; draw_frame rebuilds the occluded swapchain.
    const auto* state_event = static_cast<QWindowStateChangeEvent*>(event);
    const bool was_minimized = (state_event->oldState() & Qt::WindowMinimized) != 0;
    if (was_minimized && !isMinimized()) {
      if (auto* window = dynamic_cast<RealtimeVulkanWindow*>(preview_window_)) {
        window->request_frame();
      }
    }
  }
}

void EditorMainWindow::on_preview_frame() {
  if (ui_manager_ == nullptr) return;
  if (playback_panel_ != nullptr) playback_panel_->sync_position();
  if (auto* panel = ui_manager_->edit_panel()) {
    const bool playing =
        ui_manager_->session().engine().playback_state() == wds::common::PlaybackState::Playing;
    panel->click_record_sync_playing(playing, timeline_now_ms());
    if (!playing) click_record_held_ = 0;
  }
  refresh_clear_marks_action();
}

void EditorMainWindow::bind_ui_manager(UiManager* manager) {
  ui_manager_ = manager;
  if (ui_manager_ == nullptr) return;
  ui_manager_->set_ui_change_handler([this](UiChange change) { handle_ui_change(change); });
  ui_manager_->set_external_status_handler([this](std::string text, StatusLevel) {
    const QString message = QString::fromUtf8(text.c_str());
    QMetaObject::invokeMethod(this, [this, message] {
      if (statusBar() != nullptr) statusBar()->showMessage(message);
    }, Qt::QueuedConnection);
  });
  ui_manager_->set_fullscreen_toggler([this] {
    isFullScreen() ? showNormal() : showFullScreen();
  });
  ui_manager_->set_open_project_handler([this] { open_project(); });
  ui_manager_->set_save_project_handler([this] { save_project(); });
  ui_manager_->session().set_backup_sink([this](std::vector<ChartBackupItem> items) {
    enqueue_backup_write(ChartBackupKind::Manual, std::move(items));
  });
  // Quit from the macOS application menu can terminate without a closeEvent.
  connect(qApp, &QCoreApplication::aboutToQuit, this,
          [this] { save_resume_position_on_exit(); });
  connect(open_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.open");
    open_project();
  });
  connect(save_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.save");
    save_project();
  });
  connect(undo_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.undo");
    ui_manager_->session().engine().undo();
    ui_manager_->notify_ui_change(UiChange::History);
    ui_manager_->notify_ui_change(UiChange::Document);
  });
  connect(redo_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.redo");
    ui_manager_->session().engine().redo();
    ui_manager_->notify_ui_change(UiChange::History);
    ui_manager_->notify_ui_change(UiChange::Document);
  });
  connect(import_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.import_chart");
    import_chart();
  });
  connect(export_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.export_chart");
    export_chart();
  });
  connect(export_video_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.export_video");
    export_video();
  });
  connect(music_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.import_music");
    import_music();
  });
  connect(jacket_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.import_jacket");
    import_jacket();
  });
  connect(check_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.check_chart");
    check_chart();
  });
  connect(clear_marks_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.clear_click_marks");
    clear_click_marks();
  });
  refresh_clear_marks_action();

  create_control_docks();
  restore_or_reset_layout();
  if (playback_panel_ != nullptr) playback_panel_->refresh_settings_widgets();
  if (toolbar_widget_ != nullptr) {
    toolbar_widget_->refresh_offset_field();
    toolbar_widget_->refresh_grid_fields();
    toolbar_widget_->refresh_flags();
    toolbar_widget_->refresh_chart_selector();
    toolbar_widget_->refresh_enabled_states();
  }
  refresh_history_actions();
  refresh_window_title();
  // Space toggles playback anywhere in the app (except while typing).
  qApp->installEventFilter(this);
}

EditorMainWindow::~EditorMainWindow() {
  ++open_generation_;
  wait_open_project_watchers();
}

void EditorMainWindow::detach_ui_manager() {
  backup_timer_.stop();
  if (stem_panel_) {
    delete stem_panel_;
    stem_panel_ = nullptr;
  }
  if (ui_manager_ != nullptr) {
    ui_manager_->session().set_backup_sink({});
    ui_manager_->set_ui_change_handler({});
  }
  ui_manager_ = nullptr;
}

void EditorMainWindow::handle_ui_change(UiChange change) {
  // The preview only presents when something changed. Any UI-side change that
  // can affect the stage (document, settings, playback chrome) requests one
  // frame; input and explicit seeks already do the same.
  if (auto* window = dynamic_cast<RealtimeVulkanWindow*>(preview_window_)) {
    window->request_frame();
  }
  switch (change) {
    case UiChange::PlaybackSettings:
      if (playback_panel_ != nullptr) playback_panel_->refresh_settings_widgets();
      break;
    case UiChange::Grid:
      if (toolbar_widget_ != nullptr) {
        toolbar_widget_->refresh_grid_fields();
        toolbar_widget_->refresh_flags();
      }
      break;
    case UiChange::Offset:
      if (toolbar_widget_ != nullptr) {
        toolbar_widget_->refresh_offset_field();
        toolbar_widget_->refresh_enabled_states();
      }
      break;
    case UiChange::Charts:
      if (toolbar_widget_ != nullptr) {
        toolbar_widget_->refresh_chart_selector();
        toolbar_widget_->refresh_enabled_states();
      }
      refresh_window_title();
      if (settings_panel_ != nullptr) settings_panel_->refresh_extended_lock();
      break;
    case UiChange::History:
      refresh_history_actions();
      if (settings_panel_ != nullptr) settings_panel_->refresh_extended_lock();
      break;
    case UiChange::Document:
      refresh_window_title();
      arm_backup_timer();
      if (settings_panel_ != nullptr) settings_panel_->refresh_extended_lock();
      // Recognize project Stem/ cache as soon as music is on the session — do not
      // wait for the user to open「本地分轨」and click「生成分轨」.
      ensure_stem_panel();
      break;
    case UiChange::PlaceTool:
      sync_toolbox_place_checks();
      break;
  }
}

void EditorMainWindow::arm_backup_timer() {
  backup_timer_.stop();
  if (ui_manager_ == nullptr) return;
  const auto& session = ui_manager_->session();
  if (session.project_path().empty() || session.read_only()) return;
  const int minutes = std::max(1, ui_manager_->backup_interval_min());
  backup_timer_.start(minutes * 60 * 1000);
}

void EditorMainWindow::fire_auto_backup() {
  if (ui_manager_ == nullptr) return;
  auto& session = ui_manager_->session();
  if (session.project_path().empty() || session.read_only()) return;
  std::vector<ChartBackupItem> items;
  if (!session.capture_auto_backup(items) || items.empty()) return;
  enqueue_backup_write(ChartBackupKind::Auto, std::move(items));
}

void EditorMainWindow::enqueue_backup_write(ChartBackupKind kind,
                                           std::vector<ChartBackupItem> items) {
  if (ui_manager_ == nullptr || items.empty()) return;
  const std::string root = ui_manager_->session().auto_backup_directory();
  if (root.empty()) return;
  arm_backup_timer();
  auto* watcher = new QFutureWatcher<bool>(this);
  connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
    const bool ok = watcher->result();
    watcher->deleteLater();
    if (!ok && ui_manager_ != nullptr) {
      ui_manager_->session().report_status("自动备份失败", StatusLevel::Error);
    }
  });
  watcher->setFuture(QtConcurrent::run([root, kind, items = std::move(items)]() {
    return write_chart_backup_folder(root, kind, items);
  }));
}

void EditorMainWindow::refresh_window_title() {
  QString title = QStringLiteral("WDS Editor");
  if (ui_manager_ != nullptr) {
    const auto& session = ui_manager_->session();
    if (session.dirty()) title += QStringLiteral(" *");
    if (!session.project_path().empty()) {
      title += QStringLiteral(" — ");
      title += QFileInfo(QString::fromStdString(session.project_path())).fileName();
    }
  }
  setWindowTitle(title);
}

void EditorMainWindow::run_with_busy(const QString& title, std::function<void()> work,
                                     std::function<void()> done) {
  if (busy_active_) {
    WDS_LOG("run_with_busy queued while busy: %s\n", qUtf8Printable(title));
  }
  busy_queue_.push_back(BusyJob{title, std::move(work), std::move(done)});
  if (!busy_active_) start_busy_job();
}

void EditorMainWindow::start_busy_job() {
  if (busy_queue_.empty()) {
    busy_active_ = false;
    return;
  }
  busy_active_ = true;
  auto job = std::move(busy_queue_.front());
  busy_queue_.pop_front();
  auto overlay = std::make_shared<BusyScope>(this, job.title);
  QTimer::singleShot(0, this, [this, overlay, job = std::move(job)]() mutable {
    if (job.work) job.work();
    overlay.reset();
    if (job.done) job.done();
    if (!busy_queue_.empty()) {
      start_busy_job();
    } else {
      busy_active_ = false;
    }
  });
}

void EditorMainWindow::track_open_watcher(QObject* watcher) {
  if (watcher == nullptr) return;
  open_watchers_.push_back(watcher);
}

void EditorMainWindow::untrack_open_watcher(QObject* watcher) {
  open_watchers_.erase(std::remove_if(open_watchers_.begin(), open_watchers_.end(),
                                      [watcher](const QPointer<QObject>& p) {
                                        return p.isNull() || p.data() == watcher;
                                      }),
                       open_watchers_.end());
}

void EditorMainWindow::wait_open_project_watchers() {
  const auto watchers = open_watchers_;
  for (const auto& p : watchers) {
    QObject* obj = p.data();
    if (obj == nullptr) continue;
    static_cast<QFutureWatcher<PreparedWdsProject>*>(obj)->waitForFinished();
  }
  open_watchers_.erase(std::remove_if(open_watchers_.begin(), open_watchers_.end(),
                                      [](const QPointer<QObject>& p) { return p.isNull(); }),
                       open_watchers_.end());
}

bool EditorMainWindow::show_startup_splash() {
  if (ui_manager_ == nullptr || !ui_manager_->session().project_path().empty()) return true;
  conceal_unrevealed_floating_chrome();
  // Keep the chooser independent from the (still hidden) main window. This
  // avoids modality/activation quirks where accepting a child dialog leaves
  // the hidden parent as the active top-level window.
  QDialog splash(nullptr);
  splash.setWindowTitle(tr("WDS Editor"));
  splash.setModal(true);
  splash.resize(720, 480);
  splash.setMinimumSize(720, 480);
  splash.setWindowFlag(Qt::WindowCloseButtonHint, true);
#ifdef Q_OS_MACOS
  // The hidden main window does not own the menu bar while this dialog is
  // frontmost. AboutRole only migrates from a QMenu on a native QMenuBar.
  auto* splash_menus = new QMenuBar(&splash);
  splash_menus->setNativeMenuBar(true);
  auto* splash_help = splash_menus->addMenu(tr("帮助"));
  auto* splash_about = splash_help->addAction(tr("关于 WDS Editor"));
  splash_about->setMenuRole(QAction::AboutRole);
  connect(splash_about, &QAction::triggered, &splash, [&splash] {
    journal_menu_action("splash.about");
    AboutDialog(&splash).exec();
  });
  auto* splash_updates = splash_help->addAction(tr("检查更新"));
  splash_updates->setMenuRole(QAction::ApplicationSpecificRole);
  connect(splash_updates, &QAction::triggered, &splash, [this, &splash] {
    journal_menu_action("splash.check_updates");
    update_checker_.check(&splash, true);
  });
#endif
  auto* root = new QVBoxLayout(&splash);
  root->setContentsMargins(24, 20, 24, 18);
  root->setSpacing(10);
  auto* title = new QLabel(tr("<h1>WDS Editor</h1><p>开始编辑你的音游谱面</p>"), &splash);
  title->setTextFormat(Qt::RichText);
  root->addWidget(title);
  root->addWidget(new QLabel(tr("从下方列表里选择一个现有工程，或创建一个新工程"), &splash));
  auto* recent_label = new QLabel(tr("最近编辑"), &splash);
  recent_label->setStyleSheet(QStringLiteral("font-weight:bold;"));
  root->addWidget(recent_label);
  auto* recent = new QListWidget(&splash);
  recent->setMinimumHeight(96);
  recent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  recent->setUniformItemSizes(false);
  recent->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  const auto recent_paths = QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                                      "WDS", "WDS Editor")
                                .value(QStringLiteral("recent/projects"))
                                .toStringList();
  const QColor path_fg = splash.palette().color(QPalette::WindowText);
  const QColor path_muted =
      path_fg.lightness() >= 128 ? path_fg.darker(140) : path_fg.lighter(180);
  // Cocoa 16pt ≈ 16px. Stylesheet `pt` is 96-DPI, so 12pt looks the same as
  // the name; pin the path in px.
  recent->setStyleSheet(
      QStringLiteral("QLabel#recentPath { font-size: 11px; color: %1; }")
          .arg(path_muted.name()));
  int shown_recent = 0;
  for (const auto& path : recent_paths) {
    if (shown_recent >= 8) break;
    if (!QFileInfo::exists(path)) continue;
    auto* item = new QListWidgetItem(recent);
    item->setToolTip(path);
    item->setData(Qt::UserRole, path);
    auto* row = new QWidget(recent);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(8, 6, 8, 6);
    row_layout->setSpacing(8);
    auto* name_label = new QLabel(QFileInfo(path).completeBaseName(), row);
    auto* path_label = new QLabel(path, row);
    path_label->setObjectName(QStringLiteral("recentPath"));
    QFont path_font = name_label->font();
    path_font.setPixelSize(11);
    path_label->setFont(path_font);
    path_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row_layout->addWidget(name_label, 0, Qt::AlignVCenter);
    row_layout->addWidget(path_label, 1, Qt::AlignVCenter);
    item->setSizeHint(row->sizeHint());
    recent->setItemWidget(item, row);
    ++shown_recent;
  }
  if (recent->count() == 0) {
    auto* item = new QListWidgetItem(tr("暂无最近工程"), recent);
    item->setFlags(Qt::NoItemFlags);
  }
  root->addWidget(recent, 1);
  auto* loading = new QLabel(&splash);
  loading->setText(tr("准备就绪"));
  loading->setVisible(false);
  root->addWidget(loading);
  auto* progress = new QProgressBar(&splash);
  progress->setRange(0, 0);
  progress->setTextVisible(false);
  progress->setVisible(false);
  root->addWidget(progress);
  auto* actions = new QHBoxLayout;
  actions->setSpacing(8);
  auto* open = new QPushButton(tr("打开工程"), &splash);
  auto* create = new QPushButton(tr("新建工程"), &splash);
  auto* untitled = new QPushButton(tr("暂不需要工程"), &splash);
  auto* about = new QPushButton(tr("关于"), &splash);
  auto* version = new QLabel(tr("版本 %1").arg(QStringLiteral(WDS_APP_VERSION)), &splash);
  version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  actions->addWidget(open);
  actions->addWidget(create);
  actions->addWidget(untitled);
  actions->addStretch(1);
  actions->addWidget(about);
  root->addLayout(actions);
  root->addWidget(version);

  std::function<void(const QString&)> start_load;
  start_load = [this, &splash, recent, open, create, untitled, about, loading,
                progress](const QString& path) {
    if (path.isEmpty()) return;
    splash.setEnabled(false);
    for (auto* button : {open, create, untitled, about}) button->setEnabled(false);
    recent->setEnabled(false);
    loading->setText(tr("正在加载工程…"));
    loading->setVisible(true);
    progress->setVisible(true);
    const std::uint64_t gen = ++open_generation_;
    auto* watcher = new QFutureWatcher<PreparedWdsProject>(&splash);
    track_open_watcher(watcher);
    connect(watcher, &QFutureWatcher<PreparedWdsProject>::finished, &splash,
            [this, &splash, path, recent, open, create, untitled, about,
             loading, progress, watcher, gen] {
              if (gen != open_generation_ || ui_manager_ == nullptr) {
                untrack_open_watcher(watcher);
                watcher->deleteLater();
                return;
              }
              auto future = watcher->future();
              const bool result = ui_manager_->session().apply_prepared_wdsproject(
                  future.takeResult());
              untrack_open_watcher(watcher);
              watcher->deleteLater();
              for (auto* button : {open, create, untitled, about})
                button->setEnabled(true);
              recent->setEnabled(true);
              splash.setEnabled(true);
              loading->setVisible(false);
              progress->setVisible(false);
              if (result) {
                remember_recent_project(path);
                splash.accept();
              } else {
                QMessageBox::warning(&splash, tr("打开失败"),
                                     tr("无法加载所选工程。"));
              }
            });
    const std::string native_path = path.toStdString();
    watcher->setFuture(QtConcurrent::run([native_path] {
      wds::common::install_thread_crash_stack();
      return EditorSession::prepare_wdsproject(native_path);
    }));
  };
  connect(recent, &QListWidget::itemDoubleClicked, &splash,
          [start_load](QListWidgetItem* item) {
            journal_menu_action("splash.recent");
            start_load(item->data(Qt::UserRole).toString());
          });
  connect(open, &QPushButton::clicked, &splash, [this, &splash, start_load] {
    journal_menu_action("splash.open");
    const auto path = QFileDialog::getOpenFileName(&splash, tr("打开 WDS 工程"), {},
                                                   tr("WDS 工程 (*.wdsproject)"));
    start_load(path);
  });
  connect(create, &QPushButton::clicked, &splash, [this, &splash] {
    journal_menu_action("splash.new");
    const QString path = pick_wdsproject_save_path(&splash);
    if (path.isEmpty() || ui_manager_ == nullptr) return;
    if (!ui_manager_->session().create_project(path.toStdString())) {
      QMessageBox::warning(&splash, tr("保存失败"), tr("无法保存工程。"));
      return;
    }
    remember_recent_project(path);
    splash.accept();
  });
  connect(untitled, &QPushButton::clicked, &splash, [this, &splash] {
    journal_menu_action("splash.untitled");
    ui_manager_->session().new_project();
    splash.accept();
  });
  connect(about, &QPushButton::clicked, &splash, [this, &splash] {
    journal_menu_action("splash.about");
    AboutDialog(&splash).exec();
  });
  const int result = splash.exec();
  if (result != QDialog::Accepted) {
    ++open_generation_;
  }
  wait_open_project_watchers();
  if (result == QDialog::Accepted) arm_backup_timer();
  // Closing the startup page means the user chose to exit, since the main
  // window has not been shown yet. The caller owns showing the editor.
  return result == QDialog::Accepted;
}

void EditorMainWindow::maybe_auto_check_updates() {
  if (!auto_check_updates_enabled()) return;
  update_checker_.check(this, false);
}

void EditorMainWindow::remember_recent_project(const QString& path) {
  if (path.isEmpty()) return;
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  auto paths = prefs.value(QStringLiteral("recent/projects")).toStringList();
  paths.removeAll(path);
  paths.prepend(path);
  while (paths.size() > 8) paths.removeLast();
  prefs.setValue(QStringLiteral("recent/projects"), paths);
}

bool EditorMainWindow::eventFilter(QObject* watched, QEvent* event) {
  const auto type = event->type();
  if ((watched == command_toolbar_ || watched == convert_toolbar_) &&
      (type == QEvent::Resize || type == QEvent::Show || type == QEvent::Hide ||
       type == QEvent::ParentChange)) {
    if (watched == command_toolbar_) suppress_qt_toolbar_extension(command_toolbar_);
    if (!command_toolbar_relayouting_) schedule_command_toolbar_relayout();
  }
  if ((type == QEvent::KeyPress || type == QEvent::KeyRelease ||
       type == QEvent::ShortcutOverride) &&
      ui_manager_ != nullptr) {
    auto* key_event = static_cast<QKeyEvent*>(event);
    QWidget* focus = QApplication::focusWidget();
    const bool typing = focus_is_typing_field(focus);
    const bool recording = focus_records_shortcut(focus);
    // macOS often omits Cmd from QKeyEvent/QMouseEvent modifiers. Query the OS
    // so Shift+Cmd curve-fill and Cmd+click stay live even if the canvas
    // never sees the modifier-only KeyPress. Skip while recording a shortcut.
    if (!recording && (type == QEvent::KeyPress || type == QEvent::KeyRelease) &&
        !key_event->isAutoRepeat() && QApplication::activeModalWidget() == nullptr) {
      const auto live = type == QEvent::KeyRelease
                            ? qt_live_modifiers()
                            : qt_modifiers(key_event->modifiers() |
                                           QGuiApplication::queryKeyboardModifiers());
      if (auto* panel = ui_manager_->edit_panel()) panel->sync_active_modifiers(live);
      if (editor_widget_ != nullptr) editor_widget_->update();
    }
    if (recording && (type == QEvent::ShortcutOverride || type == QEvent::KeyPress)) {
      return false;
    }
    if (typing && is_plain_text_or_field_edit_key(key_event)) {
      if (type == QEvent::ShortcutOverride) {
        key_event->accept();
        return true;
      }
      if (type == QEvent::KeyPress) return false;
    }
    if (typing && matches_standard_edit_chord(key_event)) return false;

    const bool playing =
        ui_manager_->session().engine().playback_state() == wds::common::PlaybackState::Playing;
    const auto record_key = qt_key_code(key_event->key());
    const int record_slot = wds::interaction::click_record_slot_for_key(record_key);
    const wds::interaction::KeyDownEvent record_event{
        record_key,
        qt_modifiers(key_event->modifiers() | QGuiApplication::queryKeyboardModifiers()),
        key_event->isAutoRepeat()};
    if (record_slot >= 0 && QApplication::activeModalWidget() == nullptr) {
      const std::uint8_t bit = static_cast<std::uint8_t>(1u << record_slot);
      if (type == QEvent::ShortcutOverride && playing &&
          wds::interaction::click_record_matches(record_event)) {
        key_event->accept();
        return true;
      }
      if (type == QEvent::KeyPress && playing && !key_event->isAutoRepeat() &&
          wds::interaction::click_record_matches(record_event)) {
        if (auto* panel = ui_manager_->edit_panel()) {
          const double now_ms = timeline_now_ms();
          panel->click_record_tap(now_ms);
          if (click_record_held_ == 0) panel->click_record_hold_begin(now_ms);
          click_record_held_ = static_cast<std::uint8_t>(click_record_held_ | bit);
        }
        if (editor_widget_ != nullptr) editor_widget_->update();
        refresh_clear_marks_action();
        return true;
      }
      if (type == QEvent::KeyRelease && !key_event->isAutoRepeat() &&
          (click_record_held_ & bit) != 0) {
        click_record_held_ = static_cast<std::uint8_t>(click_record_held_ & ~bit);
        if (auto* panel = ui_manager_->edit_panel()) {
          if (click_record_held_ == 0) panel->click_record_hold_end(timeline_now_ms());
        }
        if (editor_widget_ != nullptr) editor_widget_->update();
        refresh_clear_marks_action();
        return true;
      }
    }

    if (!key_event->isAutoRepeat()) {
      const wds::interaction::KeyDownEvent command{
          qt_key_code(key_event->key()),
          qt_modifiers(key_event->modifiers() | QGuiApplication::queryKeyboardModifiers()),
          false};
      if (ui_manager_->shortcuts().contains(command)) {
        if (type == QEvent::ShortcutOverride) {
          key_event->accept();
          return true;
        }
        if (type == QEvent::KeyPress) {
          ui_manager_->dispatch_shortcut(command);
          return true;
        }
      }
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void EditorMainWindow::check_chart() {
  const auto ticks = ui_manager_->collect_chart_error_ticks();
  if (ticks.empty()) {
    ui_manager_->set_status("未发现音符重叠", StatusLevel::Info);
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(tr("谱面检查"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* label = new QLabel(&dialog);
  layout->addWidget(label);
  auto* buttons = new QHBoxLayout;
  auto* prev = new QPushButton(tr("上一个"), &dialog);
  auto* next = new QPushButton(tr("下一个"), &dialog);
  auto* close = new QPushButton(tr("关闭"), &dialog);
  buttons->addWidget(prev);
  buttons->addWidget(next);
  buttons->addStretch(1);
  buttons->addWidget(close);
  layout->addLayout(buttons);
  int index = 0;
  const auto show_current = [&] {
    label->setText(tr("共 %1 处音符重叠 — 当前第 %2 处（tick %3）")
                       .arg(ticks.size())
                       .arg(index + 1)
                       .arg(ticks[static_cast<std::size_t>(index)]));
    prev->setEnabled(index > 0);
    next->setEnabled(index + 1 < static_cast<int>(ticks.size()));
    ui_manager_->jump_to_error_tick(ticks[static_cast<std::size_t>(index)]);
  };
  connect(prev, &QPushButton::clicked, &dialog, [&] { if (index > 0) { --index; show_current(); } });
  connect(next, &QPushButton::clicked, &dialog, [&] {
    if (index + 1 < static_cast<int>(ticks.size())) { ++index; show_current(); }
  });
  connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
  dialog.setFixedSize(420, 180);
  show_current();
  dialog.exec();
}

void EditorMainWindow::discard_unsaved_on_close() {
  force_close_ = true;
  persist_chrome_layout_ = false;
  persist_resume_position_ = false;
}

void EditorMainWindow::set_viewport_windows(QWindow* preview, QWindow* editor) {
  preview_window_ = preview;
  editor_window_ = editor;
  preview_dock_ = new QDockWidget(tr("实时预览"), this);
  preview_dock_->setObjectName(QStringLiteral("previewViewportDock"));
  preview_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  preview_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
  // The Vulkan QWindow must fill this dock. Letterboxing a createWindowContainer
  // with setGeometry leaves the Cocoa surface 0×0 / unexposed, so the stage
  // never presents. 16:9 contain lives in resize_preview_viewport instead.
  preview->setMinimumSize(QSize(160, 90));
  auto* preview_container = QWidget::createWindowContainer(preview, preview_dock_);
  preview_container->setMinimumSize(160, 90);
  preview_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  preview_dock_->setContentsMargins(0, 0, 0, 0);
  preview_dock_->setWidget(preview_container);
  preview_dock_->setMinimumHeight(0);
  preview_dock_->setMaximumHeight(QWIDGETSIZE_MAX);
  preview_dock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  addDockWidget(Qt::LeftDockWidgetArea, preview_dock_);
  add_dock_toggle(preview_dock_);
  preview_dock_->installEventFilter(this);

  if (editor == nullptr) return;
  editor_dock_ = new QDockWidget(tr("谱面编辑器"), this);
  editor_dock_->setObjectName(QStringLiteral("editorViewportDock"));
  editor_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  editor_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                            QDockWidget::DockWidgetFloatable |
                            QDockWidget::DockWidgetClosable);
  auto* editor_container = QWidget::createWindowContainer(editor, editor_dock_);
  editor_container->setMinimumSize(360, 300);
  editor_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  editor_dock_->setWidget(editor_container);
  splitDockWidget(preview_dock_, editor_dock_, Qt::Horizontal);
  add_dock_toggle(editor_dock_);
}

void EditorMainWindow::set_editor_widget(QWidget* editor) {
  editor_widget_ = editor;
  editor_dock_ = new QDockWidget(tr("谱面编辑器"), this);
  // Preserve the stable object name: QMainWindow's saved state keys docks by
  // objectName, independent of whether their viewport is Vulkan or QWidget.
  editor_dock_->setObjectName(QStringLiteral("editorViewportDock"));
  editor_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  editor_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                            QDockWidget::DockWidgetClosable);
  editor_dock_->setWidget(editor);
  addDockWidget(Qt::LeftDockWidgetArea, editor_dock_);
  if (preview_dock_) splitDockWidget(preview_dock_, editor_dock_, Qt::Horizontal);
  add_dock_toggle(editor_dock_);
  // bind_ui_manager runs before the application supplies its viewport widgets,
  // so restore the dock state only now, after every named dock exists.
  restore_or_reset_layout();
}

void EditorMainWindow::resizeEvent(QResizeEvent* event) {
  if (auto* window = static_cast<RealtimeVulkanWindow*>(preview_window_))
    window->set_resize_suspended(true);
  if (auto* window = static_cast<RealtimeVulkanWindow*>(editor_window_))
    window->set_resize_suspended(true);
  QMainWindow::resizeEvent(event);
  resize_settle_timer_.start();
  relayout_command_toolbar();
}

bool EditorMainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
  const auto* msg = static_cast<MSG*>(message);
  if (msg->message == WM_ENTERSIZEMOVE) {
    native_resizing_ = true;
    for (auto* window : {preview_window_, editor_window_})
      if (window) static_cast<RealtimeVulkanWindow*>(window)->set_resize_suspended(true);
  } else if (msg->message == WM_EXITSIZEMOVE) {
    native_resizing_ = false;
    resize_settle_timer_.start();
  }
#endif
  return QMainWindow::nativeEvent(eventType, message, result);
}

void EditorMainWindow::confirm_pending_changes(std::function<void()> on_proceed) {
  if (!on_proceed) return;
  if (ui_manager_ == nullptr || !ui_manager_->session().dirty()) {
    on_proceed();
    return;
  }
  QMessageBox box(QMessageBox::Warning, tr("未保存的修改"),
                  tr("当前工程有未保存的修改。"), QMessageBox::NoButton, this);
  auto* save = box.addButton(tr("保存"), QMessageBox::AcceptRole);
  auto* discard = box.addButton(tr("不保存"), QMessageBox::DestructiveRole);
  box.addButton(tr("取消"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == save) {
    save_project_then([this, on_proceed = std::move(on_proceed)] {
      if (ui_manager_ != nullptr && !ui_manager_->session().dirty()) on_proceed();
    });
    return;
  }
  if (box.clickedButton() == discard) on_proceed();
}

void EditorMainWindow::open_project() {
  confirm_pending_changes([this] {
    const auto path = QFileDialog::getOpenFileName(this, tr("打开 WDS 工程"), {},
                                                    tr("WDS 工程 (*.wdsproject)"));
    if (path.isEmpty() || ui_manager_ == nullptr) return;
    // Parse off the GUI thread; the overlay stays up and the preview keeps
    // ticking while the worker runs. Apply happens on the GUI thread in
    // `finished`, guarded by open_generation_ so an older load cannot
    // overwrite a newer one.
    auto overlay = std::make_shared<BusyScope>(this, tr("正在打开工程…"));
    const std::uint64_t gen = ++open_generation_;
    auto* watcher = new QFutureWatcher<PreparedWdsProject>(this);
    track_open_watcher(watcher);
    connect(watcher, &QFutureWatcher<PreparedWdsProject>::finished, this,
            [this, path, watcher, gen, overlay]() mutable {
              untrack_open_watcher(watcher);
              watcher->deleteLater();
              if (gen != open_generation_ || ui_manager_ == nullptr) {
                overlay.reset();
                return;
              }
              auto future = watcher->future();
              const bool ok =
                  ui_manager_->session().apply_prepared_wdsproject(future.takeResult());
              overlay.reset();
              if (!ok) {
                QMessageBox::warning(this, tr("打开失败"), tr("无法打开所选工程。"));
              } else {
                remember_recent_project(path);
                clear_click_marks();
                arm_backup_timer();
              }
            });
    const std::string native_path = path.toStdString();
    watcher->setFuture(QtConcurrent::run([native_path] {
      wds::common::install_thread_crash_stack();
      return EditorSession::prepare_wdsproject(native_path);
    }));
  });
}

void EditorMainWindow::save_project() { save_project_then({}); }

QString EditorMainWindow::pick_wdsproject_save_path(QWidget* parent) {
  QString path = QFileDialog::getSaveFileName(parent, tr("保存 WDS 工程"),
                                              QStringLiteral("untitled.wdsproject"),
                                              tr("WDS 工程 (*.wdsproject)"));
  if (path.isEmpty()) return {};
  if (!path.endsWith(QStringLiteral(".wdsproject"), Qt::CaseInsensitive)) {
    const QFileInfo info(path);
    path = info.dir().filePath(info.completeBaseName() + QStringLiteral(".wdsproject"));
  }
  return path;
}

void EditorMainWindow::save_project_then(std::function<void()> done) {
  if (ui_manager_ == nullptr) return;
  auto& session = ui_manager_->session();
  if (session.read_only()) {
    QMessageBox::warning(this, tr("只读"), tr("只读预览无法保存。"));
    return;
  }
  QString path = QString::fromStdString(session.project_path());
  if (path.isEmpty()) {
    path = pick_wdsproject_save_path(this);
  }
  if (path.isEmpty()) return;
  auto ok = std::make_shared<bool>(false);
  run_with_busy(
      tr("正在保存工程…"),
      [this, path, ok] {
        if (ui_manager_ == nullptr) return;
        auto& session = ui_manager_->session();
        *ok = session.project_path().empty() ? session.save_as(path.toStdString())
                                             : session.save();
      },
      [this, path, ok, done = std::move(done)] {
        if (!*ok) {
          QMessageBox::warning(this, tr("保存失败"), tr("无法保存工程。"));
        } else {
          remember_recent_project(path);
          arm_backup_timer();
        }
        if (*ok && done) done();
      });
}

void EditorMainWindow::import_chart() {
  confirm_pending_changes([this] {
    const auto path = QFileDialog::getOpenFileName(this, tr("导入官方谱面"), {},
                                                    tr("谱面文件 (*.csv *.sus)"));
    if (path.isEmpty() || ui_manager_ == nullptr) return;
    auto ok = std::make_shared<bool>(false);
    run_with_busy(
        tr("正在导入谱面…"),
        [this, path, ok] {
          if (ui_manager_ == nullptr) return;
          *ok = ui_manager_->session().import_official(path.toStdString());
        },
        [this, ok] {
          if (!*ok) QMessageBox::warning(this, tr("导入失败"), tr("无法导入所选谱面。"));
          else arm_backup_timer();
        });
  });
}

void EditorMainWindow::import_music() {
  const auto path = QFileDialog::getOpenFileName(this, tr("导入音乐"), {},
                                                  tr("音频文件 (*.ogg *.wav *.mp3)"));
  if (path.isEmpty() || ui_manager_ == nullptr) return;
  auto ok = std::make_shared<bool>(false);
  run_with_busy(
      tr("正在导入音乐…"),
      [this, path, ok] {
        if (ui_manager_ == nullptr) return;
        *ok = ui_manager_->session().import_music(path.toStdString());
      },
      [this, ok] {
        if (!*ok) QMessageBox::warning(this, tr("导入失败"), tr("无法导入所选音乐。"));
      });
}

void EditorMainWindow::import_jacket() {
  const auto path = QFileDialog::getOpenFileName(this, tr("导入曲绘"), {},
                                                  tr("图片文件 (*.jpg *.jpeg *.png)"));
  if (path.isEmpty() || ui_manager_ == nullptr) return;
  auto ok = std::make_shared<bool>(false);
  run_with_busy(
      tr("正在导入曲绘…"),
      [this, path, ok] {
        if (ui_manager_ == nullptr) return;
        *ok = ui_manager_->session().import_jacket(path.toStdString());
      },
      [this, ok] {
        if (!*ok) QMessageBox::warning(this, tr("导入失败"), tr("无法导入所选曲绘。"));
      });
}

void EditorMainWindow::add_chart() {
  if (ui_manager_ == nullptr || ui_manager_->session().read_only()) return;
  QMessageBox box(QMessageBox::Question, tr("添加谱面"), tr("新建空白谱面，还是添加已有谱面文件？"), QMessageBox::NoButton, this);
  auto* create = box.addButton(tr("新建"), QMessageBox::AcceptRole);
  auto* existing = box.addButton(tr("添加已有"), QMessageBox::ActionRole);
  box.addButton(tr("取消"), QMessageBox::RejectRole);
  box.exec();
  const bool make_blank = box.clickedButton() == create;
  const bool add_existing = box.clickedButton() == existing;
  if (!make_blank && !add_existing) return;
  auto proceed = [this, make_blank] {
    if (ui_manager_ == nullptr) return;
    if (make_blank) {
      if (!ui_manager_->session().add_chart()) {
        QMessageBox::warning(this, tr("添加失败"), tr("无法添加谱面。"));
      }
      return;
    }
    const auto path = QFileDialog::getOpenFileName(this, tr("添加谱面"), {}, tr("WDS 谱面 (*.wdschart)"));
    if (path.isEmpty()) return;
    if (!ui_manager_->session().add_chart_from_file(path.toStdString())) {
      QMessageBox::warning(this, tr("添加失败"), tr("无法添加所选谱面。"));
    }
  };
  if (ui_manager_->session().project_path().empty()) {
    save_project_then(std::move(proceed));
    return;
  }
  proceed();
}

void EditorMainWindow::export_chart() {
  auto& session = ui_manager_->session();
  if (session.read_only()) return;
  // Format + scope choice, matching the old export dialog.
  QDialog dialog(this);
  dialog.setWindowTitle(tr("导出"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* formatRow = new QHBoxLayout;
  auto* format_label = new QLabel(tr("格式"), &dialog);
  format_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  formatRow->addWidget(format_label, 0, Qt::AlignVCenter);
  auto* format = new QComboBox(&dialog);
  format->addItems({tr("官方 CSV"), tr("SUS")});
  formatRow->addWidget(format, 1, Qt::AlignVCenter);
  layout->addLayout(formatRow);
  auto* project = new QPushButton(tr("导出整个项目"), &dialog);
  auto* chart = new QPushButton(tr("仅导出当前谱面"), &dialog);
  auto* cancel = new QPushButton(tr("取消"), &dialog);
  layout->addWidget(project);
  layout->addWidget(chart);
  layout->addWidget(cancel);
  dialog.setFixedSize(360, 180);
  enum class Choice { None, Project, Chart };
  Choice choice = Choice::None;
  connect(project, &QPushButton::clicked, &dialog, [&] { choice = Choice::Project; dialog.accept(); });
  connect(chart, &QPushButton::clicked, &dialog, [&] { choice = Choice::Chart; dialog.accept(); });
  connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted || choice == Choice::None) return;
  const bool sus = format->currentIndex() == 1;
  if (choice == Choice::Project) {
    const auto dir = QFileDialog::getExistingDirectory(this, tr("导出项目"));
    if (dir.isEmpty()) return;
    const bool ok = sus ? session.export_sus_project(dir.toStdString())
                        : session.export_official_project(dir.toStdString());
    if (!ok) QMessageBox::warning(this, tr("导出失败"), tr("无法导出项目。"));
    return;
  }
  QString name = QString::fromStdString(
      session.official_chart_filename(session.active_chart_index()));
  if (sus) name = QFileInfo(name).completeBaseName() + QStringLiteral(".sus");
  const auto path = QFileDialog::getSaveFileName(
      this, tr("导出谱面"), name, sus ? tr("SUS 谱面 (*.sus)") : tr("CSV 谱面 (*.csv)"));
  if (path.isEmpty()) return;
  const bool ok = sus ? session.export_sus(path.toStdString())
                      : session.export_official(path.toStdString());
  if (!ok) QMessageBox::warning(this, tr("导出失败"), tr("无法导出谱面。"));
}

void EditorMainWindow::closeEvent(QCloseEvent* event) {
  if (busy_active_ && !force_close_) {
    event->ignore();
    return;
  }
  if (!force_close_ && ui_manager_ != nullptr && ui_manager_->session().dirty()) {
    event->ignore();
    confirm_pending_changes([this] {
      force_close_ = true;
      close();
    });
    return;
  }
  save_chrome_layout();
  ++open_generation_;
  wait_open_project_watchers();
  save_resume_position_on_exit();
  event->accept();
}

void EditorMainWindow::save_resume_position_on_exit() {
  // Normal exit always records the chart + tick being edited so the next open
  // jumps straight back. Skipped for automation closes (smoke tests,
  // --quit-after) that must not rewrite the user's project entry.
  if (!persist_resume_position_ || resume_position_saved_ || ui_manager_ == nullptr) return;
  resume_position_saved_ = true;
  auto& session = ui_manager_->session();
  if (session.project_path().empty() || session.read_only()) return;
  if (!session.save_resume_position()) {
    WDS_LOG("exit: failed to save resume position for %s\n",
            session.project_path().c_str());
  }
}

void EditorMainWindow::hideEvent(QHideEvent* event) {
  QMainWindow::hideEvent(event);
  // restoreState() shows floating docks/toolbars as top-level windows even
  // while this shell is still hidden for the startup splash. Keep them
  // concealed until the editor chrome is revealed.
  if (!chrome_revealed_) {
    conceal_unrevealed_floating_chrome();
    return;
  }
  // After the editor is up, floating chrome stays visible when the main
  // shell is temporarily hidden (for example while switching apps).
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr && dock->isFloating()) dock->show();
  }
  for (auto* bar : {command_toolbar_, convert_toolbar_}) {
    if (bar != nullptr && bar->isFloating()) bar->show();
  }
}

void EditorMainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  reveal_concealed_floating_chrome();
  schedule_command_toolbar_relayout();
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr && dock->isFloating() && !dock->isHidden()) dock->raise();
  }
  for (auto* bar : {command_toolbar_, convert_toolbar_}) {
    if (bar != nullptr && bar->isFloating() && !bar->isHidden()) bar->raise();
  }
}
}

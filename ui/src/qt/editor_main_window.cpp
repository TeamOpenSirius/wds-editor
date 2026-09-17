#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/playback_dock.hpp"
#include "wds/ui/qt/qt_input_adapter.hpp"
#include "wds/ui/qt/settings_dialog.hpp"
#include "wds/ui/qt/curve_templates_panel.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/qt/about_dialog.hpp"
#include "wds/ui/qt/busy_dialog.hpp"
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
#include <QLabel>
#include <QPalette>
#include <QColor>
#include <QSizePolicy>
#include <QWindow>
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

  switch (event->key()) {
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Space:
      return true;
    default:
      break;
  }

  const int key = event->key();
  if (key >= Qt::Key_A && key <= Qt::Key_Z) return true;
  if (key >= Qt::Key_0 && key <= Qt::Key_9) return true;
  // Remaining US-ASCII punctuation (',' '!' etc.). Letters/digits already matched.
  if (key >= 0x21 && key <= 0x7e) return true;

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
  command_toolbar_ = addToolBar(tr("功能区"));
  command_toolbar_->setObjectName(QStringLiteral("editorToolBar"));
  command_toolbar_->setIconSize(QSize(22, 22));
  command_toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  command_toolbar_->setMovable(true);
  command_toolbar_->setFloatable(true);
  command_toolbar_->setAllowedAreas(Qt::AllToolBarAreas);
  command_toolbar_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  // Theme QToolBar spacing is ignored or stacked; pin item spacing to 0 so the
  // 4px spacers are the only gap (same as the convert ribbon).
  command_toolbar_->setStyleSheet(QStringLiteral("QToolBar#editorToolBar { spacing: 0px; }"));
  // File/edit commands, then convert. A trailing gap keeps the convert ribbon
  // from sitting flush against the last command button.
  bool first_command = true;
  auto addCommand = [this, &first_command](QMenu* menu, const QString& text,
                                           const QKeySequence& shortcut) {
    if (!first_command) {
      auto* gap = new QWidget(command_toolbar_);
      gap->setFixedWidth(4);
      gap->setAttribute(Qt::WA_TransparentForMouseEvents);
      command_toolbar_->addWidget(gap);
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
    command_toolbar_->addAction(action);
    return action;
  };
  open_action_ = addCommand(fileMenu, tr("打开工程"), QKeySequence::Open);
  save_action_ = addCommand(fileMenu, tr("保存工程"), QKeySequence::Save);
  music_action_ = addCommand(fileMenu, tr("导入音乐"), {});
  import_action_ = addCommand(fileMenu, tr("导入谱面"), {});
  export_action_ = addCommand(fileMenu, tr("导出谱面"), {});
  check_action_ = addCommand(fileMenu, tr("检查谱面"), {});
  undo_action_ = addCommand(editMenu, tr("撤销"), QKeySequence::Undo);
  redo_action_ = addCommand(editMenu, tr("重做"), QKeySequence::Redo);
  if (auto* layout = command_toolbar_->layout()) layout->setSpacing(0);
  apply_command_icons();
  auto* convert_gap = new QWidget(command_toolbar_);
  convert_gap->setObjectName(QStringLiteral("commandConvertGap"));
  convert_gap->setFixedWidth(48);
  convert_gap->setAttribute(Qt::WA_TransparentForMouseEvents);
  command_toolbar_->addWidget(convert_gap);
  convert_toolbar_ = addToolBar(tr("转换"));
  convert_toolbar_->setObjectName(QStringLiteral("convertToolBar"));
  convert_toolbar_->setIconSize(QSize(26, 26));
  convert_toolbar_->setMovable(true);
  convert_toolbar_->setFloatable(true);
  convert_toolbar_->setAllowedAreas(Qt::AllToolBarAreas);
  convert_toolbar_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  // AboutRole only takes effect when the action lives in a QMenu.
  // menuBar()->addAction() makes a top-level title and macOS ignores the role.
  about_action_ = new QAction(tr("关于"), this);
  about_action_->setIcon(fluent_icon(fluent::Info));
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
      std::max(180, preview_content_height_for_width(left_w) + 28);
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
}

void EditorMainWindow::restore_or_reset_layout() {
  if (preview_dock_ == nullptr || editor_dock_ == nullptr || playback_dock_ == nullptr ||
      toolbar_dock_ == nullptr || curve_dock_ == nullptr || settings_dock_ == nullptr) {
    return;
  }
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  const auto state = prefs.value("window/state-v16").toByteArray();
  if (state.isEmpty() || !restoreState(state)) reset_default_layout();
}

void EditorMainWindow::create_control_docks() {
  playback_dock_ = new QDockWidget(tr("播放"), this);
  playback_dock_->setObjectName(QStringLiteral("playbackAudioDock"));
  playback_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  playback_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
  playback_panel_ = new PlaybackBar(ui_manager_, playback_dock_);
  playback_dock_->setWidget(playback_panel_);
  playback_dock_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  playback_dock_->setMinimumHeight(0);
  playback_dock_->setMaximumHeight(QWIDGETSIZE_MAX);
  addDockWidget(Qt::BottomDockWidgetArea, playback_dock_);
  add_dock_toggle(playback_dock_);

  convert_bar_ = new ConvertBar(ui_manager_, skins_dir_, convert_toolbar_);
  convert_bar_->set_ribbon_mode();
  if (convert_toolbar_ != nullptr) convert_toolbar_->addWidget(convert_bar_);

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
  settings_panel_->set_on_applied([this] { sync_toolbox_place_checks(); });
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
  const auto icon_for = [&](const char* stem, char32_t fallback) {
    const QIcon svg = themed_svg_icon(dir.filePath(QString::fromLatin1(stem) + QStringLiteral(".svg")));
    return svg.isNull() ? fluent_icon(fallback) : svg;
  };
  if (open_action_ != nullptr) open_action_->setIcon(icon_for("open", fluent::OpenFolder));
  if (save_action_ != nullptr) save_action_->setIcon(icon_for("save", fluent::Save));
  if (undo_action_ != nullptr) undo_action_->setIcon(icon_for("undo", fluent::Undo));
  if (redo_action_ != nullptr) redo_action_->setIcon(icon_for("redo", fluent::Redo));
  if (import_action_ != nullptr) import_action_->setIcon(icon_for("import", fluent::Import));
  if (export_action_ != nullptr) export_action_->setIcon(icon_for("export", fluent::Export));
  if (music_action_ != nullptr) music_action_->setIcon(icon_for("import-audio", fluent::Music));
  if (check_action_ != nullptr) check_action_->setIcon(icon_for("check", fluent::Checklist));
}

void EditorMainWindow::refresh_history_actions() {
  if (ui_manager_ == nullptr || undo_action_ == nullptr || redo_action_ == nullptr) return;
  const auto& history = ui_manager_->session().engine().history();
  undo_action_->setEnabled(history.can_undo());
  redo_action_->setEnabled(history.can_redo());
}

void EditorMainWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  if (event != nullptr && (event->type() == QEvent::PaletteChange ||
                           event->type() == QEvent::ApplicationPaletteChange)) {
    apply_command_icons();
  }
}

void EditorMainWindow::on_preview_frame() {
  if (ui_manager_ == nullptr) return;
  if (playback_panel_ != nullptr) playback_panel_->sync_position();
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
  connect(music_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.import_music");
    import_music();
  });
  connect(check_action_, &QAction::triggered, this, [this] {
    journal_menu_action("menu.check_chart");
    check_chart();
  });

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
  if (ui_manager_ != nullptr) {
    ui_manager_->set_ui_change_handler({});
  }
  ui_manager_ = nullptr;
}

void EditorMainWindow::handle_ui_change(UiChange change) {
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
      break;
    case UiChange::History:
      refresh_history_actions();
      break;
    case UiChange::Document:
      refresh_window_title();
      break;
    case UiChange::PlaceTool:
      sync_toolbox_place_checks();
      break;
  }
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
  open_watchers_.erase(std::remove(open_watchers_.begin(), open_watchers_.end(), watcher),
                       open_watchers_.end());
}

void EditorMainWindow::wait_open_project_watchers() {
  const auto watchers = open_watchers_;
  for (auto* obj : watchers) {
    if (obj == nullptr) continue;
    static_cast<QFutureWatcher<PreparedWdsProject>*>(obj)->waitForFinished();
  }
}

bool EditorMainWindow::show_startup_splash() {
  if (ui_manager_ == nullptr || !ui_manager_->session().project_path().empty()) return true;
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
  open->setIcon(fluent_icon(fluent::OpenFolder));
  auto* create = new QPushButton(tr("新建工程"), &splash);
  create->setIcon(fluent_icon(fluent::Add));
  auto* about = new QPushButton(tr("关于"), &splash);
  about->setIcon(fluent_icon(fluent::Info));
  auto* version = new QLabel(tr("版本 %1").arg(QStringLiteral(WDS_APP_VERSION)), &splash);
  version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  actions->addWidget(open);
  actions->addWidget(create);
  actions->addStretch(1);
  actions->addWidget(about);
  root->addLayout(actions);
  root->addWidget(version);

  std::function<void(const QString&)> start_load;
  start_load = [this, &splash, recent, open, create, about, loading,
                progress](const QString& path) {
    if (path.isEmpty()) return;
    splash.setEnabled(false);
    for (auto* button : {open, create, about}) button->setEnabled(false);
    recent->setEnabled(false);
    loading->setText(tr("正在加载工程…"));
    loading->setVisible(true);
    progress->setVisible(true);
    const std::uint64_t gen = ++open_generation_;
    auto* watcher = new QFutureWatcher<PreparedWdsProject>(&splash);
    track_open_watcher(watcher);
    connect(watcher, &QFutureWatcher<PreparedWdsProject>::finished, &splash,
            [this, &splash, path, recent, open, create, about,
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
              for (auto* button : {open, create, about})
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
  // Closing the startup page means the user chose to exit, since the main
  // window has not been shown yet. The caller owns showing the editor.
  return result == QDialog::Accepted;
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

void EditorMainWindow::save_project_then(std::function<void()> done) {
  if (ui_manager_ == nullptr) return;
  auto& session = ui_manager_->session();
  if (session.read_only()) {
    QMessageBox::warning(this, tr("只读"), tr("只读预览无法保存。"));
    return;
  }
  QString path = QString::fromStdString(session.project_path());
  if (path.isEmpty()) {
    path = QFileDialog::getSaveFileName(this, tr("保存 WDS 工程"),
                                        QStringLiteral("untitled.wdsproject"),
                                        tr("WDS 工程 (*.wdsproject)"));
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

void EditorMainWindow::add_chart() {
  if (ui_manager_->session().read_only()) return;
  QMessageBox box(QMessageBox::Question, tr("添加谱面"), tr("新建空白谱面，还是添加已有谱面文件？"), QMessageBox::NoButton, this);
  auto* create = box.addButton(tr("新建"), QMessageBox::AcceptRole);
  auto* existing = box.addButton(tr("添加已有"), QMessageBox::ActionRole);
  box.addButton(tr("取消"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == create) { ui_manager_->session().add_chart(); return; }
  if (box.clickedButton() == existing) {
    const auto path = QFileDialog::getOpenFileName(this, tr("添加谱面"), {}, tr("WDS 谱面 (*.wdschart)"));
    if (!path.isEmpty()) ui_manager_->session().add_chart_from_file(path.toStdString());
  }
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
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  prefs.setValue("window/geometry", saveGeometry());
  prefs.setValue("window/state-v16", saveState());
  ++open_generation_;
  wait_open_project_watchers();
  event->accept();
}

void EditorMainWindow::hideEvent(QHideEvent* event) {
  QMainWindow::hideEvent(event);
  // Floating docks are independent top-level windows. Keep them visible when
  // the main shell is temporarily hidden (for example while switching apps).
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr && dock->isFloating()) dock->show();
  }
}

void EditorMainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  for (auto* dock : chrome_docks()) {
    if (dock != nullptr && dock->isFloating() && !dock->isHidden()) dock->raise();
  }
}
}

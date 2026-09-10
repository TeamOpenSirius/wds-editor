#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/flow_layout.hpp"
#include "wds/ui/qt/note_icons.hpp"
#include "wds/ui/qt/playback_dock.hpp"
#include "wds/ui/qt/settings_dialog.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/qt/about_dialog.hpp"
#include "wds/ui/qt/busy_dialog.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/core/chart_editor_engine.hpp"
#include <QCloseEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QDockWidget>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QPushButton>
#include <QMenu>
#include <QToolBar>
#include <QToolButton>
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
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QKeyEvent>
#include <QAbstractSpinBox>
#include <QAbstractItemView>
#include <QKeySequenceEdit>
#include <QApplication>
#include <QProgressBar>
#include <QMetaObject>
#include <QSizePolicy>
#include <wds/interaction/editor_input.hpp>
#include "wds/ui/curve_template.hpp"
#include <algorithm>
#include <array>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

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
  menuBar()->addMenu(tr("文件"));
  menuBar()->addMenu(tr("编辑"));
  menuBar()->addMenu(tr("视图"));
  auto* fileMenu = menuBar()->actions().at(0)->menu();
  auto* editMenu = menuBar()->actions().at(1)->menu();
  auto* viewMenu = menuBar()->actions().at(2)->menu();
  auto* toolbar = addToolBar(tr("功能区"));
  toolbar->setObjectName(QStringLiteral("editorToolBar"));
  toolbar->setIconSize(QSize(20, 20));
  toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  // Same eight commands as the old toolbar; undo/redo live in the 编辑 menu.
  auto addCommand = [this, fileMenu, toolbar](const QString& text, const QKeySequence& shortcut,
                                              const QString& tooltip) {
    auto* action = new QAction(text, this);
    action->setShortcut(shortcut);
    action->setToolTip(tooltip);
    fileMenu->addAction(action);
    toolbar->addAction(action);
    return action;
  };
  open_action_ = addCommand(tr("打开工程"), QKeySequence::Open, tr("打开 WDS 工程"));
  save_action_ = addCommand(tr("保存工程"), QKeySequence::Save, tr("保存当前工程"));
  import_action_ = addCommand(tr("导入谱面"), QKeySequence(), tr("导入官方谱面（只读）"));
  export_action_ = addCommand(tr("导出"), QKeySequence(), tr("导出谱面 / 项目"));
  music_action_ = addCommand(tr("导入音乐"), QKeySequence(), tr("导入 ogg / wav / mp3 音乐"));
  curve_templates_action_ = addCommand(tr("曲线模板"), QKeySequence(), tr("编辑曲线模板"));
  check_action_ = addCommand(tr("检查谱面"), QKeySequence(), tr("检查谱面错误"));
  open_action_->setIcon(fluent_icon(fluent::OpenFolder));
  save_action_->setIcon(fluent_icon(fluent::Save));
  import_action_->setIcon(fluent_icon(fluent::Import));
  export_action_->setIcon(fluent_icon(fluent::Export));
  music_action_->setIcon(fluent_icon(fluent::Music));
  curve_templates_action_->setIcon(curve_template_icon());
  check_action_->setIcon(fluent_icon(fluent::Checklist));
  // 设置 lives in the top menu bar, not the toolbar.
  settings_action_ = new QAction(tr("设置"), this);
  settings_action_->setToolTip(tr("编辑器设置"));
  settings_action_->setIcon(fluent_icon(fluent::Settings));
  menuBar()->addAction(settings_action_);
  about_action_ = new QAction(tr("关于"), this);
  about_action_->setIcon(fluent_icon(fluent::Info));
  menuBar()->addAction(about_action_);
  connect(about_action_, &QAction::triggered, this, [this] {
    AboutDialog(this).exec();
  });

  undo_action_ = new QAction(tr("撤销"), this);
  undo_action_->setShortcut(QKeySequence::Undo);
  undo_action_->setIcon(fluent_icon(fluent::Undo));
  redo_action_ = new QAction(tr("重做"), this);
  redo_action_->setShortcut(QKeySequence::Redo);
  redo_action_->setIcon(fluent_icon(fluent::Redo));
  editMenu->addAction(undo_action_);
  editMenu->addAction(redo_action_);
  statusBar()->showMessage(tr("就绪"));

  settings_dock_ = new QDockWidget(tr("属性 / 设置"), this);
  auto* settingsDock = settings_dock_;
  settingsDock->setObjectName(QStringLiteral("previewSettingsDock"));
  auto* settingsPanel = new QWidget(settingsDock);
  auto* form = new QFormLayout(settingsPanel);
  auto* speed = new QDoubleSpinBox(settingsPanel);
  speed->setRange(1.0, 15.0); speed->setValue(5.0); speed->setSingleStep(0.1);
  speed->setObjectName(QStringLiteral("noteSpeed"));
  form->addRow(tr("音符速度"), speed);
  auto* offset = new QSpinBox(settingsPanel);
  offset->setRange(0, 100); offset->setSingleStep(5);
  offset->setObjectName(QStringLiteral("noteStartOffset"));
  form->addRow(tr("起始偏移"), offset);
  auto* noteHeight = new QSpinBox(settingsPanel);
  noteHeight->setRange(1, 10);
  noteHeight->setObjectName(QStringLiteral("noteHeightLevel"));
  form->addRow(tr("音符厚度"), noteHeight);
  auto* splitOpacity = new QSpinBox(settingsPanel);
  splitOpacity->setRange(10, 100); splitOpacity->setSingleStep(10);
  splitOpacity->setObjectName(QStringLiteral("splitLineOpacity"));
  form->addRow(tr("分割线透明度"), splitOpacity);
  auto* lanes = new QSpinBox(settingsPanel);
  lanes->setRange(1, 32); lanes->setValue(12);
  lanes->setObjectName(QStringLiteral("laneCount"));
  form->addRow(tr("轨道数量"), lanes);
  // Scroll container keeps the form usable at any dock size / orientation.
  auto* settingsScroll = new QScrollArea(settingsDock);
  settingsScroll->setWidgetResizable(true);
  settingsScroll->setFrameShape(QFrame::NoFrame);
  settingsScroll->setWidget(settingsPanel);
  settingsDock->setWidget(settingsScroll);
  settingsDock->setMinimumWidth(200);
  viewMenu->addAction(settingsDock->toggleViewAction());
  auto* fullscreen = viewMenu->addAction(tr("全屏"));
  fullscreen->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F11));
  connect(fullscreen, &QAction::triggered, this, [this] {
    isFullScreen() ? showNormal() : showFullScreen();
  });
  auto* reset = viewMenu->addAction(tr("重置布局"));
  connect(reset, &QAction::triggered, this, [this] {
    QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
    prefs.remove("window/geometry");
    prefs.remove("window/state-v4");
    reset_default_layout();
  });

  connect(speed, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    if (ui_manager_) ui_manager_->set_preview_note_speed(value);
  });
  auto applyDisplay = [this, speed, offset, noteHeight, splitOpacity] {
    if (!ui_manager_) return;
    ui_manager_->chart_preview().apply_display_settings(speed->value(), offset->value(),
                                                         noteHeight->value(), splitOpacity->value());
    ui_manager_->request_save_ui_config(false);
  };
  connect(offset, qOverload<int>(&QSpinBox::valueChanged), this, [applyDisplay](int) { applyDisplay(); });
  connect(noteHeight, qOverload<int>(&QSpinBox::valueChanged), this, [applyDisplay](int) { applyDisplay(); });
  connect(splitOpacity, qOverload<int>(&QSpinBox::valueChanged), this, [applyDisplay](int) { applyDisplay(); });
  connect(lanes, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (ui_manager_) ui_manager_->set_preview_lane_count(value);
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

void EditorMainWindow::reset_default_layout() {
  if (preview_dock_ == nullptr || editor_dock_ == nullptr) return;
  const std::array<QDockWidget*, 6> docks = {preview_dock_,  editor_dock_, settings_dock_,
                                             playback_dock_, audio_dock_,  toolbox_dock_};
  for (auto* dock : docks) {
    if (dock != nullptr) removeDockWidget(dock);
  }
  // Top row: preview | editor | 属性 (属性 pinned to its minimum on the right,
  // preview/editor split the rest evenly). Bottom row: 播放 | 音频 | 工具箱.
  addDockWidget(Qt::LeftDockWidgetArea, preview_dock_);
  splitDockWidget(preview_dock_, editor_dock_, Qt::Horizontal);
  if (settings_dock_ != nullptr) splitDockWidget(editor_dock_, settings_dock_, Qt::Horizontal);
  if (playback_dock_ != nullptr) addDockWidget(Qt::BottomDockWidgetArea, playback_dock_);
  if (playback_dock_ != nullptr && audio_dock_ != nullptr)
    splitDockWidget(playback_dock_, audio_dock_, Qt::Horizontal);
  if (audio_dock_ != nullptr && toolbox_dock_ != nullptr)
    splitDockWidget(audio_dock_, toolbox_dock_, Qt::Horizontal);
  for (auto* dock : docks) {
    if (dock != nullptr) dock->show();
  }
  const int side = settings_dock_ != nullptr ? settings_dock_->minimumWidth() : 0;
  const int half = std::max(200, (width() - side) / 2);
  if (settings_dock_ != nullptr)
    resizeDocks({preview_dock_, editor_dock_, settings_dock_}, {half, half, side},
                Qt::Horizontal);
  if (playback_dock_ != nullptr && audio_dock_ != nullptr && toolbox_dock_ != nullptr) {
    const int w = std::max(600, width());
    resizeDocks({playback_dock_, audio_dock_, toolbox_dock_},
                {w * 2 / 5, w / 5, w * 2 / 5}, Qt::Horizontal);
  }
  QTimer::singleShot(0, this, [this] {
    for (auto* dock : {playback_dock_, audio_dock_, toolbox_dock_}) {
      update_control_dock_height(dock);
    }
  });
}

void EditorMainWindow::create_playback_and_toolbox_docks() {
  auto* viewMenu = menuBar()->actions().at(2)->menu();

  playback_dock_ = new QDockWidget(tr("播放"), this);
  playback_dock_->setObjectName(QStringLiteral("playbackAudioDock"));
  playback_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  playback_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
  playback_panel_ = new PlaybackAudioPanel(ui_manager_, playback_dock_);
  playback_panel_->set_add_chart_handler([this] { add_chart(); });
  playback_dock_->setWidget(playback_panel_);
  addDockWidget(Qt::BottomDockWidgetArea, playback_dock_);
  connect(playback_dock_, &QDockWidget::dockLocationChanged, this,
          [this](Qt::DockWidgetArea) { update_control_dock_height(playback_dock_); });
  connect(playback_dock_, &QDockWidget::topLevelChanged, this,
          [this](bool) { update_control_dock_height(playback_dock_); });
  viewMenu->addAction(playback_dock_->toggleViewAction());

  audio_dock_ = new QDockWidget(tr("音频"), this);
  audio_dock_->setObjectName(QStringLiteral("audioMixDock"));
  audio_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  audio_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
  audio_dock_->setWidget(new AudioMixPanel(ui_manager_, audio_dock_));
  addDockWidget(Qt::BottomDockWidgetArea, audio_dock_);
  connect(audio_dock_, &QDockWidget::dockLocationChanged, this,
          [this](Qt::DockWidgetArea) { update_control_dock_height(audio_dock_); });
  connect(audio_dock_, &QDockWidget::topLevelChanged, this,
          [this](bool) { update_control_dock_height(audio_dock_); });
  splitDockWidget(playback_dock_, audio_dock_, Qt::Horizontal);
  viewMenu->addAction(audio_dock_->toggleViewAction());

  toolbox_dock_ = new QDockWidget(tr("编辑工具箱"), this);
  toolbox_dock_->setObjectName(QStringLiteral("editorToolboxDock"));
  toolbox_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  toolbox_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
  auto* toolboxScroll = new QScrollArea(toolbox_dock_);
  toolboxScroll->setWidgetResizable(true);
  toolboxScroll->setFrameShape(QFrame::NoFrame);
  auto* toolbox = new QWidget(toolboxScroll);
  // Flow layout: the big buttons re-wrap into rows/columns to fit any dock shape.
  auto* toolboxLayout = new FlowLayout(toolbox, 4, 8, 8);
  // Convert buttons carry the same skin previews + tooltips as the old toolbar.
  const auto icons = build_convert_note_icons(skins_dir_);
  struct ConvertSpec {
    const char* name;
    wds::chart_editor::NoteType type;
    int direction;
  };
  const std::array<ConvertSpec, 8> specs = {{
      {"Tap", wds::chart_editor::NoteType::Normal, 0},
      {"ExTap", wds::chart_editor::NoteType::Critical, 0},
      {"Hold Head", wds::chart_editor::NoteType::HoldStart, 0},
      {"Hold", wds::chart_editor::NoteType::Hold, 0},
      {"Left Flick", wds::chart_editor::NoteType::Flick, -1},
      {"Flick", wds::chart_editor::NoteType::Flick, 0},
      {"Right Flick", wds::chart_editor::NoteType::Flick, 1},
      {"Scratch Hold", wds::chart_editor::NoteType::ScratchHold, 0},
  }};
  // New-style flow: with nothing selected the button locks the left-click place
  // type instead (toggle in 设置 → 输入).
  static constexpr std::array<wds::interaction::PlaceIntent, 8> kPlaceIntents = {{
      wds::interaction::PlaceIntent::None,  // Tap = classic default
      wds::interaction::PlaceIntent::ExTap,
      wds::interaction::PlaceIntent::HoldStart,
      wds::interaction::PlaceIntent::HoldBody,
      wds::interaction::PlaceIntent::FlickLeft,
      wds::interaction::PlaceIntent::Flick,
      wds::interaction::PlaceIntent::FlickRight,
      wds::interaction::PlaceIntent::ScratchHoldBody,
  }};
  for (std::size_t i = 0; i < specs.size(); ++i) {
    auto* button = new QToolButton(toolbox);
    button->setIcon(icons[i]);
    button->setIconSize(QSize(56, 56));
    button->setMinimumSize(72, 72);
    button->setToolTip(tr("转换为%1（无选中时切换放置类型）")
                           .arg(QString::fromUtf8(specs[i].name)));
    button->setCheckable(true);
    button->setAutoRaise(true);
    convert_buttons_[i] = button;
    toolboxLayout->addWidget(button);
    connect(button, &QToolButton::clicked, this, [this, i, spec = specs[i]] {
      auto* edit = ui_manager_ != nullptr ? ui_manager_->edit_panel() : nullptr;
      if (edit == nullptr) return;
      if (ui_manager_->new_note_place_logic()) {
        // New-style flow: the buttons only pick the mouse place type; convert
        // stays a classic-mode feature.
        const auto intent = kPlaceIntents[i];
        if (edit->place_intent_override() == intent) {
          edit->set_place_intent_override(wds::interaction::PlaceIntent::None);
          ui_manager_->set_status("放置类型已恢复为 Tap", StatusLevel::Info);
        } else {
          edit->set_place_intent_override(intent);
          ui_manager_->set_status(std::string("放置类型切换为 ") + spec.name,
                                  StatusLevel::Info);
        }
      } else if (!edit->selected().empty()) {
        const std::optional<int32_t> direction =
            spec.type == wds::chart_editor::NoteType::Flick
                ? std::optional<int32_t>(spec.direction)
                : std::nullopt;
        if (edit->convert_selected(spec.type, direction)) {
          ui_manager_->set_status(std::string("已转换为 ") + spec.name, StatusLevel::Info);
        }
      } else {
        ui_manager_->set_status("未选中音符（可在设置→输入中开启新版放置逻辑）",
                                StatusLevel::Info);
      }
      sync_toolbox_place_checks();
    });
  }
  auto* widthGroup = new QWidget(toolbox);
  auto* widthRow = new QHBoxLayout(widthGroup);
  widthRow->setContentsMargins(0, 0, 0, 0);
  widthRow->addWidget(new QLabel(tr("默认宽度"), widthGroup));
  auto* width = new QSpinBox(widthGroup);
  width->setRange(1, 12);
  width->setValue(1);
  widthRow->addWidget(width);
  toolboxLayout->addWidget(widthGroup);
  connect(width, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (ui_manager_ && ui_manager_->edit_panel()) ui_manager_->edit_panel()->set_default_width(value);
  });
  curve_fill_widget_ = new CurveFillWidget(ui_manager_, toolbox);
  toolboxLayout->addWidget(curve_fill_widget_);
  toolboxScroll->setWidget(toolbox);
  toolbox_dock_->setWidget(toolboxScroll);
  addDockWidget(Qt::BottomDockWidgetArea, toolbox_dock_);
  connect(toolbox_dock_, &QDockWidget::dockLocationChanged, this,
          [this](Qt::DockWidgetArea) { update_control_dock_height(toolbox_dock_); });
  connect(toolbox_dock_, &QDockWidget::topLevelChanged, this,
          [this](bool) { update_control_dock_height(toolbox_dock_); });
  splitDockWidget(audio_dock_, toolbox_dock_, Qt::Horizontal);
  viewMenu->addAction(toolbox_dock_->toggleViewAction());
}

void EditorMainWindow::sync_toolbox_place_checks() {
  auto* edit = ui_manager_ != nullptr ? ui_manager_->edit_panel() : nullptr;
  const bool enabled = ui_manager_ != nullptr && ui_manager_->new_note_place_logic();
  static constexpr std::array<wds::interaction::PlaceIntent, 8> kPlaceIntents = {{
      wds::interaction::PlaceIntent::None,
      wds::interaction::PlaceIntent::ExTap,
      wds::interaction::PlaceIntent::HoldStart,
      wds::interaction::PlaceIntent::HoldBody,
      wds::interaction::PlaceIntent::FlickLeft,
      wds::interaction::PlaceIntent::Flick,
      wds::interaction::PlaceIntent::FlickRight,
      wds::interaction::PlaceIntent::ScratchHoldBody,
  }};
  const auto current = edit != nullptr ? edit->place_intent_override()
                                       : wds::interaction::PlaceIntent::None;
  for (std::size_t i = 0; i < convert_buttons_.size(); ++i) {
    if (convert_buttons_[i] == nullptr) continue;
    const QSignalBlocker blocker(convert_buttons_[i]);
    convert_buttons_[i]->setChecked(enabled && current != wds::interaction::PlaceIntent::None &&
                                    kPlaceIntents[i] == current);
  }
}

void EditorMainWindow::bind_ui_manager(UiManager* manager) {
  ui_manager_ = manager;
  if (ui_manager_ == nullptr) return;
  if (auto* speed = findChild<QDoubleSpinBox*>(QStringLiteral("noteSpeed"))) {
    QSignalBlocker blocker(speed);
    speed->setValue(ui_manager_->chart_preview().preview().config().note_speed);
  }
  if (auto* lanes = findChild<QSpinBox*>(QStringLiteral("laneCount"))) {
    QSignalBlocker blocker(lanes);
    lanes->setValue(ui_manager_->chart_preview().preview().config().lane_count);
  }
  const auto& visual = ui_manager_->chart_preview().preview().config();
  if (auto* offset = findChild<QSpinBox*>(QStringLiteral("noteStartOffset"))) offset->setValue(visual.note_start_offset);
  if (auto* noteHeight = findChild<QSpinBox*>(QStringLiteral("noteHeightLevel"))) noteHeight->setValue(visual.note_height_level);
  if (auto* split = findChild<QSpinBox*>(QStringLiteral("splitLineOpacity")))
    split->setValue(static_cast<int>(std::lround(visual.split_line_opacity * 100.0f)));
  ui_manager_->set_external_status_handler([this](std::string text, StatusLevel) {
    const QString message = QString::fromUtf8(text.c_str());
    QMetaObject::invokeMethod(this, [this, message] {
      if (statusBar() != nullptr) statusBar()->showMessage(message);
    }, Qt::QueuedConnection);
  });
  connect(open_action_, &QAction::triggered, this, &EditorMainWindow::open_project);
  connect(save_action_, &QAction::triggered, this, &EditorMainWindow::save_project);
  connect(undo_action_, &QAction::triggered, this,
          [this] { ui_manager_->session().engine().undo(); });
  connect(redo_action_, &QAction::triggered, this,
          [this] { ui_manager_->session().engine().redo(); });
  connect(import_action_, &QAction::triggered, this, &EditorMainWindow::import_chart);
  connect(export_action_, &QAction::triggered, this, &EditorMainWindow::export_chart);
  connect(music_action_, &QAction::triggered, this, &EditorMainWindow::import_music);
  connect(check_action_, &QAction::triggered, this, &EditorMainWindow::check_chart);
  connect(settings_action_, &QAction::triggered, this, &EditorMainWindow::show_settings);
  connect(curve_templates_action_, &QAction::triggered, this,
          &EditorMainWindow::show_curve_templates);

  create_playback_and_toolbox_docks();
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  const auto state = prefs.value("window/state-v4").toByteArray();
  if (!state.isEmpty() && restoreState(state)) {
    QTimer::singleShot(0, this, &EditorMainWindow::pin_bottom_row);
  } else {
    reset_default_layout();
  }
  // Space toggles playback anywhere in the app (except while typing).
  qApp->installEventFilter(this);
  for (auto* dock : {playback_dock_, audio_dock_, toolbox_dock_})
    dock->widget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
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
  splash.setMinimumSize(520, 360);
  splash.setWindowFlag(Qt::WindowCloseButtonHint, true);
  auto* root = new QVBoxLayout(&splash);
  root->setContentsMargins(24, 20, 24, 18);
  root->setSpacing(10);
  auto* title = new QLabel(tr("<h1>WDS Editor</h1><p>开始编辑你的音游谱面</p>"), &splash);
  title->setTextFormat(Qt::RichText);
  root->addWidget(title);
  root->addWidget(new QLabel(tr("打开最近工程，或创建一个空白工程。"), &splash));
  auto* recent_label = new QLabel(tr("最近编辑"), &splash);
  recent_label->setStyleSheet(QStringLiteral("font-weight:bold;"));
  root->addWidget(recent_label);
  auto* recent = new QListWidget(&splash);
  recent->setMinimumHeight(96);
  recent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  recent->setUniformItemSizes(true);
  recent->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  const auto recent_paths = QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                                      "WDS", "WDS Editor")
                                .value(QStringLiteral("recent/projects"))
                                .toStringList();
  int shown_recent = 0;
  for (const auto& path : recent_paths) {
    if (shown_recent >= 8) break;
    if (QFileInfo::exists(path)) {
      auto* item = new QListWidgetItem(QFileInfo(path).fileName(), recent);
      item->setToolTip(path);
      item->setData(Qt::UserRole, path);
      ++shown_recent;
    }
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
  auto* later = new QPushButton(tr("直接打开主窗口"), &splash);
  later->setIcon(fluent_icon(fluent::Clear));
  auto* settings = new QPushButton(tr("设置"), &splash);
  settings->setIcon(fluent_icon(fluent::Settings));
  auto* about = new QPushButton(tr("关于"), &splash);
  about->setIcon(fluent_icon(fluent::Info));
  auto* version = new QLabel(tr("版本 %1").arg(QStringLiteral(WDS_APP_VERSION)), &splash);
  version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  actions->addWidget(open);
  actions->addWidget(create);
  actions->addStretch(1);
  actions->addWidget(settings);
  actions->addWidget(about);
  actions->addWidget(later);
  root->addLayout(actions);
  root->addWidget(version);

  std::function<void(const QString&)> start_load;
  start_load = [this, &splash, recent, open, create, later, settings, about, loading,
                progress](const QString& path) {
    if (path.isEmpty()) return;
    // Session/transport/Vulkan objects belong to the GUI thread. Yield once so
    // the progress state paints, then perform the small project/chart commit
    // there; full-song waveform/FFT work is dispatched asynchronously by the
    // preview panel.
    splash.setEnabled(false);
    for (auto* button : {open, create, later, settings, about}) button->setEnabled(false);
    recent->setEnabled(false);
    loading->setText(tr("正在加载工程…"));
    loading->setVisible(true);
    progress->setVisible(true);
    QTimer::singleShot(0, &splash,
                     [this, &splash, path, recent, open, create, later, settings,
                      about, loading, progress] {
                       const bool result =
                           ui_manager_->session().open_wdsproject(path.toStdString());
                       for (auto* button : {open, create, later, settings, about})
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
  };
  connect(recent, &QListWidget::itemDoubleClicked, &splash,
          [start_load](QListWidgetItem* item) {
            start_load(item->data(Qt::UserRole).toString());
          });
  connect(open, &QPushButton::clicked, &splash, [this, &splash, start_load] {
    const auto path = QFileDialog::getOpenFileName(&splash, tr("打开 WDS 工程"), {},
                                                   tr("WDS 工程 (*.wdsproject)"));
    start_load(path);
  });
  connect(create, &QPushButton::clicked, &splash, [this, &splash] {
    ui_manager_->session().new_project();
    splash.accept();
  });
  connect(later, &QPushButton::clicked, &splash, &QDialog::accept);
  connect(settings, &QPushButton::clicked, &splash, [this, &splash] {
    SettingsDialog dialog(ui_manager_, theme_dir_, &splash);
    dialog.exec();
    sync_toolbox_place_checks();
  });
  connect(about, &QPushButton::clicked, &splash, [this, &splash] {
    AboutDialog(&splash).exec();
  });
  const int result = splash.exec();
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
  // QMainWindow dock separators are private layout items rather than public
  // QSplitterHandles. Adopt sizes after a mouse gesture anywhere in this
  // window; ordinary clicks simply re-pin the unchanged layout, while a dock
  // separator release records the user's new bottom-row height.
  if (event->type() == QEvent::MouseButtonRelease && !native_resizing_) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (watched == this || (widget != nullptr && isAncestorOf(widget))) {
      QTimer::singleShot(0, this, &EditorMainWindow::pin_bottom_row);
    }
  }
  if (event->type() == QEvent::KeyPress && ui_manager_ != nullptr) {
    // Space is an application command. Handle it before any child widget,
    // including the Qt edit canvas, can consume it.
    {
      auto* key_event = static_cast<QKeyEvent*>(event);
      if (key_event->key() == Qt::Key_Space && !key_event->isAutoRepeat() &&
          QApplication::activeModalWidget() == nullptr) {
        QWidget* focus = QApplication::focusWidget();
        const bool typing = qobject_cast<QLineEdit*>(focus) != nullptr ||
                            qobject_cast<QAbstractSpinBox*>(focus) != nullptr ||
                            qobject_cast<QKeySequenceEdit*>(focus) != nullptr;
        if (!typing) {
          wds::interaction::Modifiers mods;
          mods.shift = key_event->modifiers().testFlag(Qt::ShiftModifier);
          wds::interaction::KeyDownEvent command;
          command.key = wds::interaction::KeyCode::Space;
          command.mods = mods;
          command.repeat = false;
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
  show_current();
  dialog.exec();
}

void EditorMainWindow::set_viewport_windows(QWindow* preview, QWindow* editor) {
  preview_window_ = preview;
  editor_window_ = editor;
  // Dock-only main window: an (even empty) central widget would sit between the
  // viewport docks and the 属性 dock and eat the horizontal slack on resize.
  preview_dock_ = new QDockWidget(tr("实时预览"), this);
  preview_dock_->setObjectName(QStringLiteral("previewViewportDock"));
  preview_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  preview_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
  auto* preview_container = QWidget::createWindowContainer(preview, preview_dock_);
  preview_container->setMinimumSize(260, 300);
  // Viewport docks absorb window-resize slack; the bottom control docks keep
  // their manually-set size (Preferred, set below).
  preview_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  preview_dock_->setWidget(preview_container);
  addDockWidget(Qt::LeftDockWidgetArea, preview_dock_);

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
  if (settings_dock_ != nullptr) {
    addDockWidget(Qt::RightDockWidgetArea, settings_dock_);
    splitDockWidget(editor_dock_, settings_dock_, Qt::Horizontal);
    resizeDocks({preview_dock_, editor_dock_, settings_dock_}, {5, 5, 1}, Qt::Horizontal);
  }
  auto* viewMenu = menuBar()->actions().at(2)->menu();
  viewMenu->addAction(preview_dock_->toggleViewAction());
  viewMenu->addAction(editor_dock_->toggleViewAction());
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
  if (settings_dock_) splitDockWidget(editor_dock_, settings_dock_, Qt::Horizontal);
  auto* viewMenu = menuBar()->actions().at(2)->menu();
  viewMenu->addAction(editor_dock_->toggleViewAction());
  // bind_ui_manager runs before the application supplies its viewport widgets,
  // so restore the dock state only now, after every named dock exists.
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  const auto state = prefs.value("window/state-v4").toByteArray();
  if (!state.isEmpty()) restoreState(state);
  else reset_default_layout();
}

void EditorMainWindow::resizeEvent(QResizeEvent* event) {
  if (auto* window = static_cast<RealtimeVulkanWindow*>(preview_window_))
    window->set_resize_suspended(true);
  if (auto* window = static_cast<RealtimeVulkanWindow*>(editor_window_))
    window->set_resize_suspended(true);
  QMainWindow::resizeEvent(event);
  resize_settle_timer_.start();
}

void EditorMainWindow::update_control_dock_height(QDockWidget* dock) {
  if (dock == nullptr) return;
  constexpr int kControlRowHeight = 200;
  const bool bottom_docked = !dock->isFloating() &&
                             dockWidgetArea(dock) == Qt::BottomDockWidgetArea;
  dock->setMinimumHeight(bottom_docked ? kControlRowHeight : 0);
  dock->setMaximumHeight(bottom_docked ? kControlRowHeight : QWIDGETSIZE_MAX);
  dock->setSizePolicy(QSizePolicy::Expanding,
                      bottom_docked ? QSizePolicy::Fixed : QSizePolicy::Preferred);
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

bool EditorMainWindow::confirm_pending_changes() {
  if (ui_manager_ == nullptr || !ui_manager_->session().dirty()) return true;
  QMessageBox box(QMessageBox::Warning, tr("未保存的修改"),
                  tr("当前工程有未保存的修改。"), QMessageBox::NoButton, this);
  auto* save = box.addButton(tr("保存"), QMessageBox::AcceptRole);
  auto* discard = box.addButton(tr("不保存"), QMessageBox::DestructiveRole);
  box.addButton(tr("取消"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == save) {
    save_project();
    return !ui_manager_->session().dirty();
  }
  return box.clickedButton() == discard;
}

void EditorMainWindow::open_project() {
  if (!confirm_pending_changes()) return;
  const auto path = QFileDialog::getOpenFileName(this, tr("打开 WDS 工程"), {},
                                                  tr("WDS 工程 (*.wdsproject)"));
  if (path.isEmpty()) return;
  bool ok = false;
  {
    BusyScope busy(this, tr("正在打开工程…"));
    ok = ui_manager_->session().open_wdsproject(path.toStdString());
  }
  if (!ok) QMessageBox::warning(this, tr("打开失败"), tr("无法打开所选工程。"));
  else remember_recent_project(path);
}

void EditorMainWindow::save_project() {
  if (ui_manager_ == nullptr) return;
  auto& session = ui_manager_->session();
  if (session.read_only()) { QMessageBox::warning(this, tr("只读"), tr("只读预览无法保存。")); return; }
  QString path = QString::fromStdString(session.project_path());
  if (path.isEmpty()) path = QFileDialog::getSaveFileName(this, tr("保存 WDS 工程"),
                                                          QStringLiteral("untitled.wdsproject"),
                                                          tr("WDS 工程 (*.wdsproject)"));
  if (path.isEmpty()) return;
  bool ok = false;
  {
    BusyScope busy(this, tr("正在保存工程…"));
    ok = session.project_path().empty() ? session.save_as(path.toStdString()) : session.save();
  }
  if (!ok) QMessageBox::warning(this, tr("保存失败"), tr("无法保存工程。"));
  else remember_recent_project(path);
}

void EditorMainWindow::import_chart() {
  if (!confirm_pending_changes()) return;
  const auto path = QFileDialog::getOpenFileName(this, tr("导入官方谱面"), {},
                                                  tr("谱面文件 (*.csv *.sus)"));
  if (path.isEmpty()) return;
  bool ok = false;
  {
    BusyScope busy(this, tr("正在导入谱面…"));
    ok = ui_manager_->session().import_official(path.toStdString());
  }
  if (!ok) QMessageBox::warning(this, tr("导入失败"), tr("无法导入所选谱面。"));
}

void EditorMainWindow::import_music() {
  const auto path = QFileDialog::getOpenFileName(this, tr("导入音乐"), {},
                                                  tr("音频文件 (*.ogg *.wav *.mp3)"));
  if (path.isEmpty()) return;
  bool ok = false;
  {
    BusyScope busy(this, tr("正在导入音乐…"));
    ok = ui_manager_->session().import_music(path.toStdString());
  }
  if (!ok) QMessageBox::warning(this, tr("导入失败"), tr("无法导入所选音乐。"));
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
  formatRow->addWidget(new QLabel(tr("格式"), &dialog));
  auto* format = new QComboBox(&dialog);
  format->addItems({tr("官方 CSV"), tr("SUS")});
  formatRow->addWidget(format, 1);
  layout->addLayout(formatRow);
  auto* project = new QPushButton(tr("导出整个项目"), &dialog);
  auto* chart = new QPushButton(tr("仅导出当前谱面"), &dialog);
  auto* cancel = new QPushButton(tr("取消"), &dialog);
  layout->addWidget(project);
  layout->addWidget(chart);
  layout->addWidget(cancel);
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

void EditorMainWindow::show_settings() {
  SettingsDialog dialog(ui_manager_, theme_dir_, this);
  dialog.exec();
  // Disabling the new place logic clears the lock; reflect it on the buttons.
  sync_toolbox_place_checks();
}

void EditorMainWindow::show_curve_templates() {
  QDialog dialog(this); dialog.setWindowTitle(tr("曲线模板")); dialog.resize(560, 360);
  auto* root = new QVBoxLayout(&dialog);
  auto* list = new QListWidget(&dialog);
  auto state = ui_manager_->curve_template_state();
  for (const auto& item : state.templates) list->addItem(QString::fromStdString(item.name));
  root->addWidget(list);
  auto* name = new QLineEdit(&dialog); name->setPlaceholderText(tr("名称")); root->addWidget(name);
  auto* parameter = new QDoubleSpinBox(&dialog); parameter->setRange(-1000.0, 1000.0); parameter->setDecimals(4); root->addWidget(parameter);
  auto* row = new QHBoxLayout; auto* add = new QPushButton(tr("添加"), &dialog); auto* remove = new QPushButton(tr("删除"), &dialog); row->addWidget(add); row->addWidget(remove); root->addLayout(row);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); root->addWidget(buttons);
  connect(list, &QListWidget::currentRowChanged, &dialog, [&](int row) { if (row >= 0 && row < static_cast<int>(state.templates.size())) { name->setText(QString::fromStdString(state.templates[static_cast<std::size_t>(row)].name)); parameter->setValue(state.templates[static_cast<std::size_t>(row)].parameter); } });
  connect(add, &QPushButton::clicked, &dialog, [&] { if (state.templates.size() >= wds::ui::kMaxCurveTemplates) return; wds::ui::CurveTemplate item; item.id = wds::ui::allocate_curve_template_id(state.templates); item.name = "Template " + std::to_string(state.templates.size() + 1); state.templates.push_back(item); list->addItem(QString::fromStdString(item.name)); list->setCurrentRow(list->count() - 1); });
  connect(remove, &QPushButton::clicked, &dialog, [&] { const int row = list->currentRow(); if (row >= 0) { state.templates.erase(state.templates.begin() + row); delete list->takeItem(row); } });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { const int row = list->currentRow(); if (row >= 0 && row < static_cast<int>(state.templates.size())) { state.templates[static_cast<std::size_t>(row)].name = name->text().toStdString(); state.templates[static_cast<std::size_t>(row)].parameter = parameter->value(); normalize_curve_template(state.templates[static_cast<std::size_t>(row)]); } dialog.accept(); });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() == QDialog::Accepted) {
    ui_manager_->set_curve_template_state_from_qt(std::move(state));
    if (curve_fill_widget_ != nullptr) curve_fill_widget_->refresh_curve_controls();
  }
}

void EditorMainWindow::closeEvent(QCloseEvent* event) {
  if (!confirm_pending_changes()) { event->ignore(); return; }
  QSettings prefs(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  prefs.setValue("window/geometry", saveGeometry());
  prefs.setValue("window/state-v4", saveState());
  event->accept();
}

void EditorMainWindow::hideEvent(QHideEvent* event) {
  QMainWindow::hideEvent(event);
  // Floating docks are independent top-level windows. Keep them visible when
  // the main shell is temporarily hidden (for example while switching apps).
  for (auto* dock : {preview_dock_, editor_dock_, settings_dock_, playback_dock_, audio_dock_,
                     toolbox_dock_}) {
    if (dock != nullptr && dock->isFloating()) dock->show();
  }
}

void EditorMainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  for (auto* dock : {preview_dock_, editor_dock_, settings_dock_, playback_dock_, audio_dock_,
                     toolbox_dock_}) {
    if (dock != nullptr && dock->isFloating() && !dock->isHidden()) dock->raise();
  }
}
}

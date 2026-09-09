#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/note_icons.hpp"
#include "wds/ui/qt/playback_dock.hpp"
#include "wds/ui/qt/settings_dialog.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/core/chart_editor_engine.hpp"
#include <QCloseEvent>
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
#include <wds/interaction/editor_input.hpp>
#include "wds/ui/curve_template.hpp"
#include <array>

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
  music_action_ = addCommand(tr("导入音乐"), QKeySequence(), tr("导入 ogg / wav 音乐"));
  curve_templates_action_ = addCommand(tr("曲线模板"), QKeySequence(), tr("编辑曲线模板"));
  check_action_ = addCommand(tr("检查谱面"), QKeySequence(), tr("检查谱面错误"));
  settings_action_ = addCommand(tr("设置"), QKeySequence(), tr("编辑器设置"));
  open_action_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  save_action_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));

  undo_action_ = new QAction(tr("撤销"), this);
  undo_action_->setShortcut(QKeySequence::Undo);
  redo_action_ = new QAction(tr("重做"), this);
  redo_action_->setShortcut(QKeySequence::Redo);
  editMenu->addAction(undo_action_);
  editMenu->addAction(redo_action_);
  statusBar()->showMessage(tr("就绪"));

  settings_dock_ = new QDockWidget(tr("属性 / 设置"), this);
  auto* settingsDock = settings_dock_;
  settingsDock->setObjectName(QStringLiteral("previewSettingsDock"));
  auto* settingsPanel = new QWidget(settingsDock);
  auto* form = new QFormLayout(settingsPanel);
  auto* speed = new QDoubleSpinBox(settingsPanel);
  speed->setRange(0.1, 10.0); speed->setValue(5.0); speed->setSingleStep(0.1);
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
  settingsDock->setWidget(settingsPanel);
  settingsDock->setMinimumWidth(220);
  viewMenu->addAction(settingsDock->toggleViewAction());
  auto* fullscreen = viewMenu->addAction(tr("全屏"));
  fullscreen->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F11));
  connect(fullscreen, &QAction::triggered, this, [this] {
    isFullScreen() ? showNormal() : showFullScreen();
  });
  auto* reset = viewMenu->addAction(tr("重置布局"));
  connect(reset, &QAction::triggered, this, [this] {
    QSettings prefs("WDS", "WDS Editor");
    prefs.remove("window/geometry");
    prefs.remove("window/state-v3");
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
  QSettings prefs("WDS", "WDS Editor");
  restoreGeometry(prefs.value("window/geometry").toByteArray());
  // Dock state is restored in bind_ui_manager once every dock exists.
}

void EditorMainWindow::reset_default_layout() {
  resize(1440, 900);
  if (preview_dock_ == nullptr || editor_dock_ == nullptr) return;
  for (auto* dock : {preview_dock_, editor_dock_, settings_dock_, playback_dock_, toolbox_dock_}) {
    if (dock != nullptr) removeDockWidget(dock);
  }
  addDockWidget(Qt::LeftDockWidgetArea, preview_dock_);
  splitDockWidget(preview_dock_, editor_dock_, Qt::Horizontal);
  if (settings_dock_ != nullptr) splitDockWidget(editor_dock_, settings_dock_, Qt::Horizontal);
  if (playback_dock_ != nullptr) addDockWidget(Qt::BottomDockWidgetArea, playback_dock_);
  if (playback_dock_ != nullptr && toolbox_dock_ != nullptr)
    tabifyDockWidget(playback_dock_, toolbox_dock_);
  if (settings_dock_ != nullptr)
    resizeDocks({preview_dock_, editor_dock_, settings_dock_}, {5, 5, 1}, Qt::Horizontal);
  for (auto* dock : {preview_dock_, editor_dock_, settings_dock_, playback_dock_, toolbox_dock_}) {
    if (dock != nullptr) dock->show();
  }
  if (playback_dock_ != nullptr) playback_dock_->raise();
}

void EditorMainWindow::create_playback_and_toolbox_docks() {
  auto* viewMenu = menuBar()->actions().at(2)->menu();

  playback_dock_ = new QDockWidget(tr("播放 / 音频"), this);
  playback_dock_->setObjectName(QStringLiteral("playbackAudioDock"));
  playback_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  playback_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
  playback_panel_ = new PlaybackAudioPanel(ui_manager_, playback_dock_);
  playback_panel_->set_add_chart_handler([this] { add_chart(); });
  playback_dock_->setWidget(playback_panel_);
  addDockWidget(Qt::BottomDockWidgetArea, playback_dock_);
  viewMenu->addAction(playback_dock_->toggleViewAction());

  toolbox_dock_ = new QDockWidget(tr("编辑工具箱"), this);
  toolbox_dock_->setObjectName(QStringLiteral("editorToolboxDock"));
  toolbox_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  toolbox_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
  auto* toolbox = new QWidget(toolbox_dock_);
  auto* toolboxLayout = new QHBoxLayout(toolbox);
  // Convert buttons carry the same skin previews + tooltips as the old toolbar.
  const auto icons = build_convert_note_icons(skins_dir_);
  struct ConvertSpec {
    const char* tip;
    wds::chart_editor::NoteType type;
    int direction;
  };
  const std::array<ConvertSpec, 8> specs = {{
      {"转换为Tap", wds::chart_editor::NoteType::Normal, 0},
      {"转换为ExTap", wds::chart_editor::NoteType::Critical, 0},
      {"转换为Hold Head", wds::chart_editor::NoteType::HoldStart, 0},
      {"转换为Hold", wds::chart_editor::NoteType::Hold, 0},
      {"转换为Left Flick", wds::chart_editor::NoteType::Flick, -1},
      {"转换为Flick", wds::chart_editor::NoteType::Flick, 0},
      {"转换为Right Flick", wds::chart_editor::NoteType::Flick, 1},
      {"转换为Scratch Hold", wds::chart_editor::NoteType::ScratchHold, 0},
  }};
  for (std::size_t i = 0; i < specs.size(); ++i) {
    auto* button = new QToolButton(toolbox);
    button->setIcon(icons[i]);
    button->setIconSize(QSize(40, 40));
    button->setToolTip(QString::fromUtf8(specs[i].tip));
    button->setAutoRaise(true);
    toolboxLayout->addWidget(button);
    connect(button, &QToolButton::clicked, this,
            [this, type = specs[i].type, direction = specs[i].direction] {
              if (!ui_manager_ || !ui_manager_->edit_panel()) return;
              ui_manager_->edit_panel()->convert_selected(type, direction);
            });
  }
  toolboxLayout->addSpacing(16);
  toolboxLayout->addWidget(new QLabel(tr("默认宽度"), toolbox));
  auto* width = new QSpinBox(toolbox);
  width->setRange(1, 12);
  width->setValue(1);
  toolboxLayout->addWidget(width);
  connect(width, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (ui_manager_ && ui_manager_->edit_panel()) ui_manager_->edit_panel()->set_default_width(value);
  });
  toolboxLayout->addStretch(1);
  toolbox_dock_->setWidget(toolbox);
  addDockWidget(Qt::BottomDockWidgetArea, toolbox_dock_);
  tabifyDockWidget(playback_dock_, toolbox_dock_);
  playback_dock_->raise();
  viewMenu->addAction(toolbox_dock_->toggleViewAction());
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
    statusBar()->showMessage(QString::fromUtf8(text.c_str()));
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
  connect(check_action_, &QAction::triggered, this,
          [this] { ui_manager_->check_chart_errors(); });
  connect(settings_action_, &QAction::triggered, this, &EditorMainWindow::show_settings);
  connect(curve_templates_action_, &QAction::triggered, this,
          &EditorMainWindow::show_curve_templates);

  create_playback_and_toolbox_docks();
  QSettings prefs("WDS", "WDS Editor");
  if (!prefs.value("window/state-v3").toByteArray().isEmpty())
    restoreState(prefs.value("window/state-v3").toByteArray());
}

void EditorMainWindow::set_viewport_windows(QWindow* preview, QWindow* editor) {
  preview_window_ = preview;
  editor_window_ = editor;
  auto* workspace = new QWidget(this);
  workspace->setObjectName(QStringLiteral("realtimeWorkspace"));
  setCentralWidget(workspace);
  preview_dock_ = new QDockWidget(tr("实时预览"), this);
  preview_dock_->setObjectName(QStringLiteral("previewViewportDock"));
  preview_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  preview_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
  auto* preview_container = QWidget::createWindowContainer(preview, preview_dock_);
  preview_container->setMinimumSize(260, 300);
  preview_dock_->setWidget(preview_container);
  addDockWidget(Qt::LeftDockWidgetArea, preview_dock_);

  editor_dock_ = new QDockWidget(tr("谱面编辑器"), this);
  editor_dock_->setObjectName(QStringLiteral("editorViewportDock"));
  editor_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
  editor_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                            QDockWidget::DockWidgetFloatable |
                            QDockWidget::DockWidgetClosable);
  auto* editor_container = QWidget::createWindowContainer(editor, editor_dock_);
  editor_container->setMinimumSize(360, 300);
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

void EditorMainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (auto* window = static_cast<RealtimeVulkanWindow*>(preview_window_))
    window->set_resize_suspended(true);
  if (auto* window = static_cast<RealtimeVulkanWindow*>(editor_window_))
    window->set_resize_suspended(true);
  resize_settle_timer_.start();
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
  if (!ui_manager_->session().open_wdsproject(path.toStdString()))
    QMessageBox::warning(this, tr("打开失败"), tr("无法打开所选工程。"));
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
  const bool ok = session.project_path().empty() ? session.save_as(path.toStdString()) : session.save();
  if (!ok) QMessageBox::warning(this, tr("保存失败"), tr("无法保存工程。"));
}

void EditorMainWindow::import_chart() {
  if (!confirm_pending_changes()) return;
  const auto path = QFileDialog::getOpenFileName(this, tr("导入官方谱面"), {},
                                                  tr("谱面文件 (*.csv *.sus)"));
  if (!path.isEmpty() && !ui_manager_->session().import_official(path.toStdString()))
    QMessageBox::warning(this, tr("导入失败"), tr("无法导入所选谱面。"));
}

void EditorMainWindow::import_music() {
  const auto path = QFileDialog::getOpenFileName(this, tr("导入音乐"), {},
                                                  tr("音频文件 (*.ogg *.wav)"));
  if (path.isEmpty()) return;
  if (!ui_manager_->session().import_music(path.toStdString()))
    QMessageBox::warning(this, tr("导入失败"), tr("无法导入所选音乐。"));
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
  SettingsDialog dialog(ui_manager_, this);
  dialog.exec();
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
    if (playback_panel_ != nullptr) playback_panel_->refresh_curve_controls();
  }
}

void EditorMainWindow::closeEvent(QCloseEvent* event) {
  if (!confirm_pending_changes()) { event->ignore(); return; }
  QSettings prefs("WDS", "WDS Editor");
  prefs.setValue("window/geometry", saveGeometry());
  prefs.setValue("window/state-v3", saveState());
  event->accept();
}
}

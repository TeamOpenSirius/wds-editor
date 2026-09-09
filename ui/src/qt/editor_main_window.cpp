#include "wds/ui/qt/editor_main_window.hpp"
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
#include <QKeySequence>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QListWidget>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTabWidget>
#include <QCheckBox>
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
  auto addCommand = [this, fileMenu, toolbar](const QString& text, const QKeySequence& shortcut) {
    auto* action = new QAction(text, this);
    action->setShortcut(shortcut);
    fileMenu->addAction(action);
    toolbar->addAction(action);
    return action;
  };
  auto* open = addCommand(tr("打开工程"), QKeySequence::Open);
  auto* save = addCommand(tr("保存工程"), QKeySequence::Save);
  auto* undo = addCommand(tr("撤销"), QKeySequence::Undo);
  auto* redo = addCommand(tr("重做"), QKeySequence::Redo);
  auto* import = addCommand(tr("导入谱面"), QKeySequence());
  auto* exportAction = addCommand(tr("导出"), QKeySequence());
  auto* addChart = addCommand(tr("添加谱面"), QKeySequence());
  auto* check = addCommand(tr("检查谱面"), QKeySequence());
  auto* widthSettings = addCommand(tr("宽度设置"), QKeySequence());
  auto* curveTemplates = addCommand(tr("曲线模板"), QKeySequence());
  open_action_ = open;
  save_action_ = save;
  undo_action_ = undo;
  redo_action_ = redo;
  import_action_ = import;
  export_action_ = exportAction;
  add_chart_action_ = addChart;
  check_action_ = check;
  width_settings_action_ = widthSettings;
  curve_templates_action_ = curveTemplates;
  open->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  save->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
  undo->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
  redo->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
  statusBar()->showMessage(tr("就绪"));
  editMenu->addAction(undo);
  editMenu->addAction(redo);
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
  connect(reset, &QAction::triggered, this, [this, settingsDock] {
    QSettings prefs("WDS", "WDS Editor");
    prefs.remove("window/geometry");
    prefs.remove("window/state-v2");
    resize(1440, 900);
    removeDockWidget(settingsDock);
    if (preview_dock_ != nullptr && editor_dock_ != nullptr) {
      addDockWidget(Qt::LeftDockWidgetArea, preview_dock_);
      splitDockWidget(preview_dock_, editor_dock_, Qt::Horizontal);
      splitDockWidget(editor_dock_, settingsDock, Qt::Horizontal);
      resizeDocks({preview_dock_, editor_dock_, settingsDock}, {5, 5, 1}, Qt::Horizontal);
    }
    settingsDock->show();
  });
  // Engine-style lower workbench: operations and inspectable runtime values
  // live in a dock so users can move, tab, or float it with the other panels.
  workbench_dock_ = new QDockWidget(tr("操作与属性"), this);
  auto* workbenchDock = workbench_dock_;
  workbenchDock->setObjectName(QStringLiteral("operationsPropertiesDock"));
  workbenchDock->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea |
                                 Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  workbenchDock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
  auto* workbenchTabs = new QTabWidget(workbenchDock);
  auto* operations = new QWidget(workbenchTabs);
  auto* operationsLayout = new QHBoxLayout(operations);
  operationsLayout->addWidget(new QLabel(tr("播放控制"), operations));
  auto* play = new QPushButton(tr("播放 / 暂停"), operations);
  auto* stop = new QPushButton(tr("回到开头"), operations);
  operationsLayout->addWidget(play);
  operationsLayout->addWidget(stop);
  operationsLayout->addStretch(1);
  auto* properties = new QWidget(workbenchTabs);
  auto* propertiesLayout = new QFormLayout(properties);
  propertiesLayout->addRow(tr("当前视图"), new QLabel(tr("谱面编辑器"), properties));
  propertiesLayout->addRow(tr("提示"), new QLabel(tr("选择对象后可在此查看属性"), properties));
  auto* pauseAtCurrent = new QCheckBox(tr("暂停时停留在当前位置"), properties);
  pauseAtCurrent->setObjectName(QStringLiteral("pauseAtCurrent"));
  pauseAtCurrent->setChecked(false);
  propertiesLayout->addRow(tr("播放行为"), pauseAtCurrent);
  workbenchTabs->addTab(operations, tr("操作"));
  workbenchTabs->addTab(properties, tr("属性"));
  workbenchDock->setWidget(workbenchTabs);
  addDockWidget(Qt::BottomDockWidgetArea, workbenchDock);
  viewMenu->addAction(workbenchDock->toggleViewAction());
  connect(play, &QPushButton::clicked, this, [this] {
    if (ui_manager_) {
      auto& transport = ui_manager_->chart_preview().transport();
      transport.playing() ? transport.request_pause() : transport.request_play();
    }
  });
  connect(stop, &QPushButton::clicked, this, [this] {
    if (ui_manager_) ui_manager_->chart_preview().reset_playback();
  });
  connect(pauseAtCurrent, &QCheckBox::toggled, this, [this](bool enabled) {
    if (ui_manager_ && ui_manager_->edit_panel()) {
      ui_manager_->edit_panel()->set_pause_at_current(enabled);
      ui_manager_->request_save_ui_config(true);
    }
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
  // Dock state is restored after the three viewport docks have been created.
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
  if (auto* pause = findChild<QCheckBox*>(QStringLiteral("pauseAtCurrent")))
    pause->setChecked(ui_manager_->edit_panel() != nullptr &&
                     ui_manager_->edit_panel()->pause_at_current());
  ui_manager_->set_external_status_handler([this](std::string text, StatusLevel) {
    statusBar()->showMessage(QString::fromUtf8(text.c_str()));
  });
  if (open_action_) connect(open_action_, &QAction::triggered, this, &EditorMainWindow::open_project);
  if (save_action_) connect(save_action_, &QAction::triggered, this,
                            &EditorMainWindow::save_project);
  if (undo_action_) connect(undo_action_, &QAction::triggered, this,
                            [this] { ui_manager_->session().engine().undo(); });
  if (redo_action_) connect(redo_action_, &QAction::triggered, this,
                            [this] { ui_manager_->session().engine().redo(); });
  if (import_action_) connect(import_action_, &QAction::triggered, this, &EditorMainWindow::import_chart);
  if (export_action_) connect(export_action_, &QAction::triggered, this, &EditorMainWindow::export_chart);
  if (add_chart_action_) connect(add_chart_action_, &QAction::triggered, this, &EditorMainWindow::add_chart);
  if (check_action_) connect(check_action_, &QAction::triggered, this,
                             [this] { ui_manager_->check_chart_errors(); });
  if (width_settings_action_) connect(width_settings_action_, &QAction::triggered, this,
                                      &EditorMainWindow::show_width_settings);
  if (curve_templates_action_) connect(curve_templates_action_, &QAction::triggered, this,
                                       &EditorMainWindow::show_curve_templates);
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
  }
  auto* toolbox = new QDockWidget(tr("编辑工具箱"), this);
  toolbox->setObjectName(QStringLiteral("editorToolboxDock"));
  toolbox->setAllowedAreas(Qt::AllDockWidgetAreas);
  toolbox->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                       QDockWidget::DockWidgetClosable);
  auto* tabs = new QTabWidget(toolbox);
  auto* noteTab = new QWidget(tabs);
  auto* noteLayout = new QGridLayout(noteTab);
  const std::array<std::pair<QString, wds::chart_editor::NoteType>, 8> tools = {{
      {tr("普通音符"), wds::chart_editor::NoteType::Normal},
      {tr("关键音符"), wds::chart_editor::NoteType::Critical},
      {tr("长按起点"), wds::chart_editor::NoteType::HoldStart},
      {tr("长按"), wds::chart_editor::NoteType::Hold},
      {tr("滑键"), wds::chart_editor::NoteType::Flick},
      {tr("左滑"), wds::chart_editor::NoteType::Flick},
      {tr("右滑"), wds::chart_editor::NoteType::Flick},
      {tr("擦键长按"), wds::chart_editor::NoteType::ScratchHold},
  }};
  for (std::size_t i = 0; i < tools.size(); ++i) {
    auto* button = new QPushButton(tools[i].first, noteTab);
    noteLayout->addWidget(button, static_cast<int>(i / 2), static_cast<int>(i % 2));
    connect(button, &QPushButton::clicked, this, [this, type = tools[i].second, i] {
      if (!ui_manager_ || !ui_manager_->edit_panel()) return;
      const int direction = i == 5 ? -1 : i == 6 ? 1 : 0;
      ui_manager_->edit_panel()->convert_selected(type, direction);
      ui_manager_->set_status(tr("已应用编辑工具").toStdString(), StatusLevel::Info);
    });
  }
  auto* viewTab = new QWidget(tabs);
  auto* viewLayout = new QFormLayout(viewTab);
  auto* width = new QSpinBox(viewTab);
  width->setRange(1, 12);
  width->setValue(1);
  viewLayout->addRow(tr("默认宽度"), width);
  connect(width, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (ui_manager_ && ui_manager_->edit_panel()) ui_manager_->edit_panel()->set_default_width(value);
  });
  tabs->addTab(noteTab, tr("音符工具"));
  tabs->addTab(viewTab, tr("视图工具"));
  toolbox->setWidget(tabs);
  addDockWidget(Qt::BottomDockWidgetArea, toolbox);
  if (workbench_dock_) tabifyDockWidget(workbench_dock_, toolbox);
  auto* viewMenu = menuBar()->actions().at(2)->menu();
  viewMenu->addAction(toolbox->toggleViewAction());
  viewMenu->addAction(preview_dock_->toggleViewAction());
  viewMenu->addAction(editor_dock_->toggleViewAction());
  if (settings_dock_ != nullptr)
    resizeDocks({preview_dock_, editor_dock_, settings_dock_}, {5, 5, 1}, Qt::Horizontal);
  QSettings prefs("WDS", "WDS Editor");
  if (!prefs.value("window/state-v2").toByteArray().isEmpty())
    restoreState(prefs.value("window/state-v2").toByteArray());
  else if (settings_dock_ != nullptr)
    resizeDocks({preview_dock_, editor_dock_, settings_dock_}, {5, 5, 1}, Qt::Horizontal);
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
  QMessageBox box(QMessageBox::Warning, tr("Unsaved changes"),
                  tr("The current project has unsaved changes."), QMessageBox::NoButton, this);
  auto* save = box.addButton(tr("Save"), QMessageBox::AcceptRole);
  auto* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
  box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == save) {
    save_project();
    return !ui_manager_->session().dirty();
  }
  return box.clickedButton() == discard;
}

void EditorMainWindow::open_project() {
  if (!confirm_pending_changes()) return;
  const auto path = QFileDialog::getOpenFileName(this, tr("Open WDS project"), {},
                                                  tr("WDS project (*.wdsproject)"));
  if (path.isEmpty()) return;
  if (!ui_manager_->session().open_wdsproject(path.toStdString()))
    QMessageBox::warning(this, tr("Open failed"), tr("Unable to open the selected project."));
}

void EditorMainWindow::save_project() {
  if (ui_manager_ == nullptr) return;
  auto& session = ui_manager_->session();
  if (session.read_only()) { QMessageBox::warning(this, tr("Read-only"), tr("This preview is read-only.")); return; }
  QString path = QString::fromStdString(session.project_path());
  if (path.isEmpty()) path = QFileDialog::getSaveFileName(this, tr("Save WDS project"),
                                                          QStringLiteral("untitled.wdsproject"),
                                                          tr("WDS project (*.wdsproject)"));
  if (path.isEmpty()) return;
  const bool ok = session.project_path().empty() ? session.save_as(path.toStdString()) : session.save();
  if (!ok) QMessageBox::warning(this, tr("Save failed"), tr("Unable to save the project."));
}

void EditorMainWindow::import_chart() {
  if (!confirm_pending_changes()) return;
  const auto path = QFileDialog::getOpenFileName(this, tr("Import chart"), {},
                                                  tr("Chart files (*.csv *.sus)"));
  if (!path.isEmpty() && !ui_manager_->session().import_official(path.toStdString()))
    QMessageBox::warning(this, tr("Import failed"), tr("Unable to import the selected chart."));
}

void EditorMainWindow::add_chart() {
  if (ui_manager_->session().read_only()) return;
  QMessageBox box(QMessageBox::Question, tr("Add chart"), tr("Create a new chart or add an existing chart?"), QMessageBox::NoButton, this);
  auto* create = box.addButton(tr("Create new"), QMessageBox::AcceptRole);
  auto* existing = box.addButton(tr("Add existing"), QMessageBox::ActionRole);
  box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == create) { ui_manager_->session().add_chart(); return; }
  if (box.clickedButton() == existing) {
    const auto path = QFileDialog::getOpenFileName(this, tr("Add chart"), {}, tr("WDS chart (*.wdschart)"));
    if (!path.isEmpty()) ui_manager_->session().add_chart_from_file(path.toStdString());
  }
}

void EditorMainWindow::export_chart() {
  if (ui_manager_->session().read_only()) return;
  QMessageBox box(QMessageBox::Question, tr("Export"), tr("Export current chart or the whole project?"), QMessageBox::NoButton, this);
  auto* current = box.addButton(tr("Current chart"), QMessageBox::AcceptRole);
  auto* project = box.addButton(tr("Whole project"), QMessageBox::ActionRole);
  box.addButton(tr("Cancel"), QMessageBox::RejectRole); box.exec();
  if (box.clickedButton() == project) {
    const auto dir = QFileDialog::getExistingDirectory(this, tr("Export project"));
    if (!dir.isEmpty()) ui_manager_->session().export_official_project(dir.toStdString());
  } else if (box.clickedButton() == current) {
    const auto name = QString::fromStdString(ui_manager_->session().official_chart_filename(ui_manager_->session().active_chart_index()));
    const auto path = QFileDialog::getSaveFileName(this, tr("Export chart"), name, tr("CSV chart (*.csv)"));
    if (!path.isEmpty()) ui_manager_->session().export_official(path.toStdString());
  }
}

void EditorMainWindow::show_width_settings() {
  QDialog dialog(this); dialog.setWindowTitle(tr("Width settings"));
  auto* layout = new QGridLayout(&dialog);
  std::array<QSpinBox*, 6> fields{};
  const auto& values = wds::interaction::width_slot_values_const();
  for (int i = 0; i < 6; ++i) {
    layout->addWidget(new QLabel(tr("Slot %1").arg(i + 1), &dialog), i, 0);
    fields[static_cast<std::size_t>(i)] = new QSpinBox(&dialog);
    fields[static_cast<std::size_t>(i)]->setRange(1, 12);
    fields[static_cast<std::size_t>(i)]->setValue(values[static_cast<std::size_t>(i)]);
    layout->addWidget(fields[static_cast<std::size_t>(i)], i, 1);
  }
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons, 6, 0, 1, 2);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  std::array<int, 6> updated{};
  for (int i = 0; i < 6; ++i) updated[static_cast<std::size_t>(i)] = fields[static_cast<std::size_t>(i)]->value();
  wds::interaction::set_width_slot_values(updated);
  ui_manager_->save_ui_config();
}

void EditorMainWindow::show_curve_templates() {
  QDialog dialog(this); dialog.setWindowTitle(tr("Curve templates")); dialog.resize(560, 360);
  auto* root = new QVBoxLayout(&dialog);
  auto* list = new QListWidget(&dialog);
  auto state = ui_manager_->curve_template_state();
  for (const auto& item : state.templates) list->addItem(QString::fromStdString(item.name));
  root->addWidget(list);
  auto* name = new QLineEdit(&dialog); name->setPlaceholderText(tr("Name")); root->addWidget(name);
  auto* parameter = new QDoubleSpinBox(&dialog); parameter->setRange(-1000.0, 1000.0); parameter->setDecimals(4); root->addWidget(parameter);
  auto* row = new QHBoxLayout; auto* add = new QPushButton(tr("Add"), &dialog); auto* remove = new QPushButton(tr("Remove"), &dialog); row->addWidget(add); row->addWidget(remove); root->addLayout(row);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); root->addWidget(buttons);
  connect(list, &QListWidget::currentRowChanged, &dialog, [&](int row) { if (row >= 0 && row < static_cast<int>(state.templates.size())) { name->setText(QString::fromStdString(state.templates[static_cast<std::size_t>(row)].name)); parameter->setValue(state.templates[static_cast<std::size_t>(row)].parameter); } });
  connect(add, &QPushButton::clicked, &dialog, [&] { if (state.templates.size() >= wds::ui::kMaxCurveTemplates) return; wds::ui::CurveTemplate item; item.id = wds::ui::allocate_curve_template_id(state.templates); item.name = "Template " + std::to_string(state.templates.size() + 1); state.templates.push_back(item); list->addItem(QString::fromStdString(item.name)); list->setCurrentRow(list->count() - 1); });
  connect(remove, &QPushButton::clicked, &dialog, [&] { const int row = list->currentRow(); if (row >= 0) { state.templates.erase(state.templates.begin() + row); delete list->takeItem(row); } });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] { const int row = list->currentRow(); if (row >= 0 && row < static_cast<int>(state.templates.size())) { state.templates[static_cast<std::size_t>(row)].name = name->text().toStdString(); state.templates[static_cast<std::size_t>(row)].parameter = parameter->value(); normalize_curve_template(state.templates[static_cast<std::size_t>(row)]); } dialog.accept(); });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() == QDialog::Accepted) ui_manager_->set_curve_template_state_from_qt(std::move(state));
}

void EditorMainWindow::closeEvent(QCloseEvent* event) {
  if (!confirm_pending_changes()) { event->ignore(); return; }
  QSettings prefs("WDS", "WDS Editor");
  prefs.setValue("window/geometry", saveGeometry());
  prefs.setValue("window/state-v2", saveState());
  event->accept();
}
}


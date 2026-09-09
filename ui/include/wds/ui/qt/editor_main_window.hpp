#pragma once

#include <QMainWindow>
#include <QAction>
#include <QTimer>
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include <functional>
class QWindow;
class QDockWidget;

namespace wds::ui {
class UiManager;
class EditorMainWindow final : public QMainWindow {
 public:
  explicit EditorMainWindow(QWidget* parent = nullptr);
  void bind_ui_manager(UiManager* manager);
  void set_viewport_windows(QWindow* preview, QWindow* editor);
 protected:
  void closeEvent(QCloseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  bool confirm_pending_changes();
  void open_project();
  void save_project();
  void import_chart();
  void export_chart();
  void add_chart();
  void show_width_settings();
  void show_curve_templates();
 private:
  UiManager* ui_manager_ = nullptr;
  ::QAction* open_action_ = nullptr;
  ::QAction* save_action_ = nullptr;
  ::QAction* undo_action_ = nullptr;
  ::QAction* redo_action_ = nullptr;
  ::QAction* import_action_ = nullptr;
  ::QAction* export_action_ = nullptr;
  ::QAction* add_chart_action_ = nullptr;
  ::QAction* check_action_ = nullptr;
  ::QAction* width_settings_action_ = nullptr;
  ::QAction* curve_templates_action_ = nullptr;
  ::QDockWidget* preview_dock_ = nullptr;
  ::QDockWidget* editor_dock_ = nullptr;
  ::QDockWidget* settings_dock_ = nullptr;
  ::QDockWidget* workbench_dock_ = nullptr;
  ::QWindow* preview_window_ = nullptr;
  ::QWindow* editor_window_ = nullptr;
  QTimer resize_settle_timer_;
};
}

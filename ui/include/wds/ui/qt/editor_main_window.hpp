#pragma once

#include <QMainWindow>
#include <QAction>
#include <QTimer>
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include <functional>
#include <string>
class QWindow;
class QDockWidget;

namespace wds::ui {
class UiManager;
class PlaybackAudioPanel;
class EditorMainWindow final : public QMainWindow {
 public:
  explicit EditorMainWindow(QWidget* parent = nullptr);
  // Skin PNG directory for the convert-button note icons; call before bind_ui_manager.
  void set_skins_dir(std::string dir) { skins_dir_ = std::move(dir); }
  void bind_ui_manager(UiManager* manager);
  void set_viewport_windows(QWindow* preview, QWindow* editor);
 protected:
  void closeEvent(QCloseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  bool confirm_pending_changes();
  void open_project();
  void save_project();
  void import_chart();
  void import_music();
  void export_chart();
  void add_chart();
  void show_settings();
  void show_curve_templates();
 private:
  void create_playback_and_toolbox_docks();
  void reset_default_layout();
  UiManager* ui_manager_ = nullptr;
  std::string skins_dir_;
  ::QAction* open_action_ = nullptr;
  ::QAction* save_action_ = nullptr;
  ::QAction* undo_action_ = nullptr;
  ::QAction* redo_action_ = nullptr;
  ::QAction* import_action_ = nullptr;
  ::QAction* export_action_ = nullptr;
  ::QAction* music_action_ = nullptr;
  ::QAction* check_action_ = nullptr;
  ::QAction* settings_action_ = nullptr;
  ::QAction* curve_templates_action_ = nullptr;
  ::QDockWidget* preview_dock_ = nullptr;
  ::QDockWidget* editor_dock_ = nullptr;
  ::QDockWidget* settings_dock_ = nullptr;
  ::QDockWidget* playback_dock_ = nullptr;
  ::QDockWidget* toolbox_dock_ = nullptr;
  PlaybackAudioPanel* playback_panel_ = nullptr;
  ::QWindow* preview_window_ = nullptr;
  ::QWindow* editor_window_ = nullptr;
  QTimer resize_settle_timer_;
};
}

#pragma once

#include <QMainWindow>
#include <QAction>
#include <QString>
#include <QTimer>
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include "wds/ui/qt/update_checker.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
class QWindow;
class QDockWidget;
class QToolBar;

namespace wds::ui {
enum class UiChange;
class UiManager;
class PlaybackBar;
class ConvertBar;
class EditorToolbarWidget;
class SettingsPanel;
class CurveTemplatesPanel;
class EditorMainWindow final : public QMainWindow {
 public:
  explicit EditorMainWindow(QWidget* parent = nullptr);
  ~EditorMainWindow() override;
  // Skin PNG directory for the convert-button note icons; call before bind_ui_manager.
  void set_skins_dir(std::string dir) { skins_dir_ = std::move(dir); }
  // Repo / bundled icons/*.svg for the top command toolbar.
  void set_icons_dir(std::string dir);
  // OBS theme directory, for the settings appearance picker.
  void set_theme_dir(QString dir) { theme_dir_ = std::move(dir); }
  void bind_ui_manager(UiManager* manager);
  void detach_ui_manager();
  void on_preview_frame();
  // Shows the startup chooser. Returns true when the editor should continue,
  // false when the user closed the splash and the application should exit.
  bool show_startup_splash();
  void maybe_auto_check_updates();
  void set_viewport_windows(QWindow* preview, QWindow* editor);
  void set_editor_widget(QWidget* editor);
  // App-wide Space → transport toggle (except while typing / edit viewport focus).
  bool eventFilter(QObject* watched, QEvent* event) override;
 protected:
  void closeEvent(QCloseEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;
  bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
  void confirm_pending_changes(std::function<void()> on_proceed);
  void open_project();
  void save_project();
  void import_chart();
  void import_music();
  void run_with_busy(const QString& title, std::function<void()> work,
                     std::function<void()> done = {});
  void export_chart();
  void add_chart();
  void check_chart();
  void remember_recent_project(const QString& path);
 private:
  void create_control_docks();
  void reset_default_layout();
  void apply_default_dock_sizes();
  void restore_or_reset_layout();
  void add_dock_toggle(QDockWidget* dock);
  void sync_toolbox_place_checks();
  void apply_command_icons();
  void refresh_history_actions();
  void refresh_window_title();
  void handle_ui_change(UiChange change);
  void save_project_then(std::function<void()> done);
  void start_busy_job();
  void track_open_watcher(QObject* watcher);
  void untrack_open_watcher(QObject* watcher);
  void wait_open_project_watchers();
  std::array<QDockWidget*, 6> chrome_docks() const;
  UiManager* ui_manager_ = nullptr;
  struct BusyJob {
    QString title;
    std::function<void()> work;
    std::function<void()> done;
  };
  std::deque<BusyJob> busy_queue_;
  bool busy_active_ = false;
  bool force_close_ = false;
  std::uint64_t open_generation_ = 0;
  std::vector<QObject*> open_watchers_;
  std::string skins_dir_;
  std::string icons_dir_;
  QString theme_dir_;
  ::QAction* open_action_ = nullptr;
  ::QAction* save_action_ = nullptr;
  ::QAction* undo_action_ = nullptr;
  ::QAction* redo_action_ = nullptr;
  ::QAction* import_action_ = nullptr;
  ::QAction* export_action_ = nullptr;
  ::QAction* music_action_ = nullptr;
  ::QAction* check_action_ = nullptr;
  ::QAction* about_action_ = nullptr;
  ::QAction* check_updates_action_ = nullptr;
  ::QAction* fullscreen_action_ = nullptr;
  ::QToolBar* command_toolbar_ = nullptr;
  ::QToolBar* convert_toolbar_ = nullptr;
  ::QDockWidget* preview_dock_ = nullptr;
  ::QDockWidget* editor_dock_ = nullptr;
  ::QDockWidget* playback_dock_ = nullptr;
  ::QDockWidget* toolbar_dock_ = nullptr;
  ::QDockWidget* curve_dock_ = nullptr;
  ::QDockWidget* settings_dock_ = nullptr;
  PlaybackBar* playback_panel_ = nullptr;
  ConvertBar* convert_bar_ = nullptr;
  EditorToolbarWidget* toolbar_widget_ = nullptr;
  SettingsPanel* settings_panel_ = nullptr;
  CurveTemplatesPanel* curve_panel_ = nullptr;
  ::QWindow* preview_window_ = nullptr;
  ::QWindow* editor_window_ = nullptr;
  ::QWidget* editor_widget_ = nullptr;
  QTimer resize_settle_timer_;
  UpdateChecker update_checker_;
  bool native_resizing_ = false;
};
}

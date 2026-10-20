#pragma once

#include <QMainWindow>
#include <QAction>
#include <QPointer>
#include <QString>
#include <QTimer>
#include "wds/ui/editor_session.hpp"
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include "wds/ui/qt/update_checker.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
class QWindow;
class QWidget;
class QDockWidget;
class QToolBar;
class QToolButton;
class QLabel;
class QMenu;

namespace wds::ui {
enum class UiChange;
class UiManager;
class PlaybackBar;
class ConvertBar;
class EditorToolbarWidget;
class SettingsPanel;
class CurveTemplatesPanel;
class StemPanel;
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
  // Automation / --smoke-test / --quit-after: close without the unsaved-changes
  // prompt, and do not write the mutated dock layout over the user's prefs.
  void discard_unsaved_on_close();
  // CLI --seek / WDS_DUMP_VERTS: a saved layout can hide or collapse the
  // preview dock, leaving the Cocoa Vulkan surface 0×0 / unexposed so no
  // frame callback (and no WDSV dump) ever runs.
  void ensure_preview_viewport_for_cli();
  // Float the preview dock and size the Vulkan surface so the framebuffer is
  // `pixel_w` × `pixel_h` (device pixels). Developer capture; no new UI.
  void prepare_preview_capture_size(int pixel_w, int pixel_h);
  void restore_preview_after_export();
  void set_export_session_busy(bool on);
  QWindow* preview_viewport() const noexcept { return preview_window_; }
  QDockWidget* preview_dock() const noexcept { return preview_dock_; }
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
  void import_jacket();
  void run_with_busy(const QString& title, std::function<void()> work,
                     std::function<void()> done = {});
  void export_chart();
  void export_video();
  void add_chart();
  void check_chart();
  void remember_recent_project(const QString& path);
 private:
  void create_control_docks();
  void ensure_stem_panel();
  void show_stem_panel();
  void reset_default_layout();
  void apply_default_dock_sizes();
  void restore_or_reset_layout();
  void save_chrome_layout();
  void schedule_chrome_layout_save();
  void watch_chrome_layout(QWidget* widget);
  void conceal_unrevealed_floating_chrome();
  void reveal_concealed_floating_chrome();
  void add_dock_toggle(QDockWidget* dock);
  void sync_toolbox_place_checks();
  void apply_command_icons();
  void schedule_command_toolbar_relayout();
  void relayout_command_toolbar();
  bool command_shares_row_with_convert() const;
  void refresh_history_actions();
  void refresh_clear_marks_action();
  void clear_click_marks();
  double timeline_now_ms() const;
  void refresh_window_title();
  void handle_ui_change(UiChange change);
  void arm_backup_timer();
  void fire_auto_backup();
  void enqueue_backup_write(ChartBackupKind kind, std::vector<ChartBackupItem> items);
  void save_project_then(std::function<void()> done);
  QString pick_wdsproject_save_path(QWidget* parent);
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
  std::vector<QPointer<QObject>> open_watchers_;
  std::string skins_dir_;
  std::string icons_dir_;
  QString theme_dir_;
  ::QAction* open_action_ = nullptr;
  ::QAction* save_action_ = nullptr;
  ::QAction* undo_action_ = nullptr;
  ::QAction* redo_action_ = nullptr;
  ::QAction* import_action_ = nullptr;
  ::QAction* export_action_ = nullptr;
  ::QAction* export_video_action_ = nullptr;
  ::QAction* music_action_ = nullptr;
  ::QAction* jacket_action_ = nullptr;
  ::QAction* check_action_ = nullptr;
  ::QAction* clear_marks_action_ = nullptr;
  ::QAction* about_action_ = nullptr;
  ::QAction* check_updates_action_ = nullptr;
  ::QAction* fullscreen_action_ = nullptr;
  std::vector<QAction*> command_actions_;
  std::vector<QToolButton*> command_buttons_;
  std::vector<QWidget*> command_gaps_;
  QWidget* command_strip_ = nullptr;
  QLabel* command_overflow_button_ = nullptr;
  QMenu* command_overflow_menu_ = nullptr;
  bool command_toolbar_relayout_pending_ = false;
  bool command_toolbar_relayouting_ = false;
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
  StemPanel* stem_panel_ = nullptr;
  ::QWindow* preview_window_ = nullptr;
  ::QWindow* editor_window_ = nullptr;
  ::QWidget* editor_widget_ = nullptr;
  QTimer resize_settle_timer_;
  QTimer backup_timer_;
  QTimer layout_save_timer_;
  UpdateChecker update_checker_;
  std::vector<QWidget*> concealed_floating_chrome_;
  bool chrome_revealed_ = false;
  bool persist_chrome_layout_ = true;
  bool native_resizing_ = false;
  std::uint8_t click_record_held_ = 0;
};
}

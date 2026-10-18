#pragma once

#include <QDialog>
#include <QProcess>
#include <QTimer>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTemporaryDir;

namespace wds::ui {
class UiManager;

// Modeless task UI; QProcess owns inference, all editor mutations stay on the GUI thread.
class StemPanel final : public QDialog {
 public:
  explicit StemPanel(UiManager* manager, QWidget* parent = nullptr);
  ~StemPanel() override;
  void detach();
  QStringList track_names() const;
  int selected_track() const { return selected_track_; }
  bool select_track(int index);
  void set_tracks_changed_handler(std::function<void()> handler);

 protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  friend struct StemPanelTestAccess;
  void sync_source();
  void update_polling();
  void start();
  void cancel();
  void install_runtime();
  void read_output();
  void finished(int code, QProcess::ExitStatus status);
  void refresh_controls();
  void refresh_cache_dir();
  bool prepare_job();
  void launch(const QString& python, const QStringList& arguments);
  void notify_tracks_changed();
  void note_download_progress(const QString& text);
  void append_process_log(const QByteArray& bytes);
  void report_task_failure(const QString& title, const QString& summary);
  // No usable interpreter on the machine: offer the official download page.
  void prompt_python_not_installed();
  QString runtime_python() const;
  // Windows Store python.exe / python3.exe under WindowsApps is an App Execution
  // Alias and opens the Store; the real Store package lives in a subdirectory.
  static bool is_windows_store_python_alias(const QString& path);
  static QString resolve_python(const QString& path);
  static QString discover_system_python();
  // Scan only these directories (tests). Full discovery also walks Homebrew / pyenv / conda.
  static QString discover_system_python_in(const QStringList& dirs);
  // Discovery preference; versions outside 3.10–3.14 lose to supported installs.
  static int python_rank(int major, int minor, bool free_threaded);
  static bool parse_python_tag(const QString& tag, int* major, int* minor, bool* free_threaded);
  // Kill the worker and everything it spawned (ffmpeg): process group on POSIX,
  // Job Object on Windows. Always finishes with QProcess::kill().
  void kill_process_tree();
  void assign_job_object();
  void close_job_object();
  // Removes `.job-*` directories in `dir` untouched for `max_age_secs`, except `keep`.
  static int sweep_stale_jobs(const QString& dir, const QString& keep, qint64 max_age_secs);

  UiManager* manager_;
  void* job_object_ = nullptr;  // Windows HANDLE; unused elsewhere
  QString swept_dir_;
  QProcess process_;
  QTimer source_timer_;
  QTimer kill_timer_;
  std::unique_ptr<QTemporaryDir> job_;
  std::unique_ptr<QTemporaryDir> ephemeral_cache_;
  QString cache_dir_;
  QString runtime_dir_;
  bool cache_ephemeral_ = false;
  QString source_;
  QString source_stamp_;
  QString job_source_;
  QString job_stamp_;
  std::uint64_t source_generation_ = 0;
  std::uint64_t job_generation_ = 0;
  QString manifest_;
  QString error_;
  QString install_message_;
  QString last_python_;
  QStringList last_arguments_;
  QStringList tracks_;
  std::function<void()> on_tracks_changed_;
  QByteArray output_;
  QByteArray process_log_;
  bool active_ = false;
  bool cancelled_ = false;
  bool downloading_model_ = false;
  bool installing_ = false;
  bool prompted_python_missing_ = false;
  int selected_track_ = 0;
  QLabel* source_label_;
  QLabel* unsaved_warning_;
  QLabel* status_;
  QLineEdit* python_;
  QComboBox* device_;
  QSpinBox* threads_;
  QCheckBox* background_;
  QPushButton* start_;
  QPushButton* cancel_;
  QPushButton* install_;
  QPushButton* browse_;
  QProgressBar* progress_;
};
}  // namespace wds::ui

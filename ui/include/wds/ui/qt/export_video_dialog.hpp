#pragma once

#include "wds/ui/qt/export_video_ui_state.hpp"
#include "wds/ui/qt/ffmpeg_resolve.hpp"

#include <QDialog>
#include <memory>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QCloseEvent;
class QKeyEvent;
class QWidget;

namespace wds::ui {

class UiManager;
class EditorMainWindow;
class ExportVideoJob;
class FfmpegDownloader;

class ExportVideoDialog final : public QDialog {
 public:
  ExportVideoDialog(UiManager* manager, EditorMainWindow* window, QWidget* parent = nullptr);
  ~ExportVideoDialog() override;

 protected:
  void closeEvent(QCloseEvent* event) override;
  void reject() override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void apply_ui();
  void probe_env();
  void on_env_button();
  void on_left();
  void on_right();
  void start_download();
  void start_generate();
  QString default_mp4_path() const;

  UiManager* manager_ = nullptr;
  EditorMainWindow* window_ = nullptr;
  ExportVideoUiState ui_;
  FfmpegProbe probe_;
  FfmpegDownloader* downloader_ = nullptr;
  ExportVideoJob* job_ = nullptr;
  QLabel* hint_ = nullptr;
  QPushButton* env_button_ = nullptr;
  QWidget* env_row_ = nullptr;
  QComboBox* resolution_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QLabel* eta_ = nullptr;
  QPushButton* left_ = nullptr;
  QPushButton* right_ = nullptr;
};

}  // namespace wds::ui

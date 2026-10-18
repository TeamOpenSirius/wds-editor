#pragma once

#include "wds/ui/qt/export_video_span.hpp"
#include "wds/ui/qt/ffmpeg_encoder.hpp"
#include "wds/ui/qt/ffmpeg_resolve.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

class QProcess;
class QTimer;

namespace wds::ui {

class UiManager;
class EditorMainWindow;

struct ExportVideoRequest {
  QString output_mp4;
  int width = 1920;
  int height = 1080;
  bool include_music = true;
  bool include_sfx = true;
};

class ExportVideoJob final : public QObject {
 public:
  ExportVideoJob(UiManager* manager, EditorMainWindow* window, QObject* parent = nullptr);
  ~ExportVideoJob() override;

  void start(const ExportVideoRequest& req);
  void pause();
  void resume();
  void abort();
  bool running() const noexcept { return running_; }

  std::function<void(int percent, const QString& eta)> on_progress;
  std::function<void(bool ok, const QString& error)> on_finished;

 private:
  void fail(const QString& error);
  void finish_ok();
  void pump();
  void encode_next_frame();
  bool bounce_wav();
  bool start_ffmpeg();
  void kill_ffmpeg();
  void collect_hits();
  void refresh_span();
  int64_t export_duration_us() const;
  void emit_progress();
  void stop_capture();
  void restore_live_preview_geometry();
  void restore_editor_timeline();
  void finish_abort();
  QString ffmpeg_fail_detail() const;
  QString with_ffmpeg_detail(const QString& prefix) const;

  UiManager* manager_ = nullptr;
  EditorMainWindow* window_ = nullptr;
  ExportVideoRequest req_;
  ExportVideoSpan span_;
  FfmpegProbe probe_;
  H264EncodeChoice encoder_;
  QString wav_path_;
  QProcess* ffmpeg_ = nullptr;
  QTimer* timer_ = nullptr;
  std::vector<uint8_t> frame_bgra_;
  int64_t total_frames_ = 0;
  int64_t submitted_frames_ = 0;
  int64_t written_frames_ = 0;
  int64_t ema_us_ = 0;
  QElapsedTimer progress_ui_clock_;
  int capture_w_ = 0;
  int capture_h_ = 0;
  std::atomic<bool> pause_{false};
  std::atomic<bool> abort_{false};
  bool running_ = false;
  bool sized_ = false;
  bool clock_on_ = false;
  bool capture_on_ = false;
  bool started_play_ = false;
  bool in_pump_ = false;
  bool have_restore_timeline_ = false;
  int64_t restore_timeline_ms_ = 0;
  QString ffmpeg_log_;
};

}  // namespace wds::ui

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace wds::ui {

struct FfmpegProbe {
  bool ok = false;
  bool found = false;
  QString path;
  int major = 0;
  int minor = 0;
  bool has_mp4 = false;
  bool has_aac = false;
  bool has_rawvideo = false;
  bool has_wav = false;
  QStringList h264_encoders;
  bool hardware = false;
  QString error;
};

struct FfmpegDownloadSpec {
  QString filename;
  QString sha256;
  QStringList urls;
  QString archive_kind;  // tar.xz / tar.gz / zip / gz
  QString binary_name;
};

QString preview_renderer_dir();
QString cached_ffmpeg_path();
FfmpegProbe probe_ffmpeg_binary(const QString& path);
FfmpegProbe probe_ffmpeg_environment();
FfmpegDownloadSpec ffmpeg_download_spec();
int parse_ffmpeg_major_version(const QString& version_text);
QStringList parse_h264_encoders(const QString& encoders_text);
bool ffmpeg_probe_qualified(const FfmpegProbe& probe);
bool ffmpeg_has_hardware_encoder(const FfmpegProbe& probe);

class FfmpegDownloader final : public QObject {
 public:
  explicit FfmpegDownloader(QObject* parent = nullptr);
  ~FfmpegDownloader() override;

  void start();
  void abort();
  bool busy() const noexcept { return reply_ != nullptr; }

  std::function<void(qint64, qint64)> on_progress;
  std::function<void(bool, QString)> on_finished;

 private:
  void try_next();
  void finish_ok(const QString& path);
  void finish_fail(const QString& message);
  bool extract_archive(const QString& archive, const FfmpegDownloadSpec& spec, QString* error);

  QNetworkAccessManager* nam_ = nullptr;
  QNetworkReply* reply_ = nullptr;
  FfmpegDownloadSpec spec_;
  int url_index_ = 0;
  QByteArray buffer_;
  bool aborting_ = false;
};

}  // namespace wds::ui

#include "wds/ui/qt/ffmpeg_encoder.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QVector>
#include <cstring>

namespace wds::ui {
namespace {

bool has_codec(const FfmpegProbe& probe, const QString& name) {
  return probe.h264_encoders.contains(name);
}

int bitrate_for_height(int height) {
  if (height >= 2160) return 80000;
  if (height >= 1440) return 40000;
  return 20000;
}

QByteArray two_black_frames(int w, int h) {
  QByteArray frame(w * h * 4, 0);
  for (int i = 3; i < frame.size(); i += 4) frame[i] = static_cast<char>(255);
  return frame + frame;
}

}  // namespace

int bitrate_kbps_for_height(int height) { return bitrate_for_height(height); }

QStringList ranked_h264_codecs(const FfmpegProbe& probe, uint32_t vulkan_vendor_id) {
  QStringList ranked;
  auto add = [&](const QString& name) {
    if (has_codec(probe, name) && !ranked.contains(name)) ranked.push_back(name);
  };
#if defined(Q_OS_MACOS)
  add(QStringLiteral("h264_videotoolbox"));
#elif defined(_WIN32)
  if (vulkan_vendor_id == 0x10DEu) add(QStringLiteral("h264_nvenc"));
  if (vulkan_vendor_id == 0x1002u) add(QStringLiteral("h264_amf"));
  if (vulkan_vendor_id == 0x8086u) add(QStringLiteral("h264_qsv"));
  add(QStringLiteral("h264_nvenc"));
  add(QStringLiteral("h264_amf"));
  add(QStringLiteral("h264_qsv"));
  add(QStringLiteral("h264_mf"));
#else
  if (vulkan_vendor_id == 0x10DEu) add(QStringLiteral("h264_nvenc"));
  add(QStringLiteral("h264_vaapi"));
  add(QStringLiteral("h264_amf"));
  add(QStringLiteral("h264_qsv"));
#endif
  add(QStringLiteral("libx264"));
  add(QStringLiteral("libopenh264"));
  return ranked;
}

H264EncodeChoice encode_choice_for_codec(const QString& codec, int width, int height) {
  H264EncodeChoice c;
  c.codec = codec;
  const QString br = QString::number(bitrate_for_height(height)) + QStringLiteral("k");
  if (codec == QLatin1String("h264_videotoolbox")) {
    c.kind = H264EncoderKind::VideoToolbox;
    c.hardware = true;
    c.extra_output << QStringLiteral("-q:v") << QStringLiteral("65") << QStringLiteral("-g")
                   << QStringLiteral("60") << QStringLiteral("-bf") << QStringLiteral("0");
  } else if (codec == QLatin1String("h264_nvenc")) {
    c.kind = H264EncoderKind::Nvenc;
    c.hardware = true;
    c.extra_output << QStringLiteral("-b:v") << br << QStringLiteral("-g") << QStringLiteral("60")
                   << QStringLiteral("-bf") << QStringLiteral("0");
  } else if (codec == QLatin1String("h264_amf")) {
    c.kind = H264EncoderKind::Amf;
    c.hardware = true;
    c.extra_output << QStringLiteral("-b:v") << br << QStringLiteral("-g") << QStringLiteral("60")
                   << QStringLiteral("-bf") << QStringLiteral("0");
  } else if (codec == QLatin1String("h264_qsv")) {
    c.kind = H264EncoderKind::Qsv;
    c.hardware = true;
    c.extra_output << QStringLiteral("-b:v") << br << QStringLiteral("-g") << QStringLiteral("60")
                   << QStringLiteral("-bf") << QStringLiteral("0");
  } else if (codec == QLatin1String("h264_mf")) {
    c.kind = H264EncoderKind::MediaFoundation;
    c.hardware = true;
    c.extra_output << QStringLiteral("-b:v") << br << QStringLiteral("-g") << QStringLiteral("60")
                   << QStringLiteral("-bf") << QStringLiteral("0");
  } else if (codec == QLatin1String("h264_vaapi")) {
    c.kind = H264EncoderKind::Vaapi;
    c.hardware = true;
    c.extra_input << QStringLiteral("-vaapi_device") << QStringLiteral("/dev/dri/renderD128");
    c.extra_output << QStringLiteral("-vf") << QStringLiteral("format=nv12,hwupload")
                   << QStringLiteral("-b:v") << br << QStringLiteral("-g") << QStringLiteral("60")
                   << QStringLiteral("-bf") << QStringLiteral("0");
  } else if (codec == QLatin1String("libopenh264")) {
    c.kind = H264EncoderKind::OpenH264;
    c.extra_output << QStringLiteral("-b:v") << br << QStringLiteral("-g") << QStringLiteral("60")
                   << QStringLiteral("-bf") << QStringLiteral("0");
  } else {
    c.kind = H264EncoderKind::Libx264;
    c.codec = QStringLiteral("libx264");
    c.extra_output << QStringLiteral("-preset") << QStringLiteral("medium") << QStringLiteral("-b:v")
                   << br << QStringLiteral("-g") << QStringLiteral("60") << QStringLiteral("-bf")
                   << QStringLiteral("0");
  }
  (void)width;
  return c;
}

QStringList ffmpeg_video_args(const H264EncodeChoice& choice, int width, int height, int fps,
                             int out_width, int out_height, const QString& audio_wav) {
  QStringList args;
  const int dst_w = out_width > 0 ? out_width : width;
  const int dst_h = out_height > 0 ? out_height : height;
  args << choice.extra_input;
  args << QStringLiteral("-f") << QStringLiteral("rawvideo") << QStringLiteral("-pix_fmt")
       << QStringLiteral("bgra") << QStringLiteral("-s")
       << QStringLiteral("%1x%2").arg(width).arg(height) << QStringLiteral("-r")
       << QString::number(fps) << QStringLiteral("-i") << QStringLiteral("pipe:0");
  // Inputs must precede -c:v. Putting -i wav after codec flags makes ffmpeg
  // treat those flags as applying to the audio file, and it exits immediately.
  if (!audio_wav.isEmpty()) {
    args << QStringLiteral("-i") << audio_wav;
  }
  args << QStringLiteral("-c:v") << choice.codec;
  args << choice.extra_output;
  if (choice.kind != H264EncoderKind::Vaapi && (dst_w != width || dst_h != height)) {
    args << QStringLiteral("-vf") << QStringLiteral("scale=%1:%2").arg(dst_w).arg(dst_h);
  }
  if (choice.kind != H264EncoderKind::Vaapi) {
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-profile:v")
         << QStringLiteral("high");
  }
  return args;
}

bool trial_encode_codec(const QString& ffmpeg, const H264EncodeChoice& choice) {
  QTemporaryDir tmp;
  if (!tmp.isValid()) return false;
  const QString out = tmp.filePath(QStringLiteral("trial.mp4"));
  QStringList args = ffmpeg_video_args(choice, 64, 64, 60);
  args << QStringLiteral("-an") << QStringLiteral("-y") << out;
  QProcess proc;
  proc.setProgram(ffmpeg);
  proc.setArguments(args);
  proc.start();
  if (!proc.waitForStarted(3000)) return false;
  const QByteArray frames = two_black_frames(64, 64);
  proc.write(frames);
  proc.closeWriteChannel();
  if (!proc.waitForFinished(8000)) {
    proc.kill();
    proc.waitForFinished(1000);
    return false;
  }
  return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0 && QFileInfo::exists(out) &&
         QFileInfo(out).size() > 32;
}

H264EncodeChoice pick_h264_encoder(const FfmpegProbe& probe, uint32_t vulkan_vendor_id, int width,
                                   int height) {
  const auto ranked = ranked_h264_codecs(probe, vulkan_vendor_id);
  for (const auto& codec : ranked) {
    auto choice = encode_choice_for_codec(codec, width, height);
    if (trial_encode_codec(probe.path, choice)) return choice;
  }
  return encode_choice_for_codec(QStringLiteral("libx264"), width, height);
}

}  // namespace wds::ui

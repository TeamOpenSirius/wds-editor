#pragma once

#include "wds/ui/qt/ffmpeg_resolve.hpp"

#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace wds::ui {

enum class H264EncoderKind {
  VideoToolbox,
  Nvenc,
  Amf,
  Qsv,
  MediaFoundation,
  Vaapi,
  Libx264,
  OpenH264,
};

struct H264EncodeChoice {
  H264EncoderKind kind = H264EncoderKind::Libx264;
  QString codec;
  QStringList extra_input;
  QStringList extra_output;
  bool hardware = false;
};

QStringList ranked_h264_codecs(const FfmpegProbe& probe, uint32_t vulkan_vendor_id);
H264EncodeChoice encode_choice_for_codec(const QString& codec, int width, int height);
bool trial_encode_codec(const QString& ffmpeg, const H264EncodeChoice& choice);
H264EncodeChoice pick_h264_encoder(const FfmpegProbe& probe, uint32_t vulkan_vendor_id, int width,
                                   int height);
QStringList ffmpeg_video_args(const H264EncodeChoice& choice, int width, int height, int fps,
                             int out_width = 0, int out_height = 0,
                             const QString& audio_wav = {});
int bitrate_kbps_for_height(int height);

}  // namespace wds::ui

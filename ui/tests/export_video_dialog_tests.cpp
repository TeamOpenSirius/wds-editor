#include "wds/ui/qt/export_video_io.hpp"
#include "wds/ui/qt/export_video_span.hpp"
#include "wds/ui/qt/export_video_ui_state.hpp"
#include "wds/ui/qt/ffmpeg_encoder.hpp"
#include "wds/ui/qt/ffmpeg_resolve.hpp"

#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

void test_need_env_disables_generate() {
  wds::ui::ExportVideoUiState ui;
  ui.enter_need_env();
  expect(ui.phase == wds::ui::ExportVideoPhase::NeedEnv, "need env phase");
  expect(!ui.generate_enabled(), "generate disabled without env");
  expect(!ui.resolution_enabled(), "resolution disabled without env");
  expect(ui.can_close(), "idle need-env can close");
  expect(ui.left_button_text() == QStringLiteral("取消"), "left is cancel");
  expect(ui.right_button_text() == QStringLiteral("生成"), "right is generate");
  expect(!ui.right_enabled(), "generate gray");
  expect(ui.env_button_text() == QStringLiteral("下载"), "download label");
}

void test_download_locks_dialog() {
  wds::ui::ExportVideoUiState ui;
  ui.enter_need_env();
  ui.enter_downloading();
  expect(!ui.can_close(), "download cannot close");
  expect(!ui.left_enabled(), "cancel gray while download");
  // Dialog must honor can_close via closeEvent/reject, not setWindowFlag on a
  // visible modal QDialog (that hides the window, ends exec, and crashes).
  expect(ui.env_button_text() == QStringLiteral("中断"), "interrupt label");
  ui.enter_need_env();
  expect(ui.can_close(), "interrupt returns closable");
  expect(ui.env_button_text() == QStringLiteral("下载"), "download again");
  ui.enter_retry(QStringLiteral("环境下载失败"));
  expect(ui.can_close(), "retry is closable");
  expect(ui.env_button_text() == QStringLiteral("重试"), "retry label");
  expect(!ui.generate_enabled(), "generate still gray");
}

void test_ready_soft_and_hw() {
  wds::ui::ExportVideoUiState ui;
  ui.enter_ready(true);
  expect(ui.generate_enabled(), "soft ready can generate");
  expect(ui.show_env_row(), "soft hint visible");
  expect(!ui.show_env_button(), "no download button when ready");
  ui.enter_ready(false);
  expect(!ui.show_env_row(), "hw ready hides env row");
}

void test_encode_pause_abort_done() {
  wds::ui::ExportVideoUiState ui;
  ui.enter_ready(false);
  ui.enter_encoding();
  expect(!ui.can_close(), "encoding cannot close");
  expect(!ui.resolution_enabled(), "resolution locked");
  expect(ui.left_button_text() == QStringLiteral("暂停"), "pause");
  expect(ui.right_button_text() == QStringLiteral("打断"), "abort");
  ui.enter_paused();
  expect(ui.left_button_text() == QStringLiteral("继续"), "resume");
  expect(!ui.can_close(), "paused cannot close");
  ui.enter_ready(false);
  expect(ui.can_close(), "abort returns ready");
  ui.enter_encoding();
  ui.enter_done();
  expect(ui.can_close(), "done can close");
  expect(!ui.left_enabled(), "cancel gray on done");
  expect(ui.right_button_text() == QStringLiteral("关闭"), "close");
  expect(ui.progress == 100, "done is 100");
}

void test_ffmpeg_parse() {
  expect(wds::ui::parse_ffmpeg_major_version(QStringLiteral("ffmpeg version 7.1.1")) == 7,
         "7.1 major");
  expect(wds::ui::parse_ffmpeg_major_version(QStringLiteral("ffmpeg version n8.1-1")) == 8,
         "n8.1 major");
  expect(wds::ui::parse_ffmpeg_major_version(QStringLiteral("ffmpeg version 5.1")) == 5, "5.1 major");
  const auto enc = wds::ui::parse_h264_encoders(
      QStringLiteral(" V..... h264_videotoolbox\n V..... libx264\n"));
  expect(enc.contains(QStringLiteral("h264_videotoolbox")), "vt listed");
  expect(enc.contains(QStringLiteral("libx264")), "x264 listed");
  wds::ui::FfmpegProbe p;
  p.major = 7;
  p.has_mp4 = true;
  p.has_aac = true;
  p.has_rawvideo = true;
  p.has_wav = true;
  p.h264_encoders = enc;
  expect(!p.ok, "ok starts false");
  expect(wds::ui::ffmpeg_probe_qualified(p), "qualified ignores ok");
  expect(wds::ui::ffmpeg_has_hardware_encoder(p), "vt is hardware");
  p.h264_encoders = {QStringLiteral("libx264")};
  expect(!wds::ui::ffmpeg_has_hardware_encoder(p), "x264 is software");
  p.major = 5;
  expect(!wds::ui::ffmpeg_probe_qualified(p), "5.x unqualified");
}

void test_ranked_windows_covers_nv_amf_qsv() {
  wds::ui::FfmpegProbe p;
  p.h264_encoders = {QStringLiteral("h264_nvenc"), QStringLiteral("h264_amf"),
                     QStringLiteral("h264_qsv"), QStringLiteral("h264_mf"),
                     QStringLiteral("libx264")};
#if defined(_WIN32)
  const uint32_t vendors[] = {0x10DEu, 0x1002u, 0x8086u};
  for (uint32_t vendor : vendors) {
    const auto ranked = wds::ui::ranked_h264_codecs(p, vendor);
    expect(ranked.contains(QStringLiteral("h264_nvenc")), "windows lists nvenc");
    expect(ranked.contains(QStringLiteral("h264_amf")), "windows lists amf");
    expect(ranked.contains(QStringLiteral("h264_qsv")), "windows lists qsv");
    expect(ranked.contains(QStringLiteral("h264_mf")), "windows lists mf");
  }
  const auto nvidia = wds::ui::ranked_h264_codecs(p, 0x10DEu);
  expect(!nvidia.isEmpty() && nvidia.front() == QStringLiteral("h264_nvenc"),
         "nvidia vendor prefers nvenc");
  const auto amd = wds::ui::ranked_h264_codecs(p, 0x1002u);
  expect(!amd.isEmpty() && amd.front() == QStringLiteral("h264_amf"), "amd vendor prefers amf");
  const auto intel = wds::ui::ranked_h264_codecs(p, 0x8086u);
  expect(!intel.isEmpty() && intel.front() == QStringLiteral("h264_qsv"),
         "intel vendor prefers qsv");
#else
  const auto ranked = wds::ui::ranked_h264_codecs(p, 0x10DEu);
  expect(!ranked.isEmpty(), "non-windows ranking still resolves");
#endif
}

void test_write_all_stdin_rejects_null() {
  std::vector<std::uint8_t> frame(16, 0);
  expect(!wds::ui::write_all_stdin(nullptr, frame), "null process");
  expect(!wds::ui::write_all_stdin(nullptr, frame.data(), frame.size()), "null pointer overload");
}

void test_ranked_macos_prefers_vt() {
  wds::ui::FfmpegProbe p;
  p.h264_encoders = {QStringLiteral("libx264"), QStringLiteral("h264_videotoolbox"),
                     QStringLiteral("h264_nvenc")};
  const auto ranked = wds::ui::ranked_h264_codecs(p, 0x106Bu);
#if defined(Q_OS_MACOS)
  expect(!ranked.isEmpty() && ranked.front() == QStringLiteral("h264_videotoolbox"),
         "macos ranks videotoolbox first");
#else
  expect(!ranked.isEmpty(), "ranked not empty");
#endif
}

void test_download_spec_is_pinned() {
  const auto spec = wds::ui::ffmpeg_download_spec();
  expect(!spec.filename.isEmpty(), "filename pinned");
  expect(!spec.sha256.isEmpty(), "sha256 pinned");
  expect(!spec.urls.isEmpty(), "url list");
#if defined(Q_OS_MACOS)
  // No npmmirror host for this artifact; CN GitHub proxies first, then origin.
  expect(spec.archive_kind == QLatin1String("tar.gz"), "macos uses tar.gz");
  expect(spec.urls.size() >= 3, "macos has three sources");
  expect(spec.urls.front().contains(QStringLiteral("gh-proxy.com")), "china proxy first");
  expect(spec.urls.back().startsWith(QStringLiteral("https://github.com/")), "origin last");
  expect(spec.urls.front().contains(QStringLiteral("ffmpeg-lgpl-builds")), "macos uses lgpl-builds");
#else
  expect(spec.urls.front().contains(QStringLiteral("npmmirror")), "china mirror first");
#endif
  for (const auto& url : spec.urls) {
    expect(!url.contains(QStringLiteral("/latest/")), "no unpinned latest");
  }
}

void test_export_video_span_pads_music_and_chart() {
  using wds::ui::compute_export_video_span;
  {
    const auto s = compute_export_video_span(true, 180000, 0, 10000);
    expect(s.start_ms == -2000, "music+chart start is -2s");
    expect(s.end_ms == 182000, "music later than notes +2s");
    expect(s.duration_ms() == 184000, "span length");
  }
  {
    const auto s = compute_export_video_span(false, 0, 0, 10000);
    expect(s.start_ms == -2000, "chart-only start is -2s");
    expect(s.end_ms == 12000, "last note +2s");
  }
  {
    const auto s = compute_export_video_span(true, 8000, 0, 10500);
    expect(s.start_ms == -2000, "split later than music still pads start");
    expect(s.end_ms == 12500, "split destroy +2s beats music");
  }
  {
    const auto s = compute_export_video_span(true, 10000, -3000, 4000);
    expect(s.start_ms == -5000, "negative chart start then -2s");
    expect(s.end_ms == 12000, "music end +2s");
  }
  {
    const auto s = compute_export_video_span(false, 0, 0, 0);
    expect(s.start_ms == -2000, "empty chart start");
    expect(s.end_ms == 2000, "empty chart end");
  }
}

void test_encode_choice_gop() {
  const auto vt = wds::ui::encode_choice_for_codec(QStringLiteral("h264_videotoolbox"), 1920, 1080);
  expect(vt.extra_output.contains(QStringLiteral("-g")), "gop flag");
  expect(vt.extra_output.contains(QStringLiteral("60")), "gop 60");
  expect(wds::ui::bitrate_kbps_for_height(1080) == 20000, "1080p 20m");
  expect(wds::ui::bitrate_kbps_for_height(1440) == 40000, "2k 40m");
  expect(wds::ui::bitrate_kbps_for_height(2160) == 80000, "4k 80m");
  expect(vt.extra_output.contains(QStringLiteral("-b:v")), "vt uses bitrate");
  expect(vt.extra_output.contains(QStringLiteral("20000k")), "vt 1080p 20m");
  const auto vt2k = wds::ui::encode_choice_for_codec(QStringLiteral("h264_videotoolbox"), 2560, 1440);
  expect(vt2k.extra_output.contains(QStringLiteral("40000k")), "vt 2k 40m");
  const auto vt4k = wds::ui::encode_choice_for_codec(QStringLiteral("h264_videotoolbox"), 3840, 2160);
  expect(vt4k.extra_output.contains(QStringLiteral("80000k")), "vt 4k 80m");
  const auto x264 = wds::ui::encode_choice_for_codec(QStringLiteral("libx264"), 3840, 2160);
  expect(x264.extra_output.contains(QStringLiteral("80000k")), "x264 4k 80m");
}

void test_ffmpeg_video_args_scale_when_capture_differs() {
  const auto vt = wds::ui::encode_choice_for_codec(QStringLiteral("h264_videotoolbox"), 1920, 1080);
  const auto same = wds::ui::ffmpeg_video_args(vt, 1920, 1080, 60, 1920, 1080);
  expect(same.contains(QStringLiteral("1920x1080")), "raw size when capture matches");
  expect(!same.contains(QStringLiteral("-vf")), "no scale when capture matches");
  const auto scaled = wds::ui::ffmpeg_video_args(vt, 1280, 720, 60, 1920, 1080);
  expect(scaled.contains(QStringLiteral("1280x720")), "raw size is capture");
  expect(scaled.contains(QStringLiteral("-vf")), "scale when capture differs");
  expect(scaled.contains(QStringLiteral("scale=1920:1080")), "scale to requested output");
}

void test_ffmpeg_audio_input_before_video_codec() {
  const auto vt = wds::ui::encode_choice_for_codec(QStringLiteral("h264_videotoolbox"), 1920, 1080);
  const QString wav = QStringLiteral("/tmp/wds-export.wav");
  const auto args = wds::ui::ffmpeg_video_args(vt, 1920, 1080, 60, 0, 0, wav);
  const int pipe = args.indexOf(QStringLiteral("pipe:0"));
  const int wav_i = args.indexOf(wav);
  const int cv = args.indexOf(QStringLiteral("-c:v"));
  expect(pipe >= 0, "rawvideo pipe input");
  const int tq = args.indexOf(QStringLiteral("-thread_queue_size"));
  expect(tq >= 0 && tq < pipe, "thread_queue_size before pipe");
  expect(wav_i == pipe + 2, "wav is the second input");
  expect(wav_i > 0 && args.at(wav_i - 1) == QStringLiteral("-i"), "-i wav");
  expect(cv > wav_i, "video codec applies to the output, not the wav");
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  test_need_env_disables_generate();
  test_download_locks_dialog();
  test_ready_soft_and_hw();
  test_encode_pause_abort_done();
  test_ffmpeg_parse();
  test_ranked_windows_covers_nv_amf_qsv();
  test_write_all_stdin_rejects_null();
  test_ranked_macos_prefers_vt();
  test_download_spec_is_pinned();
  test_export_video_span_pads_music_and_chart();
  test_encode_choice_gop();
  test_ffmpeg_video_args_scale_when_capture_differs();
  test_ffmpeg_audio_input_before_video_codec();
  if (failures != 0) {
    std::fprintf(stderr, "%d export-video dialog tests failed\n", failures);
    return 1;
  }
  return 0;
}

#include "wds/ui/qt/export_video_job.hpp"
#include "wds/ui/qt/export_video_io.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/preview/hit_sfx_mapping.hpp"
#include "wds/ui/regions/settings/preview_settings_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/audio/hit_sfx_policy.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/core/types.hpp>
#include <wds/common/time.hpp>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace wds::ui {
namespace {

using wds::chart_editor::NoteType;
using wds::chart_editor::is_hold_body;
using wds::chart_editor::is_hold_mid_star;
using wds::chart_editor::is_hold_start;
using wds::chart_editor::is_hold_with_tail;
using wds::chart_editor::is_split_lane_gimmick;

uint64_t span_key(int64_t start_ms, int32_t lane, int32_t width) noexcept {
  return (static_cast<uint64_t>(static_cast<uint32_t>(start_ms)) << 32) |
         (static_cast<uint64_t>(static_cast<uint16_t>(lane)) << 16) |
         static_cast<uint16_t>(width);
}

QString format_eta(int64_t remain_us) {
  if (remain_us <= 0) return QStringLiteral("约 0:00");
  const int64_t sec = (remain_us + 500000) / 1000000;
  const int m = static_cast<int>(sec / 60);
  const int s = static_cast<int>(sec % 60);
  return QStringLiteral("约 %1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

}  // namespace

ExportVideoJob::ExportVideoJob(UiManager* manager, EditorMainWindow* window, QObject* parent)
    : QObject(parent), manager_(manager), window_(window) {
  timer_ = new QTimer(this);
  timer_->setTimerType(Qt::PreciseTimer);
  timer_->setInterval(1);
  QObject::connect(timer_, &QTimer::timeout, this, [this] { pump(); });
}

ExportVideoJob::~ExportVideoJob() {
  on_progress = {};
  on_finished = {};
  abort_ = true;
  if (timer_ != nullptr) timer_->stop();
  kill_ffmpeg();
  stop_capture();
}

void ExportVideoJob::start(const ExportVideoRequest& req) {
  if (running_ || manager_ == nullptr || window_ == nullptr) return;
  req_ = req;
  span_ = {};
  abort_ = false;
  pause_ = false;
  running_ = true;
  sized_ = false;
  clock_on_ = false;
  capture_on_ = false;
  started_play_ = false;
  in_pump_ = false;
  have_restore_timeline_ = false;
  restore_timeline_ms_ = 0;
  ffmpeg_log_.clear();
  submitted_frames_ = 0;
  written_frames_ = 0;
  ema_us_ = 0;
  progress_ui_clock_.invalidate();
  capture_w_ = 0;
  capture_h_ = 0;
  window_->set_export_session_busy(true);
  timer_->start();
}

void ExportVideoJob::pause() {
  pause_ = true;
  if (timer_ != nullptr) timer_->stop();
}

void ExportVideoJob::resume() {
  pause_ = false;
  if (running_ && timer_ != nullptr) timer_->start();
}

void ExportVideoJob::abort() {
  abort_ = true;
  pause_ = false;
  if (timer_ != nullptr) timer_->stop();
  if (running_) finish_abort();
}

void ExportVideoJob::kill_ffmpeg() {
  if (ffmpeg_ == nullptr) return;
  ffmpeg_->closeWriteChannel();
  ffmpeg_->kill();
  ffmpeg_->waitForFinished(2000);
  ffmpeg_->deleteLater();
  ffmpeg_ = nullptr;
}

void ExportVideoJob::restore_live_preview_geometry() {
  if (manager_ == nullptr) return;
  auto* pw = window_ != nullptr ? window_->preview_viewport() : nullptr;
  if (pw == nullptr) {
    auto& preview = manager_->chart_preview();
    const int w = std::max(1, preview.preview().vulkan().framebuffer_width());
    const int h = std::max(1, preview.preview().vulkan().framebuffer_height());
    manager_->resize_preview_viewport(w, h, w, h);
    return;
  }
  const qreal dpr = std::max<qreal>(1.0, pw->devicePixelRatio());
  const int lw = std::max(1, pw->width());
  const int lh = std::max(1, pw->height());
  manager_->resize_preview_viewport(lw, lh, std::max(1, qRound(static_cast<qreal>(lw) * dpr)),
                                   std::max(1, qRound(static_cast<qreal>(lh) * dpr)));
}

void ExportVideoJob::stop_capture() {
  if (manager_ == nullptr) return;
  auto& preview = manager_->chart_preview();
  if (capture_on_) {
    preview.preview().vulkan().end_export_capture();
    capture_on_ = false;
  }
  if (clock_on_) {
    preview.transport().request_pause();
    preview.tick(0);
    preview.set_video_export_clock(false);
    clock_on_ = false;
  }
  restore_live_preview_geometry();
  restore_editor_timeline();
  if (window_ != nullptr) window_->set_export_session_busy(false);
}

void ExportVideoJob::restore_editor_timeline() {
  if (!have_restore_timeline_ || manager_ == nullptr) return;
  have_restore_timeline_ = false;
  auto& preview = manager_->chart_preview();
  preview.transport().request_pause();
  preview.request_seek_ms(restore_timeline_ms_);
  preview.tick(0);
  if (window_ != nullptr) window_->on_preview_frame();
}

void ExportVideoJob::finish_abort() {
  if (!running_) return;
  running_ = false;
  if (timer_ != nullptr) timer_->stop();
  kill_ffmpeg();
  if (!req_.output_mp4.isEmpty()) QFile::remove(req_.output_mp4);
  if (!wav_path_.isEmpty()) QFile::remove(wav_path_);
  stop_capture();
  submitted_frames_ = 0;
  written_frames_ = 0;
  sized_ = false;
  if (on_finished) on_finished(false, {});
}

void ExportVideoJob::fail(const QString& error) {
  if (!running_) return;
  running_ = false;
  if (timer_ != nullptr) timer_->stop();
  kill_ffmpeg();
  if (!req_.output_mp4.isEmpty()) QFile::remove(req_.output_mp4);
  if (!wav_path_.isEmpty()) QFile::remove(wav_path_);
  stop_capture();
  if (on_finished) on_finished(false, error);
}

void ExportVideoJob::finish_ok() {
  if (!running_) return;
  if (timer_ != nullptr) timer_->stop();
  if (ffmpeg_ != nullptr) {
    ffmpeg_->closeWriteChannel();
    ffmpeg_->waitForFinished(300000);
    if (!running_) return;
    const int code = ffmpeg_->exitCode();
    const bool ok = ffmpeg_->exitStatus() == QProcess::NormalExit && code == 0;
    ffmpeg_->deleteLater();
    ffmpeg_ = nullptr;
    if (!ok) {
      fail(with_ffmpeg_detail(QStringLiteral("编码失败")));
      return;
    }
  }
  if (!running_) return;
  if (!wav_path_.isEmpty()) QFile::remove(wav_path_);
  stop_capture();
  running_ = false;
  if (on_progress) on_progress(100, QStringLiteral("—"));
  if (on_finished) on_finished(true, {});
}

void ExportVideoJob::emit_progress() {
  if (total_frames_ <= 0 || written_frames_ <= 0) return;
  // ~1 s of wall time (≈60 frames at 60 fps). Per-frame updates made the ETA flicker.
  if (progress_ui_clock_.isValid() && progress_ui_clock_.elapsed() < 1000) return;
  progress_ui_clock_.start();
  const int pct = static_cast<int>(std::clamp((written_frames_ * 100) / total_frames_, int64_t{0}, int64_t{99}));
  int64_t remain = 0;
  if (ema_us_ > 0 && written_frames_ < total_frames_) {
    remain = ema_us_ * (total_frames_ - written_frames_);
  }
  if (on_progress) on_progress(pct, format_eta(remain));
}

void ExportVideoJob::refresh_span() {
  auto& preview = manager_->chart_preview();
  const auto& doc = preview.engine().document();
  const auto& timing = doc.timing();
  int64_t chart_start = preview.transport().chart_start_ms();
  int64_t chart_end = chart_start;
  for (const auto& note : doc.notes()) {
    const int64_t s = note.start_ms(timing);
    const int64_t e = note.end_ms(timing);
    chart_start = std::min(chart_start, s);
    chart_end = std::max(chart_end, s);
    chart_end = std::max(chart_end, e);
    if (is_split_lane_gimmick(note.gimmick_type)) {
      chart_end = std::max(chart_end, wds::chart_editor::official_split_destroy_ms(s, e));
    }
  }
  const auto& audio = preview.transport().audio();
  const bool has_music = audio.has_music() && audio.duration().count() > 0;
  const int64_t music_end_ms = has_music ? audio.duration_ms() : 0;
  span_ = compute_export_video_span(has_music, music_end_ms, chart_start, chart_end);
}

int64_t ExportVideoJob::export_duration_us() const {
  return std::max<int64_t>(wds::common::ms_to_us(span_.duration_ms()).count(), 1'000'000 / 60);
}

void ExportVideoJob::collect_hits() {}

bool ExportVideoJob::bounce_wav() {
  auto& preview = manager_->chart_preview();
  auto& audio = preview.transport().audio();
  const int64_t dur = export_duration_us();
  wav_path_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
              QStringLiteral("/wds-export-") +
              QString::number(reinterpret_cast<quintptr>(this), 16) + QStringLiteral(".wav");
  std::vector<wds::audio::AudioEngine::BounceHit> hits;
  std::vector<wds::audio::AudioEngine::BounceHold> holds;
  if (req_.include_sfx) {
    const auto& doc = preview.engine().document();
    const auto& timing = doc.timing();
    std::unordered_set<uint64_t> duration_heads;
    for (const auto& note : doc.notes()) {
      if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type) &&
          note.end_ms(timing) > note.start_ms(timing)) {
        duration_heads.insert(span_key(note.start_ms(timing), note.lane, note.width));
      }
    }
    wds::audio::SfxPlayHistory history;
    wds::audio::SfxInstantBudget budget;
    auto emit_hit = [&](NoteType type, wds::audio::HitSfxClip clip, int64_t ms, bool bypass) {
      if (clip == wds::audio::HitSfxClip::Count) return;
      if (!history.can_admit(ms, clip, bypass) || !budget.can_admit(ms)) return;
      history.record(ms, clip, bypass);
      budget.record(ms);
      wds::audio::AudioEngine::BounceHit h;
      h.clip = clip;
      h.at = wds::common::ms_to_us(ms);
      hits.push_back(h);
      (void)type;
    };
    for (const auto& note : doc.notes()) {
      const bool hold_body = is_hold_body(note.note_type);
      const bool mid_star = is_hold_mid_star(note.note_type);
      const bool with_tail = is_hold_with_tail(note.note_type);
      const bool duration_hold = hold_body && !mid_star && note.end_ms(timing) > note.start_ms(timing);
      if (!hold_body && !mid_star && !is_split_lane_gimmick(note.gimmick_type)) {
        if (is_hold_start(note.note_type) || note.note_type == NoteType::Normal ||
            note.note_type == NoteType::Critical || wds::chart_editor::is_flick_like(note.note_type) ||
            note.note_type == NoteType::BlueTap) {
          const bool paired =
              is_hold_start(note.note_type) &&
              duration_heads.count(span_key(note.start_ms(timing), note.lane, note.width)) != 0;
          if (!paired) {
            emit_hit(note.note_type, hit_sfx_clip_for_head(note.note_type), note.start_ms(timing),
                     hit_sfx_bypasses_history(note.note_type));
          }
        }
      }
      if (duration_hold) {
        emit_hit(note.note_type, hit_sfx_clip_for_hold_body_start(note.note_type),
                 note.start_ms(timing), false);
        if (!preview.preview().mute_hold_body_sfx()) {
          wds::audio::AudioEngine::BounceHold hold;
          hold.start = wds::common::ms_to_us(note.start_ms(timing));
          hold.end = wds::common::ms_to_us(note.end_ms(timing));
          holds.push_back(hold);
        }
      }
      if (with_tail && note.end_ms(timing) > note.start_ms(timing)) {
        emit_hit(note.note_type, hit_sfx_clip_for_hold_tail(note.note_type), note.end_ms(timing),
                 false);
      }
      if (mid_star) {
        emit_hit(note.note_type, hit_sfx_clip_for_mid_star(note.note_type), note.start_ms(timing),
                 hit_sfx_bypasses_history(note.note_type));
      }
    }
  }
  if (!req_.include_music && !req_.include_sfx) {
    wav_path_.clear();
    return true;
  }
  if (!audio.bounce_mix_wav(wav_path_.toStdString(), wds::common::ms_to_us(span_.start_ms),
                            wds::common::Microseconds{dur}, hits, holds, req_.include_music,
                            req_.include_sfx)) {
    wav_path_.clear();
    return req_.include_music == false && req_.include_sfx == false;
  }
  return true;
}

QString ExportVideoJob::ffmpeg_fail_detail() const {
  QString s = ffmpeg_log_;
  if (ffmpeg_ != nullptr) {
    s += QString::fromUtf8(ffmpeg_->readAll());
    if (s.trimmed().isEmpty()) s = ffmpeg_->errorString();
  }
  s = s.trimmed();
  if (s.size() > 360) s = s.right(360);
  return s;
}

QString ExportVideoJob::with_ffmpeg_detail(const QString& prefix) const {
  const QString d = ffmpeg_fail_detail();
  if (d.isEmpty()) return prefix;
  return prefix + QLatin1Char('\n') + d;
}

bool ExportVideoJob::start_ffmpeg() {
  ffmpeg_log_.clear();
  probe_ = probe_ffmpeg_environment();
  if (!probe_.ok) {
    ffmpeg_log_ = probe_.error.isEmpty() ? QStringLiteral("找不到 ffmpeg") : probe_.error;
    return false;
  }
  auto& vk = manager_->chart_preview().preview().vulkan();
  encoder_ = pick_h264_encoder(probe_, vk.physical_vendor_id(), req_.width, req_.height);
  const QString wav = (!wav_path_.isEmpty() && QFileInfo::exists(wav_path_)) ? wav_path_ : QString();
  QStringList args = ffmpeg_video_args(encoder_, capture_w_, capture_h_, 60, 0, 0, wav);
  if (!wav.isEmpty()) {
    args << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a")
         << QStringLiteral("320k") << QStringLiteral("-shortest");
  } else {
    args << QStringLiteral("-an");
  }
  args << QStringLiteral("-movflags") << QStringLiteral("+faststart") << QStringLiteral("-y")
       << req_.output_mp4;
  ffmpeg_ = new QProcess(this);
  ffmpeg_->setProgram(probe_.path);
  ffmpeg_->setArguments(args);
  ffmpeg_->setProcessChannelMode(QProcess::MergedChannels);
  QObject::connect(ffmpeg_, &QProcess::readyRead, this, [this] {
    if (ffmpeg_ == nullptr) return;
    ffmpeg_log_ += QString::fromUtf8(ffmpeg_->readAll());
    if (ffmpeg_log_.size() > 8000) ffmpeg_log_ = ffmpeg_log_.right(4000);
  });
  ffmpeg_->start();
  if (!ffmpeg_->waitForStarted(5000)) {
    ffmpeg_log_ = ffmpeg_->errorString();
    return false;
  }
  // Bad graphs (codec flags applied to the wrong input) exit during encoder
  // init, before any frame is written. waitForStarted still returns true.
  if (ffmpeg_->waitForFinished(80)) {
    ffmpeg_log_ += QString::fromUtf8(ffmpeg_->readAll());
    return false;
  }
  return ffmpeg_->state() == QProcess::Running;
}

void ExportVideoJob::pump() {
  if (!running_ || in_pump_) return;
  in_pump_ = true;
  if (abort_) {
    finish_abort();
    in_pump_ = false;
    return;
  }
  if (!pause_) encode_next_frame();
  in_pump_ = false;
}

void ExportVideoJob::encode_next_frame() {
  if (!running_ || abort_ || pause_) return;
  auto& preview = manager_->chart_preview();
  if (!preview.ready()) return;

  auto& vk = preview.preview().vulkan();
  if (!sized_) {
    if (timer_ != nullptr) timer_->stop();
    restore_timeline_ms_ = preview.engine().timeline_ms();
    have_restore_timeline_ = true;
    preview.set_video_export_clock(true);
    clock_on_ = true;
    refresh_span();
    preview.request_seek_ms(span_.start_ms);
    preview.transport().request_play();
    preview.tick(0);
    if (on_progress) on_progress(0, QStringLiteral("正在准备音频…"));
    if (!bounce_wav()) {
      fail(QStringLiteral("音频混合失败"));
      return;
    }
    if (!running_ || abort_) return;
    if (!vk.begin_export_capture(req_.width, req_.height)) {
      fail(QStringLiteral("无法创建离屏画面"));
      return;
    }
    capture_on_ = true;
    capture_w_ = vk.framebuffer_width();
    capture_h_ = vk.framebuffer_height();
    if (capture_w_ < 16 || capture_h_ < 16) {
      fail(QStringLiteral("无法创建离屏画面"));
      return;
    }
    total_frames_ = std::max<int64_t>(1, (export_duration_us() * 60) / 1'000'000);
    if (!start_ffmpeg()) {
      fail(with_ffmpeg_detail(QStringLiteral("无法启动编码器")));
      return;
    }
    if (!running_ || abort_) return;
    preview.apply_export_geometry(capture_w_, capture_h_);
    sized_ = true;
    started_play_ = true;
    if (running_ && !pause_ && !abort_ && timer_ != nullptr) timer_->start();
  }

  if (ffmpeg_ == nullptr || ffmpeg_->state() != QProcess::Running) {
    fail(with_ffmpeg_detail(QStringLiteral("编码失败")));
    return;
  }

  if (vk.framebuffer_width() != capture_w_ || vk.framebuffer_height() != capture_h_) {
    fail(QStringLiteral("预览尺寸已改变"));
    return;
  }

  QElapsedTimer frame_timer;
  frame_timer.start();
  constexpr int kSubmitAhead = 2;
  if (ffmpeg_->bytesToWrite() == 0 && submitted_frames_ < total_frames_ &&
      submitted_frames_ - written_frames_ < kSubmitAhead) {
    preview.render();
    ++submitted_frames_;
    if (submitted_frames_ < total_frames_) preview.tick(0);
  }

  int w = 0;
  int h = 0;
  while (written_frames_ < total_frames_) {
    const std::uint8_t* aliased = nullptr;
    if (!vk.take_export_bgra(frame_bgra_, w, h, &aliased)) break;
    if (w != capture_w_ || h != capture_h_) continue;
    const std::uint8_t* data = aliased != nullptr ? aliased : frame_bgra_.data();
    const std::size_t bytes = aliased != nullptr
                                  ? static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u
                                  : frame_bgra_.size();
    if (!write_all_stdin(ffmpeg_, data, bytes)) {
      fail(with_ffmpeg_detail(QStringLiteral("编码失败")));
      return;
    }
    ++written_frames_;
  }

  if (submitted_frames_ >= 8 && written_frames_ == 0) {
    fail(QStringLiteral("无法读取画面"));
    return;
  }

  const int64_t used = frame_timer.nsecsElapsed() / 1000;
  ema_us_ = ema_us_ == 0 ? used : (ema_us_ * 7 + used) / 8;
  emit_progress();
  if (submitted_frames_ >= total_frames_) {
    while (written_frames_ < total_frames_) {
      const std::uint8_t* aliased = nullptr;
      if (!vk.drain_export_bgra(frame_bgra_, w, h, &aliased)) break;
      if (w != capture_w_ || h != capture_h_) continue;
      const std::uint8_t* data = aliased != nullptr ? aliased : frame_bgra_.data();
      const std::size_t bytes = aliased != nullptr
                                    ? static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u
                                    : frame_bgra_.size();
      if (!write_all_stdin(ffmpeg_, data, bytes)) {
        fail(with_ffmpeg_detail(QStringLiteral("编码失败")));
        return;
      }
      ++written_frames_;
    }
    if (written_frames_ >= total_frames_) {
      finish_ok();
      return;
    }
  }
}

}  // namespace wds::ui

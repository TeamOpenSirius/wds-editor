#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include <wds/audio/transport.hpp>
#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/wds_theme.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/frame_diag.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/resource_paths.hpp"
#include "wds/ui/startup_deps.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/preview/playback_preview.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/renderer/preview_visual_config.hpp"
#include <wds/core/official_playfield.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/types.hpp>
#include "wds/common/crash_handler.hpp"
#include "wds/common/log.hpp"
#include "wds/common/perf_stages.hpp"
#include "wds/common/runtime_env.hpp"
#include "wds/common/utf8_path.hpp"
#include "wds/renderer/vulkan_renderer.hpp"
#include <wds/chart_render/split_effect_particles.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QPushButton>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScreen>
#include <QSysInfo>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTimer>
#include <QUrl>
#include <QMessageBox>
#include <QWidget>
#include <QFontDatabase>
#include <QFont>
#include <QPixmap>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#if !defined(Q_OS_WIN)
#include <sys/resource.h>
#endif
#include <unordered_map>
#include <vector>

#if defined(Q_OS_MACOS)
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#endif
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void add_qt_plugin_path(const QString& plugins, bool prepend) {
  if (plugins.isEmpty() || !QDir(plugins).exists()) return;
  const QByteArray extra = QFile::encodeName(QDir::cleanPath(plugins));
  const QByteArray cur = qgetenv("QT_PLUGIN_PATH");
#if defined(Q_OS_WIN)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  if (cur.isEmpty()) {
    qputenv("QT_PLUGIN_PATH", extra);
  } else if (!cur.contains(extra)) {
    qputenv("QT_PLUGIN_PATH", prepend ? extra + sep + cur : cur + sep + extra);
  }
}

void prepare_bundled_qt_plugins(const char* argv0) {
  QString bundled;
#if defined(Q_OS_MACOS)
  // Qt 6.6+ backs Vulkan/Metal QWindows with QMetalLayer, whose per-frame
  // displayLayer: cycle (display lock + presentsWithTransaction toggling around a
  // synchronous expose) races MoltenVK's main-thread presents: the preview can
  // stop updating at playback start while frames keep being presented. Opt out
  // to a plain CAMetalLayer; respect an explicit user setting.
  if (!qEnvironmentVariableIsSet("QT_MTL_NO_TRANSACTION")) qputenv("QT_MTL_NO_TRANSACTION", "1");
  // Flattened Homebrew Qt has no framework prefix, so QLibraryInfo cannot find
  // plugins. Point Qt at the bundled cocoa plugin before QApplication starts.
  char path[PATH_MAX];
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    bundled = QFileInfo(QString::fromUtf8(path)).absoluteDir().filePath(
        QStringLiteral("lib/plugins"));
    add_qt_plugin_path(bundled, true);
  }
#endif
#if defined(Q_OS_WIN)
  wchar_t exe[MAX_PATH];
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0) {
    bundled = QFileInfo(QString::fromWCharArray(exe)).absoluteDir().filePath(
        QStringLiteral("plugins"));
    add_qt_plugin_path(bundled, true);
  } else if (argv0 != nullptr) {
    bundled = QFileInfo(QString::fromLocal8Bit(argv0)).absoluteDir().filePath(
        QStringLiteral("plugins"));
    add_qt_plugin_path(bundled, true);
  }
#else
  (void)argv0;
#endif
#if defined(Q_OS_LINUX)
  const auto exe_dir = wds::common::executable_dir(argv0);
  bundled = QString::fromStdString(wds::common::path_to_utf8(exe_dir / ".." / "plugins"));
  if (QDir(bundled).exists()) add_qt_plugin_path(bundled, true);
#endif
#if defined(WDS_QT_SVG_PLUGIN_DIR)
  // Homebrew / MinGW splits qtsvg into its own prefix; qtbase's plugin root
  // has no qsvg. A packaged .app already ships qsvg under the bundled path.
  // Appending the host prefix on the build machine loads a second Qt
  // (moveToThread warnings, blank first preview / pixmap ghosts).
  if (bundled.isEmpty() || !QDir(bundled).exists()) {
    add_qt_plugin_path(QString::fromUtf8(WDS_QT_SVG_PLUGIN_DIR), false);
  }
#endif
}

struct ProcessCpuSnap {
  int64_t user_us = 0;
  int64_t sys_us = 0;
  std::unordered_map<uint64_t, int64_t> thread_us;
  int thread_n = 0;
};

#if !defined(Q_OS_WIN)
int64_t timeval_to_us(const timeval& t) noexcept {
  return static_cast<int64_t>(t.tv_sec) * 1000000LL + static_cast<int64_t>(t.tv_usec);
}
#endif

#if defined(Q_OS_WIN)
// FILETIME is in 100 ns units.
int64_t filetime_to_us(const FILETIME& ft) noexcept {
  ULARGE_INTEGER v;
  v.LowPart = ft.dwLowDateTime;
  v.HighPart = ft.dwHighDateTime;
  return static_cast<int64_t>(v.QuadPart / 10ULL);
}
#endif

// Cumulative user / system CPU time of this process in microseconds; false if unavailable.
bool process_cpu_times_us(int64_t& user_us, int64_t& sys_us) noexcept {
#if defined(Q_OS_WIN)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    return false;
  }
  user_us = filetime_to_us(user);
  sys_us = filetime_to_us(kernel);
  return true;
#else
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return false;
  }
  user_us = timeval_to_us(ru.ru_utime);
  sys_us = timeval_to_us(ru.ru_stime);
  return true;
#endif
}

// Minor page faults since process start (0 where the OS has no such counter, e.g. Windows).
long process_minor_faults() noexcept {
#if defined(Q_OS_WIN)
  return 0;
#else
  rusage ru{};
  return getrusage(RUSAGE_SELF, &ru) == 0 ? ru.ru_minflt : 0;
#endif
}

#if defined(Q_OS_MACOS)
int64_t mach_time_to_us(time_value_t t) noexcept {
  return static_cast<int64_t>(t.seconds) * 1000000LL + static_cast<int64_t>(t.microseconds);
}

void sample_thread_cpu(std::unordered_map<uint64_t, int64_t>& out, int& thread_n) {
  out.clear();
  thread_n = 0;
  thread_act_array_t list = nullptr;
  mach_msg_type_number_t n = 0;
  if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) {
    return;
  }
  thread_n = static_cast<int>(n);
  for (mach_msg_type_number_t i = 0; i < n; ++i) {
    thread_identifier_info_data_t idinfo{};
    mach_msg_type_number_t id_cnt = THREAD_IDENTIFIER_INFO_COUNT;
    thread_basic_info_data_t info{};
    mach_msg_type_number_t info_cnt = THREAD_BASIC_INFO_COUNT;
    uint64_t tid = static_cast<uint64_t>(list[i]);
    if (thread_info(list[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&idinfo),
                    &id_cnt) == KERN_SUCCESS) {
      tid = idinfo.thread_id;
    }
    if (thread_info(list[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info),
                    &info_cnt) == KERN_SUCCESS) {
      out[tid] = mach_time_to_us(info.user_time) + mach_time_to_us(info.system_time);
    }
    mach_port_deallocate(mach_task_self(), list[i]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list),
                sizeof(*list) * static_cast<vm_size_t>(n));
}
#endif

void sample_process_cpu(ProcessCpuSnap& snap) {
  (void)process_cpu_times_us(snap.user_us, snap.sys_us);
#if defined(Q_OS_MACOS)
  sample_thread_cpu(snap.thread_us, snap.thread_n);
#else
  snap.thread_us.clear();
  snap.thread_n = 0;
#endif
}

class FrameDiagLogger {
 public:
  FrameDiagLogger() {
    if (!wds::ui::frame_diag_enabled_from_env()) return;
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
        QStringLiteral("/WDS/logs");
    if (!QDir().mkpath(dir)) return;
    file_.setFileName(dir + QStringLiteral("/frame-diag.log"));
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    active_ = true;
    wds::common::perf::set_enabled(wds::ui::frame_diag_perf_enabled_from_env());
    started_ = std::chrono::steady_clock::now();
    write_startup_header();
    arm_cold("open");
  }

  ~FrameDiagLogger() { close(); }

  FrameDiagLogger(const FrameDiagLogger&) = delete;
  FrameDiagLogger& operator=(const FrameDiagLogger&) = delete;

  bool active() const noexcept { return active_; }

  int64_t begin_callback(std::chrono::steady_clock::time_point now) noexcept {
    int64_t wall_us = -1;
    if (have_prev_cb_) {
      wall_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_cb_).count();
    }
    last_cb_ = now;
    have_prev_cb_ = true;
    return wall_us;
  }

  void enable_path_diagnostics(wds::ui::UiManager& editor) {
    if (path_diag_on_ || !editor.chart_preview().ready()) return;
    editor.chart_preview().preview().vulkan().set_path_diagnostics_enabled(true);
    path_diag_on_ = true;
  }

  void finish_frame(wds::ui::UiManager& editor, const wds::ui::ChartEditWidget& widget,
                    int64_t wall_us, int64_t delta_us, int64_t tick_us, int64_t render_us) {
    if (!active_) return;
    auto& vk = editor.chart_preview().preview().vulkan();
    const int64_t fence_us = vk.last_fence_wait_us();
    const int64_t acquire_us = vk.last_acquire_wait_us();
    const int64_t submit_us = vk.last_gpu_submit_us();
    const int64_t present_us = vk.last_present_us();
    const int64_t paint_us = widget.last_paint_us();
    const uint64_t paint_count = widget.paint_count();
    const int64_t ui_us = editor.last_update_flush_us() + editor.last_update_bounds_us() +
                          editor.last_update_layout_us() + editor.last_update_sync_us() +
                          editor.last_update_process_us();
    const bool playing = editor.chart_preview().transport().playing();
    const auto path_counts = vk.path_diagnostics().counts;
    if (editor.chart_preview().ready()) {
      last_chart_sec_ =
          static_cast<double>(editor.chart_preview().engine().timeline_us()) / 1e6;
    }
    last_transport_playing_ = playing;
    if (editor.chart_preview().ready()) {
      last_split_diag_ = editor.chart_preview().preview().last_split_frame_diag();
      if (last_split_diag_.collect_us > 0) {
        acc(collect_sum_us_, collect_max_us_, last_split_diag_.collect_us);
        ++collect_samples_;
      }
      if (last_split_diag_.verts_us > 0) {
        acc(verts_sum_us_, verts_max_us_, last_split_diag_.verts_us);
        ++verts_samples_;
      }
      auto& preview = editor.chart_preview();
      auto& tp = preview.transport();
      const int64_t audio_us =
          tp.audio().has_music() ? tp.audio().position().count() : tp.committed_position().count();
      const int64_t clock_us = tp.committed_position().count();
      const int64_t displayed_us = preview.last_displayed_now_us();
      const int64_t sync_err_us = audio_us - clock_us;
      last_audio_pos_sec_ = static_cast<double>(audio_us) / 1e6;
      last_clock_sec_ = static_cast<double>(clock_us) / 1e6;
      last_displayed_sec_ = static_cast<double>(displayed_us) / 1e6;
      last_sync_err_us_ = sync_err_us;
      last_injected_stall_us_ = preview.last_injected_stall_us();
      last_play_start_stall_us_ = preview.last_play_start_stall_us();
      last_bass_after_present_sec_ = static_cast<double>(preview.last_bass_after_present_us()) / 1e6;
      last_heard_vs_picture_us_ = preview.last_heard_vs_picture_us();
      last_pictured_sec_ = static_cast<double>(preview.last_pictured_now_us()) / 1e6;
      last_has_music_ = tp.audio().has_music();
      if (playing) {
        const int64_t abs_e = sync_err_us >= 0 ? sync_err_us : -sync_err_us;
        acc(sync_abs_sum_us_, sync_abs_max_us_, abs_e);
        ++sync_samples_;
        const bool stall_hook = wds::ui::preview_stall_ms_from_env() > 0;
        if (last_injected_stall_us_ > 0 && !stall_logged_) {
          stall_logged_ = true;
          stall_follow_frames_ = 8;
          char sbuf[256];
          const int sn = std::snprintf(
              sbuf, sizeof(sbuf),
              "STALL inject_ms=%.1f play_start_ms=%.1f chart_sec=%.3f audio_pos=%.3f "
              "clock_sec=%.3f err_ms=%.2f\n",
              static_cast<double>(last_injected_stall_us_) / 1000.0,
              static_cast<double>(last_play_start_stall_us_) / 1000.0, last_displayed_sec_,
              last_audio_pos_sec_, last_clock_sec_, static_cast<double>(sync_err_us) / 1000.0);
          if (sn > 0) pending_.append(sbuf, static_cast<std::size_t>(sn));
        }
        if (stall_hook || stall_follow_frames_ > 0) {
          if (stall_follow_frames_ > 0) {
            --stall_follow_frames_;
          }
          char sbuf[256];
          const int sn = std::snprintf(
              sbuf, sizeof(sbuf),
              "SYNC audio_pos=%.3f chart_sec=%.3f clock_sec=%.3f err_ms=%.2f lead_ms=%.2f "
              "stall_ms=%.1f\n",
              last_audio_pos_sec_, last_displayed_sec_, last_clock_sec_,
              static_cast<double>(sync_err_us) / 1000.0,
              static_cast<double>(preview.last_visual_lead_us()) / 1000.0,
              static_cast<double>(last_injected_stall_us_) / 1000.0);
          if (sn > 0) pending_.append(sbuf, static_cast<std::size_t>(sn));
        }
        const int64_t heard_abs =
            last_heard_vs_picture_us_ >= 0 ? last_heard_vs_picture_us_ : -last_heard_vs_picture_us_;
        if (last_injected_stall_us_ > 0 && stall_logged_) {
          if (stall_follow_frames_ >= 7) {
            stall_heard_us_ = last_heard_vs_picture_us_;
          } else {
            acc(post_stall_heard_sum_us_, post_stall_heard_max_us_, heard_abs);
            ++post_stall_heard_samples_;
          }
        } else {
          acc(heard_sum_us_, heard_max_us_, heard_abs);
          ++heard_samples_;
        }
        // Independent of pictured: content we drew vs BASS at present
        // return + one-vsync scanout estimate (FIFO, no CAMetalLayer time).
        const int64_t scanout_us = preview.display_frame_lead_us();
        const int64_t align_present_us = displayed_us - preview.last_bass_after_present_us();
        const int64_t align_screen_us = align_present_us - scanout_us;
        if (align_screen_samples_.size() < 4000) {
          align_screen_samples_.push_back(align_screen_us);
          align_present_samples_.push_back(align_present_us);
        }
        char avbuf[256];
        const int avn = std::snprintf(
            avbuf, sizeof(avbuf),
            "AVSYNC content=%.6f bass=%.6f scanout_ms=%.2f align_present_ms=%.2f "
            "align_screen_ms=%.2f lead_ms=%.2f pulse=%.6f\n",
            static_cast<double>(displayed_us) / 1e6,
            static_cast<double>(preview.last_bass_after_present_us()) / 1e6,
            static_cast<double>(scanout_us) / 1000.0, static_cast<double>(align_present_us) / 1000.0,
            static_cast<double>(align_screen_us) / 1000.0,
            static_cast<double>(preview.last_visual_lead_us()) / 1000.0,
            static_cast<double>(preview.last_pulse_now_us()) / 1e6);
        if (avn > 0) pending_.append(avbuf, static_cast<std::size_t>(avn));
      }
      const bool play_edge = playing && !prev_playing_;
      const double chart_jump = last_chart_sec_ - prev_chart_sec_;
      const bool seek_edge = chart_jump > 0.25 || chart_jump < -0.04;
      if (play_edge) {
        arm_cold(seek_edge ? "seek_play" : "play");
      } else if (seek_edge) {
        arm_cold(playing ? "seek_play" : "seek");
      }
      if (cold_left_ > 0) {
        ++cold_n_;
        const auto& d = last_split_diag_;
        char cbuf[384];
        const int cn = std::snprintf(
            cbuf, sizeof(cbuf),
            "COLD n=%d/3 why=%s playing=%d chart_sec=%.3f collect_ms=%.3f verts_ms=%.3f "
            "lanes_ms=%.3f geom_miss=%d tex_ms=%.3f upload_ms=%.3f submit_ms=%.3f total_ms=%.3f "
            "render_ms=%.3f\n",
            cold_n_, cold_why_, playing ? 1 : 0, last_chart_sec_,
            static_cast<double>(d.collect_us) / 1000.0, static_cast<double>(d.verts_us) / 1000.0,
            static_cast<double>(d.lanes_us) / 1000.0, d.lanes_geom_miss,
            static_cast<double>(d.tex_us) / 1000.0,
            static_cast<double>(d.upload_us) / 1000.0, static_cast<double>(d.submit_us) / 1000.0,
            static_cast<double>(d.total_us) / 1000.0, static_cast<double>(render_us) / 1000.0);
        if (cn > 0) pending_.append(cbuf, static_cast<std::size_t>(cn));
        --cold_left_;
      }
      prev_chart_sec_ = last_chart_sec_;
      prev_playing_ = playing;
    }

    if (!window_open_) {
      window_start_ = std::chrono::steady_clock::now();
      paint_count0_ = paint_count;
      last_seen_paint_count_ = paint_count;
      window_chart_sec0_ = last_chart_sec_;
      sample_process_cpu(cpu0_);
      window_open_ = true;
    }

    ++frames_;
    if (playing) ++playing_frames_;
    if (wall_us >= 0) {
      acc(wall_sum_us_, wall_max_us_, wall_us);
      ++wall_samples_;
    }
    const auto cpu = vk.last_draw_cpu();
    // MoltenVK synchronous submits: vsync/drawable back-pressure waits inside
    // vkQueueSubmit on idle draws. Do not count that pad as render/submit CPU.
    const int64_t submit_wait = classify_submit_wait_us(submit_us, cpu.record_us, cpu.total_verts);
    const int64_t submit_cpu = submit_us > submit_wait ? submit_us - submit_wait : 0;
    const int64_t render_cpu = render_us > submit_wait ? render_us - submit_wait : 0;
    acc(delta_sum_us_, delta_max_us_, delta_us);
    acc(tick_sum_us_, tick_max_us_, tick_us);
    acc(render_sum_us_, render_max_us_, render_cpu);
    acc(fence_sum_us_, fence_max_us_, fence_us);
    acc(acquire_sum_us_, acquire_max_us_, acquire_us);
    acc(submit_sum_us_, submit_max_us_, submit_cpu);
    acc(submit_wait_sum_us_, submit_wait_max_us_, submit_wait);
    acc(present_sum_us_, present_max_us_, present_us);
    acc(ui_sum_us_, ui_max_us_, ui_us);
    if (paint_count > last_seen_paint_count_) {
      last_seen_paint_count_ = paint_count;
      acc(paint_sum_us_, paint_max_us_, paint_us);
      ++paint_samples_;
    }

    if (wall_us > kMissedVsyncUs) {
      ++missed_vsync_;
      if (wall_us <= kHitchUs) {
        append_miss(wall_us, tick_us, render_cpu, submit_cpu, submit_wait, paint_us, ui_us,
                    cpu.total_verts, cpu.used_bytes, cpu.cap_bytes, cpu.ring_bytes, cpu.staging,
                    playing);
      }
    }
    if (wall_us > kHitchUs) {
      ++hitch_count_;
      append_hitch(wall_us, delta_us, tick_us, render_cpu, fence_us, acquire_us, submit_cpu,
                   present_us, paint_us, ui_us, playing);
    }

    last_path_counts_ = path_counts;
    last_paint_count_ = paint_count;

    {
      int64_t gpu_total_ns = 0;
      for (const auto& seg : vk.last_gpu_segments()) {
        if (seg.first != nullptr && std::strcmp(seg.first, "gpu.total") == 0) {
          gpu_total_ns = seg.second;
          break;
        }
      }
      char dbuf[800];
      const int dn = std::snprintf(
          dbuf, sizeof(dbuf),
          "DRAWCHK record_ms=%.3f submit_cpu_ms=%.3f submit_wait_ms=%.3f wall_ms=%.3f "
          "fence_ms=%.3f acquire_ms=%.3f present_ms=%.3f gpu_ms=%.3f used_kb=%.1f cap_kb=%.1f "
          "ring_kb=%.1f ib_used_kb=%.1f ib_cap_kb=%.1f ib_ring_kb=%.1f verts=%zu early=%zu "
          "carve=%zu staging=%d bloom=%d carve_pass=%d "
          "draws=%lld pipes=%lld descs=%lld push=%lld passes=%lld vb=%lld "
          "collect_ms=%.3f verts_ms=%.3f lanes_ms=%.3f paint_ms=%.3f ui_ms=%.3f\n",
          static_cast<double>(cpu.record_us) / 1000.0, static_cast<double>(submit_cpu) / 1000.0,
          static_cast<double>(submit_wait) / 1000.0, static_cast<double>(wall_us) / 1000.0,
          static_cast<double>(fence_us) / 1000.0, static_cast<double>(acquire_us) / 1000.0,
          static_cast<double>(present_us) / 1000.0, static_cast<double>(gpu_total_ns) / 1e6,
          static_cast<double>(cpu.used_bytes) / 1024.0, static_cast<double>(cpu.cap_bytes) / 1024.0,
          static_cast<double>(cpu.ring_bytes) / 1024.0,
          static_cast<double>(cpu.ib_used_bytes) / 1024.0,
          static_cast<double>(cpu.ib_cap_bytes) / 1024.0,
          static_cast<double>(cpu.ib_ring_bytes) / 1024.0, cpu.total_verts, cpu.early_verts,
          cpu.carve_verts, cpu.staging, cpu.bloom, cpu.carve_pass,
          static_cast<long long>(cpu.draws), static_cast<long long>(cpu.pipe_binds),
          static_cast<long long>(cpu.desc_binds), static_cast<long long>(cpu.push_consts),
          static_cast<long long>(cpu.render_passes), static_cast<long long>(cpu.vb_binds),
          static_cast<double>(last_split_diag_.collect_us) / 1000.0,
          static_cast<double>(last_split_diag_.verts_us) / 1000.0,
          static_cast<double>(last_split_diag_.lanes_us) / 1000.0, static_cast<double>(paint_us) / 1000.0,
          static_cast<double>(ui_us) / 1000.0);
      if (dn > 0) pending_.append(dbuf, static_cast<std::size_t>(dn));
    }

    if (frame_left_ > 0 && cold_why_[0] != '\0' && std::strcmp(cold_why_, "open") != 0) {
      ++frame_n_;
      const auto& d = last_split_diag_;
      auto& preview_panel = editor.chart_preview();
      const int64_t seek_us = preview_panel.last_seek_handle_us();
      const int64_t tick_preload = preview_panel.last_tick_preload_us();
      const int64_t tick_transport = preview_panel.last_tick_transport_us();
      const int64_t tick_geom = preview_panel.last_tick_geom_us();
      const int64_t tick_sfx = preview_panel.last_tick_sfx_us();
      const int64_t tick_explained = tick_preload + tick_transport + tick_geom + tick_sfx;
      const int64_t leftover_tick = tick_us > tick_explained ? tick_us - tick_explained : 0;
      const int64_t render_explained = d.collect_us + d.verts_us + d.lanes_us + d.tex_us +
                                       d.upload_us + cpu.record_us + submit_cpu;
      const int64_t leftover_render = render_cpu > render_explained ? render_cpu - render_explained : 0;
      const int64_t leftover_work = leftover_tick + leftover_render;
      const int64_t wall_parts = tick_us + render_cpu + submit_wait + fence_us + acquire_us + present_us;
      const int64_t display_gap = wall_us > wall_parts ? wall_us - wall_parts : 0;
      const long minflt = process_minor_faults();
      const long dminflt = last_minflt_ > 0 ? minflt - last_minflt_ : minflt;
      last_minflt_ = minflt;
      const long seek_minflt = preview_panel.last_seek_minflt();
      char fbuf[960];
      const int fn = std::snprintf(
          fbuf, sizeof(fbuf),
          "FRAME i=%d/%d why=%s playing=%d chart_sec=%.3f wall=%.3f tick=%.3f render=%.3f "
          "collect=%.3f verts=%.3f lanes=%.3f paint=%.3f ui=%.3f record=%.3f "
          "submit_cpu=%.3f submit_wait=%.3f fence=%.3f acquire=%.3f present=%.3f "
          "tex=%.3f upload=%.3f seek=%.3f tick_preload=%.3f tick_transport=%.3f "
          "tick_geom=%.3f tick_sfx=%.3f leftover_tick=%.3f leftover_render=%.3f "
          "leftover_work=%.3f display_gap=%.3f leftover_wall=%.3f particles=%d epoch=%u "
          "dminflt=%ld seek_minflt=%ld\n",
          frame_n_, frame_n_ + frame_left_ - 1, cold_why_[0] != '\0' ? cold_why_ : "-",
          playing ? 1 : 0, last_chart_sec_, ms(wall_us), ms(tick_us), ms(render_cpu),
          static_cast<double>(d.collect_us) / 1000.0, static_cast<double>(d.verts_us) / 1000.0,
          static_cast<double>(d.lanes_us) / 1000.0, ms(paint_us), ms(ui_us),
          static_cast<double>(cpu.record_us) / 1000.0, ms(submit_cpu), ms(submit_wait),
          ms(fence_us), ms(acquire_us), ms(present_us), static_cast<double>(d.tex_us) / 1000.0,
          static_cast<double>(d.upload_us) / 1000.0, ms(seek_us), ms(tick_preload),
          ms(tick_transport), ms(tick_geom), ms(tick_sfx), ms(leftover_tick), ms(leftover_render),
          ms(leftover_work), ms(display_gap), ms(display_gap), d.particles, d.cache_epoch,
          dminflt, seek_minflt);
      if (fn > 0) pending_.append(fbuf, static_cast<std::size_t>(fn));
      --frame_left_;
    }

    wds::common::perf::end_frame();

    if (std::chrono::steady_clock::now() - window_start_ >= std::chrono::seconds(1)) {
      emit_summary();
      flush_file();
      reset_window();
    }
  }

  void flush() {
    if (!active_) return;
    if (window_open_ && frames_ > 0) {
      emit_summary();
      reset_window();
    }
    flush_file();
  }

  void close() {
    flush();
    if (file_.isOpen()) file_.close();
    active_ = false;
  }

 private:
  static constexpr int64_t kHitchUs = 25000;
  static constexpr int64_t kMissedVsyncUs = 16700;

  // MoltenVK sync submits park vsync/drawable back-pressure in vkQueueSubmit.
  // Only idle draws (tiny record, no particle verts) with a vsync-sized submit
  // are wait. Heavy frames keep the whole submit as submit_cpu.
  static int64_t classify_submit_wait_us(int64_t submit_us, int64_t record_us,
                                        size_t verts) noexcept {
    if (submit_us <= 0) {
      return 0;
    }
    if (record_us >= 500 || verts >= 120000) {
      return 0;
    }
    const int64_t encode_floor = record_us > 250 ? record_us : 250;
    if (submit_us <= encode_floor + 500) {
      return 0;
    }
    return submit_us - encode_floor;
  }

  static void acc(int64_t& sum, int64_t& mx, int64_t v) noexcept {
    sum += v;
    if (v > mx) mx = v;
  }

  static double ms(int64_t us) noexcept { return static_cast<double>(us) / 1000.0; }

  static double avg_ms(int64_t sum_us, int n) noexcept {
    return n > 0 ? static_cast<double>(sum_us) / static_cast<double>(n) / 1000.0 : 0.0;
  }

  static uint64_t count_delta(uint64_t now, uint64_t then) noexcept {
    return now >= then ? now - then : 0;
  }

  void write_startup_header() {
    const char* mvk = std::getenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS");
    double refresh_hz = 0;
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
      refresh_hz = screen->refreshRate();
    }
    const QByteArray started = QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8();
    char buf[384];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "# frame-diag started=%s qt=%s refresh_hz=%.1f mvk_sync_queue_submits=%s "
        "perf_scopes=%d\n",
        started.constData(), qVersion(), refresh_hz, mvk ? mvk : "unset",
        wds::ui::frame_diag_perf_enabled_from_env() ? 1 : 0);
    if (n > 0) file_.write(buf, n);
    file_.flush();
  }

  void append_miss(int64_t wall_us, int64_t tick_us, int64_t render_us, int64_t submit_cpu,
                   int64_t submit_wait, int64_t paint_us, int64_t ui_us, size_t verts,
                   size_t used_bytes, size_t cap_bytes, size_t ring_bytes, int staging,
                   bool playing) {
    char buf[512];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "MISS wall=%.1f collect=%.2f verts_us=%.2f lanes=%.2f submit_cpu=%.2f "
        "submit_wait=%.2f paint=%.2f ui=%.2f tick=%.2f render=%.2f verts=%zu "
        "used_kb=%.1f cap_kb=%.1f ring_kb=%.1f staging=%d playing=%d\n",
        ms(wall_us), static_cast<double>(last_split_diag_.collect_us) / 1000.0,
        static_cast<double>(last_split_diag_.verts_us) / 1000.0,
        static_cast<double>(last_split_diag_.lanes_us) / 1000.0, ms(submit_cpu), ms(submit_wait),
        ms(paint_us), ms(ui_us), ms(tick_us), ms(render_us), verts,
        static_cast<double>(used_bytes) / 1024.0, static_cast<double>(cap_bytes) / 1024.0,
        static_cast<double>(ring_bytes) / 1024.0, staging, playing ? 1 : 0);
    if (n > 0) pending_.append(buf, static_cast<std::size_t>(n));
  }

  void append_hitch(int64_t wall_us, int64_t delta_us, int64_t tick_us, int64_t render_us,
                    int64_t fence_us, int64_t acquire_us, int64_t submit_us, int64_t present_us,
                    int64_t paint_us, int64_t ui_us, bool playing) {
    char buf[384];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "HITCH wall=%.1f delta=%.1f tick=%.1f render=%.1f fence=%.1f acquire=%.1f "
        "submit=%.1f present=%.1f paint=%.1f ui=%.1f playing=%d\n",
        ms(wall_us), ms(delta_us), ms(tick_us), ms(render_us), ms(fence_us), ms(acquire_us),
        ms(submit_us), ms(present_us), ms(paint_us), ms(ui_us), playing ? 1 : 0);
    if (n > 0) pending_.append(buf, static_cast<std::size_t>(n));
  }

  void emit_summary() {
    const auto now = std::chrono::steady_clock::now();
    const double t =
        std::chrono::duration<double>(now - started_).count();
    const double window_s =
        std::chrono::duration<double>(now - window_start_).count();
    const double fps = window_s > 0.0 ? static_cast<double>(frames_) / window_s : 0.0;
    const double playing =
        frames_ > 0 ? static_cast<double>(playing_frames_) / static_cast<double>(frames_) : 0.0;
    const uint64_t paints = count_delta(last_paint_count_, paint_count0_);
    const auto& c = last_path_counts_;
    char buf[1100];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "t=%.1f chart_sec=%.3f chart_sec_start=%.3f transport_playing=%d frames=%d fps=%.1f "
        "playing=%.2f wall_avg=%.1f wall_max=%.1f hitch=%d missed_vsync=%d "
        "window_s=%.2f partial=%d "
        "delta_avg=%.1f delta_max=%.1f tick_avg=%.1f tick_max=%.1f render_avg=%.1f "
        "render_max=%.1f fence_avg=%.1f fence_max=%.1f acquire_avg=%.1f acquire_max=%.1f "
        "submit_avg=%.1f submit_max=%.1f submit_wait_avg=%.1f submit_wait_max=%.1f "
        "present_avg=%.1f present_max=%.1f paint_avg=%.1f "
        "paint_max=%.1f paints=%llu ui_avg=%.1f ui_max=%.1f swapchain_recreate=%llu "
        "create_texture_rgba=%llu upload_fence_wait=%llu upload_submit=%llu "
        "upload_fence_reap=%llu descriptor_allocate=%llu apply_msaa=%llu "
        "apply_msaa_idle_wait=%llu\n",
        t, last_chart_sec_, window_chart_sec0_, last_transport_playing_ ? 1 : 0, frames_, fps, playing,
        avg_ms(wall_sum_us_, wall_samples_), ms(wall_max_us_), hitch_count_, missed_vsync_,
        window_s, window_s < 0.95 ? 1 : 0,
        avg_ms(delta_sum_us_, frames_), ms(delta_max_us_), avg_ms(tick_sum_us_, frames_),
        ms(tick_max_us_), avg_ms(render_sum_us_, frames_), ms(render_max_us_),
        avg_ms(fence_sum_us_, frames_), ms(fence_max_us_), avg_ms(acquire_sum_us_, frames_),
        ms(acquire_max_us_), avg_ms(submit_sum_us_, frames_), ms(submit_max_us_),
        avg_ms(submit_wait_sum_us_, frames_), ms(submit_wait_max_us_),
        avg_ms(present_sum_us_, frames_), ms(present_max_us_), avg_ms(paint_sum_us_, paint_samples_),
        ms(paint_max_us_), static_cast<unsigned long long>(paints), avg_ms(ui_sum_us_, frames_),
        ms(ui_max_us_),
        static_cast<unsigned long long>(count_delta(c.swapchain_recreate, path_counts0_.swapchain_recreate)),
        static_cast<unsigned long long>(count_delta(c.create_texture_rgba, path_counts0_.create_texture_rgba)),
        static_cast<unsigned long long>(count_delta(c.upload_fence_wait, path_counts0_.upload_fence_wait)),
        static_cast<unsigned long long>(count_delta(c.upload_submit, path_counts0_.upload_submit)),
        static_cast<unsigned long long>(count_delta(c.upload_fence_reap, path_counts0_.upload_fence_reap)),
        static_cast<unsigned long long>(count_delta(c.descriptor_allocate, path_counts0_.descriptor_allocate)),
        static_cast<unsigned long long>(count_delta(c.apply_msaa, path_counts0_.apply_msaa)),
        static_cast<unsigned long long>(count_delta(c.apply_msaa_idle_wait, path_counts0_.apply_msaa_idle_wait)));
    if (n > 0) pending_.append(buf, static_cast<std::size_t>(n));
    {
      ProcessCpuSnap now_cpu{};
      sample_process_cpu(now_cpu);
      const int64_t user_d = now_cpu.user_us >= cpu0_.user_us ? now_cpu.user_us - cpu0_.user_us : 0;
      const int64_t sys_d = now_cpu.sys_us >= cpu0_.sys_us ? now_cpu.sys_us - cpu0_.sys_us : 0;
      const double cpu_pct =
          window_s > 0.0 ? static_cast<double>(user_d + sys_d) / window_s / 10000.0 : 0.0;
      int thr_busy = 0;
      int64_t thr_cpu_us = 0;
      for (const auto& kv : now_cpu.thread_us) {
        const auto it = cpu0_.thread_us.find(kv.first);
        const int64_t prev = it == cpu0_.thread_us.end() ? 0 : it->second;
        const int64_t d = kv.second >= prev ? kv.second - prev : 0;
        thr_cpu_us += d;
        if (d >= 500) {
          ++thr_busy;
        }
      }
      char cbuf[384];
      const int cn = std::snprintf(
          cbuf, sizeof(cbuf),
          "cpu self_pct=%.1f user_ms=%.1f sys_ms=%.1f thr_n=%d thr_busy=%d thr_cpu_ms=%.1f "
          "pool_participants=%d pool_last=%d particles=%d\n",
          cpu_pct, ms(user_d), ms(sys_d), now_cpu.thread_n, thr_busy, ms(thr_cpu_us),
          last_split_diag_.pool_participants, last_split_diag_.pool_last,
          last_split_diag_.particles);
      if (cn > 0) pending_.append(cbuf, static_cast<std::size_t>(cn));
    }
    {
      char sbuf[1152];
      const int sn = std::snprintf(
          sbuf, sizeof(sbuf),
          "split_diag rev=%llu now_us=%lld q=%d splits=%d particles=%d epoch=%u "
          "pool_cap=%d pool_participants=%d pool_last=%d pool_p_cores=%d pool_e_cores=%d "
          "simd_path=%s "
          "entries=%u "
          "lookup=%llu/%llu contend=%llu store=%llu/%llu "
          "cleared=%d collect_memo=%d verts_memo=%d playing=%d "
          "collect_ms=%.3f collect_avg=%.3f collect_max=%.3f "
          "verts_ms=%.3f verts_avg=%.3f verts_max=%.3f "
          "sync audio_pos=%.3f chart_sec=%.3f clock_sec=%.3f "
          "err_avg_ms=%.2f err_max_ms=%.2f stall_ms=%.1f play_start_stall_ms=%.1f\n",
          static_cast<unsigned long long>(last_split_diag_.revision),
          static_cast<long long>(last_split_diag_.now_us), last_split_diag_.quality,
          last_split_diag_.splits, last_split_diag_.particles, last_split_diag_.cache_epoch,
          last_split_diag_.pool_cap, last_split_diag_.pool_participants,
          last_split_diag_.pool_last, last_split_diag_.pool_p_cores, last_split_diag_.pool_e_cores,
          last_split_diag_.simd_path != nullptr && last_split_diag_.simd_path[0] != '\0'
              ? last_split_diag_.simd_path
              : "-",
          last_split_diag_.cache_entries,
          static_cast<unsigned long long>(last_split_diag_.cache_lookup_hits),
          static_cast<unsigned long long>(last_split_diag_.cache_lookup_ops),
          static_cast<unsigned long long>(last_split_diag_.cache_lookup_contend),
          static_cast<unsigned long long>(last_split_diag_.cache_store_ops),
          static_cast<unsigned long long>(last_split_diag_.cache_store_contend),
          last_split_diag_.cleared, last_split_diag_.collect_memo,
          last_split_diag_.verts_memo, last_split_diag_.playing,
          static_cast<double>(last_split_diag_.collect_us) / 1000.0,
          avg_ms(collect_sum_us_, collect_samples_), ms(collect_max_us_),
          static_cast<double>(last_split_diag_.verts_us) / 1000.0,
          avg_ms(verts_sum_us_, verts_samples_), ms(verts_max_us_),
          last_audio_pos_sec_, last_displayed_sec_, last_clock_sec_,
          sync_samples_ > 0 ? static_cast<double>(sync_abs_sum_us_) /
                                  static_cast<double>(sync_samples_) / 1000.0
                            : 0.0,
          ms(sync_abs_max_us_), static_cast<double>(last_injected_stall_us_) / 1000.0,
          static_cast<double>(last_play_start_stall_us_) / 1000.0);
      if (sn > 0) pending_.append(sbuf, static_cast<std::size_t>(sn));
    }
    {
      char bbuf[384];
      const int bn = std::snprintf(
          bbuf, sizeof(bbuf),
          "BASSCHK has_music=%d bass_after_present=%.3f pictured=%.3f heard_err_ms=%.2f "
          "heard_avg_ms=%.2f heard_max_ms=%.2f stall_heard_ms=%.2f "
          "post_stall_avg_ms=%.2f post_stall_max_ms=%.2f\n",
          last_has_music_ ? 1 : 0, last_bass_after_present_sec_, last_pictured_sec_,
          static_cast<double>(last_heard_vs_picture_us_) / 1000.0,
          heard_samples_ > 0 ? static_cast<double>(heard_sum_us_) /
                                   static_cast<double>(heard_samples_) / 1000.0
                             : 0.0,
          ms(heard_max_us_), static_cast<double>(stall_heard_us_) / 1000.0,
          post_stall_heard_samples_ > 0
              ? static_cast<double>(post_stall_heard_sum_us_) /
                    static_cast<double>(post_stall_heard_samples_) / 1000.0
              : 0.0,
          ms(post_stall_heard_max_us_));
      if (bn > 0) pending_.append(bbuf, static_cast<std::size_t>(bn));
    }
    if (!align_screen_samples_.empty()) {
      auto pct = [](std::vector<int64_t> v, double q) -> double {
        if (v.empty()) {
          return 0.0;
        }
        const size_t i = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
        return static_cast<double>(v[i]) / 1000.0;
      };
      int64_t sum_s = 0;
      int64_t sum_p = 0;
      int64_t max_s = align_screen_samples_[0];
      int64_t min_s = align_screen_samples_[0];
      for (int64_t x : align_screen_samples_) {
        sum_s += x;
        if (x > max_s) max_s = x;
        if (x < min_s) min_s = x;
      }
      for (int64_t x : align_present_samples_) {
        sum_p += x;
      }
      const double n = static_cast<double>(align_screen_samples_.size());
      char abuf[384];
      const int an = std::snprintf(
          abuf, sizeof(abuf),
          "AVSUM n=%zu align_screen_avg_ms=%.2f p50=%.2f p95=%.2f min=%.2f max=%.2f "
          "align_present_avg_ms=%.2f\n",
          align_screen_samples_.size(), static_cast<double>(sum_s) / n / 1000.0,
          pct(align_screen_samples_, 0.50), pct(align_screen_samples_, 0.95),
          static_cast<double>(min_s) / 1000.0, static_cast<double>(max_s) / 1000.0,
          static_cast<double>(sum_p) / n / 1000.0);
      if (an > 0) pending_.append(abuf, static_cast<std::size_t>(an));
    }
    const std::string perf = wds::common::perf::window_report();
    if (!perf.empty()) {
      std::size_t line_start = 0;
      for (std::size_t i = 0; i <= perf.size(); ++i) {
        if (i == perf.size() || perf[i] == '\n') {
          if (i > line_start) {
            pending_.append("perf ", 5);
            pending_.append(perf.data() + static_cast<std::ptrdiff_t>(line_start),
                            static_cast<std::size_t>(i - line_start));
            pending_.push_back('\n');
          }
          line_start = i + 1;
        }
      }
    }
  }

  void flush_file() {
    if (!file_.isOpen() || pending_.empty()) return;
    file_.write(pending_.data(), static_cast<qint64>(pending_.size()));
    file_.flush();
    pending_.clear();
  }

  void reset_window() {
    path_counts0_ = last_path_counts_;
    frames_ = 0;
    playing_frames_ = 0;
    hitch_count_ = 0;
    missed_vsync_ = 0;
    paint_samples_ = 0;
    wall_samples_ = 0;
    wall_sum_us_ = wall_max_us_ = 0;
    delta_sum_us_ = delta_max_us_ = 0;
    tick_sum_us_ = tick_max_us_ = 0;
    render_sum_us_ = render_max_us_ = 0;
    fence_sum_us_ = fence_max_us_ = 0;
    acquire_sum_us_ = acquire_max_us_ = 0;
    submit_sum_us_ = submit_max_us_ = 0;
    submit_wait_sum_us_ = submit_wait_max_us_ = 0;
    present_sum_us_ = present_max_us_ = 0;
    paint_sum_us_ = paint_max_us_ = 0;
    ui_sum_us_ = ui_max_us_ = 0;
    collect_samples_ = 0;
    verts_samples_ = 0;
    collect_sum_us_ = collect_max_us_ = 0;
    verts_sum_us_ = verts_max_us_ = 0;
    sync_samples_ = 0;
    sync_abs_sum_us_ = sync_abs_max_us_ = 0;
    window_open_ = false;
    align_screen_samples_.clear();
    align_present_samples_.clear();
    sample_process_cpu(cpu0_);
  }

  bool active_ = false;
  bool path_diag_on_ = false;
  bool have_prev_cb_ = false;
  bool window_open_ = false;
  QFile file_;
  std::string pending_;
  std::chrono::steady_clock::time_point started_{};
  std::chrono::steady_clock::time_point last_cb_{};
  std::chrono::steady_clock::time_point window_start_{};
  int frames_ = 0;
  int playing_frames_ = 0;
  int hitch_count_ = 0;
  int missed_vsync_ = 0;
  int paint_samples_ = 0;
  int wall_samples_ = 0;
  int64_t wall_sum_us_ = 0;
  int64_t wall_max_us_ = 0;
  int64_t delta_sum_us_ = 0;
  int64_t delta_max_us_ = 0;
  int64_t tick_sum_us_ = 0;
  int64_t tick_max_us_ = 0;
  int64_t render_sum_us_ = 0;
  int64_t render_max_us_ = 0;
  int64_t fence_sum_us_ = 0;
  int64_t fence_max_us_ = 0;
  int64_t acquire_sum_us_ = 0;
  int64_t acquire_max_us_ = 0;
  int64_t submit_sum_us_ = 0;
  int64_t submit_max_us_ = 0;
  int64_t submit_wait_sum_us_ = 0;
  int64_t submit_wait_max_us_ = 0;
  int64_t present_sum_us_ = 0;
  int64_t present_max_us_ = 0;
  int64_t paint_sum_us_ = 0;
  int64_t paint_max_us_ = 0;
  int64_t ui_sum_us_ = 0;
  int64_t ui_max_us_ = 0;
  int collect_samples_ = 0;
  int verts_samples_ = 0;
  int64_t collect_sum_us_ = 0;
  int64_t collect_max_us_ = 0;
  int64_t verts_sum_us_ = 0;
  int64_t verts_max_us_ = 0;
  uint64_t paint_count0_ = 0;
  uint64_t last_seen_paint_count_ = 0;
  uint64_t last_paint_count_ = 0;
  wds::renderer::RendererPathCounts path_counts0_{};
  wds::renderer::RendererPathCounts last_path_counts_{};
  double last_chart_sec_ = 0.0;
  double window_chart_sec0_ = 0.0;
  bool last_transport_playing_ = false;
  wds::ui::PlaybackPreviewView::SplitFrameDiag last_split_diag_{};
  ProcessCpuSnap cpu0_{};
  double last_audio_pos_sec_ = 0.0;
  double last_clock_sec_ = 0.0;
  double last_displayed_sec_ = 0.0;
  int64_t last_sync_err_us_ = 0;
  int64_t last_injected_stall_us_ = 0;
  int64_t last_play_start_stall_us_ = 0;
  int sync_samples_ = 0;
  int64_t sync_abs_sum_us_ = 0;
  int64_t sync_abs_max_us_ = 0;
  bool stall_logged_ = false;
  int stall_follow_frames_ = 0;
  double last_bass_after_present_sec_ = 0.0;
  double last_pictured_sec_ = 0.0;
  bool last_has_music_ = false;
  int64_t last_heard_vs_picture_us_ = 0;
  int heard_samples_ = 0;
  int64_t heard_sum_us_ = 0;
  int64_t heard_max_us_ = 0;
  int64_t stall_heard_us_ = 0;
  int post_stall_heard_samples_ = 0;
  int64_t post_stall_heard_sum_us_ = 0;
  int64_t post_stall_heard_max_us_ = 0;
  int cold_left_ = 0;
  int cold_n_ = 0;
  int frame_left_ = 0;
  int frame_n_ = 0;
  long last_minflt_ = 0;
  const char* cold_why_ = "";
  double prev_chart_sec_ = 0.0;
  bool prev_playing_ = false;
  std::vector<int64_t> align_screen_samples_;
  std::vector<int64_t> align_present_samples_;

  void arm_cold(const char* why) noexcept {
    cold_left_ = 3;
    cold_n_ = 0;
    cold_why_ = why != nullptr ? why : "";
    const int extra = wds::ui::frame_diag_frames_from_env();
    if (extra > 0 && why != nullptr && std::strcmp(why, "open") != 0) {
      frame_left_ = extra;
      frame_n_ = 0;
    }
  }
};

QtMessageHandler g_prev_qt_handler = nullptr;

void wds_qt_message_handler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
  if (type == QtFatalMsg) {
    const QByteArray utf8 = msg.toUtf8();
    wds::common::report_fatal("Qt fatal", utf8.constData());
    std::abort();
  }
  if (type == QtCriticalMsg || type == QtWarningMsg) {
    const QByteArray utf8 = msg.toUtf8();
    const char* kind = type == QtCriticalMsg ? "critical" : "warning";
    WDS_LOG("qt %s %s\n", kind, utf8.constData());
    if (context.file != nullptr && context.file[0] != '\0') {
      WDS_LOG("qt at %s:%d\n", context.file, context.line);
    }
    return;
  }
#if !defined(NDEBUG)
  if (g_prev_qt_handler != nullptr) {
    g_prev_qt_handler(type, context, msg);
  }
#else
  (void)context;
#endif
}

void maybe_notify_previous_crash(QWidget* parent) {
  if (qEnvironmentVariableIsSet("WDS_CRASH_NO_DIALOG")) return;
  char path[1024] = {};
  if (!wds::common::previous_session_crashed(path, sizeof(path))) return;
  QMessageBox box(parent);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(QStringLiteral("上次运行异常退出"));
  box.setText(QStringLiteral("上一次运行 WDS Editor 时发生了崩溃。崩溃报告：%1")
                  .arg(QString::fromUtf8(path)));
  auto* open = box.addButton(QStringLiteral("打开日志文件夹"), QMessageBox::ActionRole);
  box.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == static_cast<QAbstractButton*>(open)) {
    wds::ui::journal_menu_action("crash_notice.open_logs");
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromUtf8(wds::common::crash_log_directory())));
  } else {
    wds::ui::journal_menu_action("crash_notice.close");
  }
}

void log_qt_environment(const QString& theme_id) {
  WDS_LOG("qt version=%s app=%s\n", qVersion(),
          qUtf8Printable(QCoreApplication::applicationVersion()));
  WDS_LOG("qt os=%s kernel=%s %s arch=%s\n",
          qUtf8Printable(QSysInfo::prettyProductName()),
          qUtf8Printable(QSysInfo::kernelType()), qUtf8Printable(QSysInfo::kernelVersion()),
          qUtf8Printable(QSysInfo::currentCpuArchitecture()));
  WDS_LOG("qt theme=%s plugin_path=%s\n", qUtf8Printable(theme_id),
          qgetenv("QT_PLUGIN_PATH").constData());
  const auto screens = QGuiApplication::screens();
  QScreen* primary = QGuiApplication::primaryScreen();
  WDS_LOG("qt screens=%d\n", static_cast<int>(screens.size()));
  for (int i = 0; i < screens.size(); ++i) {
    QScreen* screen = screens[i];
    if (screen == nullptr) {
      continue;
    }
    const QSize sz = screen->size();
    WDS_LOG("screen[%d] '%s' %dx%d dpr=%.2f hz=%.1f primary=%d\n", i,
            qUtf8Printable(screen->name()), sz.width(), sz.height(),
            screen->devicePixelRatio(), screen->refreshRate(), screen == primary ? 1 : 0);
  }
}

void log_qt_vulkan_instance(const QVulkanInstance& vk_instance) {
  const QVersionNumber api = vk_instance.apiVersion();
  const QVersionNumber supported = vk_instance.supportedApiVersion();
  WDS_LOG("QVulkanInstance api=%d.%d.%d supported=%d.%d.%d ok=1\n", api.majorVersion(),
          api.minorVersion(), api.microVersion(), supported.majorVersion(),
          supported.minorVersion(), supported.microVersion());
  const auto exts = vk_instance.extensions();
  WDS_LOG("QVulkanInstance extensions=%d\n", static_cast<int>(exts.size()));
  for (const QByteArray& ext : exts) {
    WDS_LOG("QVulkanInstance ext %s\n", ext.constData());
  }
  const auto layers = vk_instance.layers();
  WDS_LOG("QVulkanInstance layers=%d\n", static_cast<int>(layers.size()));
  for (const QByteArray& layer : layers) {
    WDS_LOG("QVulkanInstance layer %s\n", layer.constData());
  }
  wds::renderer::log_physical_devices(vk_instance.vkInstance());
}

struct CaptureShot {
  int effect_id = 0;
  double note_start_s = 0;
  double onfadein_s = 0;
  double capture_chart_s = 0;
  QString png;
  qint64 capture_ms = -1;
  bool ok = false;
};

struct CaptureJob {
  QString out_dir;
  QString times_path;
  QString chart_path;
  int pixel_w = 1920;
  int pixel_h = 1080;
  QJsonObject source_root;
  std::vector<CaptureShot> shots;
};

bool parse_preview_size(const QString& spec, int& w, int& h) {
  const int x = spec.indexOf(QLatin1Char('x'));
  if (x <= 0 || x + 1 >= spec.size()) return false;
  bool wok = false;
  bool hok = false;
  w = spec.left(x).toInt(&wok);
  h = spec.mid(x + 1).toInt(&hok);
  return wok && hok && w > 0 && h > 0;
}

bool load_capture_times(const QString& path, CaptureJob& job, QString& err) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    err = QStringLiteral("cannot open capture-times %1").arg(path);
    return false;
  }
  const QByteArray raw = file.readAll();
  file.close();

  QJsonParseError pe{};
  const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
  if (pe.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
    QJsonArray effects;
    if (doc.isObject()) {
      job.source_root = doc.object();
      effects = job.source_root.value(QStringLiteral("effects")).toArray();
    } else {
      effects = doc.array();
    }
    if (effects.isEmpty()) {
      err = QStringLiteral("capture-times JSON has no effects");
      return false;
    }
    job.shots.reserve(static_cast<size_t>(effects.size()));
    for (const QJsonValue& v : effects) {
      const QJsonObject o = v.toObject();
      CaptureShot shot;
      shot.effect_id = o.value(QStringLiteral("effect_id")).toInt();
      shot.note_start_s = o.value(QStringLiteral("note_start_s")).toDouble();
      shot.onfadein_s = o.value(QStringLiteral("onfadein_s")).toDouble();
      shot.capture_chart_s = o.value(QStringLiteral("capture_chart_s")).toDouble();
      shot.png = o.value(QStringLiteral("png")).toString();
      if (shot.effect_id <= 0 || shot.capture_chart_s < 0.0) {
        err = QStringLiteral("capture-times row missing effect_id / capture_chart_s");
        return false;
      }
      if (shot.png.isEmpty()) {
        shot.png = QStringLiteral("%1.png").arg(shot.effect_id);
      }
      job.shots.push_back(shot);
    }
    return true;
  }

  const QStringList lines = QString::fromUtf8(raw).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (const QString& line : lines) {
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;
    const QStringList parts =
        trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
      err = QStringLiteral("capture-times line needs: effect_id chart_s [png]");
      return false;
    }
    CaptureShot shot;
    shot.effect_id = parts[0].toInt();
    shot.capture_chart_s = parts[1].toDouble();
    shot.note_start_s = shot.capture_chart_s + 0.5;
    shot.onfadein_s = shot.note_start_s - 1.0;
    shot.png = parts.size() >= 3 ? parts[2] : QStringLiteral("%1.png").arg(shot.effect_id);
    if (shot.effect_id <= 0) {
      err = QStringLiteral("capture-times line has invalid effect_id");
      return false;
    }
    job.shots.push_back(shot);
  }
  if (job.shots.empty()) {
    err = QStringLiteral("capture-times is empty");
    return false;
  }
  return true;
}

bool write_png_rgba(const QString& path, const std::vector<std::uint8_t>& rgba, int w, int h,
                    int target_w, int target_h) {
  if (w <= 0 || h <= 0 ||
      rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4u) {
    return false;
  }
  const QImage img(rgba.data(), w, h, w * 4, QImage::Format_RGBA8888);
  QImage owned = img.copy();
  if (owned.width() != target_w || owned.height() != target_h) {
    owned = owned.scaled(target_w, target_h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  QDir().mkpath(QFileInfo(path).absolutePath());
  return owned.save(path, "PNG");
}

QJsonObject capture_manifest_settings(const CaptureJob& job, int fb_w, int fb_h,
                                      const QString& chart_used) {
  QJsonObject settings = job.source_root.value(QStringLiteral("settings")).toObject();
  if (settings.isEmpty()) {
    settings = job.source_root.value(QStringLiteral("game_settings")).toObject();
  }
  settings.insert(QStringLiteral("native_note_speed"), 5);
  settings.insert(QStringLiteral("native_lane_alpha"), 80);
  settings.insert(QStringLiteral("native_split_line_opacity"), 100);
  settings.insert(QStringLiteral("native_split_effect_quality"), 1);
  settings.insert(QStringLiteral("native_split_effect_quality_name"), QStringLiteral("Normal"));
  settings.insert(
      QStringLiteral("native_quality_mapping"),
      QStringLiteral(
          "ChartPreviewConfig.split_effect_quality 0=Rich 1=Normal 2=Light matches Unity "
          "SpritEffectSettingType; Normal+Full emits every split line (split_effect_gate.hpp). "
          "LaneAlpha 80 is hardcoded kOfficialBgLaneAlpha=0.8x0.8=0.64 in playback_preview."));
  settings.insert(QStringLiteral("native_preview_size"),
                  QStringLiteral("%1x%2").arg(job.pixel_w).arg(job.pixel_h));
  settings.insert(QStringLiteral("native_capture"), QStringLiteral("swapchain_readback"));
  settings.insert(QStringLiteral("native_chart"), chart_used);
  settings.insert(QStringLiteral("native_fb_w"), fb_w);
  settings.insert(QStringLiteral("native_fb_h"), fb_h);
  if (!settings.contains(QStringLiteral("SpritEffectSettingType"))) {
    settings.insert(QStringLiteral("SpritEffectSettingType"), 1);
  }
  if (!settings.contains(QStringLiteral("NoteSpeed"))) {
    settings.insert(QStringLiteral("NoteSpeed"), 5);
  }
  if (!settings.contains(QStringLiteral("LaneAlpha")) &&
      !settings.contains(QStringLiteral("LaneAlphaValue"))) {
    settings.insert(QStringLiteral("LaneAlpha"), 80);
  }
  if (!settings.contains(QStringLiteral("line_opacity")) &&
      !settings.contains(QStringLiteral("SplitEffectLineOpacity"))) {
    settings.insert(QStringLiteral("line_opacity"), 100);
  }
  if (!settings.contains(QStringLiteral("IsLowFrameRate"))) {
    settings.insert(QStringLiteral("IsLowFrameRate"), true);
  }
  return settings;
}

bool write_capture_manifest(const CaptureJob& job, const QString& path, int fb_w, int fb_h,
                            const QString& chart_used, qint64 elapsed_ms, int written) {
  QJsonObject root;
  if (!job.source_root.isEmpty()) {
    root = job.source_root;
  }
  root.insert(QStringLiteral("set"), QStringLiteral("shots-native-318-v1"));
  root.insert(QStringLiteral("count"), written);
  QJsonArray res;
  res.append(job.pixel_w);
  res.append(job.pixel_h);
  root.insert(QStringLiteral("resolution"), res);
  root.insert(QStringLiteral("settings"), capture_manifest_settings(job, fb_w, fb_h, chart_used));
  root.insert(QStringLiteral("native_elapsed_ms"), elapsed_ms);
  QJsonArray effects;
  for (const CaptureShot& shot : job.shots) {
    QJsonObject o;
    o.insert(QStringLiteral("effect_id"), shot.effect_id);
    o.insert(QStringLiteral("note_start_s"), shot.note_start_s);
    o.insert(QStringLiteral("onfadein_s"), shot.onfadein_s);
    o.insert(QStringLiteral("capture_chart_s"), shot.capture_chart_s);
    o.insert(QStringLiteral("png"), shot.png);
    o.insert(QStringLiteral("ok"), shot.ok);
    if (shot.capture_ms >= 0) {
      o.insert(QStringLiteral("native_capture_ms"), shot.capture_ms);
    }
    effects.append(o);
  }
  root.insert(QStringLiteral("effects"), effects);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return true;
}

bool preview_atlases_ready(const wds::ui::ChartPreviewPanel& preview) {
  const auto& skin = preview.preview().skin();
  return static_cast<bool>(skin.split_effect_uvs) && static_cast<bool>(skin.split_effect_trail);
}

}  // namespace

int main(int argc, char** argv) {
  wds::common::install_crash_handlers();
  wds::common::log_open_debug_session();
  g_prev_qt_handler = qInstallMessageHandler(&wds_qt_message_handler);
  prepare_bundled_qt_plugins(argv[0]);
  // Pin the bundled MoltenVK ICD before Qt or the loader enumerates Homebrew
  // drivers. Two MoltenVK copies make vkGetDeviceQueue jump to NULL.
  wds::ui::prepare_macos_vulkan_environment(argv[0]);
  wds::common::log_runtime_environment(argv[0]);
  wds::ui::WdsApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("WDS"));
  QCoreApplication::setApplicationName(QStringLiteral("WDS Editor"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("WDS Editor"));
  QCoreApplication::setApplicationVersion(QStringLiteral(WDS_APP_VERSION));
  wds::ui::install_no_wheel_value_inputs(app);
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/wds/app_icon.png")));
  // Fonts must register before the theme QSS (which names 'Noto Sans SC').
  const auto bundled_font = wds::ui::resolve_ui_font_path(argv[0]);
  const int font_id = QFontDatabase::addApplicationFont(QString::fromStdString(bundled_font));
  if (font_id >= 0) {
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (!families.isEmpty()) {
      QFont font(families.front());
      font.setPointSizeF(wds::ui::wds_ref_font_pt());
      font.setHintingPreference(QFont::PreferFullHinting);
      QApplication::setFont(font);
    }
  }
  wds::ui::load_fluent_font(QString::fromStdString(wds::ui::resolve_fluent_font_path(argv[0])));
  const QString theme_dir = QString::fromStdString(wds::ui::resolve_theme_dir(argv[0]));
  const QString theme_id =
      QSettings("WDS", "WDS Editor").value("appearance/theme").toString();
  wds::ui::apply_wds_theme(app, theme_dir, theme_id);
  log_qt_environment(theme_id);

  auto deps = wds::ui::check_startup_dependencies(argv[0]);
  if (!deps.ok()) {
    wds::common::mark_clean_exit();
    return wds::ui::fail_startup_dependencies(deps);
  }

  QVulkanInstance vk_instance;
  // Qt leaves apiVersion at 0, and the spec treats that as Vulkan 1.0. The
  // preview flips Y with a negative viewport height, which is legal only on
  // 1.1+ (or with VK_KHR_maintenance1). AMD discards those draws, so the
  // preview stays at the near-black clear color. NVIDIA and MoltenVK still
  // rasterize, which is why this shows up only on some Windows GPUs.
  vk_instance.setApiVersion(QVersionNumber(1, 1));
  const bool want_validation =
      wds::ui::frame_diag_env_enabled(std::getenv("WDS_VK_VALIDATION"));
  if (want_validation) {
    const auto supported = vk_instance.supportedLayers();
    std::fprintf(stderr, "WDS_VK_VALIDATION supportedLayers=%d\n",
                 static_cast<int>(supported.size()));
    for (const QVulkanLayer& layer : supported) {
      std::fprintf(stderr, "WDS_VK_VALIDATION supportedLayer %s\n", layer.name.constData());
    }
    vk_instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    std::fprintf(stderr, "WDS_VK_VALIDATION requesting VK_LAYER_KHRONOS_validation\n");
  }
  bool vk_ok = vk_instance.create();
  if (!vk_ok && want_validation) {
    WDS_LOG("QVulkanInstance create failed with validation: errorCode=%d; retrying bare\n",
            static_cast<int>(vk_instance.errorCode()));
    vk_instance.setLayers({});
    vk_ok = vk_instance.create();
  }
  if (!vk_ok) {
    const int vk_err = static_cast<int>(vk_instance.errorCode());
    WDS_LOG("QVulkanInstance create failed: errorCode=%d\n", vk_err);
    std::fprintf(stderr, "QVulkanInstance create failed: errorCode=%d\n", vk_err);
    std::fflush(stderr);
    wds::ui::add_vulkan_unavailable(deps);
    wds::common::mark_clean_exit();
    return wds::ui::fail_startup_dependencies(deps);
  }
  if (want_validation) {
    const auto enabled = vk_instance.layers();
    std::fprintf(stderr, "WDS_VK_VALIDATION enabledLayers=%d\n", static_cast<int>(enabled.size()));
    for (const QByteArray& layer : enabled) {
      std::fprintf(stderr, "WDS_VK_VALIDATION enabledLayer %s\n", layer.constData());
    }
  }
  log_qt_vulkan_instance(vk_instance);
  wds::ui::EditorMainWindow window;
  auto* preview_window = new wds::ui::RealtimeVulkanWindow(&vk_instance);
  window.set_viewport_windows(preview_window, nullptr);

  wds::ui::UiManager editor;
  editor.enable_qt_chrome();
  editor.set_config_path(wds::ui::resolve_editor_config_path(argv[0]));
  editor.load_ui_config();
  preview_window->set_idle_frame_rate(30);
  preview_window->set_idle_throttle_bypass([&editor] {
    return editor.chart_preview().ready() && editor.chart_preview().transport().playing();
  });
  window.set_skins_dir(wds::ui::resolve_skins_dir(argv[0]));
  window.set_icons_dir(wds::ui::resolve_icons_dir(argv[0]));
  window.set_theme_dir(theme_dir);
  window.bind_ui_manager(&editor);
  editor.set_request_close([&window] { window.close(); });
  auto* editor_widget = new wds::ui::ChartEditWidget(editor.edit_panel(), &window);
  editor_widget->set_skins_directory(QString::fromStdString(wds::ui::resolve_skins_dir(argv[0])));
  editor_widget->set_global_key_handler([&editor](const wds::interaction::KeyDownEvent& event) {
    editor.dispatch_shortcut(event);
  });
  window.set_editor_widget(editor_widget);
  editor.chart_preview().set_after_seek_prepare(
      [editor_widget](int64_t ms) { editor_widget->prepare_after_seek(ms); });
  wds::renderer::PreviewVisualConfig visual;
  visual.skins_directory = wds::ui::resolve_skins_dir(argv[0]);
  visual.effects_directory = wds::ui::resolve_effects_dir(argv[0]);
  visual.bgm_path.clear();
  visual.lane_count = 12;
  visual.msaa_samples = 2;
  const std::string ui_font = wds::ui::resolve_ui_font_path(argv[0]);

  // load_ui_config already pushed the persisted display prefs into the preview
  // panel's config; fold them into the boot visual so initialization keeps them.
  const auto overlay_loaded_display = [&editor](wds::renderer::PreviewVisualConfig v) {
    const auto& loaded = editor.chart_preview().preview().config();
    v.note_speed = loaded.note_speed;
    v.note_start_offset = loaded.note_start_offset;
    v.note_height_level = loaded.note_height_level;
    v.split_line_opacity = loaded.split_line_opacity;
    v.split_effect_quality = loaded.split_effect_quality;
    v.lane_count = loaded.lane_count;
    v.msaa_samples = loaded.msaa_samples;
    return v;
  };


  FrameDiagLogger frame_diag;
  QObject::connect(&app, &QCoreApplication::aboutToQuit, [&frame_diag] { frame_diag.close(); });

  auto last_editor_tick = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now() - std::chrono::seconds(1));
  auto ui_alive = std::make_shared<std::atomic<bool>>(true);

  QString open_path;
  // `--screenshot <path>[@<ms>]` may repeat; each entry schedules its own grab.
  QList<std::pair<QString, int>> screenshots;
  CaptureJob capture_job;
  bool capture_preview = false;
  bool autoplay = false;
  int quit_after_sec = -1;
  double seek_sec = -1.0;
  double pause_at_sec = -1.0;
  double jump_at_sec = -1.0;
  double jump_to_sec = -1.0;
  int inject_id = 0;
  int inject_count = 1;
  double inject_at_sec = 8.5;
  double inject_end_sec = 10.5;
  int inject_delay_ms = 800;
  int precommit_wait_ms = 2500;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == QLatin1String("--open") && i + 1 < args.size()) {
      open_path = args[++i];
    } else if (args[i] == QLatin1String("--chart") && i + 1 < args.size()) {
      capture_job.chart_path = args[++i];
    } else if (args[i] == QLatin1String("--capture-preview") && i + 1 < args.size()) {
      capture_preview = true;
      capture_job.out_dir = args[++i];
    } else if (args[i] == QLatin1String("--capture-times") && i + 1 < args.size()) {
      capture_job.times_path = args[++i];
    } else if (args[i] == QLatin1String("--preview-size") && i + 1 < args.size()) {
      int w = 0;
      int h = 0;
      if (!parse_preview_size(args[++i], w, h)) {
        std::fprintf(stderr, "invalid --preview-size (expected WxH)\n");
        return 2;
      }
      capture_job.pixel_w = w;
      capture_job.pixel_h = h;
    } else if (args[i] == QLatin1String("--autoplay")) {
      autoplay = true;
    } else if (args[i] == QLatin1String("--seek") && i + 1 < args.size()) {
      bool ok = false;
      const double sec = args[++i].toDouble(&ok);
      if (ok && sec >= 0.0) seek_sec = sec;
    } else if (args[i] == QLatin1String("--pause-at") && i + 1 < args.size()) {
      bool ok = false;
      const double sec = args[++i].toDouble(&ok);
      if (ok && sec >= 0.0) pause_at_sec = sec;
    } else if (args[i] == QLatin1String("--jump-at") && i + 1 < args.size()) {
      bool ok = false;
      const double sec = args[++i].toDouble(&ok);
      if (ok && sec >= 0.0) jump_at_sec = sec;
    } else if (args[i] == QLatin1String("--jump-to") && i + 1 < args.size()) {
      bool ok = false;
      const double sec = args[++i].toDouble(&ok);
      if (ok && sec >= 0.0) jump_to_sec = sec;
    } else if (args[i] == QLatin1String("--inject-split") && i + 1 < args.size()) {
      const QString spec = args[++i];
      const int x = spec.indexOf(QLatin1Char('x'));
      bool id_ok = false;
      bool n_ok = false;
      inject_id = (x > 0 ? spec.left(x) : spec).toInt(&id_ok);
      inject_count = x > 0 ? spec.mid(x + 1).toInt(&n_ok) : 1;
      if (!id_ok) inject_id = 0;
      if (!n_ok || inject_count < 1) inject_count = 1;
    } else if (args[i] == QLatin1String("--inject-at-sec") && i + 1 < args.size()) {
      bool ok = false;
      const double sec = args[++i].toDouble(&ok);
      if (ok && sec >= 0.0) inject_at_sec = sec;
    } else if (args[i] == QLatin1String("--inject-end-sec") && i + 1 < args.size()) {
      bool ok = false;
      const double sec = args[++i].toDouble(&ok);
      if (ok && sec >= 0.0) inject_end_sec = sec;
    } else if (args[i] == QLatin1String("--inject-delay-ms") && i + 1 < args.size()) {
      bool ok = false;
      const int ms = args[++i].toInt(&ok);
      if (ok && ms >= 0) inject_delay_ms = ms;
    } else if (args[i] == QLatin1String("--precommit-wait-ms") && i + 1 < args.size()) {
      bool ok = false;
      const int ms = args[++i].toInt(&ok);
      if (ok && ms >= 0) precommit_wait_ms = ms;
    } else if (args[i] == QLatin1String("--quit-after") && i + 1 < args.size()) {
      bool ok = false;
      const int sec = args[++i].toInt(&ok);
      if (ok && sec >= 0) quit_after_sec = sec;
    } else if (args[i] == QLatin1String("--screenshot") && i + 1 < args.size()) {
      const QString spec = args[++i];
      const int at = spec.lastIndexOf(QLatin1Char('@'));
      bool ms_ok = false;
      const int ms = at > 0 ? spec.mid(at + 1).toInt(&ms_ok) : 0;
      if (at > 0 && ms_ok && ms >= 0) {
        screenshots.append({spec.left(at), ms});
      } else {
        screenshots.append({spec, 6000});
      }
    }
  }

  if (capture_preview) {
    if (capture_job.out_dir.isEmpty() || capture_job.times_path.isEmpty()) {
      std::fprintf(stderr,
                   "usage: wds_editor --capture-preview <out_dir> --capture-times <file> "
                   "[--preview-size 1920x1080] [--open project.wdsproject] [--chart chart.wdschart]\n");
      return 2;
    }
    QString times_err;
    if (!load_capture_times(capture_job.times_path, capture_job, times_err)) {
      std::fprintf(stderr, "capture-times: %s\n", qUtf8Printable(times_err));
      return 2;
    }
    QDir().mkpath(capture_job.out_dir + QStringLiteral("/png"));
  }

  const auto clamped_tick_delta = [](std::chrono::steady_clock::time_point now,
                                     std::chrono::steady_clock::time_point last) -> float {
    const float raw = std::chrono::duration<float>(now - last).count();
    return std::clamp(raw, 0.0f, 0.08f);
  };

  std::function<void(float)> finish_editor_frame = [&](float delta) {
    editor.resize_editor_viewport(editor_widget->width(), editor_widget->height(),
                                  editor_widget->width(), editor_widget->height());
    editor.update(delta, {});
    editor_widget->set_split_effect_quality(
        editor.chart_preview().preview().config().split_effect_quality);
    const bool playing = editor.chart_preview().ready() &&
                         editor.chart_preview().transport().intends_playing();
    if (editor.chart_preview().ready()) {
      auto& transport = editor.chart_preview().transport();
      const double chart_sec =
          static_cast<double>(editor.chart_preview().engine().timeline_us()) / 1e6;
      if (pause_at_sec >= 0.0 && transport.intends_playing() && chart_sec >= pause_at_sec) {
        transport.request_pause();
      }
      if (jump_at_sec >= 0.0 && jump_to_sec >= 0.0 && transport.intends_playing() &&
          chart_sec + 1e-9 >= jump_at_sec) {
        jump_at_sec = -1.0;
        editor.chart_preview().request_seek_ms(static_cast<int64_t>(jump_to_sec * 1000.0));
      }
    }
    if (editor_widget->isVisible() && editor_widget->take_dirty_for_frame(playing)) {
      editor_widget->update();
    }
    *last_editor_tick = std::chrono::steady_clock::now();
  };

  preview_window->set_frame_callback([&editor, preview_window, visual, ui_font,
                                      overlay_loaded_display, &frame_diag, editor_widget,
                                      ui_alive, &finish_editor_frame, &window](
                                         float delta, int logical_w, int logical_h, int fb_w,
                                         int fb_h,
                                         const std::vector<wds::interaction::InputEvent>& events) mutable {
    const bool diag = frame_diag.active();
    int64_t wall_us = -1;
    if (diag) wall_us = frame_diag.begin_callback(std::chrono::steady_clock::now());
    if (!editor.chart_preview().ready()) {
      finish_editor_frame(delta);
      if (editor_widget->isVisible()) editor_widget->repaint();
      auto host = preview_window->host_surface();
      if (host.external_instance == VK_NULL_HANDLE || host.external_surface == VK_NULL_HANDLE) {
        WDS_LOG("preview init wait: instance=%p surface=%p\n",
                static_cast<void*>(host.external_instance),
                static_cast<void*>(host.external_surface));
        const char* dump_verts = std::getenv("WDS_DUMP_VERTS");
        if (dump_verts != nullptr && dump_verts[0] != '\0') {
          std::fprintf(stderr, "WDS_DUMP_VERTS: waiting for vk surface instance=%p surface=%p\n",
                       static_cast<void*>(host.external_instance),
                       static_cast<void*>(host.external_surface));
        }
        return;
      }
      auto contract_visual = overlay_loaded_display(visual);
      if (!editor.chart_preview().initialize_empty(host, contract_visual, ui_font)) {
        WDS_LOG("preview initialize_empty failed\n");
        const char* dump_verts = std::getenv("WDS_DUMP_VERTS");
        if (dump_verts != nullptr && dump_verts[0] != '\0') {
          static bool logged_init_fail = false;
          if (!logged_init_fail) {
            logged_init_fail = true;
            std::fprintf(stderr, "WDS_DUMP_VERTS: initialize_empty failed: %s\n",
                         editor.chart_preview().last_init_error().c_str());
          }
        }
        return;
      }
      WDS_LOG("preview initialize_empty ok\n");
      if (editor.session().chart_count() == 0) {
        (void)editor.session().new_project();
      }
      if (!editor.session().music_path().empty()) {
        const std::string music = editor.session().music_path();
        QTimer::singleShot(0, &window, [ui_alive, &editor, music] {
          if (!ui_alive->load()) return;
          (void)editor.chart_preview().load_music(music, false);
        });
      }
      editor.set_status("ui资源加载完毕", wds::ui::StatusLevel::Info);
    }
    if (diag) frame_diag.enable_path_diagnostics(editor);
    editor.resize_preview_viewport(logical_w, logical_h, fb_w, fb_h);
    std::chrono::steady_clock::time_point phase_t0{};
    if (diag) phase_t0 = std::chrono::steady_clock::now();
    const auto work_t0 = std::chrono::steady_clock::now();
    editor.chart_preview().tick(static_cast<int64_t>(delta * 1000000.0f));
    int64_t tick_us = 0;
    if (diag) {
      const auto tick_t1 = std::chrono::steady_clock::now();
      tick_us = std::chrono::duration_cast<std::chrono::microseconds>(tick_t1 - phase_t0).count();
      phase_t0 = tick_t1;
    }
    editor.chart_preview().render();
    preview_window->mark_presented();
    static bool logged_present = false;
    if (!logged_present) {
      logged_present = true;
      WDS_LOG("preview first frame callback fb=%dx%d logical=%dx%d\n", fb_w, fb_h, logical_w,
              logical_h);
    }
    int64_t render_us = 0;
    if (diag) {
      render_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - phase_t0)
                      .count();
    }
    editor.chart_preview().flush_retired_font_textures();
    {
      const double work_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - work_t0)
                                 .count();
      wds::chart_render::split_effect_note_prev_frame_ms(work_ms);
    }
    window.on_preview_frame();
    finish_editor_frame(delta);
    if (diag) {
      frame_diag.finish_frame(editor, *editor_widget, wall_us,
                             static_cast<int64_t>(delta * 1000000.0f), tick_us, render_us);
    }
    (void)events;
  });
  preview_window->set_surface_teardown(
      [&editor] { editor.chart_preview().abandon_host_surface(); });

  auto* edit_timer = new QTimer(&window);
  edit_timer->setTimerType(Qt::CoarseTimer);
  QObject::connect(edit_timer, &QTimer::timeout, &window,
                   [&editor, &window, editor_widget, last_editor_tick, preview_window, edit_timer,
                    &finish_editor_frame, clamped_tick_delta] {
                     const bool ready = editor.chart_preview().ready();
                     const bool intends =
                         ready && editor.chart_preview().transport().intends_playing();
                     const int interval_ms = intends ? 16 : 33;
                     if (edit_timer->interval() != interval_ms) {
                       edit_timer->setInterval(interval_ms);
                     }

                     const auto now = std::chrono::steady_clock::now();
                     const bool unexposed =
                         !preview_window->isExposed() || !preview_window->isVisible();
                     const char* dump_verts = std::getenv("WDS_DUMP_VERTS");
                     const bool cli_dump = dump_verts != nullptr && dump_verts[0] != '\0';
                     if (unexposed && cli_dump) {
                       if (now - *last_editor_tick < std::chrono::milliseconds(interval_ms - 2)) {
                         return;
                       }
                       preview_window->pump_cli_frame();
                       return;
                     }
                     if (unexposed && ready) {
                       if (now - *last_editor_tick < std::chrono::milliseconds(interval_ms - 2)) {
                         return;
                       }
                       const float delta = clamped_tick_delta(now, *last_editor_tick);
                       editor.chart_preview().tick(static_cast<int64_t>(delta * 1000000.0f));
                       window.on_preview_frame();
                       finish_editor_frame(delta);
                       return;
                     }

                     if (now - *last_editor_tick < std::chrono::milliseconds(50)) return;
                     finish_editor_frame(clamped_tick_delta(now, *last_editor_tick));
                   });
  edit_timer->start(33);

  if (!open_path.isEmpty()) {
    const QByteArray utf8 = open_path.toUtf8();
    const std::string path(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    if (!editor.session().open_wdsproject(path)) {
      WDS_LOG("failed to open project: %s\n", path.c_str());
    }
  }

  window.setWindowIcon(QApplication::windowIcon());
  const bool cli_automation =
      capture_preview || seek_sec >= 0.0 || quit_after_sec >= 0 ||
      (std::getenv("WDS_DUMP_VERTS") != nullptr && std::getenv("WDS_DUMP_VERTS")[0] != '\0');
  if (!args.contains("--smoke-test") && !cli_automation) {
    maybe_notify_previous_crash(nullptr);
    if (open_path.isEmpty() && !window.show_startup_splash()) {
      wds::common::mark_clean_exit();
      return 0;
    }
  }
  window.show();
  window.raise();
  window.activateWindow();
  if (cli_automation) {
    window.ensure_preview_viewport_for_cli();
    std::fprintf(stderr, "WDS_DUMP_VERTS: main window shown dump=%s seek=%.1f quit_after=%d\n",
                 std::getenv("WDS_DUMP_VERTS") ? std::getenv("WDS_DUMP_VERTS") : "",
                 seek_sec, quit_after_sec);
  }
  WDS_LOG("main window shown\n");
  QTimer::singleShot(0, &window, [preview_window] { preview_window->request_frame(); });
  if (cli_automation) {
    QTimer::singleShot(400, &window, [preview_window, &window] {
      window.ensure_preview_viewport_for_cli();
      std::fprintf(stderr, "WDS_DUMP_VERTS: 400ms pump exposed=%d visible=%d %dx%d\n",
                   preview_window->isExposed() ? 1 : 0, preview_window->isVisible() ? 1 : 0,
                   preview_window->width(), preview_window->height());
      preview_window->pump_cli_frame();
    });
  }
  if (!args.contains("--smoke-test") && !cli_automation) {
    QTimer::singleShot(0, &window, [&window] { window.maybe_auto_check_updates(); });
  }
  auto capture_screenshot = [&window, preview_window](const QString& screenshot_path) {
    auto luminance_sum = [](const QPixmap& pix) -> qint64 {
      if (pix.isNull()) return 0;
      const QImage img = pix.toImage().scaled(32, 18);
      qint64 sum = 0;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
          sum += qMax(qMax(qRed(img.pixel(x, y)), qGreen(img.pixel(x, y))), qBlue(img.pixel(x, y)));
      return sum;
    };
    QScreen* screen = preview_window != nullptr ? preview_window->screen() : nullptr;
    if (screen == nullptr && window.windowHandle() != nullptr) screen = window.windowHandle()->screen();
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    const QPixmap native_window = screen != nullptr ? screen->grabWindow(window.winId()) : QPixmap{};
    QPixmap shot;
    QPixmap candidates[3];
    int n_cand = 0;
    if (preview_window != nullptr && preview_window->handle() != nullptr && screen != nullptr)
      candidates[n_cand++] = screen->grabWindow(preview_window->winId());
    if (screen != nullptr) candidates[n_cand++] = native_window;
    candidates[n_cand++] = window.grab();
    qint64 best = -1;
    for (int i = 0; i < n_cand; ++i) {
      if (!candidates[i].isNull() && luminance_sum(candidates[i]) > best) {
        best = luminance_sum(candidates[i]);
        shot = candidates[i];
      }
    }
#if defined(Q_OS_LINUX)
    // QWidget::grab() omits the native Vulkan child. Its blank background may
    // be brighter than the real preview, so prefer the full X11 window grab.
    if (QGuiApplication::platformName() == QLatin1String("xcb") && !native_window.isNull())
      shot = native_window;
#endif
    if (shot.isNull() || !shot.save(screenshot_path, "PNG")) {
      std::fprintf(stderr, "failed to save screenshot: %s\n", screenshot_path.toUtf8().constData());
      return false;
    }
    std::fprintf(stderr, "screenshot: %s %dx%d\n", screenshot_path.toUtf8().constData(),
                 shot.width(), shot.height());
    return true;
  };

  for (const auto& item : screenshots) {
    const QString screenshot_path = item.first;
    const int screenshot_ms = item.second;
#if defined(Q_OS_LINUX)
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
      // X11 screen grabs include occluding windows. Raise before an explicitly
      // requested capture, leaving the window manager time to process it.
      QTimer::singleShot(std::max(0, screenshot_ms - 200), &window, [&window] {
        window.raise();
        window.activateWindow();
      });
    }
#endif
    QTimer::singleShot(screenshot_ms, &window, [&, screenshot_path] {
      if (preview_window != nullptr) preview_window->request_frame();
      window.repaint();
      QApplication::processEvents();
      (void)capture_screenshot(screenshot_path);
    });
  }

  if (capture_preview) {
    window.discard_unsaved_on_close();
    struct CaptureRun {
      CaptureJob job;
      wds::renderer::PreviewVisualConfig saved{};
      bool settings_applied = false;
      bool chart_loaded = false;
      bool sized = false;
      bool settled = false;
      int settle_left = 4;
      int wait_ticks = 0;
      int shot_i = 0;
      int written = 0;
      int fb_w = 0;
      int fb_h = 0;
      QElapsedTimer wall;
    };
    auto* run = new CaptureRun;
    run->job = std::move(capture_job);
    run->wall.start();
    auto* timer = new QTimer(&window);
    timer->setInterval(16);
    QObject::connect(
        timer, &QTimer::timeout, &window,
        [&app, &window, &editor, preview_window, run, timer, open_path]() {
          auto fail_quit = [&](const char* why) {
            std::fprintf(stderr, "capture-preview failed: %s\n", why);
            const QString manifest =
                run->job.out_dir + QStringLiteral("/manifest.json");
            write_capture_manifest(run->job, manifest, run->fb_w, run->fb_h,
                                   run->job.chart_path.isEmpty() ? open_path : run->job.chart_path,
                                   run->wall.elapsed(), run->written);
            timer->stop();
            window.discard_unsaved_on_close();
            window.close();
            app.exit(1);
          };

          if (!editor.chart_preview().ready()) {
            window.prepare_preview_capture_size(run->job.pixel_w, run->job.pixel_h);
            if (preview_window != nullptr) preview_window->pump_cli_frame();
            if (++run->wait_ticks > 90 * 60) {
              fail_quit("preview never became ready");
            }
            return;
          }

          auto& preview = editor.chart_preview();
          auto& vulkan = preview.preview().vulkan();
          if (!run->settings_applied) {
            run->saved = preview.preview().config();
            preview.transport().request_pause();
            preview.apply_display_settings(
                5.0, run->saved.note_start_offset, run->saved.note_height_level, 100,
                wds::chart_editor::kOfficialSpritEffectNormal);
            run->settings_applied = true;
          }
          if (!run->chart_loaded && !run->job.chart_path.isEmpty() && open_path.isEmpty()) {
            const QByteArray utf8 = run->job.chart_path.toUtf8();
            const std::string path(utf8.constData(), static_cast<std::size_t>(utf8.size()));
            if (!editor.session().add_chart_from_file(path)) {
              fail_quit("failed to load --chart");
              return;
            }
            run->chart_loaded = true;
          } else {
            run->chart_loaded = true;
          }

          if (!run->sized || run->fb_w < 160 || run->fb_h < 90) {
            window.prepare_preview_capture_size(run->job.pixel_w, run->job.pixel_h);
            run->sized = true;
            if (preview_window != nullptr) preview_window->pump_cli_frame();
            run->fb_w = vulkan.framebuffer_width();
            run->fb_h = vulkan.framebuffer_height();
            if (preview_window != nullptr && run->fb_w > 0 && run->fb_h > 0) {
              editor.resize_preview_viewport(preview_window->width(), preview_window->height(),
                                             run->fb_w, run->fb_h);
            }
            preview.render();
            if (!preview_atlases_ready(preview) || run->fb_w < 160 || run->fb_h < 90 ||
                !vulkan.ready()) {
              run->sized = false;
              if (++run->wait_ticks > 90 * 60) {
                fail_quit("preview assets / framebuffer never ready");
              }
              return;
            }
          }

          if (!run->settled && !run->job.shots.empty()) {
            // Discard warmup frames at the first pictured T so the first
            // written PNG is not the process-cold atlas/geom/cache fill.
            const CaptureShot& warm = run->job.shots.front();
            const int64_t seek_ms =
                static_cast<int64_t>(std::llround(warm.capture_chart_s * 1000.0));
            preview.transport().request_pause();
            preview.request_seek_ms(seek_ms);
            preview.tick(0);
            if (preview_window != nullptr) {
              const int fb_w = std::max(1, vulkan.framebuffer_width());
              const int fb_h = std::max(1, vulkan.framebuffer_height());
              editor.resize_preview_viewport(preview_window->width(), preview_window->height(), fb_w,
                                             fb_h);
            }
            preview.tick(0);
            preview.render();
            if (--run->settle_left > 0) {
              return;
            }
            run->settled = true;
          }

          if (run->shot_i >= static_cast<int>(run->job.shots.size())) {
            preview.apply_display_settings(
                run->saved.note_speed, run->saved.note_start_offset, run->saved.note_height_level,
                static_cast<int>(std::lround(static_cast<double>(run->saved.split_line_opacity) *
                                             100.0)),
                run->saved.split_effect_quality);
            const QString manifest =
                run->job.out_dir + QStringLiteral("/manifest.json");
            const QString chart_used =
                run->job.chart_path.isEmpty() ? open_path : run->job.chart_path;
            write_capture_manifest(run->job, manifest, run->fb_w, run->fb_h, chart_used,
                                   run->wall.elapsed(), run->written);
            std::fprintf(stderr, "capture-preview done written=%d/%zu elapsed_ms=%lld fb=%dx%d\n",
                         run->written, run->job.shots.size(),
                         static_cast<long long>(run->wall.elapsed()), run->fb_w, run->fb_h);
            timer->stop();
            window.discard_unsaved_on_close();
            window.close();
            app.exit(run->written == static_cast<int>(run->job.shots.size()) ? 0 : 1);
            return;
          }

          CaptureShot& shot = run->job.shots[static_cast<size_t>(run->shot_i)];
          QElapsedTimer one;
          one.start();
          const int64_t seek_ms =
              static_cast<int64_t>(std::llround(shot.capture_chart_s * 1000.0));
          preview.transport().request_pause();
          preview.request_seek_ms(seek_ms);
          preview.tick(0);
          if (preview_window != nullptr) {
            const int fb_w = std::max(1, vulkan.framebuffer_width());
            const int fb_h = std::max(1, vulkan.framebuffer_height());
            editor.resize_preview_viewport(preview_window->width(), preview_window->height(), fb_w,
                                           fb_h);
          }
          preview.tick(0);
          preview.render();
          if (!preview_atlases_ready(preview)) {
            if (++run->wait_ticks > 90 * 60) {
              fail_quit("split-effect atlas never ready");
            }
            return;
          }
          preview.render();
          std::vector<std::uint8_t> rgba;
          int rw = 0;
          int rh = 0;
          if (!vulkan.readback_rgba8(rgba, rw, rh)) {
            preview.render();
            if (!vulkan.readback_rgba8(rgba, rw, rh)) {
              std::fprintf(stderr, "capture-preview readback failed effect_id=%d chart_s=%.3f\n",
                           shot.effect_id, shot.capture_chart_s);
              shot.ok = false;
              shot.capture_ms = one.elapsed();
              ++run->shot_i;
              return;
            }
          }
          run->fb_w = rw;
          run->fb_h = rh;
          const QString png_path = run->job.out_dir + QStringLiteral("/png/") + shot.png;
          if (!write_png_rgba(png_path, rgba, rw, rh, run->job.pixel_w, run->job.pixel_h)) {
            std::fprintf(stderr, "capture-preview png write failed %s\n",
                         qUtf8Printable(png_path));
            shot.ok = false;
            shot.capture_ms = one.elapsed();
            ++run->shot_i;
            return;
          }
          shot.ok = true;
          shot.capture_ms = one.elapsed();
          ++run->written;
          ++run->shot_i;
          if (run->written == 1 || run->written % 20 == 0 ||
              run->shot_i == static_cast<int>(run->job.shots.size())) {
            std::fprintf(stderr, "capture-preview %d/%zu id=%d chart_s=%.3f %dx%d %lldms\n",
                         run->written, run->job.shots.size(), shot.effect_id,
                         shot.capture_chart_s, rw, rh,
                         static_cast<long long>(shot.capture_ms));
          }
        });
    timer->start();
  }

  const auto apply_cli_transport = [&editor, preview_window](double seek, bool play) {
    if (!editor.chart_preview().ready()) return false;
    auto& transport = editor.chart_preview().transport();
    if (seek >= 0.0) {
      const int64_t ms = static_cast<int64_t>(seek * 1000.0);
      editor.chart_preview().request_seek_ms(ms);
    }
    if (play && !transport.playing()) {
      transport.request_play();
    }
    preview_window->request_frame();
    return true;
  };
  const auto inject_cli_splits = [&editor, inject_id, inject_count, inject_at_sec,
                                  inject_end_sec]() -> bool {
    if (!editor.chart_preview().ready() || inject_id <= 0 || inject_count <= 0) {
      return false;
    }
    auto& engine = editor.chart_preview().engine();
    const auto& timing = engine.document().timing();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < inject_count; ++i) {
      wds::chart_editor::NotationNote note;
      note.gimmick_type = wds::chart_editor::GimmickType::FullSplit6;
      note.scratch_length = inject_id;
      note.lane = 0;
      note.width = 1;
      note.start_tick = wds::chart_editor::milliseconds_to_tick(
          static_cast<int64_t>(std::llround(inject_at_sec * 1000.0)), timing);
      note.end_tick = wds::chart_editor::milliseconds_to_tick(
          static_cast<int64_t>(std::llround(inject_end_sec * 1000.0)), timing);
      if (engine.add_note(note) < 0) {
        std::fprintf(stderr, "INJECT_SPLIT failed id=%d i=%d\n", inject_id, i);
        return false;
      }
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::fprintf(stderr, "INJECT_SPLIT id=%d count=%d at=%.3f end=%.3f add_us=%lld\n", inject_id,
                 inject_count, inject_at_sec, inject_end_sec, static_cast<long long>(us));
    return true;
  };
  if (seek_sec >= 0.0 || autoplay || inject_id > 0) {
    auto* transport_timer = new QTimer(&window);
    transport_timer->setInterval(250);
    struct CliSeq {
      int attempts = 0;
      bool injected = false;
    };
    auto seq = std::make_shared<CliSeq>();
    QObject::connect(
        transport_timer, &QTimer::timeout, &window,
        [transport_timer, apply_cli_transport, inject_cli_splits, seek_sec, autoplay, inject_id,
         inject_delay_ms, precommit_wait_ms, seq, &editor]() mutable {
          if (!editor.chart_preview().ready()) {
            if (++seq->attempts >= 40) transport_timer->stop();
            return;
          }
          if (inject_id > 0 && !seq->injected) {
            transport_timer->stop();
            QTimer::singleShot(inject_delay_ms, transport_timer, [transport_timer, inject_cli_splits,
                                                                  apply_cli_transport, seek_sec,
                                                                  autoplay, precommit_wait_ms, seq,
                                                                  &editor]() {
              inject_cli_splits();
              seq->injected = true;
              QTimer::singleShot(precommit_wait_ms, transport_timer,
                                 [transport_timer, apply_cli_transport, seek_sec, autoplay, &editor] {
                                   const auto st = editor.chart_preview().preview().last_precommit();
                                   std::fprintf(stderr,
                                                "PRECOMMIT_WAIT done pending=%d wall_us=%lld "
                                                "rss_kb=%ld->%ld pool=%d\n",
                                                st.pending, static_cast<long long>(st.wall_us),
                                                st.rss_kb_before, st.rss_kb_after,
                                                st.plan.particle_cap);
                                   apply_cli_transport(seek_sec, autoplay);
                                   transport_timer->stop();
                                 });
            });
            return;
          }
          if (apply_cli_transport(seek_sec, autoplay)) {
            transport_timer->stop();
          } else if (++seq->attempts >= 40) {
            transport_timer->stop();
          }
        });
    QTimer::singleShot(2500, &window, [transport_timer] { transport_timer->start(); });
  }
  if (quit_after_sec >= 0 || args.contains("--smoke-test")) {
    window.discard_unsaved_on_close();
  }
  if (quit_after_sec >= 0) {
    QTimer::singleShot(quit_after_sec * 1000, &app, [&window, &app] {
      window.discard_unsaved_on_close();
      window.close();
      app.quit();
    });
  }
  if (args.contains("--smoke-test") && quit_after_sec < 0) {
    QTimer::singleShot(1200, &app, [&window, &app] {
      window.discard_unsaved_on_close();
      window.close();
      app.quit();
    });
  }
  int rc = 1;
  try {
    rc = app.exec();
  } catch (const std::exception& e) {
    wds::common::report_fatal("uncaught exception (event loop)", e.what());
    std::abort();
  } catch (...) {
    wds::common::report_fatal("uncaught exception (event loop)", "unknown");
    std::abort();
  }
  ui_alive->store(false);
  preview_window->set_frame_callback({});
  preview_window->set_idle_throttle_bypass({});
  preview_window->set_surface_teardown({});
  edit_timer->stop();
  window.detach_ui_manager();
  editor_widget->set_global_key_handler({});
  frame_diag.close();
  wds::common::mark_clean_exit();
  return rc;
}

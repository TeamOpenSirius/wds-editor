#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/wds_theme.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/frame_diag.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/resource_paths.hpp"
#include "wds/ui/startup_deps.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/renderer/preview_visual_config.hpp"
#include "wds/common/crash_handler.hpp"
#include "wds/common/log.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QPushButton>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QScreen>
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
#include <exception>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#if defined(Q_OS_MACOS)
#include <limits.h>
#include <mach-o/dyld.h>
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
    add_qt_plugin_path(QFileInfo(QString::fromUtf8(path)).absoluteDir().filePath(
                          QStringLiteral("lib/plugins")),
                      true);
  }
#endif
#if defined(Q_OS_WIN)
  wchar_t exe[MAX_PATH];
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0) {
    add_qt_plugin_path(QFileInfo(QString::fromWCharArray(exe)).absoluteDir().filePath(
                          QStringLiteral("plugins")),
                      true);
  } else if (argv0 != nullptr) {
    add_qt_plugin_path(QFileInfo(QString::fromLocal8Bit(argv0)).absoluteDir().filePath(
                          QStringLiteral("plugins")),
                      true);
  }
#else
  (void)argv0;
#endif
#if defined(WDS_QT_SVG_PLUGIN_DIR)
  // Homebrew / MinGW splits qtsvg into its own prefix; qtbase's plugin root
  // has no qsvg. Packaged builds already have it under the paths above.
  add_qt_plugin_path(QString::fromUtf8(WDS_QT_SVG_PLUGIN_DIR), false);
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
    started_ = std::chrono::steady_clock::now();
    write_startup_header();
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

    if (!window_open_) {
      window_start_ = std::chrono::steady_clock::now();
      paint_count0_ = paint_count;
      last_seen_paint_count_ = paint_count;
      window_open_ = true;
    }

    ++frames_;
    if (playing) ++playing_frames_;
    if (wall_us >= 0) {
      acc(wall_sum_us_, wall_max_us_, wall_us);
      ++wall_samples_;
    }
    acc(delta_sum_us_, delta_max_us_, delta_us);
    acc(tick_sum_us_, tick_max_us_, tick_us);
    acc(render_sum_us_, render_max_us_, render_us);
    acc(fence_sum_us_, fence_max_us_, fence_us);
    acc(acquire_sum_us_, acquire_max_us_, acquire_us);
    acc(submit_sum_us_, submit_max_us_, submit_us);
    acc(present_sum_us_, present_max_us_, present_us);
    acc(ui_sum_us_, ui_max_us_, ui_us);
    if (paint_count > last_seen_paint_count_) {
      last_seen_paint_count_ = paint_count;
      acc(paint_sum_us_, paint_max_us_, paint_us);
      ++paint_samples_;
    }

    if (wall_us > kHitchUs) {
      ++hitch_count_;
      append_hitch(wall_us, delta_us, tick_us, render_us, fence_us, acquire_us, submit_us,
                   present_us, paint_us, ui_us, playing);
    }

    last_path_counts_ = path_counts;
    last_paint_count_ = paint_count;

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
        "# frame-diag started=%s qt=%s refresh_hz=%.1f mvk_sync_queue_submits=%s\n",
        started.constData(), qVersion(), refresh_hz, mvk ? mvk : "unset");
    if (n > 0) file_.write(buf, n);
    file_.flush();
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
    char buf[768];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "t=%.1f frames=%d fps=%.1f playing=%.2f wall_avg=%.1f wall_max=%.1f hitch=%d "
        "delta_avg=%.1f delta_max=%.1f tick_avg=%.1f tick_max=%.1f render_avg=%.1f "
        "render_max=%.1f fence_avg=%.1f fence_max=%.1f acquire_avg=%.1f acquire_max=%.1f "
        "submit_avg=%.1f submit_max=%.1f present_avg=%.1f present_max=%.1f paint_avg=%.1f "
        "paint_max=%.1f paints=%llu ui_avg=%.1f ui_max=%.1f swapchain_recreate=%llu "
        "create_texture_rgba=%llu upload_fence_wait=%llu upload_submit=%llu "
        "upload_fence_reap=%llu descriptor_allocate=%llu apply_msaa=%llu "
        "apply_msaa_idle_wait=%llu\n",
        t, frames_, fps, playing, avg_ms(wall_sum_us_, wall_samples_), ms(wall_max_us_), hitch_count_,
        avg_ms(delta_sum_us_, frames_), ms(delta_max_us_), avg_ms(tick_sum_us_, frames_),
        ms(tick_max_us_), avg_ms(render_sum_us_, frames_), ms(render_max_us_),
        avg_ms(fence_sum_us_, frames_), ms(fence_max_us_), avg_ms(acquire_sum_us_, frames_),
        ms(acquire_max_us_), avg_ms(submit_sum_us_, frames_), ms(submit_max_us_),
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
    paint_samples_ = 0;
    wall_samples_ = 0;
    wall_sum_us_ = wall_max_us_ = 0;
    delta_sum_us_ = delta_max_us_ = 0;
    tick_sum_us_ = tick_max_us_ = 0;
    render_sum_us_ = render_max_us_ = 0;
    fence_sum_us_ = fence_max_us_ = 0;
    acquire_sum_us_ = acquire_max_us_ = 0;
    submit_sum_us_ = submit_max_us_ = 0;
    present_sum_us_ = present_max_us_ = 0;
    paint_sum_us_ = paint_max_us_ = 0;
    ui_sum_us_ = ui_max_us_ = 0;
    window_open_ = false;
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
  int64_t present_sum_us_ = 0;
  int64_t present_max_us_ = 0;
  int64_t paint_sum_us_ = 0;
  int64_t paint_max_us_ = 0;
  int64_t ui_sum_us_ = 0;
  int64_t ui_max_us_ = 0;
  uint64_t paint_count0_ = 0;
  uint64_t last_seen_paint_count_ = 0;
  uint64_t last_paint_count_ = 0;
  wds::renderer::RendererPathCounts path_counts0_{};
  wds::renderer::RendererPathCounts last_path_counts_{};
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
    WDS_LOG("qt: %s\n", utf8.constData());
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
}  // namespace

int main(int argc, char** argv) {
  wds::common::install_crash_handlers();
  g_prev_qt_handler = qInstallMessageHandler(&wds_qt_message_handler);
  prepare_bundled_qt_plugins(argv[0]);
  // Pin the bundled MoltenVK ICD before Qt or the loader enumerates Homebrew
  // drivers. Two MoltenVK copies make vkGetDeviceQueue jump to NULL.
  wds::ui::prepare_macos_vulkan_environment(argv[0]);
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

  auto deps = wds::ui::check_startup_dependencies(argv[0]);
  if (!deps.ok()) {
    wds::common::mark_clean_exit();
    return wds::ui::fail_startup_dependencies(deps);
  }

  QVulkanInstance vk_instance;
  if (!vk_instance.create()) {
    wds::ui::add_vulkan_unavailable(deps);
    wds::common::mark_clean_exit();
    return wds::ui::fail_startup_dependencies(deps);
  }
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
    v.lane_count = loaded.lane_count;
    v.msaa_samples = loaded.msaa_samples;
    return v;
  };


  FrameDiagLogger frame_diag;
  QObject::connect(&app, &QCoreApplication::aboutToQuit, [&frame_diag] { frame_diag.close(); });

  auto last_editor_tick = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now() - std::chrono::seconds(1));
  auto ui_alive = std::make_shared<std::atomic<bool>>(true);

  const auto clamped_tick_delta = [](std::chrono::steady_clock::time_point now,
                                     std::chrono::steady_clock::time_point last) -> float {
    const float raw = std::chrono::duration<float>(now - last).count();
    return std::clamp(raw, 0.0f, 0.08f);
  };

  std::function<void(float)> finish_editor_frame = [&](float delta) {
    editor.resize_editor_viewport(editor_widget->width(), editor_widget->height(),
                                  editor_widget->width(), editor_widget->height());
    editor.update(delta, {});
    const bool playing = editor.chart_preview().ready() &&
                         editor.chart_preview().transport().intends_playing();
    if (editor_widget->isVisible() && editor_widget->take_dirty_for_frame(playing)) {
      editor_widget->update();
    }
    *last_editor_tick = std::chrono::steady_clock::now();
  };

  preview_window->set_frame_callback([&editor, &window, preview_window, visual, ui_font,
                                      overlay_loaded_display, &frame_diag, editor_widget, ui_alive,
                                      &finish_editor_frame](
                                         float delta, int logical_w, int logical_h, int fb_w,
                                         int fb_h,
                                         const std::vector<wds::interaction::InputEvent>& events) mutable {
    const bool diag = frame_diag.active();
    int64_t wall_us = -1;
    if (diag) wall_us = frame_diag.begin_callback(std::chrono::steady_clock::now());
    if (!editor.chart_preview().ready()) {
      auto host = preview_window->host_surface();
      if (host.external_instance == VK_NULL_HANDLE || host.external_surface == VK_NULL_HANDLE ||
          !editor.chart_preview().initialize_empty(host, overlay_loaded_display(visual), ui_font)) {
        return;
      }
      if (editor.session().chart_count() == 0) {
        // No project was chosen on the splash (or smoke/direct launch): seed
        // the normal blank document only after the preview backend is live.
        (void)editor.session().new_project();
      }
      // Startup project selection can happen before the Vulkan surface exists;
      // attach its BGM once the live transport has been initialized.
      if (!editor.session().music_path().empty()) {
        // Do not decode the project audio inside the first exposed frame. Let
        // Qt paint the shell and schedule the decode on the next event turn.
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
    editor.chart_preview().tick(static_cast<int64_t>(delta * 1000000.0f));
    int64_t tick_us = 0;
    if (diag) {
      const auto tick_t1 = std::chrono::steady_clock::now();
      tick_us = std::chrono::duration_cast<std::chrono::microseconds>(tick_t1 - phase_t0).count();
      phase_t0 = tick_t1;
    }
    editor.chart_preview().render();
    int64_t render_us = 0;
    if (diag) {
      render_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - phase_t0)
                      .count();
    }
    editor.chart_preview().flush_retired_font_textures();
    window.on_preview_frame();
    finish_editor_frame(delta);
    if (diag) {
      frame_diag.finish_frame(editor, *editor_widget, wall_us,
                             static_cast<int64_t>(delta * 1000000.0f), tick_us, render_us);
    }
    (void)events;
  });

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

  QString open_path;
  // `--screenshot <path>[@<ms>]` may repeat; each entry schedules its own grab.
  QList<std::pair<QString, int>> screenshots;
  bool autoplay = false;
  int quit_after_sec = -1;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    if (args[i] == QLatin1String("--open") && i + 1 < args.size()) {
      open_path = args[++i];
    } else if (args[i] == QLatin1String("--autoplay")) {
      autoplay = true;
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

  if (!open_path.isEmpty()) {
    const QByteArray utf8 = open_path.toUtf8();
    const std::string path(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    if (!editor.session().open_wdsproject(path)) {
      std::fprintf(stderr, "failed to open project: %s\n", path.c_str());
    }
  }

  window.setWindowIcon(QApplication::windowIcon());
  if (!args.contains("--smoke-test")) {
    maybe_notify_previous_crash(nullptr);
    if (!window.show_startup_splash()) {
      wds::common::mark_clean_exit();
      return 0;
    }
  }
  window.show();
  window.raise();
  window.activateWindow();
  if (!args.contains("--smoke-test")) {
    QTimer::singleShot(0, &window, [&window] { window.maybe_auto_check_updates(); });
  }
  for (const auto& [screenshot_path, screenshot_ms] : screenshots) {
    QTimer::singleShot(screenshot_ms, &window, [&window, screenshot_path] {
      QScreen* screen = nullptr;
      if (QWindow* handle = window.windowHandle()) screen = handle->screen();
      if (screen == nullptr) screen = QGuiApplication::primaryScreen();
      QPixmap shot;
      if (screen != nullptr) shot = screen->grabWindow(window.winId());
      if (shot.isNull()) shot = window.grab();
      if (shot.isNull() || !shot.save(screenshot_path, "PNG")) {
        const QByteArray utf8 = screenshot_path.toUtf8();
        std::fprintf(stderr, "failed to save screenshot: %s\n", utf8.constData());
      }
    });
  }
  if (autoplay) {
    auto* play_timer = new QTimer(&window);
    play_timer->setInterval(250);
    const auto request_play = [&editor] {
      if (!editor.chart_preview().ready()) return false;
      auto& transport = editor.chart_preview().transport();
      if (!transport.playing()) transport.request_play();
      return true;
    };
    QObject::connect(play_timer, &QTimer::timeout, &window,
                     [play_timer, request_play, attempts = 0]() mutable {
                       if (request_play()) {
                         play_timer->stop();
                         return;
                       }
                       if (++attempts >= 40) play_timer->stop();
                     });
    QTimer::singleShot(2500, &window, [play_timer, request_play] {
      if (!request_play()) play_timer->start();
    });
  }
  if (quit_after_sec >= 0) {
    QTimer::singleShot(quit_after_sec * 1000, &app, &QApplication::quit);
  }
  if (args.contains("--smoke-test"))
    QTimer::singleShot(1200, &app, &QApplication::quit);
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
  edit_timer->stop();
  window.detach_ui_manager();
  editor_widget->set_global_key_handler({});
  frame_diag.close();
  wds::common::mark_clean_exit();
  return rc;
}

#include "wds/ui/qt/stem_panel.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#if !defined(_WIN32)
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace wds::ui {
struct StemPanelTestAccess {
  static void prepare(StemPanel& panel, const QString& cache) {
    panel.cache_dir_ = cache;
    panel.python_->setText(QCoreApplication::applicationFilePath());
    if (!panel.prepare_job()) std::abort();
    panel.job_source_ = panel.source_;
    panel.job_stamp_ = panel.source_stamp_;
    panel.job_generation_ = panel.source_generation_;
  }
  static QString job(StemPanel& panel) { return panel.job_->path(); }
  static void launch(StemPanel& panel, bool hang = false) {
    panel.launch(QCoreApplication::applicationFilePath(),
                 {"--fake-worker", panel.job_->path(), hang ? "hang" : "normal"});
  }
  static void launch_grandchild(StemPanel& panel) {
    panel.launch(QCoreApplication::applicationFilePath(),
                 {"--fake-worker", panel.job_->path(), "grandchild"});
  }
  static int sweep(const QString& dir, const QString& keep, qint64 max_age) {
    return StemPanel::sweep_stale_jobs(dir, keep, max_age);
  }
  static void cancel(StemPanel& panel) { panel.cancel(); }
  static void complete(StemPanel& panel) {
    panel.launch(QCoreApplication::applicationFilePath(), {"--fake-worker", panel.job_->path(), "complete"});
  }
  static int tracks(StemPanel& panel) { return panel.track_names().size(); }
  static bool active(StemPanel& panel) { return panel.active_; }
  static int progress(StemPanel& panel) { return panel.progress_->value(); }
  static QString status(StemPanel& panel) { return panel.status_->text(); }
  static void failed_launch(StemPanel& panel) { panel.launch("/nonexistent/wds-stem-python", {}); }
  static bool is_store_alias(const QString& path) { return StemPanel::is_windows_store_python_alias(path); }
  static QString resolve_python(const QString& path) { return StemPanel::resolve_python(path); }
  static QString discover_python() { return StemPanel::discover_system_python(); }
  static QString discover_python_in(const QStringList& dirs) {
    return StemPanel::discover_system_python_in(dirs);
  }
  static int rank(int major, int minor, bool free_threaded) {
    return StemPanel::python_rank(major, minor, free_threaded);
  }
  static bool tag(const QString& text, int* major, int* minor, bool* free_threaded) {
    return StemPanel::parse_python_tag(text, major, minor, free_threaded);
  }
  static QString sha256(const QString& path) { return StemPanel::sha256_hex_file(path); }
  static void inject_cache_source(StemPanel& panel, const QString& cache, const QString& source) {
    panel.cache_dir_ = cache;
    panel.source_ = source;
  }
  static bool try_cached(StemPanel& panel) { return panel.try_attach_cached_tracks(); }
  static void set_runtime_dir(StemPanel& panel, const QString& dir) { panel.runtime_dir_ = dir; }
  static void refresh_python(StemPanel& panel, bool prefer_runtime) {
    panel.refresh_python_selection(prefer_runtime);
  }
  static QString python_text(StemPanel& panel) { return panel.python_->text(); }
  static QString pick_python(StemPanel& panel, bool prefer_runtime) {
    return panel.pick_python(prefer_runtime);
  }
};
}

namespace {
int failures = 0;
void expect(bool condition, const char* text) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", text); ++failures; }
}
bool until(const std::function<bool()>& predicate, int timeout = 5000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout) {
    QCoreApplication::processEvents();
    QThread::msleep(2);
  }
  return predicate();
}
}

int main(int argc, char** argv) {
  if (argc > 2 && QByteArray(argv[1]) == "--fake-worker") {
#if !defined(_WIN32)
    if (QByteArray(argv[3]) == "grandchild") {
      // Stand-in for ffmpeg: a child that outlives a plain kill of this process.
      const pid_t child = fork();
      if (child == 0) {
        execlp("sleep", "sleep", "30", static_cast<char*>(nullptr));
        _exit(127);
      }
      QFile pid_file(QString::fromUtf8(argv[2]) + "/grandchild.pid");
      pid_file.open(QIODevice::WriteOnly);
      pid_file.write(QByteArray::number(static_cast<qlonglong>(child)));
      pid_file.close();
    }
#endif
    // Deliberately split a JSON line across pipe reads; don't require Python/torch.
    std::printf("{\"protocol\":1,\"event\":\"progress\",");
    std::fflush(stdout);
    QThread::msleep(60);
    std::printf("\"percent\":50,\"message\":\"working\"}\n");
    std::fflush(stdout);
    if (QByteArray(argv[3]) == "complete") {
      QThread::msleep(200);
      const auto result = QDir(QString::fromUtf8(argv[2])).absoluteFilePath("../completed");
      QDir().mkpath(result);
      QJsonObject tracks;
      for (const auto* name : {"drums", "bass", "vocals", "other"}) {
        const auto filename = QString::fromLatin1(name) + ".wav";
        QFile payload(result + '/' + filename);
        payload.open(QIODevice::WriteOnly);
        payload.write("fixture");
        tracks[name] = QJsonObject{{"file", filename}};
      }
      QFile manifest(result + "/manifest.json");
      manifest.open(QIODevice::WriteOnly);
      manifest.write(QJsonDocument(QJsonObject{{"schema", 1}, {"tracks", tracks}}).toJson());
      manifest.close();
      const auto event = QJsonDocument(QJsonObject{{"protocol", 1}, {"event", "complete"},
          {"manifest", result + "/manifest.json"}}).toJson(QJsonDocument::Compact);
      std::printf("%s\n", event.constData());
      return 0;
    }
    for (int i = 0; i < 200; ++i) {
      if (QByteArray(argv[3]) == "normal" && QFile::exists(QString::fromUtf8(argv[2]) + "/cancel")) return 2;
      QThread::msleep(25);
    }
    return 0;
  }
  QApplication app(argc, argv);
  QTemporaryDir cache;
  using wds::ui::StemPanel;
  using Access = wds::ui::StemPanelTestAccess;
  {
    expect(Access::is_store_alias(
               QStringLiteral("C:/Users/x/AppData/Local/Microsoft/WindowsApps/python3.exe")),
           "WindowsApps python3.exe is a Store alias");
    expect(Access::is_store_alias(QStringLiteral(
               R"(C:\Users\x\AppData\Local\Microsoft\WindowsApps\python.exe)")),
           "WindowsApps python.exe with backslashes is a Store alias");
    expect(Access::is_store_alias(
               QStringLiteral("C:/Users/x/AppData/Local/Microsoft/WindowsApps/python3.12.exe")),
           "WindowsApps python3.12.exe is a Store alias");
    expect(!Access::is_store_alias(QStringLiteral(
               "C:/Users/x/AppData/Local/Microsoft/WindowsApps/"
               "PythonSoftwareFoundation.Python.3.12_qbz5n2kfra8p0/python.exe")),
           "Store package python.exe is not an alias");
    expect(!Access::is_store_alias(QStringLiteral(
               "C:/Users/x/AppData/Local/Programs/Python/Python312/python.exe")),
           "python.org install is not an alias");
    expect(!Access::is_store_alias(QStringLiteral("/usr/bin/python3")),
           "unix python is not an alias");
    expect(!Access::is_store_alias(QString()), "empty path is not an alias");
    expect(Access::resolve_python(
               QStringLiteral("C:/Users/x/AppData/Local/Microsoft/WindowsApps/python3.exe"))
               .isEmpty(),
           "resolve rejects Store alias without launching it");
  }
  {
    expect(Access::rank(3, 14, false) > Access::rank(3, 13, false) &&
               Access::rank(3, 13, false) > Access::rank(3, 10, false),
           "discovery prefers the newest Python within 3.10–3.14");
    expect(Access::rank(3, 10, false) > Access::rank(3, 15, false) &&
               Access::rank(3, 10, false) > Access::rank(3, 9, false),
           "supported range beats too-new and too-old");
    expect(Access::rank(3, 10, false) > Access::rank(3, 14, true),
           "GIL builds beat free-threaded builds");
    expect(Access::rank(3, 13, true) > Access::rank(-1, -1, false) &&
               Access::rank(-1, -1, false) > Access::rank(3, 9, false),
           "unknown versions beat too-old ones");
    int major = 0, minor = 0;
    bool free_threaded = true;
    expect(Access::tag("3.15", &major, &minor, &free_threaded) && major == 3 && minor == 15 && !free_threaded,
           "PEP 514 tag 3.15");
    expect(Access::tag("3.13t-arm64", &major, &minor, &free_threaded) && minor == 13 && free_threaded,
           "PEP 514 free-threaded ARM64 tag");
    expect(Access::tag("3.14-64", &major, &minor, &free_threaded) && minor == 14 && !free_threaded,
           "install manager 64-bit tag");
    expect(!Access::tag("3.12-32", &major, &minor, &free_threaded), "32-bit tag is skipped");
    expect(!Access::tag("ContinuumAnalytics", &major, &minor, &free_threaded), "non-version tag is skipped");
  }
#if !defined(_WIN32)
  {
    // Full discover also walks /opt/homebrew/bin; a real 3.14 there outranks this fixture.
    QTemporaryDir bin;
    auto make = [&](const QString& name, bool executable) {
      QFile file(bin.filePath(name));
      file.open(QIODevice::WriteOnly);
      file.write("#!/bin/sh\n");
      file.close();
      file.setPermissions(executable ? QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                     : QFile::ReadOwner | QFile::WriteOwner);
    };
    make("python3.12", true);
    make("python3.13", true);
    make("python3.14t", true);
    make("python3.14-config", true);
    make("python3.14", false);
    make("python3.15", true);
    // A free-threaded build installed under the plain name.
    make("python3.11t", true);
    QFile::link(bin.filePath("python3.11t"), bin.filePath("python3.11"));
    expect(Access::discover_python_in({bin.path()}) == QFileInfo(bin.filePath("python3.13")).absoluteFilePath(),
           "POSIX discovery scans versioned interpreters in 3.10–3.14, skipping -config, free-threaded "
           "(even behind a plain-name symlink), too-new, and non-executables");
  }
#endif
  int ticks = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
  heartbeat.start(5);
  {
    StemPanel panel(nullptr);
    if (const char* screenshot = std::getenv("WDS_STEM_TEST_SCREENSHOT")) {
      panel.show();
      QCoreApplication::processEvents();
      panel.grab().save(QString::fromUtf8(screenshot));
      panel.hide();
    }
    expect(!panel.isModal(), "inference panel is modeless");
    Access::prepare(panel, cache.path());
    const auto job = Access::job(panel);
    Access::launch(panel);
    expect(until([&] { return Access::progress(panel) == 50; }), "incremental protocol parsed");
    expect(ticks > 5, "GUI event loop continues during worker execution");
    Access::cancel(panel);
    expect(until([&] { return !Access::active(panel); }), "cooperative cancel completes");
    expect(!QFile::exists(job), "cancel cleans owned intermediate files");
  }
  {
    StemPanel panel(nullptr);
    Access::prepare(panel, cache.path());
    const auto job = Access::job(panel);
    Access::launch(panel, true);
    expect(until([&] { return Access::progress(panel) == 50; }), "unresponsive worker starts");
    Access::cancel(panel);
    expect(until([&] { return !Access::active(panel); }), "cancel deadline kills unresponsive worker");
    expect(!QFile::exists(job), "hard cancel cleans owned intermediates");
  }
  {
    wds::ui::UiManager manager;
    StemPanel panel(&manager);
    QStringList published_tracks;
    int published_selection = -1;
    panel.set_tracks_changed_handler([&] {
      published_tracks = panel.track_names();
      published_selection = panel.selected_track();
    });
    expect(published_tracks.size() == 1 && published_selection == 0,
           "playback subscriber initially receives original track");
    Access::prepare(panel, cache.path());
    Access::complete(panel);
    expect(until([&] { return !Access::active(panel); }), "successful job completes");
    expect(Access::tracks(panel) == 5, "valid result exposes original plus four stems");
    expect(!panel.isVisible() && published_tracks.size() == 5,
           "hidden task panel publishes completed tracks to playback");
    // The fake worker deliberately returns unreadable audio files.
    expect(!panel.select_track(3) && published_selection == 0,
           "failed audition restores the actual playback selection");
    manager.chart_preview().load_music({}, false);
    expect(until([&] { return published_tracks.size() == 1; }) && published_selection == 0,
           "source reload clears playback tracks even when task panel is hidden");
  }
  {
    wds::ui::UiManager manager;
    StemPanel panel(&manager);
    Access::prepare(panel, cache.path());
    Access::complete(panel);
    expect(until([&] { return Access::progress(panel) == 50; }), "stale-result fixture starts");
    // Reloading the same path must invalidate the previous job, even with identical file metadata.
    manager.chart_preview().load_music({}, false);
    expect(until([&] { return !Access::active(panel); }), "stale job finishes or cancels");
    expect(Access::tracks(panel) == 1, "old generation never attaches stems to new song");
  }
  {
    // Opening a project with an existing Stem/ folder must expose tracks without running a job.
    wds::ui::UiManager manager;
    StemPanel panel(&manager);
    QTemporaryDir project;
    const QString source = project.filePath(QStringLiteral("song.bin"));
    {
      QFile file(source);
      expect(file.open(QIODevice::WriteOnly) && file.write("fixture-audio") == 13, "source fixture written");
    }
    const QString hash = Access::sha256(source);
    expect(!hash.isEmpty(), "source sha256");
    const QString entry = project.filePath(QStringLiteral("Stem")) + QLatin1Char('/') + hash.left(16);
    QDir().mkpath(entry);
    QJsonObject tracks;
    for (const auto* name : {"drums", "bass", "vocals", "other"}) {
      const auto filename = QString::fromLatin1(name) + QStringLiteral(".wav");
      QFile payload(entry + QLatin1Char('/') + filename);
      expect(payload.open(QIODevice::WriteOnly) && payload.write(name) > 0, "stem wav written");
      tracks[QLatin1String(name)] = QJsonObject{{QStringLiteral("file"), filename}};
    }
    QFile manifest(entry + QStringLiteral("/manifest.json"));
    expect(manifest.open(QIODevice::WriteOnly), "manifest open");
    manifest.write(QJsonDocument(QJsonObject{
                                    {QStringLiteral("schema"), 1},
                                    {QStringLiteral("spec"),
                                     QJsonObject{{QStringLiteral("source_sha256"), hash}}},
                                    {QStringLiteral("tracks"), tracks}})
                       .toJson());
    manifest.close();
    Access::inject_cache_source(panel, project.filePath(QStringLiteral("Stem")), source);
    expect(Access::try_cached(panel), "existing Stem cache attaches without a worker");
    expect(Access::tracks(panel) == 5, "cached stems publish original plus four tracks");
    {
      StemPanel other_panel(&manager);
      const QString other = project.filePath(QStringLiteral("other.bin"));
      QFile file(other);
      expect(file.open(QIODevice::WriteOnly) && file.write("different") > 0, "other source written");
      Access::inject_cache_source(other_panel, project.filePath(QStringLiteral("Stem")), other);
      expect(!Access::try_cached(other_panel), "cache for a different source is not attached");
      expect(Access::tracks(other_panel) == 1, "mismatched source keeps original-only tracks");
    }
  }
  {
    // Installed stem-runtime must win over a stale stems/python settings path without
    // requiring the user to click「安装」again after an app update.
    QTemporaryDir runtime_root;
    QTemporaryDir other_root;
#ifdef Q_OS_WIN
    const QString runtime_py = runtime_root.filePath(QStringLiteral("Scripts/python.exe"));
#else
    const QString runtime_py = runtime_root.filePath(QStringLiteral("bin/python"));
#endif
    const QString stale_py = other_root.filePath(QStringLiteral("stale-python"));
    QDir().mkpath(QFileInfo(runtime_py).absolutePath());
    for (const auto& path : {runtime_py, stale_py}) {
      QFile file(path);
      expect(file.open(QIODevice::WriteOnly) && file.write("x") > 0, "python fixture written");
      file.setPermissions(file.permissions() | QFileDevice::ExeUser);
    }
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
    const QVariant previous = settings.value("stems/python");
    settings.setValue("stems/python", stale_py);
    StemPanel panel(nullptr);
    Access::set_runtime_dir(panel, runtime_root.path());
    expect(Access::pick_python(panel, true) == QFileInfo(runtime_py).absoluteFilePath(),
           "prefer_installed_runtime selects stem-runtime over stale settings");
    Access::refresh_python(panel, true);
    expect(QFileInfo(Access::python_text(panel)).canonicalFilePath() ==
               QFileInfo(runtime_py).canonicalFilePath(),
           "refresh fills the field with the installed runtime");
    expect(Access::status(panel).contains(QStringLiteral("运行环境已就绪")),
           "status recognizes an existing runtime without reinstalling");
    if (previous.isValid()) settings.setValue("stems/python", previous);
    else settings.remove("stems/python");
  }
  {
    StemPanel panel(nullptr);
    Access::prepare(panel, cache.path());
    const auto job = Access::job(panel);
    Access::failed_launch(panel);
    expect(until([&] { return !Access::active(panel); }), "missing runtime unlocks controls");
    expect(!QFile::exists(job), "failed launch cleans task directory");
  }
  {
    StemPanel panel(nullptr);
    Access::prepare(panel, cache.path());
    const auto job = Access::job(panel);
    Access::launch(panel, true);
    expect(until([&] { return Access::progress(panel) == 50; }), "shutdown fixture starts");
    panel.detach();
    expect(!QFile::exists(job), "editor shutdown reaps worker before cleaning job");
  }
  {
    // Stale `.job-*` sweep: old removed, fresh / current / unrelated kept.
    QTemporaryDir stem;
    auto make = [&](const QString& name, bool old, bool with_child) {
      const auto path = stem.filePath(name);
      QDir().mkpath(path);
      if (with_child) { QFile f(path + "/decoded.wav"); f.open(QIODevice::WriteOnly); f.write("x"); }
      if (old) {
        const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::hours(3);
        if (with_child) std::filesystem::last_write_time((path + "/decoded.wav").toStdString(), past);
        std::filesystem::last_write_time(path.toStdString(), past);
      }
      return path;
    };
    const auto old_dir = make(".job-old111", true, true);
    const auto fresh_dir = make(".job-new222", false, false);
    const auto current_dir = make(".job-cur333", true, false);
    const auto busy_dir = make(".job-busy44", true, false);
    { QFile f(busy_dir + "/decoded.wav"); f.open(QIODevice::WriteOnly); f.write("x"); }  // fresh child
    std::filesystem::last_write_time(busy_dir.toStdString(),
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(3));
    const auto other = make("other-dir", true, false);
    const int removed = Access::sweep(stem.path(), current_dir, 3600);
    expect(removed == 1, "sweep reports one removal");
    expect(!QFile::exists(old_dir), "stale job dir removed");
    expect(QFile::exists(fresh_dir), "fresh job dir kept");
    expect(QFile::exists(current_dir), "current job dir kept");
    expect(QFile::exists(busy_dir), "job dir with recently written file kept");
    expect(QFile::exists(other), "non-job dir kept");
  }
#if !defined(_WIN32)
  {
    StemPanel panel(nullptr);
    Access::prepare(panel, cache.path());
    const auto job = Access::job(panel);
    Access::launch_grandchild(panel);
    expect(until([&] { return Access::progress(panel) == 50; }), "grandchild fixture starts");
    QFile pid_file(job + "/grandchild.pid");
    pid_t grandchild = 0;
    if (pid_file.open(QIODevice::ReadOnly)) grandchild = static_cast<pid_t>(pid_file.readAll().toLongLong());
    expect(grandchild > 1 && ::kill(grandchild, 0) == 0, "grandchild is alive before cancel");
    Access::cancel(panel);
    expect(until([&] { return !Access::active(panel); }, 8000), "cancel finishes with grandchild");
    expect(grandchild > 1 && until([&] { return ::kill(grandchild, 0) != 0; }, 3000),
           "cancel kills the worker's whole process group");
    if (grandchild > 1) ::kill(grandchild, SIGKILL);  // hygiene if the assertion failed
  }
#endif
  return failures ? 1 : 0;
}

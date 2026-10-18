#include "wds/ui/qt/stem_panel.hpp"

#include "wds/common/crash_handler.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#endif
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <filesystem>

namespace wds::ui {
namespace {

bool windows_store_python_alias(const QString& path) {
  if (path.isEmpty()) return false;
  QString normalized = QDir::fromNativeSeparators(path.trimmed());
  normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
  const QFileInfo info(normalized);
  const QString name = info.fileName().toLower();
  const bool alias_name = name == QLatin1String("python.exe") || name == QLatin1String("python3.exe") ||
                          (name.startsWith(QLatin1String("python3.")) && name.endsWith(QLatin1String(".exe")));
  if (!alias_name) return false;
  return info.dir().dirName().compare(QLatin1String("WindowsApps"), Qt::CaseInsensitive) == 0;
}

#ifdef Q_OS_WIN
bool parse_python_version(const QString& text, int* major, int* minor) {
  static const QRegularExpression re(QStringLiteral(R"(^(\d+)\.(\d+))"));
  const auto match = re.match(text);
  if (!match.hasMatch()) return false;
  *major = match.captured(1).toInt();
  *minor = match.captured(2).toInt();
  return true;
}

QStringList path_dirs_without_windowsapps() {
  QStringList dirs;
  const auto path = qEnvironmentVariable("PATH");
  for (const auto& dir : path.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
    if (QFileInfo(dir).fileName().compare(QLatin1String("WindowsApps"), Qt::CaseInsensitive) == 0) continue;
    dirs << dir;
  }
  return dirs;
}

QString find_named_python(const QString& name) {
  const auto dirs = path_dirs_without_windowsapps();
  QString found = QStandardPaths::findExecutable(name, dirs);
  if (found.isEmpty() && !name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
    found = QStandardPaths::findExecutable(name + QLatin1String(".exe"), dirs);
  }
  if (found.isEmpty() || windows_store_python_alias(found)) return {};
  return QFileInfo(found).absoluteFilePath();
}

QString python_from_py_launcher() {
  auto dirs = path_dirs_without_windowsapps();
  const QString root = qEnvironmentVariable("SystemRoot");
  if (!root.isEmpty()) dirs.prepend(root);
  QString py = QStandardPaths::findExecutable(QStringLiteral("py.exe"), dirs);
  if (py.isEmpty()) py = QStandardPaths::findExecutable(QStringLiteral("py"), dirs);
  if (py.isEmpty() || windows_store_python_alias(py)) return {};
  QProcess probe;
  probe.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
    args->flags |= CREATE_NO_WINDOW;
  });
  probe.start(py, {QStringLiteral("-3"), QStringLiteral("-c"),
                   QStringLiteral("import sys; print(sys.executable)")});
  if (!probe.waitForStarted(2000)) return {};
  if (!probe.waitForFinished(4000)) {
    probe.kill();
    probe.waitForFinished(1000);
    return {};
  }
  if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) return {};
  const auto resolved = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
  if (resolved.isEmpty() || windows_store_python_alias(resolved) || !QFileInfo::exists(resolved)) return {};
  return QFileInfo(resolved).absoluteFilePath();
}

void collect_pep514(QStringList* out, QSettings::Scope scope, QSettings::Format format) {
  QSettings root(format, scope, QStringLiteral("Python"), QStringLiteral("PythonCore"));
  for (const auto& ver : root.childGroups()) {
    int major = 0;
    int minor = 0;
    if (parse_python_version(ver, &major, &minor)) {
      if (major < 3 || (major == 3 && minor < 10)) continue;
    }
    root.beginGroup(ver);
    root.beginGroup(QStringLiteral("InstallPath"));
    QString exe = root.value(QStringLiteral("ExecutablePath")).toString();
    if (exe.isEmpty()) {
      QString dir = root.value(QStringLiteral(".")).toString();
      if (dir.isEmpty()) dir = root.value(QString()).toString();
      if (!dir.isEmpty()) exe = QDir(dir).filePath(QStringLiteral("python.exe"));
    }
    root.endGroup();
    root.endGroup();
    if (exe.isEmpty()) continue;
    const QFileInfo info(exe);
    if (!info.exists() || !info.isFile() || windows_store_python_alias(info.absoluteFilePath())) continue;
    out->append(info.absoluteFilePath());
  }
}

QString python_from_pep514() {
  QStringList found;
  collect_pep514(&found, QSettings::UserScope, QSettings::Registry64Format);
  collect_pep514(&found, QSettings::SystemScope, QSettings::Registry64Format);
  collect_pep514(&found, QSettings::UserScope, QSettings::Registry32Format);
  collect_pep514(&found, QSettings::SystemScope, QSettings::Registry32Format);
  QString best;
  int best_key = -1;
  for (const auto& exe : found) {
    int major = 3;
    int minor = 10;
    const QFileInfo info(exe);
    parse_python_version(info.dir().dirName(), &major, &minor);
    const int key = major * 1000 + minor;
    if (key >= best_key) {
      best_key = key;
      best = exe;
    }
  }
  return best;
}

QString python_from_standard_dirs() {
  QStringList roots;
  const QString local = qEnvironmentVariable("LOCALAPPDATA");
  if (!local.isEmpty()) roots << local + QLatin1String("/Programs/Python");
  const QString pf = qEnvironmentVariable("ProgramFiles");
  if (!pf.isEmpty()) roots << pf;
  const QString pf86 = qEnvironmentVariable("ProgramFiles(x86)");
  if (!pf86.isEmpty()) roots << pf86;
  QString best;
  int best_key = -1;
  static const QRegularExpression dir_re(QStringLiteral(R"(^Python(\d)(\d+)$)"),
                                         QRegularExpression::CaseInsensitiveOption);
  for (const auto& root : roots) {
    QDir dir(root);
    if (!dir.exists()) continue;
    const auto entries = dir.entryList(QStringList{QStringLiteral("Python3*")},
                                       QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& name : entries) {
      const QString exe = dir.filePath(name + QLatin1String("/python.exe"));
      const QFileInfo info(exe);
      if (!info.exists() || !info.isFile() || windows_store_python_alias(info.absoluteFilePath())) continue;
      int key = 0;
      const auto match = dir_re.match(name);
      if (match.hasMatch()) {
        const int major = match.captured(1).toInt();
        const int minor = match.captured(2).toInt();
        if (major < 3 || (major == 3 && minor < 10)) continue;
        key = major * 1000 + minor;
      }
      if (key >= best_key) {
        best_key = key;
        best = info.absoluteFilePath();
      }
    }
  }
  return best;
}
#endif

}  // namespace

bool StemPanel::is_windows_store_python_alias(const QString& path) {
  return windows_store_python_alias(path);
}

QString StemPanel::resolve_python(const QString& path) {
  const QString trimmed = path.trimmed();
  if (trimmed.isEmpty() || windows_store_python_alias(trimmed)) return {};
  const bool has_sep = trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char('\\'));
  if (has_sep || QFileInfo(trimmed).isAbsolute()) {
    const QFileInfo info(trimmed);
    if (info.exists() && info.isFile() && !windows_store_python_alias(info.absoluteFilePath())) {
      return info.absoluteFilePath();
    }
#ifdef Q_OS_WIN
    if (!trimmed.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
      const QFileInfo exe(trimmed + QLatin1String(".exe"));
      if (exe.exists() && exe.isFile() && !windows_store_python_alias(exe.absoluteFilePath())) {
        return exe.absoluteFilePath();
      }
    }
#endif
    return {};
  }
#ifdef Q_OS_WIN
  return find_named_python(trimmed);
#else
  return QStandardPaths::findExecutable(trimmed);
#endif
}

namespace {

bool stem_error_ui_allowed() {
  if (qEnvironmentVariableIsSet("WDS_CRASH_NO_DIALOG") || qEnvironmentVariableIsSet("WDS_STEM_NO_DIALOG")) {
    return false;
  }
  const auto platform = QGuiApplication::platformName();
  return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
}

QString write_stem_error_log(const QString& body) {
  const QString dir = QString::fromUtf8(wds::common::crash_log_directory());
  if (dir.isEmpty() || !QDir().mkpath(dir)) return {};
  const auto now = QDateTime::currentDateTime();
  const QString name = QStringLiteral("error-%1-%2.log")
                           .arg(now.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")))
                           .arg(QCoreApplication::applicationPid());
  const QString path = QDir(dir).filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
  file.write(body.toUtf8());
  if (!body.endsWith(QLatin1Char('\n'))) file.write("\n");
  return QFileInfo(file).absoluteFilePath();
}

void reveal_in_file_manager(const QString& path) {
  const QFileInfo info(path);
  if (!info.exists()) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
    return;
  }
#if defined(Q_OS_WIN)
  QProcess::startDetached(QStringLiteral("explorer.exe"),
                          {QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(info.absoluteFilePath()))});
#elif defined(Q_OS_MACOS)
  QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), info.absoluteFilePath()});
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
}

}  // namespace

QString StemPanel::discover_system_python() {
#ifdef Q_OS_WIN
  if (const auto pep = python_from_pep514(); !pep.isEmpty()) return pep;
  if (const auto local = python_from_standard_dirs(); !local.isEmpty()) return local;
  if (const auto named = find_named_python(QStringLiteral("python")); !named.isEmpty()) return named;
  if (const auto named = find_named_python(QStringLiteral("python3")); !named.isEmpty()) return named;
  return python_from_py_launcher();
#else
  QString found = QStandardPaths::findExecutable(QStringLiteral("python3"));
  if (found.isEmpty()) found = QStandardPaths::findExecutable(QStringLiteral("python"));
  return found;
#endif
}

StemPanel::StemPanel(UiManager* manager, QWidget* parent)
    : QDialog(parent), manager_(manager) {
  setWindowTitle(tr("本地分轨"));
  setModal(false);
  resize(620, 420);
  runtime_dir_ = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/wds-editor/stem-runtime";
  auto* layout = new QVBoxLayout(this);
  auto* explanation = new QLabel(tr("后台分离鼓、贝斯、人声和其他声部，可继续写谱和播放。\n首次使用需要安装运行环境并下载模型，之后可离线使用。"), this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  unsaved_warning_ = new QLabel(
      tr("当前工程尚未保存到磁盘：分轨缓存将写入临时目录，退出编辑器后无法继续使用。"),
      this);
  unsaved_warning_->setWordWrap(true);
  unsaved_warning_->setObjectName(QStringLiteral("stemUnsavedWarning"));
  layout->addWidget(unsaved_warning_);
  source_label_ = new QLabel(this);
  source_label_->setWordWrap(true);
  layout->addWidget(source_label_);
  auto* form = new QFormLayout;
  python_ = new QLineEdit(this);
  python_->setObjectName("stemPython");
  QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  QString python = resolve_python(qEnvironmentVariable("WDS_STEM_PYTHON"));
  if (python.isEmpty()) python = resolve_python(settings.value("stems/python").toString());
  if (python.isEmpty()) python = resolve_python(runtime_python());
  if (python.isEmpty()) python = discover_system_python();
  python_->setText(python);
  python_->setPlaceholderText(tr("选择 Python 3.10 或更新版本"));
  auto* interpreter = new QHBoxLayout;
  interpreter->addWidget(python_);
  browse_ = new QPushButton(tr("选择…"), this);
  interpreter->addWidget(browse_);
  form->addRow(tr("Python"), interpreter);
  install_ = new QPushButton(tr("安装分轨运行环境"), this);
  install_->setObjectName("stemInstall");
  form->addRow(QString(), install_);
  device_ = new QComboBox(this);
  device_->addItem(tr("自动选择"), "auto");
  device_->addItem("NVIDIA CUDA", "cuda");
  device_->addItem("Apple MPS", "mps");
  device_->addItem("CPU", "cpu");
  form->addRow(tr("计算设备"), device_);
  threads_ = new QSpinBox(this);
  threads_->setRange(1, 16);
  threads_->setValue(2);
  form->addRow(tr("CPU 线程"), threads_);
  background_ = new QCheckBox(tr("优先保证编辑流畅"), this);
  background_->setChecked(true);
  form->addRow(QString(), background_);
  layout->addLayout(form);
  auto* actions = new QHBoxLayout;
  start_ = new QPushButton(tr("生成分轨"), this);
  start_->setObjectName("stemStart");
  cancel_ = new QPushButton(tr("取消"), this);
  cancel_->setObjectName("stemCancel");
  auto* cache = new QPushButton(tr("打开缓存目录"), this);
  actions->addWidget(start_);
  actions->addWidget(cancel_);
  actions->addStretch();
  actions->addWidget(cache);
  layout->addLayout(actions);
  progress_ = new QProgressBar(this);
  progress_->setRange(0, 100);
  progress_->setValue(0);
  layout->addWidget(progress_);
  status_ = new QLabel(tr("就绪"), this);
  status_->setWordWrap(true);
  layout->addWidget(status_);

  connect(start_, &QPushButton::clicked, this, [this] { start(); });
  connect(cancel_, &QPushButton::clicked, this, [this] { cancel(); });
  connect(install_, &QPushButton::clicked, this, [this] { install_runtime(); });
  connect(browse_, &QPushButton::clicked, this, [this] {
    const auto file = QFileDialog::getOpenFileName(this, tr("选择 Python"), python_->text());
    if (!file.isEmpty()) python_->setText(file);
  });
  connect(cache, &QPushButton::clicked, this, [this] {
    refresh_cache_dir();
    if (cache_dir_.isEmpty()) return;
    QDir().mkpath(cache_dir_);
    QDesktopServices::openUrl(QUrl::fromLocalFile(cache_dir_));
  });
  connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { read_output(); });
  connect(&process_, &QProcess::readyReadStandardError, this, [this] {
    const auto err = process_.readAllStandardError();
    append_process_log(err);
    note_download_progress(QString::fromUtf8(err));
  });
  connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this](int code, QProcess::ExitStatus status) { finished(code, status); });
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      error_ = tr("无法启动 Python：%1").arg(process_.errorString());
      finished(-1, QProcess::CrashExit);
    }
  });
  kill_timer_.setSingleShot(true);
  kill_timer_.setInterval(3000);
  connect(&kill_timer_, &QTimer::timeout, this, [this] { kill_process_tree(); });
#if !defined(_WIN32)
  // Own session/process group so killpg() reaches ffmpeg and other grandchildren.
  process_.setChildProcessModifier([] { ::setsid(); });
#else
  connect(&process_, &QProcess::started, this, [this] { assign_job_object(); });
#endif
  connect(&source_timer_, &QTimer::timeout, this, [this] { sync_source(); });
  source_timer_.setInterval(300);
  refresh_cache_dir();
  sync_source();
}

void StemPanel::update_polling() {
  // Stat-ing the source can block on slow mounts, so only poll while someone
  // can see the panel or a job/published stems depend on source changes.
  const bool needed = manager_ && (isVisible() || active_ || !tracks_.isEmpty());
  if (needed) {
    if (!source_timer_.isActive()) source_timer_.start();
  } else {
    source_timer_.stop();
  }
}

void StemPanel::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  sync_source();
}

void StemPanel::hideEvent(QHideEvent* event) {
  QDialog::hideEvent(event);
  update_polling();
}

StemPanel::~StemPanel() { detach(); }

void StemPanel::assign_job_object() {
#if defined(_WIN32)
  close_job_object();
  const qint64 pid = process_.processId();
  if (pid <= 0) return;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
  ZeroMemory(&limits, sizeof(limits));
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  bool ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0;
  if (ok) {
    HANDLE worker = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    ok = worker && AssignProcessToJobObject(job, worker) != 0;
    if (worker) CloseHandle(worker);
  }
  if (!ok) {
    // e.g. already inside a non-nestable job; fall back to killing the worker only.
    CloseHandle(job);
    return;
  }
  job_object_ = job;
#endif
}

void StemPanel::close_job_object() {
#if defined(_WIN32)
  // KILL_ON_JOB_CLOSE reaps any stragglers still in the job.
  if (job_object_) CloseHandle(static_cast<HANDLE>(job_object_));
  job_object_ = nullptr;
#endif
}

void StemPanel::kill_process_tree() {
#if defined(_WIN32)
  if (job_object_) TerminateJobObject(static_cast<HANDLE>(job_object_), 1);
#else
  // processId() is non-zero only while running; the worker is its own group leader.
  const qint64 pid = process_.processId();
  if (pid > 1) ::killpg(static_cast<pid_t>(pid), SIGKILL);
#endif
  process_.kill();
}

int StemPanel::sweep_stale_jobs(const QString& dir, const QString& keep, qint64 max_age_secs) {
  const QDir root(dir);
  if (!root.exists()) return 0;
  const QString keep_path = keep.isEmpty() ? QString() : QFileInfo(keep).canonicalFilePath();
  const auto now = QDateTime::currentDateTime();
  int removed = 0;
  const auto entries = root.entryInfoList({QStringLiteral(".job-*")},
                                          QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
  for (const auto& entry : entries) {
    if (entry.isSymLink()) continue;
    if (!keep_path.isEmpty() && entry.canonicalFilePath() == keep_path) continue;
    // A long job may not touch the directory itself; use the newest direct child too.
    auto newest = entry.lastModified();
    const auto children = QDir(entry.absoluteFilePath()).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const auto& child : children) newest = qMax(newest, child.lastModified());
    if (newest.secsTo(now) < max_age_secs) continue;
    if (QDir(entry.absoluteFilePath()).removeRecursively()) ++removed;
  }
  return removed;
}

QStringList StemPanel::track_names() const {
  if (tracks_.isEmpty()) return {tr("原曲")};
  return {tr("原曲"), tr("鼓"), tr("贝斯"), tr("人声"), tr("其他")};
}

void StemPanel::set_tracks_changed_handler(std::function<void()> handler) {
  on_tracks_changed_ = std::move(handler);
  notify_tracks_changed();
}

void StemPanel::notify_tracks_changed() {
  if (on_tracks_changed_) on_tracks_changed_();
}

QString StemPanel::runtime_python() const {
#ifdef Q_OS_WIN
  return runtime_dir_ + "/Scripts/python.exe";
#else
  return runtime_dir_ + "/bin/python";
#endif
}

void StemPanel::detach() {
  manager_ = nullptr;
  source_timer_.stop();
  kill_timer_.stop();
  disconnect(&process_, nullptr, this, nullptr);
  if (process_.state() != QProcess::NotRunning) {
    if (job_) {
      QFile marker(job_->filePath("cancel"));
      if (!marker.open(QIODevice::WriteOnly)) qWarning("StemPanel: cannot write cancel marker");
    }
    if (!process_.waitForFinished(300)) {
      kill_process_tree();
      process_.waitForFinished(1000);
    }
  }
  close_job_object();
  job_.reset();
  // Ephemeral stem cache lives only for this editor session.
  ephemeral_cache_.reset();
  cache_dir_.clear();
  cache_ephemeral_ = false;
}

void StemPanel::sync_source() {
  if (!manager_) return;
  refresh_cache_dir();
  const auto path = QString::fromStdString(manager_->session().music_path());
  const QFileInfo info(path);
  const auto stamp = path + ':' + QString::number(info.size()) + ':' + QString::number(info.lastModified().toMSecsSinceEpoch());
  const auto generation = manager_->chart_preview().music_generation();
  if (stamp != source_stamp_ || generation != source_generation_) {
    if (active_ && !install_step_) cancel();
    source_ = path;
    source_stamp_ = stamp;
    source_generation_ = generation;
    tracks_.clear();
    selected_track_ = 0;
    notify_tracks_changed();
    source_label_->setText(path.isEmpty() ? tr("请先导入音乐或打开工程") : tr("音源：%1").arg(info.fileName()));
    source_label_->setToolTip(path);
  }
  refresh_controls();
}

void StemPanel::refresh_cache_dir() {
  if (!manager_) {
    // Unit tests inject cache_dir_ without a session; leave it alone.
    if (unsaved_warning_) unsaved_warning_->setVisible(false);
    return;
  }
  if (!manager_->session().project_path().empty()) {
    const QFileInfo project(QString::fromStdString(manager_->session().project_path()));
    cache_dir_ = project.absolutePath() + QStringLiteral("/Stem");
    cache_ephemeral_ = false;
    // Keep ephemeral_cache_ until detach so an in-session audition of
    // stems generated before save still finds its files.
  } else {
    if (!ephemeral_cache_) {
      ephemeral_cache_ = std::make_unique<QTemporaryDir>();
      ephemeral_cache_->setAutoRemove(true);
    }
    if (!ephemeral_cache_->isValid()) {
      cache_dir_.clear();
      cache_ephemeral_ = true;
      if (unsaved_warning_) unsaved_warning_->setVisible(true);
      return;
    }
    cache_dir_ = ephemeral_cache_->path();
    cache_ephemeral_ = true;
  }
  if (unsaved_warning_) unsaved_warning_->setVisible(cache_ephemeral_);
  if (!cache_ephemeral_ && !cache_dir_.isEmpty() && cache_dir_ != swept_dir_) {
    // One-time per Stem folder: reclaim jobs left by crashed sessions.
    swept_dir_ = cache_dir_;
    QTimer::singleShot(0, this, [this, dir = cache_dir_] {
      sweep_stale_jobs(dir, job_ ? job_->path() : QString(), 3600);
    });
  }
}

void StemPanel::refresh_controls() {
  start_->setEnabled(manager_ && !source_.isEmpty() && !active_);
  cancel_->setEnabled(active_ && !cancelled_);
  install_->setEnabled(!active_);
  python_->setEnabled(!active_);
  browse_->setEnabled(!active_);
  device_->setEnabled(!active_);
  threads_->setEnabled(!active_);
  background_->setEnabled(!active_);
  update_polling();
}

bool StemPanel::prepare_job() {
  QString python = resolve_python(python_->text());
  if (python.isEmpty()) python = resolve_python(runtime_python());
  if (python.isEmpty()) {
    report_task_failure(tr("无法启动任务"),
                       is_windows_store_python_alias(python_->text())
                           ? tr("当前 Python 是 Windows 商店占位程序，无法创建运行环境。请安装 Python 3.10 或更新版本，或用「选择…」指定 python.exe。")
                           : tr("请先选择 Python 3.10 或更新版本，再安装分轨运行环境。"));
    return false;
  }
  python_->setText(python);
  if (cache_dir_.isEmpty() || !QDir().mkpath(cache_dir_)) {
    report_task_failure(tr("无法启动任务"), tr("无法创建分轨缓存目录"));
    return false;
  }
  job_ = std::make_unique<QTemporaryDir>(cache_dir_ + "/.job-XXXXXX");
  if (!job_->isValid() || !QFile::copy(":/wds/stem-worker.py", job_->filePath("worker.py")) ||
      !QFile::copy(":/wds/stem-requirements.txt", job_->filePath("requirements.txt"))) {
    job_.reset();
    report_task_failure(tr("无法启动任务"), tr("无法创建分轨任务目录"));
    return false;
  }
  cancelled_ = false;
  downloading_model_ = false;
  error_.clear();
  manifest_.clear();
  output_.clear();
  process_log_.clear();
  last_python_.clear();
  last_arguments_.clear();
  progress_->setRange(0, 100);
  progress_->setValue(0);
  return true;
}

void StemPanel::launch(const QString& python, const QStringList& arguments) {
  active_ = true;
  refresh_controls();
  last_python_ = python;
  last_arguments_ = arguments;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("PYTHONUNBUFFERED", "1");
  environment.insert("PYTHONUTF8", "1");
  environment.insert("PYTHONDONTWRITEBYTECODE", "1");
  process_.setProcessEnvironment(environment);
  process_.setWorkingDirectory(job_->path());
  process_.start(python, arguments);
}

void StemPanel::start() {
  sync_source();
  if (active_ || source_.isEmpty() || !prepare_job()) return;
  install_step_ = 0;
  job_source_ = source_;
  job_stamp_ = source_stamp_;
  job_generation_ = source_generation_;
  QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  settings.setValue("stems/python", python_->text().trimmed());
  QStringList args{"-u", job_->filePath("worker.py"), "--input", source_, "--cache", cache_dir_,
                   "--job", job_->path(), "--device", device_->currentData().toString(),
                   "--threads", QString::number(threads_->value())};
  if (background_->isChecked()) args << "--background";
  status_->setText(tr("正在启动后台分轨，可继续编辑…"));
  launch(python_->text().trimmed(), args);
}

void StemPanel::cancel() {
  if (!active_ || cancelled_) return;
  cancelled_ = true;
  if (job_) {
    QFile marker(job_->filePath("cancel"));
    if (!marker.open(QIODevice::WriteOnly)) qWarning("StemPanel: cannot write cancel marker");
  }
  if (install_step_) process_.terminate();
  kill_timer_.start();
  status_->setText(tr("正在取消…"));
  refresh_controls();
}

void StemPanel::install_runtime() {
  refresh_cache_dir();
  if (active_ || !prepare_job()) return;
  install_step_ = 1;
  status_->setText(tr("正在创建独立运行环境…"));
  progress_->setRange(0, 0);
  if (QFileInfo::exists(runtime_python())) {
    install_step_ = 2;
    launch(runtime_python(), {"-m", "pip", "install", "--disable-pip-version-check", "-r", job_->filePath("requirements.txt")});
  } else {
    launch(python_->text().trimmed(), {"-m", "venv", runtime_dir_});
  }
}

void StemPanel::note_download_progress(const QString& text) {
  if (text.isEmpty() || cancelled_) return;
  int percent = -1;
  static const QRegularExpression percent_re(QStringLiteral(R"((\d{1,3}(?:\.\d+)?)\s*%)"));
  auto it = percent_re.globalMatch(text);
  while (it.hasNext()) {
    const int value = qRound(it.next().captured(1).toDouble());
    if (value >= 0 && value <= 100) percent = value;
  }
  if (percent < 0) {
    static const QRegularExpression bytes_re(
        QStringLiteral(R"((\d+(?:\.\d+)?)\s*/\s*(\d+(?:\.\d+)?)\s*([KMG])?B)"),
        QRegularExpression::CaseInsensitiveOption);
    auto bytes = bytes_re.globalMatch(text);
    while (bytes.hasNext()) {
      const auto m = bytes.next();
      const double done = m.captured(1).toDouble();
      const double total = m.captured(2).toDouble();
      if (total > 0.0) percent = qBound(0, qRound(100.0 * done / total), 100);
    }
  }
  if (percent < 0) return;
  progress_->setRange(0, 100);
  progress_->setValue(percent);
  if (install_step_) {
    status_->setText(tr("正在下载并安装推理依赖… %1%").arg(percent));
  } else if (downloading_model_) {
    status_->setText(tr("正在下载 HTDemucs 模型… %1%").arg(percent));
  }
}

void StemPanel::append_process_log(const QByteArray& bytes) {
  if (bytes.isEmpty()) return;
  process_log_ += bytes;
  constexpr int kMax = 524288;
  if (process_log_.size() > kMax) process_log_ = process_log_.right(kMax);
}

void StemPanel::report_task_failure(const QString& title, const QString& summary) {
  status_->setText(summary);
  if (!stem_error_ui_allowed()) return;
  QString body;
  body += title + QLatin1Char('\n');
  body += summary + QLatin1String("\n\n");
  if (!last_python_.isEmpty()) {
    body += QStringLiteral("python=%1\n").arg(last_python_);
    body += QStringLiteral("args=%1\n").arg(last_arguments_.join(QLatin1Char(' ')));
  }
  if (job_) body += QStringLiteral("job=%1\n").arg(job_->path());
  if (!runtime_dir_.isEmpty()) body += QStringLiteral("runtime=%1\n").arg(runtime_dir_);
  if (!process_log_.isEmpty()) {
    body += QLatin1String("\n--- process output ---\n");
    body += QString::fromUtf8(process_log_);
  }
  const QString path = write_stem_error_log(body);
  QMessageBox box(this);
  box.setIcon(QMessageBox::Critical);
  box.setWindowTitle(title);
  box.setText(summary);
  if (!path.isEmpty()) {
    box.setInformativeText(tr("详细信息已写入日志：%1").arg(QDir::toNativeSeparators(path)));
  }
  box.addButton(tr("确定"), QMessageBox::AcceptRole);
  auto* open = box.addButton(tr("打开日志位置"), QMessageBox::ActionRole);
  box.exec();
  if (box.clickedButton() == static_cast<QAbstractButton*>(open)) {
    reveal_in_file_manager(path.isEmpty() ? QString::fromUtf8(wds::common::crash_log_directory())
                                          : path);
  }
}

void StemPanel::read_output() {
  const auto bytes = process_.readAllStandardOutput();
  append_process_log(bytes);
  if (install_step_) {
    note_download_progress(QString::fromUtf8(bytes));
    return;
  }
  output_ += bytes;
  if (output_.size() > 65536) { error_ = tr("分轨进程返回了过长的消息"); cancel(); return; }
  while (true) {
    const auto end = output_.indexOf('\n');
    if (end < 0) break;
    const auto line = output_.left(end);
    output_.remove(0, end + 1);
    const auto object = QJsonDocument::fromJson(line).object();
    if (object.value("protocol").toInt() != 1) {
      note_download_progress(QString::fromUtf8(line));
      continue;
    }
    const auto event = object.value("event").toString();
    if (event == "progress" && !cancelled_) {
      const auto message = object.value("message").toString();
      downloading_model_ = message.contains(QStringLiteral("下载"));
      progress_->setValue(object.value("percent").toInt());
      status_->setText(message);
    } else if (event == "complete") {
      downloading_model_ = false;
      manifest_ = object.value("manifest").toString();
    } else if (event == "error") {
      downloading_model_ = false;
      error_ = object.value("message").toString();
    }
  }
}

void StemPanel::finished(int code, QProcess::ExitStatus status) {
  if (!active_) return;
  read_output();
  const auto err = process_.readAllStandardError();
  append_process_log(err);
  note_download_progress(QString::fromUtf8(err));
  kill_timer_.stop();
  close_job_object();
  const bool success = code == 0 && status == QProcess::NormalExit && !cancelled_;
  if (success && install_step_ == 1) {
    process_log_ += QByteArrayLiteral("\n--- pip install ---\n");
    install_step_ = 2;
    status_->setText(tr("正在下载并安装推理依赖，可继续编辑…"));
    launch(runtime_python(), {"-m", "pip", "install", "--disable-pip-version-check", "-r", job_->filePath("requirements.txt")});
    return;
  }
  active_ = false;
  downloading_model_ = false;
  progress_->setRange(0, 100);
  sync_source();
  if (cancelled_) {
    status_->setText(tr("已取消"));
  } else if (!success) {
    if (!process_log_.isEmpty()) {
      process_log_ += QByteArrayLiteral("\n--- exit ---\n");
      process_log_ += QByteArray("code=") + QByteArray::number(code) +
                      " status=" + (status == QProcess::NormalExit ? "normal" : "crash") + "\n";
    }
    report_task_failure(install_step_ ? tr("安装运行环境失败") : tr("分轨失败"),
                        error_.isEmpty() ? tr("任务失败。") : error_);
  } else if (install_step_) {
    python_->setText(runtime_python());
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
    settings.setValue("stems/python", runtime_python());
    status_->setText(tr("运行环境已安装，可以生成分轨。"));
    progress_->setValue(100);
  } else if (manager_ && source_ == job_source_ && source_stamp_ == job_stamp_ && source_generation_ == job_generation_) {
    QFile file(manifest_);
    if (file.open(QIODevice::ReadOnly)) {
      const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
      const auto entries = manifest.value("tracks").toObject();
      QStringList paths;
      const auto dir = QFileInfo(manifest_).absoluteDir();
      for (const auto* name : {"drums", "bass", "vocals", "other"}) {
        const auto filename = QString::fromLatin1(name) + ".wav";
        if (entries.value(name).toObject().value("file").toString() != filename ||
            !QFileInfo::exists(dir.filePath(filename))) { paths.clear(); break; }
        paths << dir.filePath(filename);
      }
      if (manifest.value("schema").toInt() == 1 && paths.size() == 4) {
        tracks_ = paths;
        notify_tracks_changed();
        progress_->setValue(100);
        status_->setText(tr("分轨已就绪，可在「播放」面板选择音轨试听。"));
      } else {
        report_task_failure(tr("分轨失败"), tr("分轨结果不完整，请重新生成。"));
      }
    } else {
      report_task_failure(tr("分轨失败"), tr("未找到分轨结果。"));
    }
  } else {
    status_->setText(tr("分轨已缓存；当前音源已更换。"));
  }
  install_step_ = 0;
  job_.reset();
  refresh_controls();
}

bool StemPanel::select_track(int index) {
  sync_source();
  if (!manager_ || index < 0 || index > tracks_.size()) {
    notify_tracks_changed();
    return false;
  }
  if (index == selected_track_) return true;
  const auto path = index == 0 ? source_ : tracks_[index - 1];
  if (!manager_->chart_preview().audition_music(path.toStdString())) {
    status_->setText(tr("无法试听该音轨（文件不可读或长度与原曲不一致）。"));
    notify_tracks_changed();
    return false;
  }
  selected_track_ = index;
  notify_tracks_changed();
  return true;
}
}  // namespace wds::ui

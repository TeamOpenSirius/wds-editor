#include "wds/ui/qt/stem_panel.hpp"

#include "wds/common/crash_handler.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/qt/system_proxy.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
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

// Keeps the highest-ranked candidate; ties keep the first one found (PATH order).
struct PythonPick {
  QString path;
  int rank = -1;
  void offer(const QString& candidate, int candidate_rank) {
    if (candidate_rank > rank) {
      rank = candidate_rank;
      path = candidate;
    }
  }
};

// "python3.12", "python3.13t", "python3.14.exe"; rejects python3.12-config and friends.
bool parse_python_name(const QString& name, int* major, int* minor, bool* free_threaded) {
  static const QRegularExpression re(QStringLiteral(R"(^python(\d+)\.(\d+)(t?)(?:\.exe)?$)"),
                                     QRegularExpression::CaseInsensitiveOption);
  const auto match = re.match(name);
  if (!match.hasMatch()) return false;
  *major = match.captured(1).toInt();
  *minor = match.captured(2).toInt();
  *free_threaded = !match.captured(3).isEmpty();
  return true;
}

// Newest supported GIL build first: free-threaded builds get wheels later and run some
// packages (e.g. PyYAML) as pure-Python fallbacks. Outside 3.10–3.14 and unknown builds
// stay selectable so install.py can explain what is wrong; supported versions win discovery.
int python_rank(int major, int minor, bool free_threaded) {
  if (major < 0) return 1;
  if (major != 3 || minor < 10 || minor > 14) return 0;
  return (free_threaded ? 2 : 3) * 1000000 + major * 1000 + minor;
}

// PEP 514 tag such as "3.12", "3.13t", "3.14-64", "3.13-arm64"; 32-bit ("3.12-32") is rejected
// because PyTorch publishes 64-bit wheels only.
bool parse_python_tag(const QString& tag, int* major, int* minor, bool* free_threaded) {
  static const QRegularExpression re(QStringLiteral(R"(^(\d+)\.(\d+)(t?)(?:-(\w+))?$)"),
                                     QRegularExpression::CaseInsensitiveOption);
  const auto match = re.match(tag);
  if (!match.hasMatch() || match.captured(4) == QLatin1String("32")) return false;
  *major = match.captured(1).toInt();
  *minor = match.captured(2).toInt();
  *free_threaded = !match.captured(3).isEmpty();
  return true;
}

#ifdef Q_OS_WIN
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

QString windows_python_exe(const QString& dir, int major, int minor, bool free_threaded) {
  return QDir(dir).filePath(free_threaded ? QStringLiteral("python%1.%2t.exe").arg(major).arg(minor)
                                          : QStringLiteral("python.exe"));
}

void offer_windows_python(PythonPick* pick, const QString& exe, int major, int minor, bool free_threaded) {
  const QFileInfo info(exe);
  if (!info.exists() || !info.isFile() || windows_store_python_alias(info.absoluteFilePath())) return;
  pick->offer(info.absoluteFilePath(), python_rank(major, minor, free_threaded));
}

void collect_pep514(PythonPick* pick, QSettings::Scope scope, QSettings::Format format) {
  QSettings root(format, scope, QStringLiteral("Python"), QStringLiteral("PythonCore"));
  for (const auto& tag : root.childGroups()) {
    int major = 0;
    int minor = 0;
    bool free_threaded = false;
    if (!parse_python_tag(tag, &major, &minor, &free_threaded)) continue;
    root.beginGroup(tag);
    const bool is_32bit = root.value(QStringLiteral("SysArchitecture")).toString() == QLatin1String("32bit");
    root.beginGroup(QStringLiteral("InstallPath"));
    QString exe = root.value(QStringLiteral("ExecutablePath")).toString();
    if (exe.isEmpty()) {
      QString dir = root.value(QStringLiteral(".")).toString();
      if (dir.isEmpty()) dir = root.value(QString()).toString();
      if (!dir.isEmpty()) exe = windows_python_exe(dir, major, minor, free_threaded);
    }
    root.endGroup();
    root.endGroup();
    if (!is_32bit && !exe.isEmpty()) offer_windows_python(pick, exe, major, minor, free_threaded);
  }
}

// Directories named after a tag: python.org's "Python313t-arm64" and the Python install
// manager's "pythoncore-3.14-64" (the only installer for Python 3.16+).
void collect_tagged_dirs(PythonPick* pick, const QString& root, const QRegularExpression& name_re) {
  const QDir dir(root);
  if (!dir.exists()) return;
  for (const auto& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const auto match = name_re.match(name);
    int major = 0;
    int minor = 0;
    bool free_threaded = false;
    if (!match.hasMatch() || !parse_python_tag(match.captured(1), &major, &minor, &free_threaded)) {
      continue;
    }
    offer_windows_python(pick, windows_python_exe(dir.filePath(name), major, minor, free_threaded), major, minor,
                         free_threaded);
  }
}

PythonPick python_from_installs() {
  PythonPick pick;
  collect_pep514(&pick, QSettings::UserScope, QSettings::Registry64Format);
  collect_pep514(&pick, QSettings::SystemScope, QSettings::Registry64Format);
  collect_pep514(&pick, QSettings::UserScope, QSettings::Registry32Format);
  collect_pep514(&pick, QSettings::SystemScope, QSettings::Registry32Format);
  static const QRegularExpression legacy_re(QStringLiteral(R"(^Python(\d)(\d+.*)$)"),
                                            QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression manager_re(QStringLiteral(R"(^pythoncore-(.+)$)"),
                                             QRegularExpression::CaseInsensitiveOption);
  const QString local = qEnvironmentVariable("LOCALAPPDATA");
  if (!local.isEmpty()) collect_tagged_dirs(&pick, local + QLatin1String("/Python"), manager_re);
  QStringList legacy_roots;
  if (!local.isEmpty()) legacy_roots << local + QLatin1String("/Programs/Python");
  // ProgramFiles(x86) only holds 32-bit Pythons, which PyTorch does not support.
  if (const QString pf = qEnvironmentVariable("ProgramFiles"); !pf.isEmpty()) legacy_roots << pf;
  for (const auto& root : legacy_roots) {
    const QDir dir(root);
    if (!dir.exists()) continue;
    for (const auto& name : dir.entryList({QStringLiteral("Python3*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
      const auto match = legacy_re.match(name);
      int major = 0;
      int minor = 0;
      bool free_threaded = false;
      // "Python313t-arm64" -> tag "3.13t-arm64".
      if (!match.hasMatch() ||
          !parse_python_tag(match.captured(1) + QLatin1Char('.') + match.captured(2), &major, &minor,
                            &free_threaded)) {
        continue;
      }
      offer_windows_python(&pick, windows_python_exe(dir.filePath(name), major, minor, free_threaded), major,
                           minor, free_threaded);
    }
  }
  return pick;
}
#else
QStringList posix_python_dirs() {
  QStringList dirs = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
  const QString home = QDir::homePath();
  // Desktop launchers do not source shell rc files, so conda/pyenv/Homebrew are often off PATH.
#ifdef Q_OS_MACOS
  dirs << QStringLiteral("/opt/homebrew/bin");
#endif
  dirs << home + QLatin1String("/.local/bin") << QStringLiteral("/usr/local/bin") << QStringLiteral("/usr/bin");
  const QDir pyenv(home + QLatin1String("/.pyenv/versions"));
  for (const auto& version : pyenv.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    dirs << pyenv.filePath(version + QLatin1String("/bin"));
  }
  for (const auto* conda : {"/miniforge3/bin", "/miniconda3/bin", "/anaconda3/bin"}) {
    dirs << home + QLatin1String(conda);
  }
  dirs << QStringLiteral("/opt/miniconda3/bin") << QStringLiteral("/opt/anaconda3/bin");
  dirs.removeDuplicates();
  return dirs;
}

int posix_python_rank(const QFileInfo& info) {
  int major = 0;
  int minor = 0;
  bool free_threaded = false;
  // The link target names the real build: python3 -> python3.12, and some distributions
  // ship python3.15 -> python3.15t for their free-threaded build.
  if (parse_python_name(QFileInfo(info.canonicalFilePath()).fileName(), &major, &minor, &free_threaded) ||
      parse_python_name(info.fileName(), &major, &minor, &free_threaded)) {
    return python_rank(major, minor, free_threaded);
  }
  return python_rank(-1, -1, false);
}

QString python_from_posix_dirs(const QStringList& dirs) {
  PythonPick pick;
  for (const auto& path : dirs) {
    const QDir dir(path);
    QStringList names = dir.entryList({QStringLiteral("python3.*")}, QDir::Files | QDir::Executable);
    names << QStringLiteral("python3") << QStringLiteral("python");
    for (const auto& name : names) {
      const QFileInfo info(dir.filePath(name));
      if (!info.isFile() || !info.isExecutable()) continue;
      int major = 0;
      int minor = 0;
      bool free_threaded = false;
      if (name.startsWith(QLatin1String("python3.")) && !parse_python_name(name, &major, &minor, &free_threaded)) {
        continue;  // python3.12-config, python3.12-gdb.py, ...
      }
      pick.offer(info.absoluteFilePath(), posix_python_rank(info));
    }
  }
  return pick.path;
}
#endif

}  // namespace

bool StemPanel::is_windows_store_python_alias(const QString& path) {
  return windows_store_python_alias(path);
}

int StemPanel::python_rank(int major, int minor, bool free_threaded) {
  return wds::ui::python_rank(major, minor, free_threaded);
}

bool StemPanel::parse_python_tag(const QString& tag, int* major, int* minor, bool* free_threaded) {
  return wds::ui::parse_python_tag(tag, major, minor, free_threaded);
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
  const PythonPick installed = python_from_installs();
  // A known-too-old install loses to an unversioned PATH/py.exe interpreter.
  if (installed.rank > python_rank(-1, -1, false)) return installed.path;
  if (const auto named = find_named_python(QStringLiteral("python")); !named.isEmpty()) return named;
  if (const auto named = find_named_python(QStringLiteral("python3")); !named.isEmpty()) return named;
  if (const auto launcher = python_from_py_launcher(); !launcher.isEmpty()) return launcher;
  return installed.path;
#else
  return python_from_posix_dirs(posix_python_dirs());
#endif
}

QString StemPanel::discover_system_python_in(const QStringList& dirs) {
#ifdef Q_OS_WIN
  Q_UNUSED(dirs);
  return {};
#else
  return python_from_posix_dirs(dirs);
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
  python_->setPlaceholderText(tr("选择 Python 3.10～3.14"));
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
  // Prefer the installed stem-runtime over a stale stems/python setting so an app
  // update does not look like a missing runtime until the user clicks「安装」.
  refresh_python_selection(true);

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
  // can see the panel, a job/published stems depend on source changes, or the
  // session already has music (so open/import can attach Stem cache without
  // opening this dialog).
  const bool needed =
      manager_ && (isVisible() || active_ || !tracks_.isEmpty() ||
                   !manager_->session().music_path().empty());
  if (needed) {
    if (!source_timer_.isActive()) source_timer_.start();
  } else {
    source_timer_.stop();
  }
}

void StemPanel::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  sync_source();
  refresh_python_selection(true);
  // First open with an empty field and nothing discoverable: tell the user before they click install.
  if (!prompted_python_missing_ && resolve_python(python_->text()).isEmpty() &&
      resolve_python(runtime_python()).isEmpty() && discover_system_python().isEmpty()) {
    prompted_python_missing_ = true;
    prompt_python_not_installed();
  }
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

void StemPanel::resync() { sync_source(); }

QString StemPanel::sha256_hex_file(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) hash.addData(file.read(1 << 20));
  return QString::fromLatin1(hash.result().toHex());
}

bool StemPanel::attach_from_manifest(const QString& manifest_path) {
  QFile file(manifest_path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
  const auto entries = manifest.value(QStringLiteral("tracks")).toObject();
  QStringList paths;
  const QDir dir = QFileInfo(manifest_path).absoluteDir();
  for (const auto* name : {"drums", "bass", "vocals", "other"}) {
    const auto filename = QString::fromLatin1(name) + QStringLiteral(".wav");
    if (entries.value(QLatin1String(name)).toObject().value(QStringLiteral("file")).toString() !=
            filename ||
        !QFileInfo::exists(dir.filePath(filename))) {
      return false;
    }
    paths << dir.filePath(filename);
  }
  if (manifest.value(QStringLiteral("schema")).toInt() != 1 || paths.size() != 4) return false;
  tracks_ = paths;
  notify_tracks_changed();
  return true;
}

bool StemPanel::try_attach_cached_tracks() {
  if (!manager_ || cache_dir_.isEmpty() || source_.isEmpty() || active_) return false;
  const QString source_hash = sha256_hex_file(source_);
  if (source_hash.isEmpty()) return false;
  QString best_manifest;
  QDateTime best_mtime;
  const QDir root(cache_dir_);
  const auto entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const auto& entry : entries) {
    if (entry.fileName().startsWith(QLatin1Char('.'))) continue;
    const QString manifest_path =
        entry.absoluteFilePath() + QStringLiteral("/manifest.json");
    QFile file(manifest_path);
    if (!file.open(QIODevice::ReadOnly)) continue;
    const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    if (manifest.value(QStringLiteral("schema")).toInt() != 1) continue;
    const auto spec = manifest.value(QStringLiteral("spec")).toObject();
    if (spec.value(QStringLiteral("source_sha256")).toString() != source_hash) continue;
    const auto track_entries = manifest.value(QStringLiteral("tracks")).toObject();
    bool complete = true;
    for (const auto* name : {"drums", "bass", "vocals", "other"}) {
      const auto filename = QString::fromLatin1(name) + QStringLiteral(".wav");
      if (track_entries.value(QLatin1String(name))
                  .toObject()
                  .value(QStringLiteral("file"))
                  .toString() != filename ||
          !QFileInfo::exists(entry.absoluteFilePath() + QLatin1Char('/') + filename)) {
        complete = false;
        break;
      }
    }
    if (!complete) continue;
    const auto mtime = entry.lastModified();
    if (best_manifest.isEmpty() || mtime > best_mtime) {
      best_manifest = manifest_path;
      best_mtime = mtime;
    }
  }
  if (best_manifest.isEmpty() || !attach_from_manifest(best_manifest)) return false;
  progress_->setValue(100);
  status_->setText(tr("分轨已就绪，可在「播放」面板选择音轨试听。"));
  return true;
}

QString StemPanel::runtime_python() const {
#ifdef Q_OS_WIN
  return runtime_dir_ + "/Scripts/python.exe";
#else
  return runtime_dir_ + "/bin/python";
#endif
}

QString StemPanel::pick_python(bool prefer_installed_runtime) const {
  if (const auto env = resolve_python(qEnvironmentVariable("WDS_STEM_PYTHON")); !env.isEmpty()) {
    return env;
  }
  const QString runtime = resolve_python(runtime_python());
  if (prefer_installed_runtime && !runtime.isEmpty()) return runtime;
  QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  if (const auto saved = resolve_python(settings.value("stems/python").toString()); !saved.isEmpty()) {
    return saved;
  }
  if (!runtime.isEmpty()) return runtime;
  return discover_system_python();
}

void StemPanel::refresh_python_selection(bool prefer_installed_runtime) {
  if (!prefer_installed_runtime) {
    if (const auto typed = resolve_python(python_->text()); !typed.isEmpty()) {
      python_->setText(typed);
      return;
    }
  }
  const QString python = pick_python(prefer_installed_runtime);
  if (!python.isEmpty()) python_->setText(python);
  const QString runtime = resolve_python(runtime_python());
  if (runtime.isEmpty() || active_) return;
  // Keep settings aligned with the venv we will actually launch after an update.
  if (!python.isEmpty() &&
      QFileInfo(python).canonicalFilePath() == QFileInfo(runtime).canonicalFilePath()) {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
    settings.setValue("stems/python", runtime);
  }
  if (status_ && !active_) status_->setText(tr("运行环境已就绪，可以生成分轨。"));
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
    if (active_ && !installing_) cancel();
    source_ = path;
    source_stamp_ = stamp;
    source_generation_ = generation;
    tracks_.clear();
    selected_track_ = 0;
    notify_tracks_changed();
    source_label_->setText(path.isEmpty() ? tr("请先导入音乐或打开工程") : tr("音源：%1").arg(info.fileName()));
    source_label_->setToolTip(path);
    // Open project / import music must recognize an existing Stem folder without
    // requiring the user to click「生成分轨」(worker cache hit).
    if (!path.isEmpty()) try_attach_cached_tracks();
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
  if (python.isEmpty()) python = discover_system_python();
  if (python.isEmpty()) {
    // Store python.exe under WindowsApps is a stub that cannot run; discovery already
    // skips it, so "only Store aliases" looks the same as no Python at all.
    if (is_windows_store_python_alias(python_->text())) {
      report_task_failure(
          tr("无法启动任务"),
          tr("当前 Python 是 Windows 商店占位程序，无法创建运行环境。请改用「选择…」指定已安装的 python.exe"));
    } else {
      prompted_python_missing_ = true;
      prompt_python_not_installed();
    }
    return false;
  }
  python_->setText(python);
  if (cache_dir_.isEmpty() || !QDir().mkpath(cache_dir_)) {
    report_task_failure(tr("无法启动任务"), tr("无法创建分轨缓存目录"));
    return false;
  }
  job_ = std::make_unique<QTemporaryDir>(cache_dir_ + "/.job-XXXXXX");
  if (!job_->isValid() || !QFile::copy(":/wds/stem-worker.py", job_->filePath("worker.py")) ||
      !QFile::copy(":/wds/stem-install.py", job_->filePath("install.py")) ||
      !QFile::copy(":/wds/stem-requirements.txt", job_->filePath("requirements.txt"))) {
    job_.reset();
    report_task_failure(tr("无法启动任务"), tr("无法创建分轨任务目录"));
    return false;
  }
  cancelled_ = false;
  downloading_model_ = false;
  error_.clear();
  install_message_.clear();
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
  // pip (PyTorch, Demucs, requirements) and torch.hub only look at proxy
  // environment variables; on Windows/macOS they do not evaluate the PAC script
  // Qt already resolves. Export the OS proxy for the host this process will use
  // so the Python download follows the same route as the ffmpeg downloader.
  apply_system_proxy(environment, QUrl(installing_
                                           ? QStringLiteral("https://pypi.org/")
                                           : QStringLiteral("https://dl.fbaipublicfiles.com/")));
  environment.insert("PYTHONUNBUFFERED", "1");
  environment.insert("PYTHONUTF8", "1");
  environment.insert("PYTHONDONTWRITEBYTECODE", "1");
  // A global PYTHONHOME/PYTHONPATH (left by other tools' installers) belongs to some
  // other interpreter and makes the selected one fail at startup or import foreign packages.
  environment.remove("PYTHONHOME");
  environment.remove("PYTHONPATH");
  process_.setProcessEnvironment(environment);
  process_.setWorkingDirectory(job_->path());
  process_.start(python, arguments);
}

void StemPanel::start() {
  sync_source();
  // Generation must use the stem venv when present; a stale settings/system path
  // would miss torch/demucs until the user re-clicks「安装」.
  refresh_python_selection(true);
  if (active_ || source_.isEmpty() || !prepare_job()) return;
  installing_ = false;
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
  if (installing_) process_.terminate();
  kill_timer_.start();
  status_->setText(tr("正在取消…"));
  refresh_controls();
}

void StemPanel::install_runtime() {
  refresh_cache_dir();
  // Keep a user-selected bootstrap interpreter; only fill the field when empty.
  refresh_python_selection(false);
  if (active_ || !prepare_job()) return;
  installing_ = true;
  status_->setText(tr("正在准备运行环境…"));
  progress_->setRange(0, 0);
  // install.py checks the interpreter, (re)builds the venv for it and picks the PyTorch
  // build for the GPU; selecting the runtime's own python repairs it in place.
  launch(python_->text().trimmed(),
         {"-u", job_->filePath("install.py"), "--runtime", runtime_dir_, "--job", job_->path(),
          "--requirements", job_->filePath("requirements.txt"), "--worker", job_->filePath("worker.py")});
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
  if (installing_) {
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

void StemPanel::prompt_python_not_installed() {
  const QString summary = tr("未检测到本机已安装的 Python，请安装 Python 3.10～3.14 后再试");
  status_->setText(summary);
  if (!stem_error_ui_allowed()) return;
  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(tr("未安装 Python"));
  box.setText(summary);
  box.addButton(tr("确定"), QMessageBox::AcceptRole);
  auto* download = box.addButton(tr("前往下载"), QMessageBox::ActionRole);
  box.exec();
  if (box.clickedButton() == static_cast<QAbstractButton*>(download)) {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.python.org/downloads/release/python-3148/")));
  }
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
      downloading_model_ = !installing_ && message.contains(QStringLiteral("下载"));
      // Installer steps carry no percentage; keep the busy indicator for them.
      if (object.contains("percent")) {
        progress_->setRange(0, 100);
        progress_->setValue(object.value("percent").toInt());
      }
      status_->setText(message);
    } else if (event == "complete") {
      downloading_model_ = false;
      manifest_ = installing_ ? QString() : object.value("manifest").toString();
      if (installing_) install_message_ = object.value("message").toString();
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
    report_task_failure(installing_ ? tr("安装运行环境失败") : tr("分轨失败"),
                        error_.isEmpty() ? (installing_ ? tr("安装失败。请确认所选 Python 为 3.10～3.14 的 64 位版本。")
                                                        : tr("任务失败。"))
                                         : error_);
  } else if (installing_) {
    python_->setText(runtime_python());
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
    settings.setValue("stems/python", runtime_python());
    status_->setText(install_message_.isEmpty() ? tr("运行环境已安装，可以生成分轨。") : install_message_);
    progress_->setValue(100);
  } else if (manager_ && source_ == job_source_ && source_stamp_ == job_stamp_ && source_generation_ == job_generation_) {
    if (attach_from_manifest(manifest_)) {
      progress_->setValue(100);
      status_->setText(tr("分轨已就绪，可在「播放」面板选择音轨试听。"));
    } else if (manifest_.isEmpty() || !QFileInfo::exists(manifest_)) {
      report_task_failure(tr("分轨失败"), tr("未找到分轨结果。"));
    } else {
      report_task_failure(tr("分轨失败"), tr("分轨结果不完整，请重新生成。"));
    }
  } else {
    status_->setText(tr("分轨已缓存；当前音源已更换。"));
  }
  installing_ = false;
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

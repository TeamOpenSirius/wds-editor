#include "wds/ui/qt/stem_panel.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>

namespace wds::ui {

StemPanel::StemPanel(UiManager* manager, QWidget* parent)
    : QDialog(parent), manager_(manager) {
  setWindowTitle(tr("本地分轨"));
  setModal(false);
  resize(620, 460);
  cache_dir_ = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/wds-editor/stems";
  runtime_dir_ = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/wds-editor/stem-runtime";
  auto* layout = new QVBoxLayout(this);
  auto* explanation = new QLabel(tr("后台分离鼓、贝斯、人声和其他声部，可继续写谱和播放。\n首次使用需要安装运行环境并下载模型，之后可离线使用。"), this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  source_label_ = new QLabel(this);
  source_label_->setWordWrap(true);
  layout->addWidget(source_label_);
  auto* form = new QFormLayout;
  python_ = new QLineEdit(this);
  python_->setObjectName("stemPython");
  QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "WDS", "WDS Editor");
  QString python = qEnvironmentVariable("WDS_STEM_PYTHON");
  if (python.isEmpty()) python = settings.value("stems/python").toString();
  if (python.isEmpty() && QFileInfo::exists(runtime_python())) python = runtime_python();
  if (python.isEmpty()) python = QStandardPaths::findExecutable("python3");
  if (python.isEmpty()) python = QStandardPaths::findExecutable("python");
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
  log_ = new QPlainTextEdit(this);
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(150);
  log_->setMaximumHeight(110);
  layout->addWidget(log_);

  connect(start_, &QPushButton::clicked, this, [this] { start(); });
  connect(cancel_, &QPushButton::clicked, this, [this] { cancel(); });
  connect(install_, &QPushButton::clicked, this, [this] { install_runtime(); });
  connect(browse_, &QPushButton::clicked, this, [this] {
    const auto file = QFileDialog::getOpenFileName(this, tr("选择 Python"), python_->text());
    if (!file.isEmpty()) python_->setText(file);
  });
  connect(cache, &QPushButton::clicked, this, [this] {
    QDir().mkpath(cache_dir_);
    QDesktopServices::openUrl(QUrl::fromLocalFile(cache_dir_));
  });
  connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { read_output(); });
  connect(&process_, &QProcess::readyReadStandardError, this, [this] {
    append_log(QString::fromUtf8(process_.readAllStandardError()));
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
  connect(&kill_timer_, &QTimer::timeout, this, [this] { process_.kill(); });
  connect(&source_timer_, &QTimer::timeout, this, [this] { sync_source(); });
  source_timer_.start(300);
  sync_source();
}

StemPanel::~StemPanel() { detach(); }

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
    if (job_) { QFile marker(job_->filePath("cancel")); marker.open(QIODevice::WriteOnly); }
    if (!process_.waitForFinished(300)) {
      process_.kill();
      process_.waitForFinished(1000);
    }
  }
  job_.reset();
}

void StemPanel::sync_source() {
  if (!manager_) return;
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

void StemPanel::refresh_controls() {
  start_->setEnabled(manager_ && !source_.isEmpty() && !active_);
  cancel_->setEnabled(active_ && !cancelled_);
  install_->setEnabled(!active_);
  python_->setEnabled(!active_);
  browse_->setEnabled(!active_);
  device_->setEnabled(!active_);
  threads_->setEnabled(!active_);
  background_->setEnabled(!active_);
}

bool StemPanel::prepare_job() {
  if (python_->text().trimmed().isEmpty()) {
    status_->setText(tr("请先选择 Python 3.10 或更新版本，再安装分轨运行环境。"));
    return false;
  }
  QDir().mkpath(cache_dir_);
  job_ = std::make_unique<QTemporaryDir>(cache_dir_ + "/.job-XXXXXX");
  if (!job_->isValid() || !QFile::copy(":/wds/stem-worker.py", job_->filePath("worker.py")) ||
      !QFile::copy(":/wds/stem-requirements.txt", job_->filePath("requirements.txt"))) {
    status_->setText(tr("无法创建分轨任务目录"));
    job_.reset();
    return false;
  }
  cancelled_ = false;
  error_.clear();
  manifest_.clear();
  output_.clear();
  log_->clear();
  progress_->setRange(0, 100);
  progress_->setValue(0);
  return true;
}

void StemPanel::launch(const QString& python, const QStringList& arguments) {
  active_ = true;
  refresh_controls();
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
  if (job_) { QFile marker(job_->filePath("cancel")); marker.open(QIODevice::WriteOnly); }
  if (install_step_) process_.terminate();
  kill_timer_.start();
  status_->setText(tr("正在取消…"));
  refresh_controls();
}

void StemPanel::install_runtime() {
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

void StemPanel::append_log(const QString& text) {
  if (!text.trimmed().isEmpty()) log_->appendPlainText(text.right(8000).trimmed());
}

void StemPanel::read_output() {
  const auto bytes = process_.readAllStandardOutput();
  if (install_step_) { append_log(QString::fromUtf8(bytes)); return; }
  output_ += bytes;
  if (output_.size() > 65536) { error_ = tr("分轨进程返回了过长的消息"); cancel(); return; }
  while (true) {
    const auto end = output_.indexOf('\n');
    if (end < 0) break;
    const auto line = output_.left(end);
    output_.remove(0, end + 1);
    const auto object = QJsonDocument::fromJson(line).object();
    if (object.value("protocol").toInt() != 1) { append_log(QString::fromUtf8(line)); continue; }
    const auto event = object.value("event").toString();
    if (event == "progress" && !cancelled_) {
      progress_->setValue(object.value("percent").toInt());
      status_->setText(object.value("message").toString());
    } else if (event == "complete") {
      manifest_ = object.value("manifest").toString();
    } else if (event == "error") {
      error_ = object.value("message").toString();
    }
  }
}

void StemPanel::finished(int code, QProcess::ExitStatus status) {
  if (!active_) return;
  read_output();
  append_log(QString::fromUtf8(process_.readAllStandardError()));
  kill_timer_.stop();
  const bool success = code == 0 && status == QProcess::NormalExit && !cancelled_;
  if (success && install_step_ == 1) {
    install_step_ = 2;
    status_->setText(tr("正在下载并安装推理依赖，可继续编辑…"));
    launch(runtime_python(), {"-m", "pip", "install", "--disable-pip-version-check", "-r", job_->filePath("requirements.txt")});
    return;
  }
  active_ = false;
  progress_->setRange(0, 100);
  sync_source();
  if (cancelled_) {
    status_->setText(tr("已取消"));
  } else if (!success) {
    status_->setText(error_.isEmpty() ? tr("任务失败，请查看下方日志。") : error_);
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
        status_->setText(tr("分轨结果不完整，请重新生成。"));
      }
    } else {
      status_->setText(tr("未找到分轨结果，请查看日志。"));
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

#include "wds/ui/qt/export_video_dialog.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/export_video_job.hpp"
#include "wds/ui/qt/ffmpeg_encoder.hpp"
#include "wds/ui/regions/settings/preview_settings_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <algorithm>

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWidget>

namespace wds::ui {

ExportVideoDialog::ExportVideoDialog(UiManager* manager, EditorMainWindow* window, QWidget* parent)
    : QDialog(parent), manager_(manager), window_(window) {
  setWindowTitle(tr("导出视频"));
  setModal(true);
  setMinimumWidth(420);

  auto* root = new QVBoxLayout(this);
  env_row_ = new QWidget(this);
  auto* env_layout = new QHBoxLayout(env_row_);
  env_layout->setContentsMargins(0, 0, 0, 0);
  hint_ = new QLabel(env_row_);
  hint_->setWordWrap(true);
  env_button_ = new QPushButton(env_row_);
  env_layout->addWidget(hint_, 1);
  env_layout->addWidget(env_button_);
  root->addWidget(env_row_);

  resolution_ = new QComboBox(this);
  resolution_->addItem(tr("1080p（1920×1080）"), 1080);
  resolution_->addItem(tr("2K（2560×1440）"), 1440);
  resolution_->addItem(tr("4K（3840×2160）"), 2160);
  QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("WDS"),
                     QStringLiteral("WDS Editor"));
  const int saved = settings.value(QStringLiteral("export-video/resolution"), 1080).toInt();
  const int idx = resolution_->findData(saved);
  resolution_->setCurrentIndex(idx >= 0 ? idx : 0);
  root->addWidget(resolution_);

  progress_ = new QProgressBar(this);
  progress_->setRange(0, 100);
  progress_->setValue(0);
  root->addWidget(progress_);
  eta_ = new QLabel(QStringLiteral("—"), this);
  root->addWidget(eta_);

  auto* buttons = new QHBoxLayout;
  left_ = new QPushButton(this);
  right_ = new QPushButton(this);
  buttons->addWidget(left_);
  buttons->addStretch(1);
  buttons->addWidget(right_);
  root->addLayout(buttons);

  connect(env_button_, &QPushButton::clicked, this, [this] { on_env_button(); });
  connect(left_, &QPushButton::clicked, this, [this] { on_left(); });
  connect(right_, &QPushButton::clicked, this, [this] { on_right(); });

  downloader_ = new FfmpegDownloader(this);
  downloader_->on_progress = [this](qint64 rec, qint64 tot) {
    if (tot > 0) {
      ui_.progress = static_cast<int>(std::clamp(rec * 100 / tot, qint64{0}, qint64{100}));
    }
    ui_.eta = tot > 0 ? tr("已下载 %1 / %2 MB")
                            .arg(rec / (1024 * 1024))
                            .arg(tot / (1024 * 1024))
                      : tr("已下载 %1 MB").arg(rec / (1024 * 1024));
    apply_ui();
  };
  downloader_->on_finished = [this](bool ok, const QString& err) {
    if (window_ != nullptr) window_->set_export_session_busy(false);
    if (ok) {
      probe_env();
    } else {
      ui_.enter_retry(err);
      apply_ui();
    }
  };

  probe_env();
}

ExportVideoDialog::~ExportVideoDialog() {
  if (downloader_ != nullptr) {
    downloader_->on_progress = {};
    downloader_->on_finished = {};
    downloader_->abort();
  }
  if (job_ != nullptr) {
    job_->on_progress = {};
    job_->on_finished = {};
    job_->abort();
  }
}

void ExportVideoDialog::apply_ui() {
  // Do not toggle WindowCloseButtonHint here. setWindowFlag() hides a visible
  // QDialog, which ends exec() and destroys this object while a download is live.
  const bool env_row = ui_.show_env_row();
  env_row_->setVisible(env_row);
  hint_->setText(ui_.hint);
  env_button_->setVisible(ui_.show_env_button());
  env_button_->setText(ui_.env_button_text());
  resolution_->setEnabled(ui_.resolution_enabled());
  progress_->setValue(ui_.progress);
  eta_->setText(ui_.eta);
  left_->setText(ui_.left_button_text());
  right_->setText(ui_.right_button_text());
  left_->setEnabled(ui_.left_enabled());
  right_->setEnabled(ui_.right_enabled());
}

void ExportVideoDialog::probe_env() {
  probe_ = probe_ffmpeg_environment();
  if (probe_.ok) {
    ui_.enter_ready(!ffmpeg_has_hardware_encoder(probe_));
  } else {
    ui_.enter_need_env();
  }
  apply_ui();
}

void ExportVideoDialog::on_env_button() {
  if (ui_.phase == ExportVideoPhase::Downloading) {
    downloader_->abort();
    if (window_ != nullptr) window_->set_export_session_busy(false);
    ui_.enter_need_env();
    apply_ui();
    return;
  }
  start_download();
}

void ExportVideoDialog::start_download() {
  ui_.enter_downloading();
  apply_ui();
  if (window_ != nullptr) window_->set_export_session_busy(true);
  downloader_->start();
}

void ExportVideoDialog::on_left() {
  if (ui_.phase == ExportVideoPhase::Encoding) {
    if (job_ != nullptr) job_->pause();
    ui_.enter_paused();
    apply_ui();
    return;
  }
  if (ui_.phase == ExportVideoPhase::Paused) {
    if (job_ != nullptr) job_->resume();
    ui_.enter_encoding();
    apply_ui();
    return;
  }
  if (ui_.can_close()) reject();
}

void ExportVideoDialog::on_right() {
  if (ui_.phase == ExportVideoPhase::Encoding || ui_.phase == ExportVideoPhase::Paused) {
    if (job_ != nullptr) job_->abort();
    return;
  }
  if (ui_.phase == ExportVideoPhase::Done) {
    accept();
    return;
  }
  if (ui_.phase == ExportVideoPhase::Ready) start_generate();
}

QString ExportVideoDialog::default_mp4_path() const {
  QString name = QStringLiteral("1.mp4");
  QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  if (manager_ != nullptr) {
    const auto& session = manager_->session();
    name = QFileInfo(QString::fromStdString(
               session.official_chart_filename(session.active_chart_index())))
               .completeBaseName() +
           QStringLiteral(".mp4");
    if (!session.project_path().empty()) {
      dir = QFileInfo(QString::fromStdString(session.project_path())).absolutePath();
    } else if (session.chart_count() > 0) {
      const QFileInfo chart(
          QString::fromStdString(session.chart_path(session.active_chart_index())));
      if (!chart.absolutePath().isEmpty() && chart.absolutePath() != QLatin1String(".")) {
        dir = chart.absolutePath();
      }
    }
  }
  return QDir(dir).filePath(name);
}

void ExportVideoDialog::start_generate() {
  const QString suggested = default_mp4_path();
  QString path = QFileDialog::getSaveFileName(this, tr("导出视频"), suggested, tr("MP4 (*.mp4)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
    path += QStringLiteral(".mp4");
  }
  QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("WDS"),
                     QStringLiteral("WDS Editor"));
  settings.setValue(QStringLiteral("export-video/resolution"), resolution_->currentData().toInt());
  ExportVideoRequest req;
  req.output_mp4 = path;
  const int h = resolution_->currentData().toInt();
  if (h >= 2160) {
    req.width = 3840;
    req.height = 2160;
  } else if (h >= 1440) {
    req.width = 2560;
    req.height = 1440;
  } else {
    req.width = 1920;
    req.height = 1080;
  }
  if (manager_ != nullptr && manager_->settings_panel() != nullptr) {
    req.include_music = !manager_->settings_panel()->music_muted();
    req.include_sfx = !manager_->settings_panel()->sfx_muted();
  }
  if (job_ == nullptr) job_ = new ExportVideoJob(manager_, window_, this);
  job_->on_progress = [this](int pct, const QString& eta) {
    ui_.progress = pct;
    ui_.eta = eta;
    apply_ui();
  };
  job_->on_finished = [this](bool ok, const QString& err) {
    if (ok) {
      ui_.enter_done();
      apply_ui();
      return;
    }
    ui_.enter_ready(!ffmpeg_has_hardware_encoder(probe_ffmpeg_environment()));
    apply_ui();
    if (!err.isEmpty()) {
      QMessageBox::warning(this, tr("导出失败"), err);
    }
  };
  ui_.enter_encoding();
  apply_ui();
  job_->start(req);
}

void ExportVideoDialog::closeEvent(QCloseEvent* event) {
  if (!ui_.can_close()) {
    event->ignore();
    return;
  }
  QDialog::closeEvent(event);
}

void ExportVideoDialog::reject() {
  if (!ui_.can_close()) return;
  QDialog::reject();
}

void ExportVideoDialog::keyPressEvent(QKeyEvent* event) {
  if (!ui_.can_close() && (event->key() == Qt::Key_Escape)) {
    event->ignore();
    return;
  }
  QDialog::keyPressEvent(event);
}

}  // namespace wds::ui

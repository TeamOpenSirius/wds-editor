#include "wds/ui/qt/ffmpeg_resolve.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#if defined(Q_OS_MACOS)
#include <QProcess>
#endif

#include <algorithm>

namespace wds::ui {
namespace {

QString run_ffmpeg(const QString& bin, const QStringList& args, int timeout_ms = 8000) {
  QProcess proc;
  proc.setProgram(bin);
  proc.setArguments(args);
  proc.setProcessChannelMode(QProcess::MergedChannels);
  proc.start();
  if (!proc.waitForFinished(timeout_ms)) {
    proc.kill();
    proc.waitForFinished(1000);
    return {};
  }
  return QString::fromUtf8(proc.readAll());
}

bool looks_like_windows_store_stub(const QString& path) {
  const QString n = QDir::fromNativeSeparators(path).toLower();
  return n.contains(QLatin1String("/windowsapps/"));
}

bool has_token(const QString& text, const QString& token) {
  return text.contains(token, Qt::CaseInsensitive);
}

QString sha256_file(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!f.atEnd()) {
    hash.addData(f.read(1 << 16));
  }
  return QString::fromLatin1(hash.result().toHex());
}

bool chmod_exec(const QString& path) {
#if defined(_WIN32)
  (void)path;
  return true;
#else
  QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner | QFile::ExeUser |
                                  QFile::ExeGroup | QFile::ExeOther);
  return true;
#endif
}

void clear_quarantine(const QString& path) {
#if defined(Q_OS_MACOS)
  QProcess::execute(QStringLiteral("xattr"), {QStringLiteral("-cr"), path});
#else
  (void)path;
#endif
}

QString find_ffmpeg_in_dir(const QString& dir) {
  QDir root(dir);
  const QStringList names = {
#if defined(_WIN32)
      QStringLiteral("ffmpeg.exe")
#else
      QStringLiteral("ffmpeg")
#endif
  };
  const auto infos = root.entryInfoList(names, QDir::Files | QDir::Executable, QDir::Name);
  if (!infos.isEmpty()) return infos.front().absoluteFilePath();
  const auto all = root.entryInfoList(QDir::Files, QDir::Name);
  for (const auto& info : all) {
    if (info.fileName().startsWith(QLatin1String("ffmpeg"))) return info.absoluteFilePath();
  }
  const auto subs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const auto& sub : subs) {
    const QString nested = find_ffmpeg_in_dir(root.filePath(sub));
    if (!nested.isEmpty()) return nested;
  }
  return {};
}

}  // namespace

QString preview_renderer_dir() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/wds-editor/preview-renderer");
}

QString cached_ffmpeg_path() {
  const QDir dir(preview_renderer_dir());
#if defined(_WIN32)
  const QString win = dir.filePath(QStringLiteral("bin/ffmpeg.exe"));
  if (QFileInfo::exists(win)) return win;
#endif
  const QString unix_bin = dir.filePath(QStringLiteral("bin/ffmpeg"));
  if (QFileInfo::exists(unix_bin)) return unix_bin;
  return find_ffmpeg_in_dir(dir.absolutePath());
}

int parse_ffmpeg_major_version(const QString& version_text) {
  static const QRegularExpression re(QStringLiteral(R"(ffmpeg version (?:n)?(\d+)\.(\d+))"));
  const auto m = re.match(version_text);
  if (!m.hasMatch()) return 0;
  return m.captured(1).toInt();
}

QStringList parse_h264_encoders(const QString& encoders_text) {
  const QStringList names = {QStringLiteral("h264_videotoolbox"), QStringLiteral("h264_nvenc"),
                             QStringLiteral("h264_amf"),          QStringLiteral("h264_qsv"),
                             QStringLiteral("h264_mf"),           QStringLiteral("h264_vaapi"),
                             QStringLiteral("libx264"),           QStringLiteral("libopenh264")};
  QStringList found;
  for (const auto& name : names) {
    if (encoders_text.contains(name)) found.push_back(name);
  }
  return found;
}

bool ffmpeg_has_hardware_encoder(const FfmpegProbe& probe) {
  for (const auto& name : probe.h264_encoders) {
    if (name != QLatin1String("libx264") && name != QLatin1String("libopenh264")) return true;
  }
  return false;
}

bool ffmpeg_probe_qualified(const FfmpegProbe& probe) {
  return probe.major >= 6 && probe.has_mp4 && probe.has_aac && probe.has_rawvideo && probe.has_wav &&
         !probe.h264_encoders.isEmpty();
}

FfmpegProbe probe_ffmpeg_binary(const QString& path) {
  FfmpegProbe p;
  p.path = path;
  if (path.isEmpty() || !QFileInfo::exists(path) || looks_like_windows_store_stub(path)) {
    p.error = QStringLiteral("not found");
    return p;
  }
  p.found = true;
  const QString ver = run_ffmpeg(path, {QStringLiteral("-hide_banner"), QStringLiteral("-version")});
  p.major = parse_ffmpeg_major_version(ver);
  static const QRegularExpression minor_re(QStringLiteral(R"(ffmpeg version (?:n)?(\d+)\.(\d+))"));
  const auto mm = minor_re.match(ver);
  if (mm.hasMatch()) p.minor = mm.captured(2).toInt();
  // FFmpeg legal.html: --enable-nonfree makes the binary unredistributable.
  // Reject cached/PATH copies that still carry it (e.g. old eugeneware static).
  if (ver.contains(QLatin1String("--enable-nonfree"))) {
    p.error = QStringLiteral("ffmpeg enable-nonfree");
    return p;
  }
  const QString formats =
      run_ffmpeg(path, {QStringLiteral("-hide_banner"), QStringLiteral("-formats")});
  p.has_mp4 = has_token(formats, QStringLiteral(" mp4 "));
  p.has_rawvideo = has_token(formats, QStringLiteral("rawvideo"));
  p.has_wav = has_token(formats, QStringLiteral(" wav ")) || has_token(formats, QStringLiteral("s16le"));
  const QString codecs =
      run_ffmpeg(path, {QStringLiteral("-hide_banner"), QStringLiteral("-codecs")});
  p.has_aac = has_token(codecs, QStringLiteral(" aac "));
  const QString enc =
      run_ffmpeg(path, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
  p.h264_encoders = parse_h264_encoders(enc);
  p.hardware = ffmpeg_has_hardware_encoder(p);
  p.ok = ffmpeg_probe_qualified(p);
  if (!p.ok) p.error = QStringLiteral("unqualified ffmpeg");
  return p;
}

FfmpegProbe probe_ffmpeg_environment() {
  const auto try_path = [](const QString& raw) -> FfmpegProbe {
    if (raw.isEmpty()) return {};
    const QFileInfo info(raw);
    if (!info.exists() || !info.isFile()) return {};
    return probe_ffmpeg_binary(info.absoluteFilePath());
  };

  if (const auto env = qEnvironmentVariable("WDS_FFMPEG"); !env.isEmpty()) {
    auto p = try_path(env);
    if (p.ok) return p;
  }
  if (const auto env = qEnvironmentVariable("FFMPEG"); !env.isEmpty()) {
    auto p = try_path(env);
    if (p.ok) return p;
  }
  if (const auto env = qEnvironmentVariable("FFMPEG_BINARY"); !env.isEmpty()) {
    auto p = try_path(env);
    if (p.ok) return p;
  }
  const QString from_path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (!from_path.isEmpty() && !looks_like_windows_store_stub(from_path)) {
    auto p = probe_ffmpeg_binary(from_path);
    if (p.ok) return p;
  }
  const QString cached = cached_ffmpeg_path();
  if (!cached.isEmpty()) {
    auto p = probe_ffmpeg_binary(cached);
    if (p.ok) return p;
    return p;
  }
  FfmpegProbe miss;
  miss.error = QStringLiteral("ffmpeg not found");
  return miss;
}

FfmpegDownloadSpec ffmpeg_download_spec() {
  FfmpegDownloadSpec spec;
  spec.binary_name =
#if defined(_WIN32)
      QStringLiteral("ffmpeg.exe");
#else
      QStringLiteral("ffmpeg");
#endif
  // Win/Linux: BtbN autobuild-2026-07-23-14-16 (n8.1.2-30-g45f1910444) LGPL.
  // macOS: serversideup/ffmpeg-lgpl-builds v8.1.2-27 (disable-gpl, disable-nonfree,
  // VideoToolbox). Same sha256 on every URL of a platform. Prefer mirrors, then origin.
#if defined(_WIN32)
  spec.filename = QStringLiteral("ffmpeg-n8.1.2-30-g45f1910444-win64-lgpl-8.1.zip");
  spec.archive_kind = QStringLiteral("zip");
  spec.sha256 = QStringLiteral("0c6dc759eb1c70804ca04e11d83c583a6b03ae0630474f862e97400c715c6376");
  spec.urls = {
      QStringLiteral("https://cdn.npmmirror.com/binaries/ffmpeg-builds/ffmpeg-n8.1.2-30-g45f1910444-win64-lgpl-8.1.zip"),
      QStringLiteral("https://registry.npmmirror.com/-/binary/ffmpeg-builds/ffmpeg-n8.1.2-30-g45f1910444-win64-lgpl-8.1.zip"),
      QStringLiteral("https://ghfast.top/https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-23-14-16/ffmpeg-n8.1.2-30-g45f1910444-win64-lgpl-8.1.zip"),
      QStringLiteral("https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-23-14-16/ffmpeg-n8.1.2-30-g45f1910444-win64-lgpl-8.1.zip"),
  };
#elif defined(Q_OS_MACOS)
  // LGPL-only Apple Silicon build (no --enable-nonfree / --enable-gpl).
  // npmmirror does not host this artifact; use CN GitHub release proxies first.
  spec.filename = QStringLiteral("ffmpeg-8.1.2-aarch64-apple-darwin.tar.gz");
  spec.archive_kind = QStringLiteral("tar.gz");
  spec.sha256 = QStringLiteral("ac0babf65798bf681da1dc81c4fe4ae049d0d26e161410174b0e120e3e783332");
  spec.urls = {
      QStringLiteral(
          "https://gh-proxy.com/https://github.com/serversideup/ffmpeg-lgpl-builds/releases/"
          "download/v8.1.2-27/ffmpeg-8.1.2-aarch64-apple-darwin.tar.gz"),
      QStringLiteral(
          "https://ghfast.top/https://github.com/serversideup/ffmpeg-lgpl-builds/releases/download/"
          "v8.1.2-27/ffmpeg-8.1.2-aarch64-apple-darwin.tar.gz"),
      QStringLiteral(
          "https://github.com/serversideup/ffmpeg-lgpl-builds/releases/download/v8.1.2-27/"
          "ffmpeg-8.1.2-aarch64-apple-darwin.tar.gz"),
  };
#elif defined(__aarch64__) || defined(_M_ARM64)
  spec.filename = QStringLiteral("ffmpeg-n8.1.2-30-g45f1910444-linuxarm64-lgpl-8.1.tar.xz");
  spec.archive_kind = QStringLiteral("tar.xz");
  spec.sha256 = QStringLiteral("c9c7653babc8ae7191100fc570703fb6a4e7fe11a1696da6e94fbf3b71a5a4a6");
  spec.urls = {
      QStringLiteral("https://cdn.npmmirror.com/binaries/ffmpeg-builds/ffmpeg-n8.1.2-30-g45f1910444-linuxarm64-lgpl-8.1.tar.xz"),
      QStringLiteral("https://registry.npmmirror.com/-/binary/ffmpeg-builds/ffmpeg-n8.1.2-30-g45f1910444-linuxarm64-lgpl-8.1.tar.xz"),
      QStringLiteral("https://ghfast.top/https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-23-14-16/ffmpeg-n8.1.2-30-g45f1910444-linuxarm64-lgpl-8.1.tar.xz"),
      QStringLiteral("https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-23-14-16/ffmpeg-n8.1.2-30-g45f1910444-linuxarm64-lgpl-8.1.tar.xz"),
  };
#else
  spec.filename = QStringLiteral("ffmpeg-n8.1.2-30-g45f1910444-linux64-lgpl-8.1.tar.xz");
  spec.archive_kind = QStringLiteral("tar.xz");
  spec.sha256 = QStringLiteral("759149752aab56335d3234f82e118c6f7d441e5b635467ff8bff307729b02e6b");
  spec.urls = {
      QStringLiteral("https://cdn.npmmirror.com/binaries/ffmpeg-builds/ffmpeg-n8.1.2-30-g45f1910444-linux64-lgpl-8.1.tar.xz"),
      QStringLiteral("https://registry.npmmirror.com/-/binary/ffmpeg-builds/ffmpeg-n8.1.2-30-g45f1910444-linux64-lgpl-8.1.tar.xz"),
      QStringLiteral("https://ghfast.top/https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-23-14-16/ffmpeg-n8.1.2-30-g45f1910444-linux64-lgpl-8.1.tar.xz"),
      QStringLiteral("https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-23-14-16/ffmpeg-n8.1.2-30-g45f1910444-linux64-lgpl-8.1.tar.xz"),
  };
#endif
  return spec;
}

FfmpegDownloader::FfmpegDownloader(QObject* parent) : QObject(parent) {
  nam_ = new QNetworkAccessManager(this);
  spec_ = ffmpeg_download_spec();
}

FfmpegDownloader::~FfmpegDownloader() {
  on_progress = {};
  on_finished = {};
  abort();
}

void FfmpegDownloader::start() {
  aborting_ = false;
  url_index_ = 0;
  try_next();
}

void FfmpegDownloader::abort() {
  aborting_ = true;
  QNetworkReply* reply = reply_;
  reply_ = nullptr;
  if (reply != nullptr) {
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
  }
  buffer_.clear();
}

void FfmpegDownloader::try_next() {
  if (aborting_) return;
  if (url_index_ >= spec_.urls.size()) {
    finish_fail(QStringLiteral("环境下载失败"));
    return;
  }
  buffer_.clear();
  QNetworkRequest req{QUrl(spec_.urls[url_index_])};
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  ++url_index_;
  reply_ = nam_->get(req);
  QObject::connect(reply_, &QNetworkReply::downloadProgress, this,
                   [this](qint64 rec, qint64 tot) {
                     if (aborting_ || !on_progress) return;
                     on_progress(rec, tot);
                   });
  QObject::connect(reply_, &QNetworkReply::readyRead, this, [this] {
    if (aborting_ || reply_ == nullptr) return;
    buffer_.append(reply_->readAll());
  });
  QObject::connect(reply_, &QNetworkReply::finished, this, [this] {
    QNetworkReply* done = reply_;
    reply_ = nullptr;
    if (done == nullptr) return;
    buffer_.append(done->readAll());
    const bool net_ok = done->error() == QNetworkReply::NoError;
    done->deleteLater();
    if (aborting_) return;
    if (!net_ok || buffer_.isEmpty()) {
      try_next();
      return;
    }
    const QString dir = preview_renderer_dir();
    QDir().mkpath(dir);
    const QString archive = dir + QLatin1Char('/') + spec_.filename;
    QFile file(archive);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      finish_fail(QStringLiteral("环境下载失败"));
      return;
    }
    file.write(buffer_);
    file.close();
    buffer_.clear();
    if (!spec_.sha256.isEmpty()) {
      const QString got = sha256_file(archive);
      if (got.compare(spec_.sha256, Qt::CaseInsensitive) != 0) {
        QFile::remove(archive);
        try_next();
        return;
      }
    }
    QString err;
    if (!extract_archive(archive, spec_, &err)) {
      QFile::remove(archive);
      finish_fail(err.isEmpty() ? QStringLiteral("环境解压失败") : err);
      return;
    }
    QFile::remove(archive);
    const QString bin = cached_ffmpeg_path();
    if (bin.isEmpty()) {
      finish_fail(QStringLiteral("环境解压失败"));
      return;
    }
    chmod_exec(bin);
    clear_quarantine(bin);
    clear_quarantine(QFileInfo(bin).absolutePath());
    auto probe = probe_ffmpeg_binary(bin);
    if (!probe.ok) {
      finish_fail(probe.error.isEmpty() ? QStringLiteral("环境不合格") : probe.error);
      return;
    }
    finish_ok(bin);
  });
}

void FfmpegDownloader::finish_ok(const QString& path) {
  QJsonObject manifest;
  manifest.insert(QStringLiteral("path"), path);
  manifest.insert(QStringLiteral("filename"), spec_.filename);
  manifest.insert(QStringLiteral("sha256"), spec_.sha256);
  manifest.insert(QStringLiteral("archive"), spec_.archive_kind);
  manifest.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
  if (url_index_ > 0 && url_index_ <= spec_.urls.size()) {
    manifest.insert(QStringLiteral("url"), spec_.urls[url_index_ - 1]);
  }
  QFile file(preview_renderer_dir() + QStringLiteral("/manifest.json"));
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
  }
  if (on_finished) on_finished(true, {});
}

void FfmpegDownloader::finish_fail(const QString& message) {
  if (on_finished) on_finished(false, message);
}

bool FfmpegDownloader::extract_archive(const QString& archive, const FfmpegDownloadSpec& spec,
                                       QString* error) {
  const QString dest = preview_renderer_dir();
  QDir().mkpath(dest + QStringLiteral("/bin"));
  QProcess proc;
  proc.setWorkingDirectory(dest);
  if (spec.archive_kind == QLatin1String("gz")) {
    const QString out = dest + QStringLiteral("/bin/") + spec.binary_name;
    proc.setProgram(QStringLiteral("gzip"));
    proc.setArguments({QStringLiteral("-dc"), archive});
    proc.setStandardOutputFile(out);
    proc.start();
    if (!proc.waitForFinished(120000) || proc.exitCode() != 0) {
      if (error) *error = QStringLiteral("解压失败");
      return false;
    }
    chmod_exec(out);
    return true;
  }
  proc.setProgram(QStringLiteral("tar"));
  QStringList args;
  if (spec.archive_kind == QLatin1String("tar.xz")) {
    args << QStringLiteral("-xJf") << archive << QStringLiteral("-C") << dest;
  } else {
    args << QStringLiteral("-xf") << archive << QStringLiteral("-C") << dest;
  }
  proc.setArguments(args);
  proc.start();
  if (!proc.waitForFinished(180000) || proc.exitCode() != 0) {
    if (error) *error = QStringLiteral("解压失败");
    return false;
  }
  const QString found = find_ffmpeg_in_dir(dest);
  if (found.isEmpty()) {
    if (error) *error = QStringLiteral("解压失败");
    return false;
  }
  const QString dest_bin = dest + QStringLiteral("/bin/") + spec.binary_name;
  if (QDir::fromNativeSeparators(found) != QDir::fromNativeSeparators(dest_bin)) {
    QDir().mkpath(QFileInfo(dest_bin).absolutePath());
    QFile::remove(dest_bin);
    QFile::copy(found, dest_bin);
  }
  chmod_exec(dest_bin);
  return QFileInfo::exists(dest_bin);
}

}  // namespace wds::ui

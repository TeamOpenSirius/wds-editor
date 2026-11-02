#include "wds/ui/qt/file_open_router.hpp"

#include "wds/ui/project_open_args.hpp"

#include <QFileInfo>
#include <QFileOpenEvent>

namespace wds::ui {
namespace {

// Only real WDS documents are routed: LaunchServices can hand us any document
// (and a folder, when an app bundle is dropped on ours), and a stale recent
// path must not make the editor open a nonexistent file. Charts use the same
// direct-open path as projects.
bool is_document_file(const QString& path) {
  if (path.isEmpty()) {
    return false;
  }
  const QFileInfo info(path);
  return info.isFile() && is_wds_document_path(info.filePath().toStdString());
}

}  // namespace

FileOpenRouter::FileOpenRouter(QObject* parent) : QObject(parent) {}

void FileOpenRouter::set_handler(std::function<void(const QString&)> handler) {
  handler_ = std::move(handler);
  if (!handler_) {
    return;
  }
  const QStringList queued = take_pending_paths();
  for (const QString& path : queued) {
    handler_(path);
  }
}

QString FileOpenRouter::take_pending_path() {
  if (pending_.isEmpty()) {
    return {};
  }
  return pending_.takeFirst();
}

QStringList FileOpenRouter::take_pending_paths() {
  QStringList paths = pending_;
  pending_.clear();
  return paths;
}

void FileOpenRouter::enqueue(const QString& path) {
  const QFileInfo info(path);
  const QString resolved = info.exists() ? info.canonicalFilePath() : path;
  if (resolved.isEmpty() || pending_.contains(resolved)) {
    return;
  }
  if (handler_) {
    handler_(resolved);
    return;
  }
  pending_.append(resolved);
}

bool FileOpenRouter::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::FileOpen) {
    const auto* open = static_cast<QFileOpenEvent*>(event);
    const QString path = open->file();
    if (is_document_file(path)) {
      enqueue(path);
    }
    // Not consumed: Qt's own handler does not act on WDS documents, and other
    // document types must keep whatever default behaviour they have.
    return false;
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace wds::ui

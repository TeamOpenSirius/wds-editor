#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace wds::ui {

// Routes document-open requests that arrive outside argv. macOS LaunchServices
// sends a QFileOpenEvent when a .wdsproject or .wdschart is double-clicked —
// both when the editor is starting and while it already runs. Qt delivers that
// event to the QApplication object, so a filter installed on the app sees it
// first.
//
// Paths that arrive before the editor is ready are queued: the startup code
// drains them, so a double-clicked document opens directly instead of showing
// the startup chooser. Once a handler is installed, later requests open live in
// the running window.
class FileOpenRouter final : public QObject {
 public:
  explicit FileOpenRouter(QObject* parent = nullptr);

  // Hand requests to `handler` from now on; queued paths are delivered first.
  void set_handler(std::function<void(const QString&)> handler);
  // Oldest queued path (removed), or an empty string.
  QString take_pending_path();
  QStringList take_pending_paths();
  bool has_pending() const { return !pending_.isEmpty(); }

  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void enqueue(const QString& path);
  QStringList pending_;
  std::function<void(const QString&)> handler_;
};

}  // namespace wds::ui

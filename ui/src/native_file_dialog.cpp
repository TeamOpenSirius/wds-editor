#include "wds/ui/native_file_dialog.hpp"

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <string>

namespace wds::ui::native_file_dialog {
namespace {

QWidget* owner_widget() { return QApplication::activeWindow(); }

QString filter_string(const Filters& filters) {
  if (filters.empty()) return QStringLiteral("All files (*)");
  QStringList patterns;
  for (const auto& raw : filters) {
    std::string ext = raw;
    while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    if (!ext.empty()) patterns.push_back(QStringLiteral("*.") + QString::fromStdString(ext));
  }
  if (patterns.isEmpty()) return QStringLiteral("All files (*)");
  return QString::fromStdString(filters.front()) + QStringLiteral(" files (") + patterns.join(' ') +
         QStringLiteral(");;All files (*)");
}

std::string to_lower_ascii(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::string strip_dot(std::string ext) {
  while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
  return ext;
}

bool ends_with_ci(const std::string& value, const std::string& suffix) {
  if (suffix.size() > value.size()) return false;
  return to_lower_ascii(value.substr(value.size() - suffix.size())) == to_lower_ascii(suffix);
}

bool path_has_allowed_extension(const std::string& path, const Filters& filters) {
  for (const auto& raw : filters) {
    const std::string ext = strip_dot(raw);
    if (ext.empty()) continue;
    if (ends_with_ci(path, "." + ext)) return true;
  }
  return false;
}

// If path lacks an allowed extension (or has a wrong one), force the preferred suffix.
std::string ensure_extension(std::string path, const Filters& filters) {
  if (path.empty() || filters.empty()) return path;
  if (path_has_allowed_extension(path, filters)) return path;

  const auto slash = path.find_last_of("/\\");
  const std::size_t base_start = slash == std::string::npos ? 0 : slash + 1;
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos && dot > base_start) {
    path.resize(dot);
  }
  path += '.';
  path += strip_dot(filters.front());
  return path;
}

}  // namespace

void set_owner_window(void*) {}

std::optional<std::string> open_file(const std::string& title, const Filters& filters) {
  const QString path = QFileDialog::getOpenFileName(owner_widget(), QString::fromStdString(title),
                                                     {}, filter_string(filters));
  return path.isEmpty() ? std::nullopt : std::optional<std::string>(path.toStdString());
}

std::optional<std::string> save_file(const std::string& title, const std::string& default_name,
                                     const Filters& filters) {
  const QString path = QFileDialog::getSaveFileName(owner_widget(), QString::fromStdString(title),
                                                     QString::fromStdString(default_name),
                                                     filter_string(filters));
  if (path.isEmpty()) return std::nullopt;
  return ensure_extension(path.toStdString(), filters);
}

std::vector<std::string> open_files(const std::string& title, const Filters& filters) {
  auto line = open_file(title, filters);
  return line ? std::vector<std::string>{*line} : std::vector<std::string>{};
}

std::optional<std::string> choose_directory(const std::string& title) {
  const QString path = QFileDialog::getExistingDirectory(owner_widget(), QString::fromStdString(title));
  return path.isEmpty() ? std::nullopt : std::optional<std::string>(path.toStdString());
}

bool confirm(const std::string& title, const std::string& message) {
  return QMessageBox::question(owner_widget(), QString::fromStdString(title),
                               QString::fromStdString(message), QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No) == QMessageBox::Yes;
}

void alert_error(const std::string& title, const std::string& message) {
  QMessageBox::critical(owner_widget(), QString::fromStdString(title), QString::fromStdString(message));
}

SaveDiscardCancel confirm_save_discard_cancel(const std::string& title, const std::string& message) {
  QMessageBox box(QMessageBox::Question, QString::fromStdString(title), QString::fromStdString(message),
                  QMessageBox::NoButton, owner_widget());
  auto* save = box.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
  auto* discard = box.addButton(QStringLiteral("不保存"), QMessageBox::DestructiveRole);
  auto* cancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
  box.setDefaultButton(qobject_cast<QPushButton*>(save));
  box.exec();
  if (box.clickedButton() == save) return SaveDiscardCancel::Save;
  if (box.clickedButton() == discard) return SaveDiscardCancel::Discard;
  (void)cancel;
  return SaveDiscardCancel::Cancel;
}

}  // namespace wds::ui::native_file_dialog

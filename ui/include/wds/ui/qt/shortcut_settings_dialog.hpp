#pragma once

#include <QDialog>
#include <array>

#include <wds/interaction/editor_shortcuts.hpp>

class QEvent;
class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QShowEvent;

namespace wds::ui {

class UiManager;

// Shortcut chords, grouped by role. Each group is two columns. Nothing is written until
// 应用 or 确定; closing the window discards unapplied edits.
class ShortcutSettingsDialog final : public QDialog {
 public:
  explicit ShortcutSettingsDialog(UiManager* manager, QWidget* parent = nullptr);

 protected:
  void showEvent(QShowEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  void build_ui(bool pause_at_current);
  void load_from_manager();
  int refresh_shortcut_conflicts();
  bool apply_changes();

  UiManager* manager_ = nullptr;
  bool loading_ = false;
  bool focused_exit_ = false;
  QPushButton* exit_button_ = nullptr;
  QLabel* status_ = nullptr;
  std::array<QKeySequenceEdit*, wds::interaction::kEditorShortcutCount> edits_{};
};

}  // namespace wds::ui

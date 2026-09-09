#pragma once

#include <QDialog>
#include <array>

#include "wds/ui/editor_ui_config.hpp"

#include <wds/interaction/editor_shortcuts.hpp>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QKeySequenceEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;

namespace wds::ui {

class UiManager;

// Full editor settings, matching the old self-drawn modal: a sidebar with
// 文件 / 音频 / 输入 / 显示 / 宽度 / 快捷键 / 隐私 pages plus 确认 / 取消.
class SettingsDialog final : public QDialog {
 public:
  explicit SettingsDialog(UiManager* manager, QWidget* parent = nullptr);

 private:
  void build_pages();
  void load_from_config();
  bool capture_into_config();
  // Returns the first duplicated shortcut row, or -1. Highlights conflicts.
  int refresh_shortcut_conflicts();
  void try_confirm();

  UiManager* manager_ = nullptr;
  EditorUiConfig cfg_{};

  QListWidget* sidebar_ = nullptr;
  QStackedWidget* pages_ = nullptr;

  QCheckBox* sus_auto_convert_ = nullptr;
  QCheckBox* mute_hold_body_sfx_ = nullptr;
  QCheckBox* invert_scroll_wheel_ = nullptr;
  QCheckBox* invert_visible_range_scroll_ = nullptr;
  QComboBox* scroll_wheel_speed_ = nullptr;
  QCheckBox* show_judgment_text_ = nullptr;
  QDoubleSpinBox* note_speed_ = nullptr;
  QSpinBox* note_start_offset_ = nullptr;
  QSpinBox* note_height_level_ = nullptr;
  QSpinBox* split_line_opacity_ = nullptr;
  QComboBox* spectrum_display_ = nullptr;
  QComboBox* msaa_samples_ = nullptr;
  std::array<QSpinBox*, 6> width_slots_{};
  std::array<QKeySequenceEdit*, wds::interaction::kEditorShortcutCount> shortcut_edits_{};
  QCheckBox* allow_crash_log_sensitive_ = nullptr;
};

}  // namespace wds::ui

#pragma once

#include <QDialog>
#include <QWidget>
#include <QString>
#include <array>
#include <functional>

#include "wds/ui/editor_ui_config.hpp"

#include <wds/interaction/editor_shortcuts.hpp>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QKeySequenceEdit;
class QResizeEvent;
class QScrollArea;
class QSpinBox;

namespace wds::ui {

class UiManager;

// Always-on settings form: one scroll page + jump rail. Changes apply immediately.
class SettingsPanel final : public QWidget {
 public:
  SettingsPanel(UiManager* manager, QString theme_dir, QWidget* parent = nullptr);
  void set_on_applied(std::function<void()> handler) { on_applied_ = std::move(handler); }

 private:
  void build_pages();
  void load_from_config();
  bool capture_into_config();
  int refresh_shortcut_conflicts();
  void apply_live();
  void layout_nav_rail();
  void jump_to_section(int row);
  void sync_nav_from_scroll();
  void update_section_scroll_pad();

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  UiManager* manager_ = nullptr;
  QString theme_dir_;
  EditorUiConfig cfg_{};
  bool applying_ = false;
  bool jumping_section_ = false;
  std::function<void()> on_applied_;

  QWidget* sidebar_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  std::array<QWidget*, 9> sections_{};
  QWidget* scroll_pad_ = nullptr;
  QComboBox* theme_combo_ = nullptr;

  QCheckBox* sus_auto_convert_ = nullptr;
  QCheckBox* mute_hold_body_sfx_ = nullptr;
  QCheckBox* invert_scroll_wheel_ = nullptr;
  QCheckBox* invert_visible_range_scroll_ = nullptr;
  QCheckBox* new_note_place_logic_ = nullptr;
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
  QCheckBox* auto_check_updates_ = nullptr;
};

// Thin Close wrapper around SettingsPanel (startup splash).
class SettingsDialog final : public QDialog {
 public:
  explicit SettingsDialog(UiManager* manager, QString theme_dir, QWidget* parent = nullptr);
};

}  // namespace wds::ui

#pragma once

#include <QWidget>
#include <QIcon>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "wds/ui/toolbar_curve_selection.hpp"

class QCheckBox;
class QComboBox;
class QEvent;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;

namespace wds::ui {

class UiManager;

// Playback dock: seek, transport, music/SFX mix, and playback rate.
class PlaybackBar final : public QWidget {
 public:
  explicit PlaybackBar(UiManager* manager, QWidget* parent = nullptr);
  void sync_position();
  void refresh_settings_widgets();

 private:
  void build_ui();
  // User-driven scrub only. Do not call from sync_position() / setValue —
  // that feedback loop was the pause-to-start forward-creep bug.
  void seek_to_slider(int value);
  void apply_play_icon(bool playing);

  UiManager* manager_ = nullptr;

  QSlider* seek_ = nullptr;
  QPushButton* play_ = nullptr;
  QPushButton* pause_here_ = nullptr;
  QPushButton* play_from_start_ = nullptr;
  QIcon play_icon_;
  QIcon pause_icon_;
  QComboBox* music_volume_ = nullptr;
  QCheckBox* music_mute_ = nullptr;
  QComboBox* sfx_volume_ = nullptr;
  QCheckBox* sfx_mute_ = nullptr;
  QComboBox* rate_ = nullptr;
  bool syncing_ = false;
  std::optional<int64_t> last_scrub_ms_;
};

// Convert dock: eight note-type buttons in a single row.
class ConvertBar final : public QWidget {
 public:
  ConvertBar(UiManager* manager, const std::string& skins_dir, QWidget* parent = nullptr);
  void sync_place_checks();
  void set_ribbon_mode();

 private:
  void build_ui(const std::string& skins_dir);

  UiManager* manager_ = nullptr;
  std::array<QToolButton*, 8> convert_buttons_{};
};

// Toolbar dock: chart/grid/curve/behavior controls.
class EditorToolbarWidget final : public QWidget {
 public:
  explicit EditorToolbarWidget(UiManager* manager, QWidget* parent = nullptr);

  void set_add_chart_handler(std::function<void()> handler) {
    on_add_chart_ = std::move(handler);
  }
  void refresh_curve_controls();
  void refresh_grid_fields();
  void refresh_offset_field();
  void refresh_chart_selector();
  void refresh_enabled_states();
  void refresh_flags();

 protected:
  void changeEvent(QEvent* event) override;

 private:
  void build_ui();
  void apply_add_chart_icon();
  void apply_delay();
  void apply_grid();

  UiManager* manager_ = nullptr;
  std::function<void()> on_add_chart_;
  class CurveFillWidget* curve_fill_widget_ = nullptr;

  QSpinBox* delay_ms_ = nullptr;
  QSpinBox* visible_range_ = nullptr;
  QSpinBox* subdivisions_ = nullptr;
  QComboBox* chart_select_ = nullptr;
  QPushButton* chart_add_ = nullptr;
  QCheckBox* pause_at_current_ = nullptr;
  QCheckBox* split_width_follow_ = nullptr;
  bool syncing_ = false;
};

// Curve fill group (template dropdown + I/O/IO/OI).
class CurveFillWidget final : public QWidget {
 public:
  explicit CurveFillWidget(UiManager* manager, QWidget* parent = nullptr);
  void refresh_curve_controls();
  void set_control_height(int height);
  QComboBox* combo() const { return curve_template_; }
  QWidget* buttons() const { return buttons_; }

 private:
  UiManager* manager_ = nullptr;
  CurveToolbarController curve_controller_;
  QWidget* buttons_ = nullptr;
  QComboBox* curve_template_ = nullptr;
  std::array<QToolButton*, 4> curve_directions_{};
};

}  // namespace wds::ui

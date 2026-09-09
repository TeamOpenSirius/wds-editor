#pragma once

#include <QWidget>
#include <array>
#include <functional>

#include "wds/ui/toolbar_curve_selection.hpp"

class QCheckBox;
class QComboBox;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QToolButton;

namespace wds::ui {

class UiManager;

// Dock content mirroring the old below-preview settings row plus the old
// toolbar's playback-adjacent controls: seek + play controls, music/SFX
// volume + mute, playback rate, chart delay, visible range, beat
// subdivisions, chart selection, the two edit checkboxes, and curve fill
// (template dropdown + I/O/IO/OI direction).
class PlaybackAudioPanel final : public QWidget {
 public:
  explicit PlaybackAudioPanel(UiManager* manager, QWidget* parent = nullptr);

  void set_add_chart_handler(std::function<void()> handler) {
    on_add_chart_ = std::move(handler);
  }
  // Re-read curve templates after the curve-templates dialog confirms.
  void refresh_curve_controls();

 private:
  void build_ui();
  void sync_from_runtime();
  void apply_delay();
  void apply_grid();

  UiManager* manager_ = nullptr;
  QTimer* sync_timer_ = nullptr;
  std::function<void()> on_add_chart_;
  CurveToolbarController curve_controller_;

  QSlider* seek_ = nullptr;
  QPushButton* play_ = nullptr;
  QPushButton* stop_ = nullptr;
  QComboBox* music_volume_ = nullptr;
  QCheckBox* music_mute_ = nullptr;
  QComboBox* sfx_volume_ = nullptr;
  QCheckBox* sfx_mute_ = nullptr;
  QComboBox* rate_ = nullptr;
  QSpinBox* delay_ms_ = nullptr;
  QComboBox* visible_range_ = nullptr;
  QComboBox* subdivisions_ = nullptr;
  QComboBox* chart_select_ = nullptr;
  QPushButton* chart_add_ = nullptr;
  QCheckBox* pause_at_current_ = nullptr;
  QCheckBox* split_width_follow_ = nullptr;
  QComboBox* curve_template_ = nullptr;
  std::array<QToolButton*, 4> curve_directions_{};
  bool syncing_ = false;
};

}  // namespace wds::ui

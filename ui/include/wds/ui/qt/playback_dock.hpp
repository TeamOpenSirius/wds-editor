#pragma once

#include <QWidget>
#include <array>
#include <functional>

#include "wds/ui/toolbar_curve_selection.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QToolButton;

namespace wds::ui {

class UiManager;

// 播放 dock: seek + transport, playback rate, chart delay, visible range,
// beat subdivisions, chart selection, and the two edit behavior checkboxes.
// Controls live in a flow layout so the dock works at any size / orientation.
class PlaybackAudioPanel final : public QWidget {
 public:
  explicit PlaybackAudioPanel(UiManager* manager, QWidget* parent = nullptr);

  void set_add_chart_handler(std::function<void()> handler) {
    on_add_chart_ = std::move(handler);
  }

 private:
  void build_ui();
  void sync_from_runtime();
  void apply_delay();
  void apply_grid();
  void seek_to_slider(int value);

  UiManager* manager_ = nullptr;
  QTimer* sync_timer_ = nullptr;
  std::function<void()> on_add_chart_;

  QSlider* seek_ = nullptr;
  QPushButton* play_ = nullptr;
  QPushButton* stop_ = nullptr;
  QComboBox* rate_ = nullptr;
  QSpinBox* delay_ms_ = nullptr;
  QComboBox* visible_range_ = nullptr;
  QComboBox* subdivisions_ = nullptr;
  QComboBox* chart_select_ = nullptr;
  QPushButton* chart_add_ = nullptr;
  QCheckBox* pause_at_current_ = nullptr;
  QCheckBox* split_width_follow_ = nullptr;
  bool syncing_ = false;
};

// 音频 dock: music / SFX volume and mute.
class AudioMixPanel final : public QWidget {
 public:
  explicit AudioMixPanel(UiManager* manager, QWidget* parent = nullptr);

 private:
  void sync_from_runtime();

  UiManager* manager_ = nullptr;
  QTimer* sync_timer_ = nullptr;
  QSlider* music_volume_ = nullptr;
  QLabel* music_value_ = nullptr;
  QCheckBox* music_mute_ = nullptr;
  QSlider* sfx_volume_ = nullptr;
  QLabel* sfx_value_ = nullptr;
  QCheckBox* sfx_mute_ = nullptr;
  bool syncing_ = false;
};

// 曲线填充 group (template dropdown + I/O/IO/OI), embedded in the 编辑工具箱.
class CurveFillWidget final : public QWidget {
 public:
  explicit CurveFillWidget(UiManager* manager, QWidget* parent = nullptr);
  // Re-read curve templates after the curve-templates dialog confirms.
  void refresh_curve_controls();

 private:
  UiManager* manager_ = nullptr;
  CurveToolbarController curve_controller_;
  QComboBox* curve_template_ = nullptr;
  std::array<QToolButton*, 4> curve_directions_{};
};

}  // namespace wds::ui

#pragma once

#include "wds/ui/editor_ui_config.hpp"

#include <wds/interaction/widget.hpp>

#include <functional>

namespace wds::ui {
class ChartPreviewPanel;

// Compact playback controls kept below the preview. It intentionally operates
// directly on Transport so changes are audible while playback continues.
class PreviewSettingsPanel final : public wds::interaction::Widget {
 public:
  explicit PreviewSettingsPanel(ChartPreviewPanel& preview);
  void layout(const wds::interaction::Rect& parent_bounds) override;
  void paint(wds::interaction::UiPainter& painter) const override;
  void update(float delta_seconds) override;

  void apply_config(const EditorUiConfig& cfg);
  void capture_config(EditorUiConfig& cfg) const;
  void set_persist_handler(std::function<void()> handler) { on_persist_ = std::move(handler); }

 private:
  void apply_music_gain();
  void apply_sfx_gain();
  void apply_playback_rate();
  void sync_from_state() const;
  void notify_persist() const;

  ChartPreviewPanel& preview_;
  double speed_ = 5.0;
  float music_gain_ = 1.0f;
  float sfx_gain_ = 1.0f;
  float playback_rate_ = 1.0f;
  bool music_muted_ = false;
  bool sfx_muted_ = false;
  std::function<void()> on_persist_;

  wds::interaction::Widget* speed_minus_ = nullptr;
  wds::interaction::Widget* speed_combo_ = nullptr;
  wds::interaction::Widget* speed_plus_ = nullptr;
  wds::interaction::Widget* seek_slider_ = nullptr;
  wds::interaction::Widget* music_combo_ = nullptr;
  wds::interaction::Widget* music_mute_ = nullptr;
  wds::interaction::Widget* sfx_combo_ = nullptr;
  wds::interaction::Widget* sfx_mute_ = nullptr;
  wds::interaction::Widget* rate_combo_ = nullptr;
};
}  // namespace wds::ui

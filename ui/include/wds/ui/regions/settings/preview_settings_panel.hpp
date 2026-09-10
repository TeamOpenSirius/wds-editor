#pragma once

#include "wds/ui/editor_ui_config.hpp"

#include <wds/interaction/widget.hpp>

#include <cstdint>
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

  // Preset playback rate (e.g. F1–F4 → 0.25x–1x). Updates transport + rate combo.
  void set_playback_rate(float rate);
  // Toggle SFX mute (default hotkey X). Persists via on_persist_.
  void toggle_sfx_mute();

  // Qt playback dock bridge: this panel stays the single owner of the audio
  // prefs so config capture/persist keeps working while Qt drives the values.
  float music_gain() const noexcept { return music_gain_; }
  bool music_muted() const noexcept { return music_muted_; }
  float sfx_gain() const noexcept { return sfx_gain_; }
  bool sfx_muted() const noexcept { return sfx_muted_; }
  float playback_rate() const noexcept { return playback_rate_; }
  void set_music_state_from_qt(float gain, bool muted);
  void set_sfx_state_from_qt(float gain, bool muted);
  void set_playback_rate_from_qt(float rate);
  // Inclusive start / exclusive-ish end of the seek slider in music ms.
  void seek_window_ms(int64_t& start_ms, int64_t& end_ms) const;

 private:
  void apply_music_gain();
  void apply_sfx_gain();
  void apply_playback_rate();
  void sync_from_state() const;
  void notify_persist() const;
  int64_t fallback_chart_duration_ms() const;

  ChartPreviewPanel& preview_;
  float music_gain_ = 1.0f;
  float sfx_gain_ = 1.0f;
  float playback_rate_ = 1.0f;
  bool music_muted_ = false;
  bool sfx_muted_ = false;
  std::function<void()> on_persist_;

  wds::interaction::Widget* seek_slider_ = nullptr;
  wds::interaction::Widget* music_combo_ = nullptr;
  wds::interaction::Widget* music_mute_ = nullptr;
  wds::interaction::Widget* sfx_combo_ = nullptr;
  wds::interaction::Widget* sfx_mute_ = nullptr;
  wds::interaction::Widget* rate_combo_ = nullptr;

  mutable uint64_t cached_span_revision_ = ~uint64_t{0};
  mutable int64_t cached_chart_span_ms_ = 1;
};
}  // namespace wds::ui

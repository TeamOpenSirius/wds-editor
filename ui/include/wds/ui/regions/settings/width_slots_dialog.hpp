#pragma once

#include "wds/ui/editor_ui_config.hpp"

#include <wds/interaction/editor_shortcuts.hpp>
#include <wds/interaction/widget.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>

namespace wds::ui {

// Settings modal with left tabs: 文件 / 音频 / 输入 / 快捷键宽 / 快捷键设置.
class WidthSlotsDialog final : public wds::interaction::Widget {
 public:
  WidthSlotsDialog();

  bool is_open() const noexcept { return open_; }
  void open();
  void close();

  void set_config(const EditorUiConfig& cfg);
  void capture_config(EditorUiConfig& cfg) const;

  // Invoked after settings are committed (persist EditorUiConfig + apply runtime).
  void set_on_applied(std::function<void()> cb) { on_applied_ = std::move(cb); }

  void layout(const wds::interaction::Rect& parent_bounds) override;
  void paint(wds::interaction::UiPainter& painter) const override;
  // Skip default popup walk — speed combo is painted in paint_dropdown() (chrome pass).
  void paint_popup_layers(wds::interaction::UiPainter& painter) const override;
  void paint_modal(wds::interaction::UiPainter& painter) const;
  // Open scroll-speed dropdown; must be flushed in a later draw pass than paint_modal.
  void paint_dropdown(wds::interaction::UiPainter& painter) const;
  Widget* hit_test(wds::interaction::Vec2 point) override;
  void on_click(const wds::interaction::ClickEvent& event) override;
  void on_scroll(const wds::interaction::ScrollEvent& event) override;

 private:
  enum class Tab { File, Audio, Input, Width, Shortcuts };

  void sync_fields_from_state();
  void apply_fields();
  void try_confirm();
  void layout_content(const wds::interaction::Rect& host);
  void set_tab(Tab tab);
  void update_tab_visibility();
  void clamp_shortcut_scroll();
  void refresh_shortcut_conflict_highlights();
  std::optional<std::size_t> first_shortcut_conflict_index() const;
  void ensure_shortcut_row_visible(std::size_t index);

  bool open_ = false;
  Tab tab_ = Tab::File;
  // Snapshot of toolbar "停止播放后停在当前时间" for pause-shortcut labels.
  bool pause_at_current_ = false;
  std::function<void()> on_applied_;
  wds::interaction::Rect content_bounds_{};
  wds::interaction::Rect tab_file_bounds_{};
  wds::interaction::Rect tab_audio_bounds_{};
  wds::interaction::Rect tab_input_bounds_{};
  wds::interaction::Rect tab_width_bounds_{};
  wds::interaction::Rect tab_shortcuts_bounds_{};
  wds::interaction::Rect shortcut_list_bounds_{};
  float shortcut_scroll_ = 0.0f;

  // Width tab: 一档…六档 fields (2 columns × 3 rows).
  std::array<wds::interaction::Widget*, 6> fields_{};
  // Shortcut tab: one ShortcutField + clear (×) button per EditorShortcut.
  std::array<wds::interaction::Widget*, wds::interaction::kEditorShortcutCount> shortcut_fields_{};
  std::array<wds::interaction::Widget*, wds::interaction::kEditorShortcutCount> shortcut_clear_buttons_{};
  // File / Audio / Input options.
  wds::interaction::Widget* sus_auto_convert_ = nullptr;
  wds::interaction::Widget* mute_hold_body_sfx_ = nullptr;
  wds::interaction::Widget* invert_scroll_wheel_ = nullptr;
  wds::interaction::Widget* invert_visible_range_scroll_ = nullptr;
  wds::interaction::Widget* scroll_wheel_speed_ = nullptr;

  wds::interaction::Widget* confirm_button_ = nullptr;
  wds::interaction::Widget* cancel_button_ = nullptr;
};

}  // namespace wds::ui

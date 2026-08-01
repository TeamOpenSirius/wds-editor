#pragma once

#include "wds/ui/editor_ui_config.hpp"

#include <wds/interaction/widget.hpp>

#include <array>
#include <functional>

namespace wds::ui {

// Settings modal with left tabs: 文件 / 音频 / 输入 / 快捷键宽.
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

 private:
  enum class Tab { File, Audio, Input, Width };

  void sync_fields_from_state();
  void apply_fields();
  void layout_content(const wds::interaction::Rect& host);
  void set_tab(Tab tab);
  void update_tab_visibility();

  bool open_ = false;
  Tab tab_ = Tab::File;
  std::function<void()> on_applied_;
  wds::interaction::Rect content_bounds_{};
  wds::interaction::Rect tab_file_bounds_{};
  wds::interaction::Rect tab_audio_bounds_{};
  wds::interaction::Rect tab_input_bounds_{};
  wds::interaction::Rect tab_width_bounds_{};

  // Width tab: Q/W/E/A/S/D fields (2 columns × 3 rows).
  std::array<wds::interaction::Widget*, 6> fields_{};
  // File / Audio / Input options.
  wds::interaction::Widget* sus_auto_convert_ = nullptr;
  wds::interaction::Widget* mute_hold_body_sfx_ = nullptr;
  wds::interaction::Widget* invert_scroll_wheel_ = nullptr;
  wds::interaction::Widget* scroll_wheel_speed_ = nullptr;

  wds::interaction::Widget* confirm_button_ = nullptr;
  wds::interaction::Widget* cancel_button_ = nullptr;
};

}  // namespace wds::ui

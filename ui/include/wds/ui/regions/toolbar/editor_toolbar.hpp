#pragma once

#include "wds/ui/editor_ui_config.hpp"

#include <wds/interaction/widget.hpp>
#include <wds/renderer/skin_catalog.hpp>
#include <wds/renderer/vulkan_renderer.hpp>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace wds::ui {

class EditorSession;
class ChartEditPanel;

class EditorToolbar final : public wds::interaction::Widget {
 public:
  EditorToolbar(EditorSession& session, ChartEditPanel& edit);
  ~EditorToolbar() override;

  void set_skin(const wds::renderer::SkinCatalog* skin);
  // target_svg_px > 0: raster at that physical size. 0: estimate from framebuffer/left column.
  void load_action_icons(wds::renderer::VulkanRenderer& vulkan, const std::string& icons_directory,
                         int target_svg_px = 0);
  void release_gpu_resources();
  void layout(const wds::interaction::Rect& parent_bounds) override;
  void paint(wds::interaction::UiPainter& painter) const override;

  void set_settings_handler(std::function<void()> handler) { on_settings_ = std::move(handler); }
  void set_export_handler(std::function<void()> handler) { on_export_ = std::move(handler); }
  void set_persist_handler(std::function<void()> handler) { on_persist_ = std::move(handler); }
  void set_open_handler(std::function<void()> handler) { on_open_ = std::move(handler); }
  void set_import_handler(std::function<void()> handler) { on_import_ = std::move(handler); }
  void set_chart_add_handler(std::function<void()> handler) { on_chart_add_ = std::move(handler); }

  void apply_config(const EditorUiConfig& cfg);
  void capture_config(EditorUiConfig& cfg) const;
  // Refresh toolbar combo after edit-panel Shift+wheel changes visible range.
  void sync_visible_range_field() const { sync_numeric_fields(); }

 private:
  enum class Action {
    Open,
    Save,
    Import,
    Export,
    Settings,
    Music,
    Undo,
    Redo,
    ConvertTap,
    ConvertCritical,
    ConvertHoldStart,
    ConvertHold,
    ConvertFlick,
    ConvertFlickLeft,
    ConvertFlickRight,
    ConvertScratchHold,
  };

  void run(Action action);
  void apply_convert_skins();
  void destroy_owned_icons();
  void sync_numeric_fields() const;
  void sync_checkboxes() const;
  void notify_persist() const;
  // Re-rasterize SVGs when content scale or toolbar width needs a larger texture.
  void ensure_action_icons_resolution(float left_w);

  EditorSession& session_;
  ChartEditPanel& edit_;
  const wds::renderer::SkinCatalog* skin_ = nullptr;
  wds::renderer::VulkanRenderer* vulkan_ = nullptr;
  std::string icons_directory_;
  int icons_raster_px_ = 0;
  float icons_content_scale_ = 0.0f;
  std::vector<wds::renderer::TextureId> owned_icon_ids_;
  std::array<wds::interaction::Widget*, 8> action_buttons_{};
  std::array<wds::interaction::Widget*, 8> convert_buttons_{};
  wds::interaction::Widget* delay_field_ = nullptr;
  wds::interaction::Widget* chart_dropdown_ = nullptr;
  wds::interaction::Widget* chart_add_button_ = nullptr;
  wds::interaction::Widget* tick_minus_ = nullptr;
  wds::interaction::Widget* tick_combo_ = nullptr;
  wds::interaction::Widget* tick_plus_ = nullptr;
  wds::interaction::Widget* division_minus_ = nullptr;
  wds::interaction::Widget* division_combo_ = nullptr;
  wds::interaction::Widget* division_plus_ = nullptr;
  wds::interaction::Widget* pause_at_current_checkbox_ = nullptr;
  wds::interaction::Widget* split_width_checkbox_ = nullptr;
  std::function<void()> on_settings_;
  std::function<void()> on_export_;
  std::function<void()> on_persist_;
  std::function<void()> on_open_;
  std::function<void()> on_import_;
  std::function<void()> on_chart_add_;
};

}  // namespace wds::ui

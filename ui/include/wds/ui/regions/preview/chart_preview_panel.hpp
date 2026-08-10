#pragma once

#include "wds/ui/regions/preview/playback_preview.hpp"

#include "wds/renderer/preview_visual_config.hpp"

#include <wds/audio/transport.hpp>

#include <wds/core/chart_editor_engine.hpp>

#include <string>
#include <vector>

struct GLFWwindow;

namespace wds::ui {

// Chart preview panel: Transport + ChartEditorEngine sync loop around PlaybackPreviewView.
class ChartPreviewPanel {
 public:
  ChartPreviewPanel() = default;
  ~ChartPreviewPanel();

  ChartPreviewPanel(const ChartPreviewPanel&) = delete;
  ChartPreviewPanel& operator=(const ChartPreviewPanel&) = delete;

  bool initialize(GLFWwindow* window, const wds::renderer::PreviewVisualConfig& visual,
                  const std::string& chart_path = {}, const std::string& music_config_path = {},
                  const std::string& ui_font_path = {});
  // Initialize with empty editable chart (default new project).
  bool initialize_empty(GLFWwindow* window, const wds::renderer::PreviewVisualConfig& visual,
                        const std::string& ui_font_path = {});
  void shutdown();

  bool ready() const noexcept { return ready_; }
  // Populated when initialize / initialize_empty returns false.
  const std::string& last_init_error() const noexcept { return last_init_error_; }

  wds::audio::Transport& transport() noexcept { return transport_; }
  const wds::audio::Transport& transport() const noexcept { return transport_; }

  wds::chart_editor::ChartEditorEngine& engine() noexcept { return engine_; }
  const wds::chart_editor::ChartEditorEngine& engine() const noexcept { return engine_; }

  PlaybackPreviewView& preview() noexcept { return preview_; }
  const PlaybackPreviewView& preview() const noexcept { return preview_; }

  wds::renderer::TextureId solid_texture() const noexcept { return solid_texture_.id; }

  // Resize the Vulkan swapchain using the complete window framebuffer.
  void resize_framebuffer(int width, int height);
  void tick(int64_t delta_us);
  void render(const wds::renderer::DrawBatch* ui_overlay = nullptr,
              const wds::renderer::DrawBatch* modal_overlay = nullptr,
              const wds::renderer::DrawBatch* modal_chrome = nullptr);

  void set_content_bounds(int x, int y, int width, int height) noexcept;
  // Full preview column (may be wider than the aspect-locked stage content).
  void set_panel_bounds(int x, int y, int width, int height) noexcept;

  // Re-upload the UI font atlas if new glyphs were packed (e.g. CJK on demand),
  // and rebake when content-scale tier crosses a bucket.
  // Previous GPU atlases are retired (not destroyed) until flush_retired_font_textures().
  void sync_ui_font_texture();
  // Destroy font atlases retired during this frame. Call after draw_frame so
  // DrawBatches that still reference the previous id remain valid through submit.
  void flush_retired_font_textures();

  // Apply note_speed to both visual and core preview configs and rebuild.
  void set_note_speed(double speed);
  // Reload BGM path (empty clears to wall-clock).
  // When preserve_playback is false, seek to 0 and do not resume play (open/import).
  bool load_music(const std::string& music_path, bool preserve_playback = true);
  // Pause and seek transport + engine to t=0 so the preview window matches the chart.
  void reset_playback();

  // One display-frame lead (µs). Used by edit scroll sync while playing and by render().
  int64_t display_frame_lead_us() const noexcept;

 private:
  bool finish_initialize(GLFWwindow* window, const wds::renderer::PreviewVisualConfig& visual,
                         const std::string& ui_font_path);
  bool load_chart(const std::string& chart_path, const std::string& music_config_path);
  void seed_empty_chart();
  bool bake_ui_font(float bake_px);
  void warm_ui_font_glyphs();
  bool ensure_ui_font_scale();

  std::string last_init_error_;
  PlaybackPreviewView preview_;
  wds::audio::Transport transport_;
  wds::chart_editor::ChartEditorEngine engine_;
  wds::renderer::TextureInfo solid_texture_{};
  wds::renderer::TextureInfo ui_font_texture_{};
  // Atlases replaced mid-frame; destroyed in flush_retired_font_textures().
  std::vector<wds::renderer::TextureInfo> retired_font_textures_;
  std::string ui_font_path_;
  GLFWwindow* window_ = nullptr;
  float font_bake_tier_ = 0.0f;
  int content_x_ = 0;
  int content_y_ = 0;
  int content_width_ = 0;
  int content_height_ = 0;
  bool ready_ = false;
};

}  // namespace wds::ui

#pragma once

#include "wds/ui/regions/preview/playback_preview.hpp"

#include "wds/renderer/preview_visual_config.hpp"

#include <wds/audio/transport.hpp>
#include <wds/audio/waveform_jobs.hpp>
#include <wds/audio/waveform_overview.hpp>

#include <wds/core/chart_editor_engine.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wds::ui {

// Full-song GPU atlas is only for a paused Spectrogram view. Playing frames
// use the Qt visible-row backdrop (and keep this false so tick cannot hitch).
inline constexpr bool should_bake_full_spectrogram_atlas(bool want_spectrogram,
                                                         bool intends_playing) noexcept {
  return want_spectrogram && !intends_playing;
}

// Chart preview panel: Transport + ChartEditorEngine sync loop around PlaybackPreviewView.
class ChartPreviewPanel {
 public:
  ChartPreviewPanel() = default;
  ~ChartPreviewPanel();

  ChartPreviewPanel(const ChartPreviewPanel&) = delete;
  ChartPreviewPanel& operator=(const ChartPreviewPanel&) = delete;

  bool initialize_empty(const wds::renderer::VulkanHostSurface& host,
                        const wds::renderer::PreviewVisualConfig& visual,
                        const std::string& ui_font_path = {}, bool initialize_audio = true);
  void shutdown();
  // Tear down the swapchain before the host destroys VkSurfaceKHR.
  void abandon_host_surface();

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
  // Idle presentation gate: true when the next frame could change pixels or
  // playback state. The window host uses this to stop scheduling idle frames.
  void mark_present_dirty() noexcept { present_dirty_ = true; }
  bool needs_present() const noexcept;
  void note_frame_presented() noexcept;
  void render(const wds::renderer::DrawBatch* ui_overlay = nullptr,
              const wds::renderer::DrawBatch* modal_overlay = nullptr,
              const wds::renderer::DrawBatch* modal_chrome = nullptr);

  // Offline 60 Hz video export: T = n/60, visual lead = 0, no speaker BGM start.
  // Seek still clears persist/carry — call only at the segment start.
  void set_video_export_clock(bool on);
  bool video_export_clock() const noexcept { return video_export_clock_; }
  // Size the stage to the offscreen export framebuffer (full 16:9, no letterbox).
  void apply_export_geometry(int pixel_w, int pixel_h);

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
  void set_lane_count(int lane_count);
  // Apply official display settings (speed / 挡板 / note 厚度 / 分割线特效透明度).
  void apply_display_settings(double note_speed, int note_start_offset, int note_height_level,
                              int split_line_opacity_percent, int split_effect_quality = 1);
  // Reload BGM path (empty clears to wall-clock).
  // When preserve_playback is false, seek to 0 and do not resume play (open/import).
  bool load_music(const std::string& music_path, bool preserve_playback = true);
  bool audition_music(const std::string& music_path);
  // Decode jpg/png (EXIF auto-orient), center-crop 1:1, scale to 800×800, upload once.
  // Empty path unloads the jacket. Missing/unloadable files return false.
  bool load_jacket(const std::string& jacket_path);
  std::uint64_t music_generation() const noexcept { return music_generation_; }
  const wds::audio::WaveformOverview& waveform() const noexcept { return waveform_; }
  wds::renderer::TextureInfo spectrogram_texture() const noexcept { return spectrogram_texture_; }
  // Edit Spectrogram mode: decode FFT and (when paused) bake the GPU atlas.
  void set_want_spectrogram(bool want);
  bool want_spectrogram() const noexcept { return want_spectrogram_; }
  // Pause and seek transport + engine to t=0 so the preview window matches the chart.
  void reset_playback();
  // Pause and seek transport + engine to a saved position (project resume).
  void restore_playback_position(int64_t time_ms);
  // Join speculative collect, then queue a user seek (scrub / jump / CLI).
  void request_seek_ms(int64_t time_ms);
  // Document-function high-water request. fill_now (chart load / new project)
  // touches every page at once; otherwise only the mark rises and idle
  // pump_buffer_precommit slices do the touching.
  void request_buffer_precommit(bool fill_now = true);
  void pump_buffer_precommit();
  void set_after_seek_prepare(std::function<void(int64_t)> fn) {
    after_seek_prepare_ = std::move(fn);
  }
  int64_t last_seek_handle_us() const noexcept { return last_seek_handle_us_; }
  long last_seek_minflt() const noexcept { return last_seek_minflt_; }
  int64_t last_tick_preload_us() const noexcept { return last_tick_preload_us_; }
  int64_t last_tick_transport_us() const noexcept { return last_tick_transport_us_; }
  int64_t last_tick_poll_us() const noexcept { return last_tick_poll_us_; }
  int64_t last_tick_apply_us() const noexcept { return last_tick_apply_us_; }
  int64_t last_tick_bass_us() const noexcept { return last_tick_bass_us_; }
  int64_t last_tick_geom_us() const noexcept { return last_tick_geom_us_; }
  int64_t last_tick_sfx_us() const noexcept { return last_tick_sfx_us_; }
  bool last_tick_has_music() const noexcept { return last_tick_has_music_; }

  // One display-frame lead (µs). Used by edit scroll sync while playing and by render().
  int64_t display_frame_lead_us() const noexcept;
  // Playing draw lead: one vsync plus last snapshot→present cost (clamped to one vsync).
  int64_t playing_visual_lead_us() const noexcept;
  // Last playing visual lead actually applied to the preview draw clock.
  int64_t last_visual_lead_us() const noexcept { return last_visual_lead_us_; }
  int64_t last_displayed_now_us() const noexcept { return last_displayed_now_us_; }
  // Syuriken pulse clock (µs) last passed to set_syuriken_uniforms. Not the
  // note/particle visual lead.
  int64_t last_pulse_now_us() const noexcept { return last_pulse_now_us_; }
  // displayed − one vsync (present latency stays in displayed, not pictured).
  int64_t last_pictured_now_us() const noexcept { return last_pictured_now_us_; }
  int64_t last_injected_stall_us() const noexcept { return last_injected_stall_us_; }
  int64_t last_play_start_stall_us() const noexcept { return last_play_start_stall_us_; }
  // After present: live BASS position, or committed + this frame's render when no BGM.
  int64_t last_bass_after_present_us() const noexcept { return last_bass_after_present_us_; }
  // bass − (displayed chart − one vsync). Present latency stays in displayed.
  int64_t last_heard_vs_picture_us() const noexcept { return last_heard_vs_picture_us_; }
  // No-op leftover: speculative collect pipeline was removed.
  void join_speculative_split_collect();

 private:
  bool finish_initialize(const wds::renderer::VulkanHostSurface& host,
                          const wds::renderer::PreviewVisualConfig& visual,
                         const std::string& ui_font_path, bool initialize_audio = true);
  bool load_chart(const std::string& chart_path, const std::string& music_config_path);
  void seed_empty_chart();
  // Blocking join of every waveform decode. Only for teardown / before BASS_Free;
  // song switches use waveform_jobs_.cancel_all() and never wait.
  void join_waveform_workers();
  void rebuild_waveform(const std::string& music_path);
  void collect_ready_waveforms();
  void destroy_spectrogram_texture();
  void bake_spectrogram_texture();
  void maybe_bake_spectrogram_texture();
  void store_waveform_cache(const std::string& path, const wds::audio::WaveformOverview& decoded);
  const wds::audio::WaveformOverview* cached_waveform(const std::string& path,
                                                      bool need_spectrogram) const;
  void maybe_inject_preview_stall(bool playing, double chart_sec);
  // Syuriken pulse clock: song/chart microseconds, no present-latency extra.
  int64_t pulse_now_us(bool playing, int64_t timeline_us) const noexcept;

  std::string last_init_error_;
  PlaybackPreviewView preview_;
  wds::audio::Transport transport_;
  wds::audio::WaveformOverview waveform_;
  // Declared after transport_ so it is destroyed (and joined) first.
  wds::audio::WaveformJobs waveform_jobs_;
  std::unordered_map<std::string, wds::audio::WaveformOverview> waveform_cache_;
  std::string waveform_path_;
  bool want_spectrogram_ = false;
  bool present_dirty_ = true;
  bool last_presented_want_spectrogram_ = false;
  std::uint64_t last_presented_doc_gen_ = ~std::uint64_t{0};
  std::uint64_t last_presented_revision_ = ~std::uint64_t{0};
  int64_t last_presented_timeline_us_ = std::numeric_limits<int64_t>::min();
  std::uint64_t music_generation_ = 0;
  wds::renderer::TextureInfo spectrogram_texture_{};
  wds::chart_editor::ChartEditorEngine engine_;
  uint64_t scroll_geometry_revision_ = std::numeric_limits<uint64_t>::max();
  wds::renderer::TextureInfo solid_texture_{};
  wds::renderer::TextureInfo ui_font_texture_{};
  // Atlases replaced mid-frame; destroyed in flush_retired_font_textures().
  std::vector<wds::renderer::TextureInfo> retired_font_textures_;
  int display_refresh_hz_ = 60;
  std::function<int()> display_refresh_hz_fn_;
  int64_t last_visual_lead_us_ = 0;
  int64_t last_displayed_now_us_ = 0;
  int64_t last_pulse_now_us_ = 0;
  int64_t last_pictured_now_us_ = 0;
  int64_t last_injected_stall_us_ = 0;
  int64_t last_play_start_stall_us_ = 0;
  int64_t last_bass_after_present_us_ = 0;
  int64_t last_heard_vs_picture_us_ = 0;
  std::function<void(int64_t)> after_seek_prepare_;
  int64_t last_seek_handle_us_ = 0;
  long last_seek_minflt_ = 0;
  int64_t last_tick_preload_us_ = 0;
  int64_t last_tick_transport_us_ = 0;
  int64_t last_tick_poll_us_ = 0;
  int64_t last_tick_apply_us_ = 0;
  int64_t last_tick_bass_us_ = 0;
  bool last_tick_has_music_ = false;
  int64_t last_tick_geom_us_ = 0;
  int64_t last_tick_sfx_us_ = 0;
  int64_t last_present_latency_us_ = 0;
  bool last_render_playing_ = false;
  bool stall_injected_ = false;
  bool has_pinned_seek_ = false;
  int64_t pinned_seek_ms_ = 0;
  uint64_t last_doc_generation_ = 0;
  bool skip_precommit_pump_ = false;
  bool precommit_inited_ = false;
  int panel_fb_w_ = 0;
  int panel_x_ = 0;
  int panel_y_ = 0;
  int panel_h_ = 0;
  // Last framebuffer size passed to resize_framebuffer(). resize_preview_viewport
  // runs every frame, so this is what makes the MEMSTEP row event-driven.
  int last_resize_fb_w_ = 0;
  int last_resize_fb_h_ = 0;
  int content_x_ = 0;
  int content_y_ = 0;
  int content_width_ = 0;
  int content_height_ = 0;
  bool ready_ = false;
  bool video_export_clock_ = false;
};

}  // namespace wds::ui

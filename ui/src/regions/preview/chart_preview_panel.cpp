#include "wds/ui/regions/preview/chart_preview_panel.hpp"

#include "wds/renderer/log.hpp"

#include <wds/common/crash_handler.hpp>
#include <wds/core/official_playfield.hpp>

#include <wds/interaction/font_atlas.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace wds::ui {
namespace {

void publish_vulkan_device_context(const wds::renderer::VulkanRenderer& vk) {
  const std::uint32_t drv = vk.device_driver_version();
  const std::uint32_t api = vk.device_api_version();
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%s | driver %u.%u.%u | api %u.%u.%u", vk.device_name(),
                VK_VERSION_MAJOR(drv), VK_VERSION_MINOR(drv), VK_VERSION_PATCH(drv),
                VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), VK_VERSION_PATCH(api));
  wds::common::crash_set_context(wds::common::CrashContextField::VulkanDevice, buf);
}

}  // namespace

ChartPreviewPanel::~ChartPreviewPanel() { shutdown(); }

bool ChartPreviewPanel::finish_initialize(const wds::renderer::VulkanHostSurface& host,
                                          const wds::renderer::PreviewVisualConfig& visual,
                                          const std::string& ui_font_path, bool initialize_audio) {
  last_init_error_.clear();
  const auto t0 = std::chrono::steady_clock::now();
  const auto ms_since = [t0]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };
  display_refresh_hz_ = host.display_refresh_hz ? std::max(1, host.display_refresh_hz()) : 60;
  if (initialize_audio && !transport_.initialize(visual.effects_directory, visual.bgm_path)) {
    last_init_error_ =
        "音频初始化失败（BASS / effects：" + visual.effects_directory + "）";
    WDS_LOG("ChartPreviewPanel: audio init failed\n");
    return false;
  }
  WDS_LOG("init phase audio_ms=%lld\n", static_cast<long long>(ms_since()));

  if (!preview_.initialize(host, visual)) {
    // Distinguish the common CI libpng header/dylib skew (skins) from Vulkan.
    last_init_error_ =
        "预览初始化失败（Vulkan 或 skins PNG）。skins=" + visual.skins_directory +
        " — 若 stderr 出现 libpng version mismatch / png_create_read_struct "
        "failed，说明程序链到了错误的 libpng，请重装完整程序包";
    WDS_LOG("ChartPreviewPanel: preview init failed\n");
    transport_.shutdown();
    return false;
  }
  WDS_LOG("init phase vulkan_skins_ms=%lld\n", static_cast<long long>(ms_since()));
  publish_vulkan_device_context(preview_.vulkan());
  if (initialize_audio) preview_.attach_audio(&transport_.audio());

  {
    wds::chart_editor::PreviewConfig core_cfg;
    core_cfg.lane_count = visual.lane_count;
    core_cfg.note_speed = visual.note_speed;
    core_cfg.note_approach_seconds = visual.appear_time();
    core_cfg.split_line_animation_start_sec = visual.split_line_animation_start;
    core_cfg.split_line_animation_end_sec = visual.split_line_animation_end;
    core_cfg.auto_hit_feedback_ms =
        std::llround(static_cast<double>(visual.effect_duration) * 1000.0);
    engine_.set_preview_config(core_cfg);
  }

  const unsigned char white[4] = {255, 255, 255, 255};
  solid_texture_ = preview_.vulkan().create_texture_rgba(white, 1, 1);

  // Qt chrome owns all UI text (QFont / QPainter). FontAtlas is leftover for
  // the unused Vulkan UiPainter flush path and is not baked at startup.
  (void)ui_font_path;
  WDS_LOG("init phase ready_ms=%lld (no FontAtlas bake)\n", static_cast<long long>(ms_since()));

  transport_.request_seek_ms(transport_.chart_start_ms());
  rebuild_waveform(visual.bgm_path);
  ready_ = true;
  return true;
}


bool ChartPreviewPanel::initialize_empty(const wds::renderer::VulkanHostSurface& host,
                                         const wds::renderer::PreviewVisualConfig& visual,
                                         const std::string& ui_font_path, bool initialize_audio) {
  if (ready_) return true;
  if (!finish_initialize(host, visual, ui_font_path, initialize_audio)) return false;
  // Keep the engine's current document intact. Startup project loading can
  // happen before the Vulkan surface is ready; seeding here would overwrite
  // that document with a blank chart when the first frame is initialized.
  return true;
}

void ChartPreviewPanel::shutdown() {
  if (!ready_) {
    return;
  }
  // Last presented frame may still sample UI font / solid / toolbar textures.
  if (preview_.vulkan().ready()) {
    preview_.vulkan().device_wait_idle();
  }
  auto& font = wds::interaction::FontAtlas::instance();
  font.clear_gpu_texture();
  flush_retired_font_textures();
  if (ui_font_texture_) {
    preview_.vulkan().destroy_texture(ui_font_texture_.id);
    ui_font_texture_ = {};
  }
  font.clear();
  if (solid_texture_) {
    preview_.vulkan().destroy_texture(solid_texture_.id);
    solid_texture_ = {};
  }
  join_waveform_workers();
  destroy_spectrogram_texture();
  preview_.shutdown();
  transport_.shutdown();
  waveform_.clear();
  ready_ = false;
  panel_fb_w_ = 0;
}

int64_t ChartPreviewPanel::display_frame_lead_us() const noexcept {
  const int hz = display_refresh_hz_;
  // Fixed one-frame wall duration — not scaled by playback_rate.
  return 1'000'000 / std::max(hz, 1);
}

void ChartPreviewPanel::resize_framebuffer(int width, int height) {
  if (!ready_ || width <= 0 || height <= 0) {
    return;
  }
  preview_.resize(width, height);
}

void ChartPreviewPanel::set_content_bounds(int x, int y, int width, int height) noexcept {
  content_x_ = x;
  content_y_ = y;
  content_width_ = width;
  content_height_ = height;
  preview_.geometry().set_content_rect(content_x_, content_y_, content_width_, content_height_);
}

void ChartPreviewPanel::set_panel_bounds(int x, int y, int width, int height) noexcept {
  panel_fb_w_ = std::max(0, width);
  preview_.geometry().set_panel_rect(x, y, width, height);
}

void ChartPreviewPanel::sync_ui_font_texture() {
  // No-op: product UI text is Qt. Leftover UiManager Vulkan flush callers
  // still invoke this; baking here would hitch the first preview frames.
}

void ChartPreviewPanel::flush_retired_font_textures() {
  if (!ready_) {
    retired_font_textures_.clear();
    return;
  }
  for (auto& tex : retired_font_textures_) {
    if (tex) {
      preview_.vulkan().destroy_texture(tex.id);
    }
  }
  retired_font_textures_.clear();
}

void ChartPreviewPanel::tick(int64_t delta_us) {
  if (!ready_) {
    return;
  }
  collect_ready_waveforms();
  // Clamp post-hitch spikes so Transport does not hard-snap (100 ms) on one frame.
  constexpr int64_t kMaxWallDeltaUs = 80000;  // 80 ms
  const int64_t clamped =
      std::clamp(delta_us, int64_t{0}, kMaxWallDeltaUs);
  // Transport clock only — no display lead. Lead is added to the draw clock in
  // render() so it tracks presented frames (FIFO); SFX stays on the music clock.
  const auto timeline = transport_.poll(clamped);
  engine_.apply_timeline(timeline);
  // Keep SFX chart-delay / lead-in mapping in sync with transport + edit blank.
  preview_.set_chart_offset_ms(transport_.chart_offset_ms());
  preview_.set_preview_lead_in_visible_ms(engine_.preview_lead_in_visible_ms());
  // Arm music POS syncs while BGM is still paused on a fresh play request, then
  // start audible music so early hits are not scheduled behind the decode frontier.
  preview_.sync_hit_sfx(engine_.snapshot());
  transport_.start_pending_music();
}

void ChartPreviewPanel::render(const wds::renderer::DrawBatch* ui_overlay,
                               const wds::renderer::DrawBatch* modal_overlay,
                               const wds::renderer::DrawBatch* modal_chrome) {
  if (!ready_) {
    return;
  }
  // One scan-out frame of visual lead while playing (does not scale with rate).
  // Added to the draw clock only — do not apply+rollback the engine snapshot
  // (that rebuilt combo / notes three times per frame).
  const auto committed = transport_.committed_snapshot();
  const bool playing = committed.state == wds::common::PlaybackState::Playing;
  const int64_t lead_us = playing ? display_frame_lead_us() : 0;
  preview_.render(engine_.snapshot(), ui_overlay, solid_texture_.id, modal_overlay, modal_chrome,
                  lead_us);
}

void ChartPreviewPanel::set_lane_count(int lane_count) {
  lane_count = std::clamp(lane_count, 1, 32);
  auto visual = preview_.config();
  visual.lane_count = lane_count;
  preview_.set_config(visual);
  auto core_cfg = engine_.preview_config();
  core_cfg.lane_count = lane_count;
  engine_.set_preview_config(core_cfg);
}

void ChartPreviewPanel::set_note_speed(double speed) {
  const auto& visual = preview_.config();
  apply_display_settings(speed, visual.note_start_offset, visual.note_height_level,
                         static_cast<int>(std::lround(static_cast<double>(visual.split_line_opacity) *
                                                      100.0)));
}

void ChartPreviewPanel::apply_display_settings(double note_speed, int note_start_offset,
                                               int note_height_level,
                                               int split_line_opacity_percent) {
  note_speed = wds::chart_editor::official_clamp_note_speed(note_speed);
  note_start_offset = wds::chart_editor::official_clamp_note_start_offset(note_start_offset);
  note_height_level = wds::chart_editor::official_clamp_note_height_level(note_height_level);
  split_line_opacity_percent =
      wds::chart_editor::official_clamp_split_effect_line_opacity(split_line_opacity_percent);

  auto visual = preview_.config();
  visual.note_speed = static_cast<float>(note_speed);
  visual.note_start_offset = note_start_offset;
  visual.note_height_level = note_height_level;
  visual.split_line_opacity = static_cast<float>(split_line_opacity_percent) / 100.0f;
  preview_.set_config(visual);

  auto core_cfg = engine_.preview_config();
  core_cfg.note_speed = note_speed;
  core_cfg.note_approach_seconds = visual.appear_time();
  engine_.set_preview_config(core_cfg);
}

bool ChartPreviewPanel::load_music(const std::string& music_path, bool preserve_playback) {
  if (!ready_) {
    return false;
  }
  // Join first: AudioEngine::shutdown() / BASS_Free() must not run while a
  // waveform worker still holds a process-global BASS decode stream.
  join_waveform_workers();
  const auto effects = preview_.config().effects_directory;
  const bool was_playing = preserve_playback && transport_.playing();
  const int64_t pos =
      preserve_playback ? transport_.committed_ms() : transport_.chart_start_ms();
  const int64_t kept_offset = transport_.chart_offset_ms();
  transport_.shutdown();
  auto restore_transport = [&](const std::string& path) -> bool {
    if (!transport_.initialize(effects, path)) {
      return false;
    }
    transport_.set_chart_offset_ms(kept_offset);
    preview_.attach_audio(&transport_.audio());
    transport_.request_seek_ms(pos);
    if (was_playing) {
      transport_.request_play();
    }
    return true;
  };
  if (!restore_transport(music_path)) {
    // Never leave the editor without a live Transport — otherwise Space / timeline die.
    (void)restore_transport({});
    destroy_spectrogram_texture();
    waveform_.clear();
    return music_path.empty();
  }
  // Non-empty path that failed to decode still leaves a healthy engine (no BGM).
  if (!music_path.empty() && !transport_.audio().has_music()) {
    destroy_spectrogram_texture();
    waveform_.clear();
    return false;
  }
  if (!preserve_playback) {
    // load_chart / publish_snapshot may still use the previous SeekableClock time
    // until the next Transport poll — snap the engine immediately.
    engine_.seek(transport_.chart_start_ms());
  }
  rebuild_waveform(music_path);
  return true;
}

void ChartPreviewPanel::join_waveform_workers() {
  // WaveformOverview::load() creates its own BASS decode stream on the same
  // process-global device (BASS_StreamCreateFile / BASS_ChannelGetData /
  // BASS_StreamFree). AudioEngine::shutdown() ends in BASS_Free(), so every
  // pending worker must be joined before any transport/engine shutdown.
  ++waveform_generation_;
  for (auto& pending : pending_waveforms_) {
    if (pending.cancel) {
      pending.cancel->store(true, std::memory_order_relaxed);
    }
  }
  for (auto& pending : pending_waveforms_) {
    if (pending.result.valid()) {
      try {
        (void)pending.result.get();
      } catch (...) {
        WDS_LOG("ChartPreviewPanel: waveform worker failed during shutdown\n");
      }
    }
  }
  pending_waveforms_.clear();
}

void ChartPreviewPanel::rebuild_waveform(const std::string& music_path) {
  destroy_spectrogram_texture();
  waveform_.clear();
  const std::uint64_t generation = ++waveform_generation_;
  if (music_path.empty() || !transport_.audio().has_music()) {
    return;
  }
  // Decoding the full song and calculating two FFTs per hop is the expensive
  // part of project opening. Keep it off the Qt/render thread; tick() installs
  // only the newest completed result and performs the Vulkan upload there.
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  pending_waveforms_.push_back(PendingWaveform{
      generation, cancel,
      std::async(std::launch::async, [music_path, cancel] {
        wds::common::install_thread_crash_stack();
        wds::audio::WaveformOverview decoded;
        if (!decoded.load(music_path, cancel.get())) {
          if (!cancel->load(std::memory_order_relaxed)) {
            WDS_LOG("ChartPreviewPanel: waveform decode failed %s\n", music_path.c_str());
          }
          decoded.clear();
        }
        return decoded;
      })});
}

void ChartPreviewPanel::collect_ready_waveforms() {
  using namespace std::chrono_literals;
  for (auto it = pending_waveforms_.begin(); it != pending_waveforms_.end();) {
    if (!it->result.valid() || it->result.wait_for(0ms) != std::future_status::ready) {
      ++it;
      continue;
    }
    wds::audio::WaveformOverview decoded;
    try {
      decoded = it->result.get();
    } catch (...) {
      WDS_LOG("ChartPreviewPanel: waveform worker failed\n");
      decoded.clear();
    }
    const bool cancelled = it->cancel && it->cancel->load(std::memory_order_relaxed);
    const bool current = it->generation == waveform_generation_;
    it = pending_waveforms_.erase(it);
    if (cancelled || !current || decoded.empty()) continue;
    waveform_ = std::move(decoded);
    bake_spectrogram_texture();
  }
}

void ChartPreviewPanel::destroy_spectrogram_texture() {
  if (!spectrogram_texture_) return;
  if (preview_.vulkan().ready()) {
    preview_.vulkan().destroy_texture(spectrogram_texture_.id);
  }
  spectrogram_texture_ = {};
}

void ChartPreviewPanel::bake_spectrogram_texture() {
  destroy_spectrogram_texture();
  if (!waveform_.has_spectrogram() || !preview_.vulkan().ready()) return;
  std::vector<unsigned char> rgba;
  int w = 0;
  int h = 0;
  if (!waveform_.rasterize_rgba(rgba, w, h) || w <= 0 || h <= 0 || rgba.empty()) return;
  spectrogram_texture_ = preview_.vulkan().create_texture_rgba(rgba.data(), w, h, /*nearest=*/false);
  if (!spectrogram_texture_) {
    WDS_LOG("ChartPreviewPanel: spectrogram texture upload failed %dx%d\n", w, h);
  }
}

void ChartPreviewPanel::reset_playback() {
  if (!ready_) {
    return;
  }
  const int64_t start = transport_.chart_start_ms();
  transport_.request_pause();
  transport_.request_seek_ms(start);
  engine_.seek(start);
}

bool ChartPreviewPanel::load_chart(const std::string& chart_path,
                                   const std::string& music_config_path) {
  const auto result = engine_.load_official_from_file(chart_path, music_config_path);
  if (result.error != wds::chart_editor::SerializeError::Ok) {
    WDS_LOG("load official chart failed: %s (%s)\n", chart_path.c_str(),
            result.message.c_str());
    return false;
  }
  return true;
}

void ChartPreviewPanel::seed_empty_chart() {
  wds::chart_editor::MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.offset_ms = 0;
  engine_.load_chart({timing, {}, {}}, wds::chart_editor::ChartEditMode::Editable);
  engine_.history().clear();
}

}  // namespace wds::ui

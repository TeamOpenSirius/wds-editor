#include "wds/ui/regions/preview/chart_preview_panel.hpp"

#include "wds/renderer/log.hpp"
#include "wds/ui/frame_diag.hpp"

#include <wds/common/crash_handler.hpp>
#include <wds/common/minflt_stage.hpp>
#include <wds/common/time.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/preview_snapshot.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/core/split_lane_simulator.hpp>
#include <wds/chart_render/split_effect_particles.hpp>

#include <wds/interaction/font_atlas.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace wds::ui {
namespace {

long process_minflt() noexcept {
#ifndef _WIN32
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    return ru.ru_minflt;
  }
#endif
  return 0;
}

std::vector<wds::chart_editor::PreviewSplitLaneInstance> collect_steady_split_lanes(
    const wds::chart_editor::ChartEditorEngine& engine) {
  std::vector<int32_t> ids;
  const auto& doc = engine.document();
  doc.index().query_split_lanes_up_to(std::numeric_limits<int64_t>::max(), ids);
  wds::chart_editor::SplitLaneSimulator sim(engine.preview_config());
  const auto& timing = doc.timing();
  std::vector<wds::chart_editor::PreviewSplitLaneInstance> out;
  out.reserve(ids.size());
  for (int32_t id : ids) {
    const auto note = doc.find_note(id);
    if (!note || !wds::chart_editor::is_split_lane_gimmick(note->gimmick_type)) {
      continue;
    }
    wds::chart_editor::PreviewSplitLaneInstance inst;
    sim.fill_instance(inst, *note, timing, note->start_ms(timing));
    if (inst.split_count <= 0) {
      continue;
    }
    out.push_back(inst);
  }
  return out;
}

std::vector<wds::chart_editor::PreviewSplitLaneInstance> collect_splits_at(
    const wds::chart_editor::ChartEditorEngine& engine, int64_t preview_time_ms) {
  std::vector<int32_t> ids;
  const auto& doc = engine.document();
  const auto& cfg = engine.preview_config();
  const int64_t appear_ms =
      wds::chart_editor::split_fade_sec_to_ms(cfg.split_line_animation_start_sec);
  const int64_t hide_ms =
      wds::chart_editor::official_split_hide_tail_ms(cfg.split_line_animation_end_sec);
  const int64_t span = doc.index().max_split_span_ms();
  int64_t lower = preview_time_ms;
  if (span > 0 && lower > std::numeric_limits<int64_t>::min() + span) {
    lower -= span;
  }
  if (hide_ms > 0 && lower > std::numeric_limits<int64_t>::min() + hide_ms) {
    lower -= hide_ms;
  }
  int64_t upper = preview_time_ms;
  if (appear_ms > 0 && upper < std::numeric_limits<int64_t>::max() - appear_ms) {
    upper += appear_ms;
  }
  doc.index().query_split_lanes_in_range(lower, upper, ids);
  wds::chart_editor::SplitLaneSimulator sim(cfg);
  std::vector<wds::chart_editor::PreviewSplitLaneInstance> out;
  sim.build_active_splits(doc.notes(), ids, doc.timing(), preview_time_ms, out);
  return out;
}

void pump_lane_geom_rendered(wds::ui::PlaybackPreviewView& preview,
                             const wds::chart_editor::ChartEditorEngine& engine,
                             bool playing, int64_t lead_us, int64_t nominal_us) {
  const auto& snap = engine.snapshot();
  int64_t timeline_us = snap.timeline_us;
  if (timeline_us == 0 && snap.timeline_ms != 0) {
    timeline_us = wds::common::ms_to_us(snap.timeline_ms).count();
  }
  // Extra clocks build plate corners only (retain=false). Coloring T+2·lead
  // would leave a warm-up batch for the wrong present time. Chart T may be
  // negative; do not use now_us < 0 as that sentinel.
  preview.pump_lane_geom_preload(snap.split_lanes, (!playing || lead_us <= 0) ? timeline_us : 0,
                                !playing || lead_us <= 0);
  const int64_t lead_ms = (lead_us + 500) / 1000;
  const int64_t nominal_ms = (nominal_us + 500) / 1000;
  if (playing && nominal_us > 0 && nominal_us != lead_us) {
    preview.pump_lane_geom_preload(collect_splits_at(engine, snap.timeline_ms + nominal_ms), -1,
                                   false);
  }
  if (lead_ms > 0) {
    const int64_t pictured_ms = snap.timeline_ms + lead_ms;
    preview.pump_lane_geom_preload(collect_splits_at(engine, pictured_ms), timeline_us + lead_us,
                                   playing);
    preview.pump_lane_geom_preload(collect_splits_at(engine, pictured_ms + lead_ms), -1, false);
  }
  if (!playing || preview.lane_geom_cache_empty()) {
    preview.pump_lane_geom_preload(collect_steady_split_lanes(engine), -1, false);
  }
}

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
  display_refresh_hz_fn_ = host.display_refresh_hz;
  display_refresh_hz_ = display_refresh_hz_fn_ ? std::max(1, display_refresh_hz_fn_()) : 60;
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
  if (solid_texture_) {
    preview_.textures().bind_identity(solid_texture_.id, "ui.solid");
  }
  preview_.request_split_effect_preload();

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
  preview_.join_speculative_split_collect();
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

void ChartPreviewPanel::set_video_export_clock(bool on) {
  video_export_clock_ = on;
  transport_.set_offline_export(on);
  if (on) {
    last_present_latency_us_ = 0;
  }
}

int64_t ChartPreviewPanel::playing_visual_lead_us() const noexcept {
  if (video_export_clock_) {
    return 0;
  }
  const int64_t nominal = display_frame_lead_us();
  // Last snapshot→present cost (not vkQueuePresent alone), clamped to one
  // vsync: a hitch must not draw a second extra frame ahead. No EMA — the
  // no-music clock is advanced by this frame's R after present so
  // extra_prev and R_this cancel when R is stable and below one vsync.
  const int64_t extra = std::clamp(last_present_latency_us_, int64_t{0}, nominal);
  return nominal + extra;
}

int64_t ChartPreviewPanel::pulse_now_us(bool playing, int64_t timeline_us) const noexcept {
  if (video_export_clock_) {
    return timeline_us;
  }
  // hop / darkband-v2 2026-10-17. Pulse analog 43.06 Hz aliases at 60 fps
  // (−0.718 cyc/frame). Present-latency extra on the note lead (0–16.7 ms)
  // flipped the nearest wrap 22/540 frames (Δt std 2.39 ms). BASS decode
  // position is song time (bytes→µs; BASS_ATTRIB_FREQ only changes wall
  // speed, so d(song)/d(chart) stays 1 at every playback_rate). Not 2022.3.
  // Pause/scrub: freeze on chart T, no lead — not Unity Time.time.
  if (!playing) {
    return timeline_us;
  }
  const int64_t scanout_us = display_frame_lead_us();
  const auto& audio = transport_.audio();
  if (audio.has_music() && audio.stream_playing()) {
    return audio.position().count() + scanout_us;
  }
  return timeline_us + scanout_us;
}

void ChartPreviewPanel::abandon_host_surface() {
  if (!ready_) {
    return;
  }
  preview_.vulkan().abandon_host_surface();
}

void ChartPreviewPanel::resize_framebuffer(int width, int height) {
  if (!ready_ || width <= 0 || height <= 0) {
    return;
  }
  if (video_export_clock_) {
    return;
  }
  preview_.resize(width, height);
}

void ChartPreviewPanel::apply_export_geometry(int pixel_w, int pixel_h) {
  if (pixel_w <= 0 || pixel_h <= 0) return;
  preview_.geometry().resize(pixel_w, pixel_h);
  preview_.geometry().set_content_rect(0, 0, pixel_w, pixel_h);
  preview_.geometry().set_panel_rect(0, 0, pixel_w, pixel_h);
  content_x_ = 0;
  content_y_ = 0;
  content_width_ = pixel_w;
  content_height_ = pixel_h;
  panel_fb_w_ = pixel_w;
  preview_.invalidate_lane_geom_cache();
}

void ChartPreviewPanel::set_content_bounds(int x, int y, int width, int height) noexcept {
  if (video_export_clock_) return;
  const bool changed = x != content_x_ || y != content_y_ || width != content_width_ ||
                       height != content_height_;
  content_x_ = x;
  content_y_ = y;
  content_width_ = width;
  content_height_ = height;
  preview_.geometry().set_content_rect(content_x_, content_y_, content_width_, content_height_);
  if (changed) {
    preview_.invalidate_lane_geom_cache();
  }
}

void ChartPreviewPanel::set_panel_bounds(int x, int y, int width, int height) noexcept {
  if (video_export_clock_) return;
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

void ChartPreviewPanel::join_speculative_split_collect() {
  preview_.join_speculative_split_collect();
}

void ChartPreviewPanel::request_buffer_precommit(bool fill_now) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto& doc = engine_.document();
  const auto& timing = doc.timing();
  const int64_t appear_ms = wds::chart_editor::split_fade_sec_to_ms(
      engine_.preview_config().split_line_animation_start_sec);
  std::vector<wds::chart_render::SplitEffectBoundNote> notes;
  notes.reserve(doc.notes().size());
  for (const auto& n : doc.notes()) {
    if (!wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) {
      continue;
    }
    wds::chart_render::SplitEffectBoundNote b;
    b.color_id = n.scratch_length;
    b.start_ms = n.start_ms(timing);
    b.end_ms = n.end_ms(timing);
    notes.push_back(b);
  }
  const auto plan = wds::chart_render::split_effect_estimate_buffer_bound(
      notes.data(), static_cast<int>(notes.size()), appear_ms);
  preview_.request_buffer_precommit(plan, fill_now);
  if (fill_now) {
    skip_precommit_pump_ = true;
  }
  static const bool diag = frame_diag_enabled_from_env();
  if (!diag) {
    return;
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  const auto st = preview_.last_precommit();
  std::fprintf(stderr,
               "PRECOMMIT_REQUEST us=%lld notes=%zu concurrent=%d pool=%d heaviest=%d×%d "
               "live=%d trail=%d verts=%zu rss_kb=%ld pending=%d raise_us=%lld\n",
               static_cast<long long>(us), notes.size(), plan.concurrent_instances,
               plan.particle_cap, plan.heaviest_id, plan.floor_instances, plan.heaviest_live,
               plan.trail_chunks, plan.draw_verts, st.rss_kb_before, st.pending,
               static_cast<long long>(st.request_us));
}

void ChartPreviewPanel::pump_buffer_precommit() {
  if (skip_precommit_pump_) {
    skip_precommit_pump_ = false;
    return;
  }
  preview_.pump_buffer_precommit(wds::chart_render::split_effect_idle_precommit_budget_us());
}

void ChartPreviewPanel::request_seek_ms(int64_t time_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  const long flt0 = process_minflt();
  wds::common::minflt_stage("seek.begin");
  preview_.join_speculative_split_collect();
  wds::common::minflt_stage("seek.after_join");
  transport_.request_seek_ms(time_ms);
  // Pin the visual snapshot to T now. Transport poll may drop a seek when
  // audio is not ready or a music set_position fails; line/lane geometry
  // must still be a function of chart T (OpenWDS SplitLaneRuntime.Tick
  // takes chartMilliseconds, not Time.deltaTime).
  engine_.seek(time_ms);
  wds::common::minflt_stage("seek.after_engine");
  pinned_seek_ms_ = time_ms;
  has_pinned_seek_ = true;
  if (display_refresh_hz_fn_) {
    display_refresh_hz_ = std::max(1, display_refresh_hz_fn_());
  }
  // Pose/carry from a previous pictured T must not leak into this seek.
  // useAutoRandomSeed stays (Play remints; seek does not). Hash buckets
  // and lane cell vectors keep capacity (memory reuse, not a result cache).
  wds::chart_render::split_effect_clear_caches();
  wds::common::minflt_stage("seek.after_clear");
  last_present_latency_us_ = 0;
  wds::chart_render::split_effect_prepare_heavy_collect(50);
  preview_.reset_lane_geom_time_state();
  const int64_t now_us = wds::common::ms_to_us(time_ms).count();
  preview_.pump_lane_geom_preload(collect_splits_at(engine_, time_ms), now_us, true);
  // First play tick draws at T+nominal lead (latency 0). Build that clock
  // here so tick_geom / first lanes fill are not on the presented frame.
  const int64_t lead_us = playing_visual_lead_us();
  pump_lane_geom_rendered(preview_, engine_, true, lead_us, display_frame_lead_us());
  // draw_split_lanes hashes snapshot lanes (committed T) with pictured now.
  // Pictured collect_splits_at percents differ during fade; color that pair last.
  preview_.pump_lane_geom_preload(engine_.snapshot().split_lanes, now_us + lead_us, true);
  if (after_seek_prepare_ && !video_export_clock_) {
    after_seek_prepare_(time_ms);
  }
  last_seek_handle_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
  last_seek_minflt_ = process_minflt() - flt0;
  wds::common::minflt_stage("seek.end");
  skip_precommit_pump_ = true;
}

void ChartPreviewPanel::maybe_inject_preview_stall(bool playing, double chart_sec) {
  if (stall_injected_) {
    return;
  }
  const int ms = preview_stall_ms_from_env();
  if (ms <= 0 || !playing) {
    return;
  }
  const double at = preview_stall_at_sec_from_env();
  if (at >= 0.0 && chart_sec + 1e-9 < at) {
    return;
  }
  stall_injected_ = true;
  last_injected_stall_us_ = static_cast<int64_t>(ms) * 1000;
  if (at < 0.0) {
    last_play_start_stall_us_ = last_injected_stall_us_;
  }
  WDS_LOG("preview stall inject_ms=%d at_sec=%.3f chart_sec=%.3f\n", ms, at, chart_sec);
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void ChartPreviewPanel::tick(int64_t delta_us) {
  if (!ready_) {
    return;
  }
  static const bool time_tick = frame_diag_enabled_from_env();
  const auto t_all = time_tick ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
  auto lapse = [&](std::chrono::steady_clock::time_point t0) -> int64_t {
    if (!time_tick) {
      return 0;
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };

  auto t0 = t_all;
  if (transport_.pending_seek()) {
    preview_.join_speculative_split_collect();
  }
  const uint64_t doc_gen = engine_.document().content_generation();
  if (doc_gen != last_doc_generation_ || !precommit_inited_) {
    if (doc_gen != last_doc_generation_) {
      preview_.join_speculative_split_collect();
      preview_.request_split_effect_preload();
      preview_.invalidate_lane_geom_cache();
    }
    last_doc_generation_ = doc_gen;
    request_buffer_precommit(!precommit_inited_);
    precommit_inited_ = true;
  }
  preview_.pump_split_effect_preload();
  collect_ready_waveforms();
  last_tick_preload_us_ = lapse(t0);

  t0 = time_tick ? std::chrono::steady_clock::now() : t0;
  if (display_refresh_hz_fn_) {
    display_refresh_hz_ = std::max(1, display_refresh_hz_fn_());
  }
  transport_.set_max_clock_error_us(display_frame_lead_us());
  // Paused / capture seeks must not eat the QWindow frame-callback wall dt
  // (pump_cli_frame always passes 16ms). OpenWDS lane Tick is chart ms.
  const bool want_play = transport_.intends_playing();
  const int64_t wall_us = want_play ? std::max<int64_t>(0, delta_us) : 0;
  const auto timeline = transport_.poll(wall_us);
  engine_.apply_timeline(timeline);
  if (has_pinned_seek_) {
    if (!want_play && engine_.timeline_ms() != pinned_seek_ms_) {
      engine_.seek(pinned_seek_ms_);
    } else if (want_play) {
      has_pinned_seek_ = false;
    }
  }
  preview_.set_chart_offset_ms(transport_.chart_offset_ms());
  preview_.set_preview_lead_in_visible_ms(engine_.preview_lead_in_visible_ms());
  last_tick_transport_us_ = lapse(t0);

  t0 = time_tick ? std::chrono::steady_clock::now() : t0;
  // Warm plates for the committed snapshot and the pictured clock. Appear
  // percents at T+lead must be ready before the first rendered frame.
  // Playing lead includes last present latency so late frames do not eat the
  // one-vsync visual lead (see render()). First play frame draws at nominal
  // only — clear leftover paused latency so prebuild matches that clock.
  const bool playing = transport_.playing();
  if (playing && !last_render_playing_) {
    last_present_latency_us_ = 0;
  }
  pump_lane_geom_rendered(preview_, engine_, playing, playing ? playing_visual_lead_us() : 0,
                         display_frame_lead_us());
  last_tick_geom_us_ = lapse(t0);

  t0 = time_tick ? std::chrono::steady_clock::now() : t0;
  // Arm POS SFX on a still-paused stream. Audible start waits until render()
  // has collected the first picture at t0 (audio follows picture).
  // Export bounce mixes SFX offline; do not arm the speaker mixer.
  if (!video_export_clock_) {
    preview_.sync_hit_sfx(engine_.snapshot());
  }
  last_tick_sfx_us_ = lapse(t0);
  pump_buffer_precommit();
}

void ChartPreviewPanel::render(const wds::renderer::DrawBatch* ui_overlay,
                               const wds::renderer::DrawBatch* modal_overlay,
                               const wds::renderer::DrawBatch* modal_chrome) {
  if (!ready_) {
    return;
  }
  const uint64_t chart_revision = engine_.document().content_generation();
  if (scroll_geometry_revision_ != chart_revision) {
    preview_.geometry().set_scroll_speed_points(engine_.document().scroll_speed_points(),
                                                engine_.document().timing());
    scroll_geometry_revision_ = chart_revision;
  }
  if (display_refresh_hz_fn_) {
    display_refresh_hz_ = std::max(1, display_refresh_hz_fn_());
  }
  const auto committed = transport_.committed_snapshot();
  const bool playing = committed.state == wds::common::PlaybackState::Playing;
  const auto& snap = engine_.snapshot();
  int64_t transport_us = snap.timeline_us;
  if (transport_us == 0 && snap.timeline_ms != 0) {
    transport_us = wds::common::ms_to_us(snap.timeline_ms).count();
  }
  const int64_t nominal_lead_us = display_frame_lead_us();
  if (playing && !last_render_playing_) {
    last_present_latency_us_ = 0;
  }
  last_render_playing_ = playing;
  const int64_t lead_us = playing ? playing_visual_lead_us() : 0;
  last_visual_lead_us_ = lead_us;
  last_displayed_now_us_ = transport_us + lead_us;
  last_pulse_now_us_ = pulse_now_us(playing, transport_us);
  maybe_inject_preview_stall(playing, static_cast<double>(last_displayed_now_us_) / 1'000'000.0);
  const auto present_t0 = std::chrono::steady_clock::now();
  preview_.render(snap, ui_overlay, solid_texture_.id, modal_overlay, modal_chrome, lead_us,
                  last_pulse_now_us_);
  last_present_latency_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - present_t0)
                                 .count();
  if (transport_.audio().has_music()) {
    last_bass_after_present_us_ = transport_.audio().position().count();
  } else {
    // No BGM: committed is the tick snapshot, not a running mixer. Advance
    // it by this frame's snapshot→present cost so it pairs with last-frame
    // extra in the lead. align_screen = extra_prev − R_this converges
    // whenever R < one vsync. Leaving committed unmoved made align≈R
    // (the 8ms/8ms signature) even though extra was already in lead.
    last_bass_after_present_us_ =
        transport_.committed_position().count() + last_present_latency_us_;
  }
  // pictured = displayed − one vsync, not minus (vsync + present latency).
  // Late present is already in displayed; subtracting only the nominal lead
  // keeps BASS−picture from locking to last-frame CPU time.
  const int64_t pictured_us = last_displayed_now_us_ - (playing ? nominal_lead_us : 0);
  last_pictured_now_us_ = pictured_us;
  last_heard_vs_picture_us_ = last_bass_after_present_us_ - pictured_us;
  // Picture is at t0; start (or retry) audible BGM so a cold first frame does
  // not leave the picture behind a running clock. Seek-while-playing same path.
  if (!video_export_clock_) {
    (void)transport_.start_pending_music();
  }
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
                                                      100.0)),
                         visual.split_effect_quality);
}

void ChartPreviewPanel::apply_display_settings(double note_speed, int note_start_offset,
                                               int note_height_level,
                                               int split_line_opacity_percent,
                                               int split_effect_quality) {
  note_speed = wds::chart_editor::official_clamp_note_speed(note_speed);
  note_start_offset = wds::chart_editor::official_clamp_note_start_offset(note_start_offset);
  note_height_level = wds::chart_editor::official_clamp_note_height_level(note_height_level);
  split_line_opacity_percent =
      wds::chart_editor::official_clamp_split_effect_line_opacity(split_line_opacity_percent);
  split_effect_quality =
      wds::chart_editor::official_clamp_sprit_effect_setting(split_effect_quality);

  auto visual = preview_.config();
  visual.note_speed = static_cast<float>(note_speed);
  visual.note_start_offset = note_start_offset;
  visual.note_height_level = note_height_level;
  visual.split_line_opacity = static_cast<float>(split_line_opacity_percent) / 100.0f;
  visual.split_effect_quality = split_effect_quality;
  preview_.join_speculative_split_collect();
  preview_.set_config(visual);

  auto core_cfg = engine_.preview_config();
  core_cfg.note_speed = note_speed;
  core_cfg.note_approach_seconds = visual.appear_time();
  engine_.set_preview_config(core_cfg);
}

bool ChartPreviewPanel::load_music(const std::string& music_path, bool preserve_playback) {
  ++music_generation_;
  if (!ready_) {
    return false;
  }
  // Never wait for the old song's waveform decode on the GUI thread: cancel it
  // (stale results are dropped by generation) and let it wind down; poll() in
  // tick() reaps it. It is only joined below, if the BASS device has to be
  // torn down.
  waveform_jobs_.cancel_all();
  preview_.join_speculative_split_collect();
  const auto effects = preview_.config().effects_directory;
  const bool was_playing = preserve_playback && transport_.playing();
  const int64_t pos =
      preserve_playback ? transport_.committed_ms() : transport_.chart_start_ms();
  const int64_t kept_offset = transport_.chart_offset_ms();
  auto finish_restore = [&] {
    transport_.set_chart_offset_ms(kept_offset);
    preview_.attach_audio(&transport_.audio());
    transport_.request_seek_ms(pos);
    if (was_playing) {
      transport_.request_play();
    }
  };
  // Fast path: keep the BASS device, SFX samples, mixer and Hold stream and swap
  // only the music stream. Anything it cannot do safely (no music yet, different
  // effects dir, output device changed, file will not open) falls through to the
  // full re-initialise below, which behaves exactly as before.
  if (transport_.reload_music(effects, music_path)) {
    finish_restore();
    if (!preserve_playback) {
      engine_.seek(transport_.chart_start_ms());
    }
    rebuild_waveform(music_path);
    return true;
  }
  // Full path ends in BASS_Free(): a decode worker's stream handle must not
  // outlive the device (handles can be reissued by the next BASS_Init), so join
  // the cancelled workers first. They check the cancel flag around the stream
  // open and every chunk, and no longer PRESCAN, so this wait is short.
  join_waveform_workers();
  transport_.shutdown();
  auto restore_transport = [&](const std::string& path) -> bool {
    if (!transport_.initialize(effects, path)) {
      return false;
    }
    finish_restore();
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

bool ChartPreviewPanel::audition_music(const std::string& music_path) {
  if (!ready_ || !transport_.audio().replace_music_source(music_path)) return false;
  // rebuild_waveform() cancels the old decode without joining (the BASS device
  // stays up here).
  rebuild_waveform(music_path);
  return true;
}

void ChartPreviewPanel::join_waveform_workers() {
  // WaveformOverview::load() creates its own BASS decode stream on the same
  // process-global device (BASS_StreamCreateFile / BASS_ChannelGetData /
  // BASS_StreamFree). AudioEngine::shutdown() ends in BASS_Free(), so every
  // worker must be joined before any transport/engine shutdown. This is the only
  // place that waits; see WaveformJobs.
  waveform_jobs_.shutdown();
}

void ChartPreviewPanel::rebuild_waveform(const std::string& music_path) {
  destroy_spectrogram_texture();
  waveform_.clear();
  // Decoding the full song and calculating two FFTs per hop is the expensive
  // part of project opening. Keep it off the Qt/render thread; tick() installs
  // only the newest completed result and performs the Vulkan upload there.
  // restart() cancels (never joins) any decode still running for a previous song.
  const bool decode = !music_path.empty() && transport_.audio().has_music();
  (void)waveform_jobs_.restart(decode ? music_path : std::string{});
}

void ChartPreviewPanel::collect_ready_waveforms() {
  wds::audio::WaveformOverview decoded;
  if (waveform_jobs_.poll(decoded)) {
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
  } else {
    preview_.textures().bind_identity(spectrogram_texture_.id, "ui.spectrogram");
  }
}

void ChartPreviewPanel::reset_playback() {
  if (!ready_) {
    return;
  }
  transport_.request_pause();
  request_seek_ms(transport_.chart_start_ms());
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

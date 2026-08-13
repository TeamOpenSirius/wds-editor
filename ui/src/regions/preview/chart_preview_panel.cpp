#include "wds/ui/regions/preview/chart_preview_panel.hpp"

#include "wds/renderer/log.hpp"

#include <wds/interaction/font_atlas.hpp>
#include <wds/interaction/theme.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace wds::ui {
namespace {

constexpr int kFallbackDisplayHz = 60;

// Body (Md/Gutter) and tip sizes are baked separately so each draw stays near 1:1.
float ui_font_body_bake_px(float tier) {
  namespace th = wds::interaction::theme;
  const float logical = std::max(th::kFontSizeMd, th::kFontSizeGutter);
  return std::max(logical * std::max(tier, 1.0f), 16.0f);
}

float ui_font_tip_bake_px(float tier) {
  namespace th = wds::interaction::theme;
  return std::max(th::kFontSizeTooltip * std::max(tier, 1.0f), 12.0f);
}

// Mild coverage sharpen (≈a^1.2) for tiers ≤1.5; full a² above that.
bool ui_font_mild_sharpen(float tier) { return tier <= 1.5f + 0.001f; }

// Prefer the monitor that currently owns the window (fullscreen or windowed).
GLFWmonitor* monitor_for_window(GLFWwindow* window) {
  if (window == nullptr) {
    return glfwGetPrimaryMonitor();
  }
  if (GLFWmonitor* exclusive = glfwGetWindowMonitor(window)) {
    return exclusive;
  }
  int wx = 0;
  int wy = 0;
  int ww = 0;
  int wh = 0;
  glfwGetWindowPos(window, &wx, &wy);
  glfwGetWindowSize(window, &ww, &wh);
  const int cx = wx + ww / 2;
  const int cy = wy + wh / 2;

  int count = 0;
  GLFWmonitor** monitors = glfwGetMonitors(&count);
  for (int i = 0; i < count; ++i) {
    int mx = 0;
    int my = 0;
    glfwGetMonitorPos(monitors[i], &mx, &my);
    const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
    if (mode == nullptr) {
      continue;
    }
    if (cx >= mx && cx < mx + mode->width && cy >= my && cy < my + mode->height) {
      return monitors[i];
    }
  }
  return glfwGetPrimaryMonitor();
}

int display_refresh_hz(GLFWwindow* window) {
  GLFWmonitor* monitor = monitor_for_window(window);
  if (monitor == nullptr) {
    return kFallbackDisplayHz;
  }
  const GLFWvidmode* mode = glfwGetVideoMode(monitor);
  if (mode == nullptr || mode->refreshRate <= 0) {
    return kFallbackDisplayHz;
  }
  return mode->refreshRate;
}

}  // namespace

ChartPreviewPanel::~ChartPreviewPanel() { shutdown(); }

void ChartPreviewPanel::warm_ui_font_glyphs() {
  auto& font = wds::interaction::FontAtlas::instance();
  // Optional startup warm only — paint/measure already ensure_glyphs on demand
  // from the full bundled face (no subset sync required for new UI text).
  font.ensure_glyphs(
      "功能区转换音符音乐延迟谱面选择可见范围拍内分割"
      "流速音乐音效静音谱面播放速度"
      "打开工程保存导入谱面（只读）导出编辑器设置导入音乐撤销重做"
      "停止播放后停在当前时间音符默认对齐分割线轨道"
      "分割轨道数分割线外观节奏信息编辑拍号取消确认"
      "深色材质设置"
      "打开WDS工程保存为导入官方谱面导出官方谱面"
      "WDS Editor"
      "转换为"
      "导出整个项目仅导出当前谱面"
      "选择导出目录"
      "导出冲突目标目录存在同名文件是否覆盖将跳过冲突文件"
      "文件音频输入快捷键宽快捷键设置"
      "一档二档三档四档五档六档"
      "播放暂停打开保存撤销重做复制粘贴镜像中心上移下移左移右移删除选中切换全屏宽度播放速度"
      "在当前位置暂停在开始播放位置暂停"
      "按下快捷键"
      "导入谱面时自动转换（实验性）"
      "关闭体音效播放"
      "反转时间轴滚轮方向时间轴滚轮速度"
      "反转滚轮调节可见范围大小方向"
      "创建新谱面添加已有谱面添加谱面"
      "未保存的更改当前项目有未保存的更改是否保存"
      "不保存"
      "格式官方"
      "就绪已新建工程已打开工程已保存工程已取消保存失败打开失败导入失败导出失败"
      "只读预览无法保存无法导出无法添加谱面音乐未加载可重新导入"
      "已导入音乐已导入官方谱面已导入官方曲包已导入SUS并转换为可编辑工程内存未绑定文件"
      "需要为未绑定谱面指定路径没有可写入的工程或谱面目录不可用存在冲突且未覆盖"
      "开启SUS自动转换后导入SUS可编辑官方预览"
      "：；（）、，。！？“”‘’—…·％");
}

bool ChartPreviewPanel::bake_ui_font(float body_px, float tip_px, bool mild_sharpen) {
  auto& font = wds::interaction::FontAtlas::instance();
  bool font_ok = false;
  if (!ui_font_path_.empty()) {
    font_ok = font.bake_font_file(ui_font_path_, body_px, tip_px, mild_sharpen);
  }
  if (!font_ok) {
    font_ok = font.bake_system_font(body_px, tip_px, mild_sharpen);
  }
  if (!font_ok || font.pixels() == nullptr) {
    return false;
  }
  warm_ui_font_glyphs();
  if (ui_font_texture_) {
    preview_.vulkan().destroy_texture(ui_font_texture_.id);
    ui_font_texture_ = {};
  }
  ui_font_texture_ = preview_.vulkan().create_texture_rgba(
      font.pixels(), font.atlas_width(), font.atlas_height(), /*nearest=*/true);
  font.set_gpu_texture(ui_font_texture_);
  font.clear_pixels_dirty();
  return static_cast<bool>(ui_font_texture_);
}

bool ChartPreviewPanel::ensure_ui_font_scale() {
  if (!ready_) return false;
  namespace th = wds::interaction::theme;
  const float tier = th::content_scale_tier();
  if (std::abs(tier - font_bake_tier_) < 0.001f) {
    return false;
  }
  if (!bake_ui_font(ui_font_body_bake_px(tier), ui_font_tip_bake_px(tier),
                    ui_font_mild_sharpen(tier))) {
    return false;
  }
  font_bake_tier_ = tier;
  return true;
}

bool ChartPreviewPanel::finish_initialize(GLFWwindow* window,
                                          const wds::renderer::PreviewVisualConfig& visual,
                                          const std::string& ui_font_path) {
  last_init_error_.clear();
  window_ = window;
  if (!transport_.initialize(visual.effects_directory, visual.bgm_path)) {
    last_init_error_ =
        "音频初始化失败（BASS / effects：" + visual.effects_directory + "）";
    std::fprintf(stderr, "ChartPreviewPanel: audio init failed\n");
    window_ = nullptr;
    return false;
  }

  if (!preview_.initialize(window, visual)) {
    // Distinguish the common CI libpng header/dylib skew (skins) from Vulkan.
    last_init_error_ =
        "预览初始化失败（Vulkan 或 skins PNG）。skins=" + visual.skins_directory +
        " — 若 stderr 出现 libpng version mismatch / png_create_read_struct "
        "failed，说明程序链到了错误的 libpng，请重装完整程序包";
    std::fprintf(stderr, "ChartPreviewPanel: preview init failed\n");
    transport_.shutdown();
    window_ = nullptr;
    return false;
  }
  preview_.attach_audio(&transport_.audio());

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

  ui_font_path_ = ui_font_path;
  namespace th = wds::interaction::theme;
  font_bake_tier_ = th::content_scale_tier();
  // Dual body+tip bake at logical×tier so Md and Tooltip each stay near 1:1.
  if (!bake_ui_font(ui_font_body_bake_px(font_bake_tier_), ui_font_tip_bake_px(font_bake_tier_),
                    ui_font_mild_sharpen(font_bake_tier_))) {
    std::fprintf(stderr, "ChartPreviewPanel: UI font bake failed\n");
  }

  transport_.request_seek_ms(0);
  ready_ = true;
  return true;
}

bool ChartPreviewPanel::initialize(GLFWwindow* window,
                                   const wds::renderer::PreviewVisualConfig& visual,
                                   const std::string& chart_path,
                                   const std::string& music_config_path,
                                   const std::string& ui_font_path) {
  if (ready_) {
    return true;
  }
  if (!finish_initialize(window, visual, ui_font_path)) {
    return false;
  }
  if (!chart_path.empty()) {
    if (!load_chart(chart_path, music_config_path)) {
      std::fprintf(stderr, "ChartPreviewPanel: falling back to empty chart\n");
      seed_empty_chart();
    }
  } else {
    seed_empty_chart();
  }
  WDS_LOG("ChartPreviewPanel ready notes=%zu\n", engine_.document().notes().size());
  return true;
}

bool ChartPreviewPanel::initialize_empty(GLFWwindow* window,
                                         const wds::renderer::PreviewVisualConfig& visual,
                                         const std::string& ui_font_path) {
  return initialize(window, visual, {}, {}, ui_font_path);
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
  preview_.shutdown();
  transport_.shutdown();
  window_ = nullptr;
  ready_ = false;
}

int64_t ChartPreviewPanel::display_frame_lead_us() const noexcept {
  const int hz = display_refresh_hz(window_);
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
  preview_.geometry().set_panel_rect(x, y, width, height);
}

void ChartPreviewPanel::sync_ui_font_texture() {
  if (!ready_) return;
  ensure_ui_font_scale();
  auto& font = wds::interaction::FontAtlas::instance();
  if (!font.pixels_dirty() || font.pixels() == nullptr) return;
  // Retire the previous atlas instead of destroying it immediately. Earlier
  // DrawBatches in this frame may still reference that TextureId through submit.
  if (ui_font_texture_) {
    retired_font_textures_.push_back(ui_font_texture_);
    ui_font_texture_ = {};
  }
  ui_font_texture_ = preview_.vulkan().create_texture_rgba(
      font.pixels(), font.atlas_width(), font.atlas_height(), /*nearest=*/true);
  font.set_gpu_texture(ui_font_texture_);
  font.clear_pixels_dirty();
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

void ChartPreviewPanel::set_note_speed(double speed) {
  speed = std::max(1.0, speed);
  auto visual = preview_.config();
  visual.note_speed = static_cast<float>(speed);
  preview_.set_config(visual);

  auto core_cfg = engine_.preview_config();
  core_cfg.note_speed = speed;
  core_cfg.note_approach_seconds = visual.appear_time();
  engine_.set_preview_config(core_cfg);
}

bool ChartPreviewPanel::load_music(const std::string& music_path, bool preserve_playback) {
  if (!ready_) {
    return false;
  }
  const auto effects = preview_.config().effects_directory;
  const bool was_playing = preserve_playback && transport_.playing();
  const int64_t pos = preserve_playback ? transport_.committed_ms() : 0;
  transport_.shutdown();
  auto restore_transport = [&](const std::string& path) -> bool {
    if (!transport_.initialize(effects, path)) {
      return false;
    }
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
    return music_path.empty();
  }
  // Non-empty path that failed to decode still leaves a healthy engine (no BGM).
  if (!music_path.empty() && !transport_.audio().has_music()) {
    return false;
  }
  if (!preserve_playback) {
    // load_chart / publish_snapshot may still use the previous SeekableClock time
    // until the next Transport poll — snap the engine immediately.
    engine_.seek(0);
  }
  return true;
}

void ChartPreviewPanel::reset_playback() {
  if (!ready_) {
    return;
  }
  transport_.request_pause();
  transport_.request_seek_ms(0);
  engine_.seek(0);
}

bool ChartPreviewPanel::load_chart(const std::string& chart_path,
                                   const std::string& music_config_path) {
  const auto result = engine_.load_official_from_file(chart_path, music_config_path);
  if (result.error != wds::chart_editor::SerializeError::Ok) {
    std::fprintf(stderr, "load official chart failed: %s (%s)\n", chart_path.c_str(),
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

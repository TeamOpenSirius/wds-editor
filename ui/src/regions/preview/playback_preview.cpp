#include "wds/ui/regions/preview/playback_preview.hpp"
#include "wds/renderer/log.hpp"

#include <wds/chart_render/note_draw_order.hpp>
#include <wds/chart_render/note_strips.hpp>
#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/ui/note_skin_mapping.hpp>
#include <wds/ui/regions/edit/edit_gutters.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <vector>

namespace wds::ui {

using wds::renderer::DrawBatch;
using wds::renderer::PreviewVisualConfig;
using wds::renderer::Quad;
using wds::renderer::SkinCatalog;
using wds::renderer::StageGeometry;
using wds::renderer::TextureCache;
using wds::renderer::TextureInfo;
using wds::renderer::TextureId;
using wds::renderer::Vec2;
using wds::renderer::VulkanRenderer;
using wds::renderer::add_sliced_note;

namespace {

using wds::chart_editor::NoteType;
using wds::chart_editor::PreviewNoteInstance;
using wds::chart_editor::PreviewNoteVisualState;
using wds::chart_editor::PreviewSnapshot;
using wds::chart_editor::is_hold_body;
using wds::chart_editor::is_hold_mid_star;
using wds::chart_editor::is_hold_start;
using wds::chart_editor::is_hold_with_tail;
using wds::chart_editor::is_scratch_hold_body;
using wds::chart_editor::is_split_lane_gimmick;

// Prefer sub-ms clock so notes do not stair-step on floored timeline_ms.
double preview_now_sec(const PreviewSnapshot& snapshot) noexcept {
  if (snapshot.timeline_us != 0 || snapshot.timeline_ms == 0) {
    return static_cast<double>(snapshot.timeline_us) / 1'000'000.0;
  }
  return static_cast<double>(snapshot.timeline_ms) / 1000.0;
}

using NoteSprites = wds::ui::NoteSprites;
using wds::ui::sprites_for;

int32_t scale_boundary(int32_t boundary_12, int32_t lane_count) {
  if (lane_count == 12) {
    return boundary_12;
  }
  return boundary_12 * lane_count / 12;
}

// Tint hit VFX to match the on-screen note family (not Sonolus particle name colors).
struct EffectTint {
  float r, g, b;
};

EffectTint tint_for(NoteType type) {
  switch (type) {
    case NoteType::Critical:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      // CriticalHold heads use Critical VFX (not the pink/blue head sprite tint).
      return {1.0f, 0.82f, 0.25f};
    case NoteType::HoldStart:
    case NoteType::Hold:
    case NoteType::CriticalHold:  // body/end HoldBomb blue
    case NoteType::NontailHold:
    case NoteType::NontailCriticalHold:
    case NoteType::BlueTap:
      return {0.25f, 0.85f, 1.0f};  // blue hold / tap
    case NoteType::ScratchHoldStart:  // pink NormalNote head, but ScratchBomb purple
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::Flick:
    case NoteType::ScratchSound:
      return {0.85f, 0.35f, 1.0f};  // purple scratch family
    case NoteType::Sound:
      return {0.35f, 0.95f, 0.55f};  // green tick
    case NoteType::HoldEighth:
      return {0.25f, 0.85f, 1.0f};
    case NoteType::Normal:
    default:
      return {1.0f, 0.35f, 0.4f};  // red normal
  }
}

float ease_in_sine(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return 1.0f - std::cos(x * 3.14159265f * 0.5f);
}

uint64_t note_span_key(int64_t start_ms, int32_t lane, int32_t width) noexcept {
  return (static_cast<uint64_t>(static_cast<uint32_t>(start_ms)) << 32) |
         (static_cast<uint64_t>(static_cast<uint16_t>(lane)) << 16) |
         static_cast<uint16_t>(width);
}

// Per-frame indexes so hit FX / SFX stay O(n) instead of O(n²).
struct PreviewLookup {
  std::vector<const PreviewNoteInstance*> mid_stars;
  std::vector<const PreviewNoteInstance*> duration_holds;
  std::unordered_set<uint64_t> duration_hold_heads;
};

PreviewLookup build_preview_lookup(const PreviewSnapshot& snapshot) {
  PreviewLookup out;
  out.mid_stars.reserve(8);
  out.duration_holds.reserve(8);
  for (const auto& note : snapshot.notes) {
    if (is_hold_mid_star(note.note_type)) {
      out.mid_stars.push_back(&note);
      continue;
    }
    if (is_hold_body(note.note_type) && note.end_ms > note.start_ms) {
      out.duration_holds.push_back(&note);
      out.duration_hold_heads.insert(note_span_key(note.start_ms, note.lane, note.width));
    }
  }
  return out;
}

}  // namespace

PlaybackPreviewView::~PlaybackPreviewView() { shutdown(); }

bool PlaybackPreviewView::initialize(GLFWwindow* window, const PreviewVisualConfig& config) {
  shutdown();
  config_ = config;
  geometry_.configure(config_);
  WDS_LOG("PlaybackPreviewView::initialize skins=%s lanes=%d speed=%.2f\n",
          config_.skins_directory.c_str(), config_.lane_count, config_.note_speed);

  if (window == nullptr || !glfwVulkanSupported()) {
    WDS_LOG("GLFW Vulkan not supported\n");
    return false;
  }

  wds::renderer::VulkanHostSurface host;
  uint32_t ext_count = 0;
  const char** exts = glfwGetRequiredInstanceExtensions(&ext_count);
  if (exts == nullptr || ext_count == 0) {
    WDS_LOG("glfwGetRequiredInstanceExtensions failed\n");
    return false;
  }
  host.instance_extensions.assign(exts, exts + ext_count);
  host.create_surface = [window](VkInstance instance) -> VkSurfaceKHR {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return surface;
  };
  host.framebuffer_size = [window](int* width, int* height) {
    glfwGetFramebufferSize(window, width, height);
  };
#if defined(_WIN32)
  host.win32_monitor = [window]() -> HMONITOR {
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr) {
      return nullptr;
    }
    return ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  };
#endif

  vulkan_.set_preferred_msaa(config_.msaa_samples);
  if (!vulkan_.create(host)) {
    WDS_LOG("vulkan_.create failed\n");
    return false;
  }
  textures_.set_renderer(&vulkan_);
  if (!skin_.load(textures_, config_.skins_directory)) {
    WDS_LOG("skin_.load failed dir=%s\n", config_.skins_directory.c_str());
    shutdown();
    return false;
  }
  // SFX comes from Transport::audio via attach_audio().
  sfx_mono_us_ = -1;
  sfx_position_generation_ = std::numeric_limits<uint64_t>::max();
  hit_sfx_played_.clear();
  sfx_was_playing_ = false;

  geometry_.resize(vulkan_.framebuffer_width(), vulkan_.framebuffer_height());
  ready_ = true;
  WDS_LOG("PlaybackPreviewView ready fb=%dx%d\n", vulkan_.framebuffer_width(),
          vulkan_.framebuffer_height());
  return true;
}

void PlaybackPreviewView::attach_audio(wds::audio::AudioEngine* audio) noexcept {
  hit_sfx_.attach(audio);
}

void PlaybackPreviewView::shutdown() {
  hit_sfx_.detach();
  sfx_mono_us_ = -1;
  sfx_position_generation_ = std::numeric_limits<uint64_t>::max();
  hit_sfx_played_.clear();
  sfx_was_playing_ = false;
  // In-flight frames still reference skin/atlas images — wait before free.
  if (vulkan_.ready()) {
    vulkan_.device_wait_idle();
  }
  textures_.clear();
  skin_ = SkinCatalog{};
  batch_.clear();
  additive_batch_.clear();
  notes_draw_indices_.clear();
  note_draw_order_.clear();
  notes_order_revision_ = std::numeric_limits<uint64_t>::max();
  vulkan_.destroy();
  ready_ = false;
}

void PlaybackPreviewView::set_config(const PreviewVisualConfig& config) {
  config_ = config;
  geometry_.configure(config_);
}

void PlaybackPreviewView::resize(int framebuffer_width, int framebuffer_height) {
  if (!ready_) {
    return;
  }
  vulkan_.resize(framebuffer_width, framebuffer_height);
  geometry_.resize(vulkan_.framebuffer_width(), vulkan_.framebuffer_height());
}

void PlaybackPreviewView::sync_hit_sfx(const wds::chart_editor::PreviewSnapshot& snapshot) {
  update_hit_sfx(snapshot);
}

void PlaybackPreviewView::render(const PreviewSnapshot& snapshot, const DrawBatch* ui_overlay,
                                 TextureId ui_solid_texture, const DrawBatch* modal_overlay,
                                 const DrawBatch* modal_chrome) {
  if (!ready_) {
    return;
  }

  // Hit SFX is armed in sync_hit_sfx() from ChartPreviewPanel::tick — before
  // Transport::start_pending_music() so POS syncs land on a still-paused stream.

  batch_.clear();
  additive_batch_.clear();
  batch_.reserve_quads(64 + snapshot.notes.size() * 4);
  additive_batch_.reserve_quads(16 + snapshot.notes.size() * 4);

  // Depth write is disabled; later draw calls win. DrawBatch::clear() keeps sticky
  // per-texture bucket indices, so "merge later in this function" does NOT move a
  // texture later in the frame — use draw_frame's post_overlay for modals instead.
  auto merge_overlay = [&](const DrawBatch* overlay, bool solid_pass) {
    if (overlay == nullptr) {
      return;
    }
    for (const auto& bucket : overlay->buckets) {
      if (bucket.vertices.empty()) {
        continue;
      }
      const bool is_solid = ui_solid_texture != wds::renderer::kInvalidTextureId &&
                            bucket.texture == ui_solid_texture;
      if (is_solid != solid_pass) {
        continue;
      }
      auto& dst = batch_.bucket_for(bucket.texture);
      dst.vertices.insert(dst.vertices.end(), bucket.vertices.begin(), bucket.vertices.end());
    }
  };

  merge_overlay(ui_overlay, true);
  draw_stage(batch_, snapshot);
  draw_split_lanes(batch_, additive_batch_, snapshot);
  draw_concurrent_lines(batch_, snapshot);
  draw_notes(batch_, snapshot);
  draw_hit_effects(additive_batch_, snapshot);
  draw_timing_effect(batch_, snapshot);
  draw_combo(batch_, snapshot);
  merge_overlay(ui_overlay, false);

  const DrawBatch* post =
      (modal_overlay != nullptr && modal_overlay->vertex_count() > 0) ? modal_overlay : nullptr;
  const DrawBatch* post2 =
      (modal_chrome != nullptr && modal_chrome->vertex_count() > 0) ? modal_chrome : nullptr;
  // Clip additive hit FX (BomFlare etc.) to the preview panel so soft rings cannot
  // bleed into the settings/seek strip below.
  wds::renderer::VulkanRenderer::ScissorRect add_scissor{};
  const wds::renderer::VulkanRenderer::ScissorRect* add_scissor_ptr = nullptr;
  int px = 0, py = 0, pw = 0, ph = 0;
  if (geometry_.panel_framebuffer_rect(px, py, pw, ph)) {
    add_scissor = {px, py, pw, ph};
    add_scissor_ptr = &add_scissor;
  }
  vulkan_.draw_frame(batch_, geometry_.screen(), 0.05f, 0.05f, 0.08f, &additive_batch_, post,
                     post2, add_scissor_ptr);
}

void PlaybackPreviewView::draw_stage(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  // Cover-fit ingame_bg into the full preview panel (gutters included). Quad == panel so
  // overflow never paints the edit/settings columns; UVs crop the texture instead.
  if (skin_.ingame_background) {
    const auto& p = geometry_.panel();
    const Quad bg{{p.l, p.b}, {p.l, p.t}, {p.r, p.t}, {p.r, p.b}};
    const auto& tex = skin_.ingame_background;
    const float tw = std::max(1.0f, static_cast<float>(tex.width));
    const float th = std::max(1.0f, static_cast<float>(tex.height));
    const float tex_aspect = tw / th;
    const float panel_aspect = std::max(p.w, 1e-6f) / std::max(p.h, 1e-6f);
    float u0 = tex.u0;
    float u1 = tex.u1;
    float v0 = tex.v0;
    float v1 = tex.v1;
    if (tex_aspect > panel_aspect) {
      const float visible = panel_aspect / tex_aspect;
      const float mid = 0.5f * (tex.u0 + tex.u1);
      const float half = 0.5f * (tex.u1 - tex.u0) * visible;
      u0 = mid - half;
      u1 = mid + half;
    } else {
      const float visible = tex_aspect / panel_aspect;
      const float mid = 0.5f * (tex.v0 + tex.v1);
      const float half = 0.5f * (tex.v1 - tex.v0) * visible;
      v0 = mid - half;
      v1 = mid + half;
    }
    batch.add_quad(tex.id, bg, -0.98f, 1.0f, u0, v0, u1, v1);
  }

  const Quad stage = geometry_.stage_quad();
  // STAGE_COVER fades out/in with split appear/disappear (same windows as Sirius lines).
  float cover_alpha = 1.0f;
  for (const auto& split : snapshot.split_lanes) {
    if (split.should_show && split.split_count > 0) {
      cover_alpha = std::min(cover_alpha, split.stage_cover_alpha);
    }
  }
  if (skin_.stage && cover_alpha > 0.001f) {
    batch.add_sprite(skin_.stage, stage, -0.9f, cover_alpha);
  }
  if (skin_.stage_background) {
    batch.add_sprite(skin_.stage_background, stage, -0.85f, config_.stage_opacity);
  }

  const auto& j = geometry_.judgeline();
  Quad jq{{j.lb_x, j.lb_y}, {j.lt_x, j.lt_y}, {j.rt_x, j.rt_y}, {j.rb_x, j.rb_y}};
  batch.add_sprite(skin_.judgeline, jq, -0.7f, 1.0f);

  // Decorative only (official ~0.45). Notes hard-clip at spawn_clip_percent().
  draw_hidden_line(batch);
}

void PlaybackPreviewView::draw_hidden_line(DrawBatch& batch) {
  if (!skin_.hidden_line) {
    return;
  }
  const float p = geometry_.hidden_line_center_percent();
  const float a = std::clamp(config_.hidden_line_alpha, 0.0f, 1.0f);
  batch.add_sprite(skin_.hidden_line, geometry_.hidden_line_quad(p), -0.55f, a);
}

float PlaybackPreviewView::spawn_clip_percent() const noexcept {
  // Judgeline-side edge of the Hidden Line band — draw only the part below the bar.
  return geometry_.hidden_line_center_percent() + geometry_.hidden_line_half_percent();
}

void PlaybackPreviewView::draw_split_lanes(DrawBatch& batch, DrawBatch& additive,
                                           const PreviewSnapshot& snapshot) {
  if (snapshot.split_lanes.empty()) {
    return;
  }
  const auto& split = snapshot.split_lanes.back();
  if (!split.should_show || split.split_count <= 0) {
    return;
  }

  // Same color id / slot tint as the edit-region split lines (split_slot_color).
  const int32_t color_id = split.scratch_length != 0 ? split.scratch_length : 1;
  // Light preview: white soft plate × LineColor (official SpriteRenderer). Never swap
  // to Transform wipe sprites (Sonolus-only); that caused a hard pop.

  auto fallback_tex = [&]() -> const TextureInfo* {
    return skin_.split_line_1 ? &skin_.split_line_1
                              : (skin_.split_line_2 ? &skin_.split_line_2 : nullptr);
  };

  const int32_t n = config_.lane_count;
  std::vector<int32_t> mids;
  split_boundaries_12(split.split_count, mids);

  // Official fadeIn: scale.y grow from judgeline → tip ⇒ visible [p0, p1].
  const float p0 = split.split_percent_start;
  const float p1 = split.split_percent_end;
  if (p1 - p0 < 1e-4f) {
    return;
  }

  const double now = preview_now_sec(snapshot);
  const float period = std::max(config_.split_line_pulse_period, 1e-3f);
  const float band_half = std::max(config_.split_line_pulse_band, 1e-4f);
  const float dip = std::clamp(config_.split_line_pulse_dip, 0.0f, 1.0f);
  // More-transparent band travels bottom (percent=1) → tip (percent=0).
  float band_travel = std::fmod(static_cast<float>(now), period) / period;
  if (band_travel < 0.0f) {
    band_travel += 1.0f;
  }
  const float band_center = 1.0f - band_travel;
  const float base_a =
      std::clamp(split.split_line_alpha * config_.split_line_opacity, 0.0f, 1.0f);
  const float tip_whiten = std::clamp(config_.split_line_tip_whiten, 0.05f, 1.0f);
  const float tip_glow = std::clamp(config_.split_line_tip_glow, 0.0f, 1.0f);

  // Official soft white plate; colored skins only supply LineColor tint.
  const TextureInfo* plate =
      skin_.soft_split_line ? &skin_.soft_split_line : fallback_tex();
  if (plate == nullptr || !*plate) {
    return;
  }

  // Dense segments + per-corner GPU lerp (alpha/color) so tip white & pulse aren't stepped.
  constexpr int kSegs = 96;
  const float tip_span = std::max((p1 - p0) * tip_whiten, 1e-4f);

  auto pulse_mul = [&](float percent) {
    // Raised-cosine lobe over ±band_half: soft edges, shallow center (higher opacity).
    const float x = std::abs(percent - band_center) / band_half;
    if (x >= 1.0f) {
      return 1.0f;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float w = 0.5f * (1.0f + std::cos(x * kPi));  // 1 at center → 0 at edge
    return 1.0f - dip * w;
  };
  auto tip_whiten_t = [&](float percent) {
    // 1 at growing tip (p0) → 0 after tip_span toward judgeline.
    float t = 1.0f - (percent - p0) / tip_span;
    t = std::clamp(t, 0.0f, 1.0f);
    // Long soft ramp (smoothstep); no extra squaring that shortens the white zone.
    return t * t * (3.0f - 2.0f * t);
  };
  auto tint_at = [&](float lr, float lg, float lb, float tip_t, float& cr, float& cg, float& cb) {
    cr = lr + (1.0f - lr) * tip_t;
    cg = lg + (1.0f - lg) * tip_t;
    cb = lb + (1.0f - lb) * tip_t;
  };

  auto draw_slot = [&](int32_t memory_slot, int32_t boundary_after_lane, bool end_line) {
    const auto slot_c = split_slot_color(color_id, memory_slot, &skin_);
    const float lr = slot_c.r;
    const float lg = slot_c.g;
    const float lb = slot_c.b;
    for (int i = 0; i < kSegs; ++i) {
      const float t0 = static_cast<float>(i) / static_cast<float>(kSegs);
      const float t1 = static_cast<float>(i + 1) / static_cast<float>(kSegs);
      const float sa = p0 + (p1 - p0) * t0;
      const float sb = p0 + (p1 - p0) * t1;
      // split_line_quad: lt/rt = percent_start (sa), lb/rb = percent_end (sb).
      const float tip_a = tip_whiten_t(sa);
      const float tip_b = tip_whiten_t(sb);
      // Tip→body is RGB mix only at full base alpha. Pulse must not dig a transparent
      // trough through that blend — gate the dip by (1 - tip_t).
      const float mul_a = 1.0f + (pulse_mul(sa) - 1.0f) * (1.0f - tip_a);
      const float mul_b = 1.0f + (pulse_mul(sb) - 1.0f) * (1.0f - tip_b);
      const float a_start = base_a * mul_a;
      const float a_end = base_a * mul_b;
      if (a_start < 0.008f && a_end < 0.008f) {
        continue;
      }
      const Quad q = end_line ? geometry_.split_end_line_quad(sa, sb)
                              : geometry_.split_line_quad(boundary_after_lane, sa, sb);

      float r0 = 1.0f, g0 = 1.0f, b0 = 1.0f;
      float r1 = 1.0f, g1 = 1.0f, b1 = 1.0f;
      tint_at(lr, lg, lb, tip_a, r0, g0, b0);
      tint_at(lr, lg, lb, tip_b, r1, g1, b1);
      batch.add_quad_corners(plate->id, q, -0.6f, a_end, a_end, a_start, a_start, plate->u0,
                             plate->v0, plate->u1, plate->v1, r1, g1, b1, r1, g1, b1, r0, g0, b0,
                             r0, g0, b0);
      // Soft tip bloom only near pure white; keep out of the color-mix midsection.
      const float glow_w_a = std::max(0.0f, tip_a - 0.55f) / 0.45f;
      const float glow_w_b = std::max(0.0f, tip_b - 0.55f) / 0.45f;
      const float glow_a = tip_glow * glow_w_a;
      const float glow_b = tip_glow * glow_w_b;
      if (tip_glow > 0.01f && (glow_a > 0.01f || glow_b > 0.01f)) {
        additive.add_quad_corners(plate->id, q, -0.55f, glow_b, glow_b, glow_a, glow_a, plate->u0,
                                  plate->v0, plate->u1, plate->v1, 1.0f, 1.0f, 1.0f);
      }
      // Mild additive body beam so overlaps with the judgeline brighten (official soft look).
      const float body_glow = std::clamp(config_.split_line_body_glow, 0.0f, 1.0f);
      if (body_glow > 0.01f) {
        const float bg_a = body_glow * mul_a * (1.0f - tip_a * 0.65f);
        const float bg_b = body_glow * mul_b * (1.0f - tip_b * 0.65f);
        if (bg_a > 0.008f || bg_b > 0.008f) {
          additive.add_quad_corners(plate->id, q, -0.58f, bg_b, bg_b, bg_a, bg_a, plate->u0,
                                    plate->v0, plate->u1, plate->v1, r1, g1, b1, r1, g1, b1, r0,
                                    g0, b0, r0, g0, b0);
        }
      }
    }
  };

  draw_slot(0, -1, false);
  int32_t mid_slot = 1;
  for (int32_t b12 : mids) {
    draw_slot(mid_slot++, scale_boundary(b12, n), false);
  }
  draw_slot(split.split_count, 0, true);
}

void PlaybackPreviewView::draw_concurrent_lines(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  if (!skin_.sync_line) {
    return;
  }
  const double now = preview_now_sec(snapshot);
  for (const auto& line : snapshot.concurrent_lines) {
    const double beat = static_cast<double>(line.milliseconds) / 1000.0;
    const float p = geometry_.note_percent(beat, now);
    if (p <= 0.0f || p > 1.05f) {
      continue;
    }
    if (p < spawn_clip_percent()) {
      continue;
    }
    const int32_t end_lane = line.start_lane + std::max(1, line.width) - 1;
    batch.add_sprite(skin_.sync_line, geometry_.sync_line_quad(line.start_lane, end_lane, p), -0.5f,
                     0.8f);
  }
}

void PlaybackPreviewView::prepare_note_draw_order(const PreviewSnapshot& snapshot) {
  // Rebuild whenever the visible set changes. Snapshot revision only bumps on
  // chart edits — IncrementalPatch grows/shrinks notes while revision stays put,
  // so caching solely on revision drops newly approaching notes (or keeps stale
  // indices after swap-removes).
  if (notes_order_revision_ != snapshot.revision ||
      notes_draw_indices_.size() != snapshot.notes.size()) {
    wds::chart_render::build_draw_order_indices(
        snapshot.notes.size(), notes_draw_indices_,
        [&](size_t i) { return snapshot.notes[i].start_ms; },
        [&](size_t i) { return static_cast<int32_t>(snapshot.notes[i].note_type); });
    notes_order_revision_ = snapshot.revision;
  }
  note_draw_order_.clear();
  note_draw_order_.reserve(snapshot.notes.size());
  for (size_t idx : notes_draw_indices_) {
    if (idx >= snapshot.notes.size()) {
      continue;
    }
    const auto& note = snapshot.notes[idx];
    // Sirius: note despawns at judgment; AutoHit only drives VFX / judge text.
    if (note.visual_state == PreviewNoteVisualState::Approaching ||
        note.visual_state == PreviewNoteVisualState::Holding) {
      note_draw_order_.push_back(&note);
    }
  }
}

void PlaybackPreviewView::draw_notes(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  prepare_note_draw_order(snapshot);
  const auto& order = note_draw_order_;

  const double now = preview_now_sec(snapshot);

  // Official sandwich (depthWrite off → draw order = visual order):
  // hold → all bottoms → all tops → mid-stars → arrows.
  for (const PreviewNoteInstance* note : order) {
    if (is_hold_body(note->note_type) && !is_hold_mid_star(note->note_type)) {
      draw_hold_body(batch, *note, now);
    }
  }
  for (const PreviewNoteInstance* note : order) {
    draw_note_flat_layer(batch, *note, snapshot, /*bottom_layer=*/true);
  }
  for (const PreviewNoteInstance* note : order) {
    draw_note_flat_layer(batch, *note, snapshot, /*bottom_layer=*/false);
  }
  for (const PreviewNoteInstance* note : order) {
    if (note->visual_state == PreviewNoteVisualState::Holding) {
      continue;
    }
    const NoteSprites sprites = sprites_for(skin_, note->note_type);
    if (sprites.is_tick) {
      draw_tick_note(batch, *note, now);
    }
  }
  for (const PreviewNoteInstance* note : order) {
    // Scratch-hold end flick (Sirius ScratchHoldEnd) — arrows at end_ms.
    if (is_scratch_hold_body(note->note_type) && is_hold_with_tail(note->note_type) &&
        (note->visual_state == PreviewNoteVisualState::Approaching ||
         note->visual_state == PreviewNoteVisualState::Holding)) {
      draw_arrows_at(batch, *note, static_cast<double>(note->end_ms) / 1000.0, now, now);
      continue;
    }
    if (note->visual_state == PreviewNoteVisualState::Holding) {
      continue;
    }
    if (is_hold_mid_star(note->note_type)) {
      continue;
    }
    if (is_hold_start(note->note_type)) {
      continue;
    }
    const NoteSprites sprites = sprites_for(skin_, note->note_type);
    if (sprites.is_scratch_family || note->uses_jump_scratch_position ||
        note->note_type == NoteType::Flick) {
      draw_arrows(batch, *note, now, now);
    }
  }
}

void PlaybackPreviewView::draw_note_flat_layer(DrawBatch& batch, const PreviewNoteInstance& note,
                                               const PreviewSnapshot& snapshot,
                                               bool bottom_layer) {
  const double now = preview_now_sec(snapshot);
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  if (sprites.is_tick) {
    return;
  }

  // Hold bodies never invent a start head — only a real HoldStart* note draws one.
  // Tailed holds still draw the approaching end cap (HoldEnd / ScratchHoldEnd).
  if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
    const double end = static_cast<double>(note.end_ms) / 1000.0;
    if (is_hold_with_tail(note.note_type) &&
        (note.visual_state == PreviewNoteVisualState::Approaching ||
         note.visual_state == PreviewNoteVisualState::Holding)) {
      draw_flat_note_at(batch, note, end, now, 0.05f, /*use_jump_lanes=*/true, bottom_layer);
    }
    return;
  }

  // HoldStart* / taps: only while Approaching; Holding means already judged → gone.
  if (note.visual_state == PreviewNoteVisualState::Holding) {
    return;
  }

  if (is_hold_start(note.note_type) || !is_hold_body(note.note_type)) {
    draw_flat_note(batch, note, now, 0.0f, bottom_layer);
  }
}

void PlaybackPreviewView::draw_note(DrawBatch& batch, const PreviewNoteInstance& note,
                                    const PreviewSnapshot& snapshot) {
  // Kept for callers that want a single-note composite (top layer only + tick).
  draw_note_flat_layer(batch, note, snapshot, /*bottom_layer=*/false);
  const double now = preview_now_sec(snapshot);
  if (note.visual_state == PreviewNoteVisualState::Holding) {
    return;
  }
  if (sprites_for(skin_, note.note_type).is_tick) {
    draw_tick_note(batch, note, now);
  }
}

void PlaybackPreviewView::draw_hold_body(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double now_sec) {
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  if (!sprites.connection) {
    return;
  }

  const double start = static_cast<double>(note.start_ms) / 1000.0;
  const double end = static_cast<double>(note.end_ms) / 1000.0;
  float p_near = std::clamp(geometry_.note_percent(start, now_sec), 0.0f, 1.0f);
  float p_far = std::clamp(geometry_.note_percent(end, now_sec), 0.0f, 1.0f);
  if (p_near < p_far) {
    std::swap(p_near, p_far);
  }
  if (p_near <= p_far) {
    return;
  }

  // Sirius drawHoldEighth: 0.8 idle / approaching, 0.85 on odd 0.1s while holding.
  float base_alpha = config_.hold_body_alpha;
  if (note.visual_state == PreviewNoteVisualState::Holding) {
    const int phase = static_cast<int>(std::floor(now_sec / 0.1)) % 2;
    base_alpha = phase == 1 ? config_.hold_body_holding_alpha : config_.hold_body_alpha;
  }

  // Fade only the ribbon tip inside the judgment-line band (p=1 is center).
  // Slightly shorter than the full band so the dissolve stays tight.
  const float band =
      std::max(0.001f, geometry_.judgeline_half_percent() * 2.0f * 0.65f);
  const float fade_lo = 1.0f - band;

  // Hard-clip tip side at the Hidden Line (only draw the part below the bar).
  const float clip_p = spawn_clip_percent();
  if (p_near <= clip_p) {
    return;
  }
  if (p_far < clip_p) {
    p_far = clip_p;
  }

  auto alpha_at = [&](float p) {
    if (p <= fade_lo) return base_alpha;
    return base_alpha * std::clamp(1.0f - (p - fade_lo) / band, 0.0f, 1.0f);
  };

  // Cap size follows flat-note height in NDC (shared border_scale_from_flat_height).
  auto flat_border_scale = [&](float p) {
    const Quad ref = geometry_.note_quad(note.lane, note.end_lane, std::clamp(p, 0.0f, 1.0f));
    const float hx0 = ref.lt.x - ref.lb.x;
    const float hy0 = ref.lt.y - ref.lb.y;
    const float hx1 = ref.rt.x - ref.rb.x;
    const float hy1 = ref.rt.y - ref.rb.y;
    const float dh =
        0.5f * (std::sqrt(hx0 * hx0 + hy0 * hy0) + std::sqrt(hx1 * hx1 + hy1 * hy1));
    return wds::chart_render::border_scale_from_flat_height(dh, skin_);
  };

  // hold_body_quad: lb/rb at percent_near, lt/rt at percent_far.
  // Official HoldLongNotes: SpriteRenderer Sliced, m_Border L/R = 10 on 157-wide art.
  auto emit = [&](float lo, float hi) {
    if (hi <= lo) return;
    add_sliced_note(batch, sprites.connection,
                    geometry_.hold_body_quad(note.lane, note.end_lane, hi, lo),
                    skin_.hold_slice_border_l, skin_.hold_slice_border_r, -0.25f, alpha_at(hi),
                    alpha_at(lo), flat_border_scale(hi), sprites.connection_r,
                    sprites.connection_g, sprites.connection_b);
  };

  if (p_near <= fade_lo) {
    emit(p_far, p_near);
  } else if (p_far >= fade_lo) {
    emit(p_far, p_near);
  } else {
    emit(p_far, fade_lo);
    emit(fade_lo, p_near);
  }
}

void PlaybackPreviewView::draw_flat_note(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double now_sec, float z_bias, bool bottom_layer) {
  draw_flat_note_at(batch, note, static_cast<double>(note.start_ms) / 1000.0, now_sec, z_bias,
                    /*use_jump_lanes=*/false, bottom_layer);
}

void PlaybackPreviewView::draw_flat_note_at(DrawBatch& batch, const PreviewNoteInstance& note,
                                            double beat_sec, double now_sec, float z_bias,
                                            bool use_jump_lanes, bool bottom_layer) {
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  NoteSprites head = sprites;
  // Hold body end caps: shared apply_hold_tail_sprites (scratch purple / hold blue).
  if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
    const bool scratch_tail = is_scratch_hold_body(note.note_type) && use_jump_lanes;
    apply_hold_tail_sprites(head, skin_, scratch_tail);
  }

  TextureInfo layer = bottom_layer ? head.bottom : head.top;
  if (!layer) {
    return;
  }

  const float p = geometry_.note_percent(beat_sec, now_sec);
  // Despawn at judgment (like taps); ease can push visual past 1.0 slightly.
  if (p < -0.05f || p >= 1.0f) {
    return;
  }

  int32_t lane = note.lane;
  int32_t end_lane = note.end_lane;
  if (use_jump_lanes && note.uses_jump_scratch_position) {
    lane = note.jump_scratch_lane_from;
    end_lane = note.jump_scratch_lane_to;
  }

  // Bottom/Top share plane footprint; Unity local Z = height, pinhole-projected.
  const float unity_z = bottom_layer ? config_.note_unity_local_z_bottom
                                     : config_.note_unity_local_z_top;
  const float pc = std::clamp(p, 0.0f, 1.0f);
  const float half = geometry_.note_half_height_percent(lane, pc);
  const float p_tip = pc - half;
  const float p_near = pc + half;
  const float clip_p = spawn_clip_percent();
  // Only the strip below the Hidden Line — hard clip, not whole-note fade.
  if (p_near <= clip_p) {
    return;
  }
  const float p_vis_tip = std::max(p_tip, clip_p);
  Quad q = geometry_.note_span_quad(lane, end_lane, p_near, p_vis_tip, unity_z);
  const float z = z_bias + unity_z - static_cast<float>(beat_sec) * 1e-4f;
  const float alpha = note.is_grayed_out ? 0.55f : 1.0f;
  // Cap scale from full note height so borders don't balloon when clipped.
  const Quad full_q = geometry_.note_quad(lane, end_lane, pc, unity_z);
  const float hx0 = full_q.lt.x - full_q.lb.x;
  const float hy0 = full_q.lt.y - full_q.lb.y;
  const float hx1 = full_q.rt.x - full_q.rb.x;
  const float hy1 = full_q.rt.y - full_q.rb.y;
  const float full_h =
      0.5f * (std::sqrt(hx0 * hx0 + hy0 * hy0) + std::sqrt(hx1 * hx1 + hy1 * hy1));
  const float border_scale =
      wds::chart_render::border_scale_from_flat_height(full_h, skin_);
  // Crop sprite V to the visible fraction (near=lb=v0 … tip=lt=v1).
  const float span = std::max(p_near - p_tip, 1e-5f);
  const float tip_t = (p_vis_tip - p_tip) / span;  // 0=full tip, →1 as tip is clipped away
  const float v_near = layer.v0;
  const float v_far = layer.v0 + (layer.v1 - layer.v0) * (1.0f - tip_t);
  wds::renderer::add_sliced_note_v(batch, layer, q, skin_.note_slice_border_l,
                                   skin_.note_slice_border_r, z, alpha, border_scale, v_near,
                                   v_far);
}

void PlaybackPreviewView::draw_tick_note(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double now_sec) {
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  if (!sprites.tick) {
    return;
  }
  const double beat = static_cast<double>(note.start_ms) / 1000.0;
  const float p = geometry_.note_percent(beat, now_sec);
  if (p < 0.0f || p >= 1.0f) {
    return;
  }
  if (p < spawn_clip_percent()) {
    return;
  }
  // Sirius drawTick: Draw(..., 200000 - beat, 0.5) — above flats, alpha 0.5.
  const float alpha = note.is_grayed_out ? config_.tick_alpha * 0.55f : config_.tick_alpha;
  batch.add_sprite(sprites.tick, geometry_.tick_quad(note.lane, note.end_lane, p), 0.15f, alpha);
}

void PlaybackPreviewView::draw_arrows(DrawBatch& batch, const PreviewNoteInstance& note,
                                      double now_sec, double anim_time_sec) {
  draw_arrows_at(batch, note, static_cast<double>(note.start_ms) / 1000.0, now_sec, anim_time_sec);
}

void PlaybackPreviewView::draw_arrows_at(DrawBatch& batch, const PreviewNoteInstance& note,
                                         double beat_sec, double now_sec, double anim_time_sec) {
  if (!skin_.scratch_arrow) {
    return;
  }
  const float p = geometry_.note_percent(beat_sec, now_sec);
  if (p <= 0.0f || p >= 1.0f) {
    return;
  }
  if (p < spawn_clip_percent()) {
    return;
  }

  int32_t lane = note.lane;
  int32_t end_lane = note.end_lane;
  if (note.uses_jump_scratch_position) {
    lane = note.jump_scratch_lane_from;
    end_lane = note.jump_scratch_lane_to;
  }
  if (end_lane < lane) {
    std::swap(lane, end_lane);
  }

  const float unit = geometry_.content_unit();
  const float w = geometry_.lane_width(lane, p);
  const float w_ref = std::max(geometry_.lane_width(lane, 1.0f), 1e-6f);
  const float multiplier = w / w_ref;
  const Vec2 c1 = geometry_.lane_position(lane, p);
  const Vec2 c2 = geometry_.lane_position(end_lane, p);
  const float W = config_.arrow_width * unit * multiplier;
  const float H = config_.arrow_height * unit * multiplier;
  if (W <= 1e-5f) {
    return;
  }

  // Match note_quad horizontal inset (note_move_length) so arrows sit inside the note.
  const float move = config_.note_move_length * unit * multiplier;
  const float L = c1.x - w * 0.5f + move;
  const float R = c2.x + w * 0.5f - move;
  if (R <= L) {
    return;
  }

  // Animated arrows (ArrowStyle::Animated); sides/density via note_visual_policy.
  wds::chart_render::AnimatedArrowLayoutParams params;
  params.span_left = L;
  params.span_right = R;
  params.arrow_w = W;
  params.scratch_length = note.scratch_length;
  params.sonolus_num =
      w * static_cast<float>(end_lane - lane + 1) * config_.arrow_percent / W;
  params.anim_time_sec = static_cast<float>(anim_time_sec);
  params.arrow_speed = config_.arrow_speed;
  for (const auto& inst : wds::chart_render::layout_animated_scratch_arrows(params)) {
    const float y = inst.flip_x ? c2.y : c1.y;
    const float x0 = inst.x0;
    const float x1 = inst.x1;
    batch.add_sprite(skin_.scratch_arrow,
                     Quad{{x0, y}, {x0, y + H * 0.5f}, {x1, y + H * 0.5f}, {x1, y}}, 0.2f,
                     inst.alpha);
  }
}

void PlaybackPreviewView::draw_hit_effects(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  const double now = preview_now_sec(snapshot);
  const double duration_d = static_cast<double>(config_.effect_duration);
  // BombControllerBase.SimulationHighSpeed — LaneEffectController.OnBomb speeds up every
  // tracked bomb whose CurrentStartMilliseconds differs from the new hit (not lane-based).
  constexpr float kSimHighSpeed = 3.0f;

  struct BombEvt {
    double t0 = 0.0;
    int64_t key_ms = 0;  // CurrentStartMilliseconds
    int32_t lane = 0;
    int32_t end_lane = 0;
    NoteType type = NoteType::Normal;
    int hit_fx_role = 0;
    bool jump_flare = false;
    float z = 0.95f;
  };

  std::vector<BombEvt> evts;
  evts.reserve(snapshot.notes.size() * 2);

  for (const auto& note : snapshot.notes) {
    const bool hold_body = is_hold_body(note.note_type);
    const bool mid_star = is_hold_mid_star(note.note_type);
    const bool with_tail = is_hold_with_tail(note.note_type);

    // Hold body / mid-stars: SFX only (no visual bomb). Heads + tails only below.
    if (!hold_body && !mid_star && !is_split_lane_gimmick(note.gimmick_type)) {
      if (is_hold_start(note.note_type) || note.note_type == NoteType::Normal ||
          note.note_type == NoteType::Critical ||
          note.note_type == NoteType::Flick || note.note_type == NoteType::BlueTap) {
        const double t0 = static_cast<double>(note.start_ms) / 1000.0;
        if (t0 <= now) {
          evts.push_back(BombEvt{t0, note.start_ms, note.lane, note.end_lane, note.note_type, 0,
                                 false, 0.95f});
        }
      }
    }

    // Tail: HoldBomb; JumpScratch ends also get ScratchBomb flare (same as Flick).
    if (with_tail && note.end_ms > note.start_ms) {
      const double t0 = static_cast<double>(note.end_ms) / 1000.0;
      if (t0 <= now) {
        const bool jump_flare =
            note.uses_jump_scratch_position || is_jump_scratch(note.gimmick_type);
        int32_t fx_lane = note.lane;
        int32_t fx_end = note.end_lane;
        if (jump_flare) {
          fx_lane = note.jump_scratch_lane_from;
          fx_end = note.jump_scratch_lane_to;
        }
        evts.push_back(BombEvt{t0, note.end_ms, fx_lane, fx_end, note.note_type, 1, jump_flare,
                               0.96f});
      }
    }
  }

  for (const auto& evt : evts) {
    const double wall_age = now - evt.t0;
    // Retail keeps the pooled instance until FireTime (wall-clock _animationTime).
    if (wall_age < 0.0 || wall_age >= duration_d) {
      continue;
    }

    // Earliest later bomb with a different startMs → ChangeSpeed(3) (TrySpeedUp).
    double speedup_at = std::numeric_limits<double>::infinity();
    for (const auto& other : evts) {
      if (other.key_ms == evt.key_ms) {
        continue;  // same-ms chord: stay 1x
      }
      if (other.t0 > evt.t0 && other.t0 <= now && other.t0 < speedup_at) {
        speedup_at = other.t0;
      }
    }

    double effective_age = wall_age;
    if (speedup_at <= now) {
      effective_age = (speedup_at - evt.t0) + (now - speedup_at) * static_cast<double>(kSimHighSpeed);
    }

    draw_hit_effect_at(batch, evt.lane, evt.end_lane, evt.type, static_cast<float>(effective_age),
                       evt.z, 1.0f, evt.hit_fx_role, evt.jump_flare);
  }
}

namespace {

// Hit FX role: head/tap (0), hold tail (1). Body/stars: no visual FX.
// Prefab map (Default Light):
//   Critical / CriticalHoldStart / ScratchCriticalHoldStart → CriticalBomb
//   Flick / JumpScratch end → ScratchBomb (+Flare)
//   ScratchHoldStart → ScratchBomb (purple; head sprite is pink NormalNote)
//   HoldStart / plain hold tail → HoldBomb
//   Normal → NormalBomb
struct BombFxSpec {
  const char* dir;
  bool flare;
};

// Flare/Square share OnBomb lane-span midpoint on the judgeline (Y=0,Z=0 in
// retail). flare.png is recentered so Unity Sprite BombEffectDefault_10
// m_Pivot≈(0.514,0.483) sits on UV mid (skins/effects README). Do NOT map
// ParticleSystemRenderer.pivot (±0.267×size) into stage Y.

BombFxSpec bomb_fx_spec_for(NoteType type, int hit_fx_role, bool jump_scratch_flare) noexcept {
  // JumpScratch ScratchHoldEnd → ScratchBomb with flare (retail keeps BomFlare in Light).
  if (hit_fx_role == 1 && jump_scratch_flare) {
    return {"scratch", true};
  }
  // Plain hold tails: HoldBomb / scratch-colored HoldBomb Square only.
  if (hit_fx_role == 1) {
    if (is_scratch_hold_body(type) || type == NoteType::Flick ||
        type == NoteType::ScratchSound) {
      return {"scratch", false};
    }
    return {"hold", false};
  }

  switch (type) {
    case NoteType::Critical:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return {"critical", true};
    case NoteType::Flick:
      return {"scratch", true};
    case NoteType::ScratchHoldStart:  // pink NormalNote head → purple ScratchBomb (no flare)
      return {"scratch", false};
    case NoteType::HoldStart:
      return {"hold", false};
    case NoteType::Sound:
    case NoteType::BlueTap:
      return {"sound", false};
    case NoteType::ScratchSound:
      return {"scratch", false};
    case NoteType::Normal:
    default:
      return {"normal", false};
  }
}

// BombController / *BombEffect.prefab (Light: Square [+ Flare]).
constexpr float kBombLaneWidthUnity = 0.925f;
// Square: lengthInSec 0.1, rate 80, startLifetime ∈ [0.3, 0.5] (RandomBetweenTwoConstants).
constexpr float kSquareEmitSec = 0.1f;
constexpr float kSquareEmitRate = 80.0f;
constexpr float kSquareLifeMin = 0.3f;
constexpr float kSquareLifeMax = 0.5f;
constexpr float kFlareLife = 0.6f;
// ColorModule atime1 = 26214/65535 ≈ 0.4 (hold), then → 0 at life end.
constexpr float kColorFadeStart = 26214.0f / 65535.0f;

float lerp01(float a, float b, float t) noexcept {
  return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

// SizeModule TwoCurves×scalar=2 — use mid of min/max (not max alone):
//   X: (1.0→1.3 + 1.0→1.1)/2 → 1.0→1.2
//   Y: (1.0→2.0 + 1.0→1.3)/2 → 1.0→1.65
// Max Y×2 over-expands on screen (Local Y is foreshortened in Unity).
float square_size_x_mul(float u) noexcept { return lerp01(1.0f, 1.2f, u); }
float square_size_y_mul(float u) noexcept { return lerp01(1.0f, 1.65f, u); }

// Official ColorModule alpha keys (atime 0 / 26214 / 65535): hold until ~0.4, then linear → 0.
float color_module_alpha(float u) noexcept {
  if (u <= kColorFadeStart) {
    return 1.0f;
  }
  const float t = (u - kColorFadeStart) / (1.0f - kColorFadeStart);
  return 1.0f - std::clamp(t, 0.0f, 1.0f);
}

struct FlareGradRgb {
  float r, g, b;
};

// BomFlare ColorModule maxGradient color keys (ctime/65535). Alpha is separate (above).
// CriticalBombEffect / ScratchBombEffect — RGB darkens hard after mid-life; using a flat
// note tint instead left the additive ring bright far longer than retail.
FlareGradRgb flare_color_module_rgb(float u, bool scratch) noexcept {
  u = std::clamp(u, 0.0f, 1.0f);
  // times: 0, 0.2, 0.4, 1.0
  struct Key {
    float t, r, g, b;
  };
  const Key* keys = nullptr;
  constexpr Key kCritical[] = {
      {0.0f, 1.0f, 0.603905f, 0.3176471f},
      {0.2f, 1.0f, 0.9862867f, 0.7783019f},
      {0.4f, 1.0f, 0.6039216f, 0.31764707f},
      {1.0f, 1.0f, 0.22532359f, 0.0f},
  };
  constexpr Key kScratch[] = {
      {0.0f, 0.6862745f, 0.25882354f, 1.0f},
      {0.2f, 0.9934902f, 0.7877358f, 1.0f},
      {0.4f, 0.68543196f, 0.25943398f, 1.0f},
      {1.0f, 0.43529412f, 0.0f, 1.0f},
  };
  keys = scratch ? kScratch : kCritical;
  if (u <= keys[0].t) {
    return {keys[0].r, keys[0].g, keys[0].b};
  }
  for (int i = 0; i < 3; ++i) {
    if (u <= keys[i + 1].t) {
      const float s = (u - keys[i].t) / (keys[i + 1].t - keys[i].t);
      return {lerp01(keys[i].r, keys[i + 1].r, s), lerp01(keys[i].g, keys[i + 1].g, s),
              lerp01(keys[i].b, keys[i + 1].b, s)};
    }
  }
  return {keys[3].r, keys[3].g, keys[3].b};
}

// Unity AnimationCurve Hermite between two keys (slopes are dValue/dTime).
float curve_hermite(float t, float t0, float t1, float p0, float p1, float out_slope0,
                    float in_slope1) noexcept {
  const float dt = t1 - t0;
  if (dt <= 1e-8f) {
    return p1;
  }
  const float u = (t - t0) / dt;
  const float u2 = u * u;
  const float u3 = u2 * u;
  const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
  const float h10 = u3 - 2.0f * u2 + u;
  const float h01 = -2.0f * u3 + 3.0f * u2;
  const float h11 = u3 - u2;
  return h00 * p0 + h10 * (out_slope0 * dt) + h01 * p1 + h11 * (in_slope1 * dt);
}

// BomFlare SizeModule X curve (Critical/Scratch prefab identical; separateAxes=0).
// Keys: (time, value, inSlope, outSlope) — linear lerp was ~150–200ms slower mid-growth.
float flare_size_mul(float u) noexcept {
  u = std::clamp(u, 0.0f, 1.0f);
  constexpr float kT0 = 0.0f;
  constexpr float kV0 = 0.0f;
  constexpr float kOut0 = 13.959289f;
  constexpr float kT1 = 0.13296969f;
  constexpr float kV1 = 0.6850656f;
  constexpr float kIn1 = 1.0710392f;
  constexpr float kOut1 = 1.0710392f;
  constexpr float kT2 = 1.0f;
  constexpr float kV2 = 1.0f;
  constexpr float kIn2 = 0.0f;
  if (u <= kT0) {
    return kV0;
  }
  if (u >= kT2) {
    return kV2;
  }
  if (u < kT1) {
    return curve_hermite(u, kT0, kT1, kV0, kV1, kOut0, kIn1);
  }
  return curve_hermite(u, kT1, kT2, kV1, kV2, kOut1, kIn2);
}

}  // namespace

void PlaybackPreviewView::draw_hit_effect_at(DrawBatch& batch, int32_t lane, int32_t end_lane,
                                             NoteType type, float age_sec, float z,
                                             float alpha_scale, int hit_fx_role,
                                             bool jump_scratch_flare) {
  if (age_sec < 0.0f || alpha_scale < 0.02f) {
    return;
  }
  // Particles are gone well before BombController._animationTime (0.7 / 1.0).
  constexpr float kMaxParticleWindow = kSquareEmitSec + kSquareLifeMax;
  if (age_sec >= std::max(kMaxParticleWindow, kFlareLife)) {
    return;
  }

  const EffectTint tint = tint_for(type);
  const BombFxSpec fx = bomb_fx_spec_for(type, hit_fx_role, jump_scratch_flare);

  TextureInfo square{};
  TextureInfo flare{};
  const bool has_bomb = skin_.bomb_light_for(fx.dir, square, flare);

  if (has_bomb) {
    // Lane width at judgeline percent≈1 (near).
    const float single_lane_w = std::max(geometry_.lane_width(lane, 1.0f), 1e-4f);
    // Unity→screen via one lane = LaneWidth (Square Y is fixed world units).
    const float unity_to_screen = single_lane_w / kBombLaneWidthUnity;

    if (square && age_sec < kMaxParticleWindow) {
      // BomSquare: lengthInSec 0.1, rateOverTime 80 → ~8 stacked frames.
      // InitializeSquare: startSizeX ∈ [laneCount*0.725, laneCount*0.925].
      constexpr int kLayers = 8;
      for (int i = 0; i < kLayers; ++i) {
        const float spawn = (static_cast<float>(i) + 0.5f) / kSquareEmitRate;
        if (spawn > kSquareEmitSec) {
          break;
        }
        const float life = lerp01(kSquareLifeMin, kSquareLifeMax, static_cast<float>(i % 3) / 2.0f);
        const float pa = age_sec - spawn;
        if (pa < 0.0f || pa >= life) {
          continue;
        }
        const float u = pa / life;
        const float start_x_frac =
            lerp01(0.725f / 0.925f, 1.0f, static_cast<float>((i * 3) % 5) / 4.0f);
        const float start_y_unity = lerp01(0.5f, 0.68f, static_cast<float>((i * 2) % 5) / 4.0f);
        const float width_scale = start_x_frac * square_size_x_mul(u);
        const float height_screen = start_y_unity * unity_to_screen * square_size_y_mul(u);
        // Official ColorModule linear × startColor.a=1 (~8 additive layers).
        const float a = color_module_alpha(u) * alpha_scale;
        if (a < 0.02f) {
          continue;
        }
        batch.add_sprite(square,
                         geometry_.bomb_frame_quad(lane, end_lane, width_scale, height_screen),
                         z + static_cast<float>(i) * 0.0001f, a, tint.r, tint.g, tint.b);
      }
    }

    if (fx.flare && flare && age_sec < kFlareLife) {
      // Axis-aligned disc at the lane-span / judgeline center. Radius is fixed Unity
      // startSize=8 (Initialize never scales _bombFlare by laneCount / note width).
      constexpr float kHitP = 1.0f;
      const Vec2 c_l = geometry_.lane_position(lane, kHitP);
      const Vec2 c_r = geometry_.lane_position(end_lane, kHitP);
      const float cx = (c_l.x + c_r.x) * 0.5f;
      const auto& jline = geometry_.judgeline();
      const float cy = (jline.lb_y + jline.lt_y) * 0.5f;
      // Critical burst=4 startColor.a=50/255; Scratch/Flick burst=2 a=150/255. startSize=8.
      const bool scratch_flare = jump_scratch_flare || type == NoteType::Flick;
      const int flare_burst = scratch_flare ? 2 : 4;
      const float flare_start_a = scratch_flare ? (150.0f / 255.0f) : (50.0f / 255.0f);
      for (int i = 0; i < flare_burst; ++i) {
        const float u = age_sec / kFlareLife;
        // Prefab startSize is constant 8 — no per-burst size jitter.
        const float size_mul = flare_size_mul(u);
        // Unity size 8 → half 4; unit scale = one lane / LaneWidth (not note span).
        // Center = OnBomb judgeline anchor (no renderer-pivot stage remap).
        const float half = 4.0f * unity_to_screen * size_mul;
        // MobileParticlesAdditive: tex * (startColor * ColorModule). startColor.rgb = 1.
        const FlareGradRgb grad = flare_color_module_rgb(u, scratch_flare);
        const float a = flare_start_a * color_module_alpha(u) * alpha_scale;
        if (a < 0.01f) {
          continue;
        }
        const Quad fq{{cx - half, cy - half},
                      {cx - half, cy + half},
                      {cx + half, cy + half},
                      {cx + half, cy - half}};
        batch.add_sprite(flare, fq, z + 0.002f + static_cast<float>(i) * 0.0001f, a, grad.r,
                         grad.g, grad.b);
      }
    }
    return;
  }

  // Fallback: legacy Sonolus linear + circular.
  const float duration = std::max(config_.effect_duration, 1e-4f);
  const float t = std::clamp(age_sec / duration, 0.0f, 1.0f);
  const float alpha = (1.0f - t) * (1.0f - t) * alpha_scale;
  if (alpha < 0.02f) {
    return;
  }
  const Quad q = geometry_.effect_quad(lane, end_lane);
  if (skin_.effect_linear_bg) {
    batch.add_sprite(skin_.effect_linear_bg, q, z, alpha * 0.85f, tint.r, tint.g, tint.b);
  }
  if (skin_.effect_linear_star) {
    batch.add_sprite(skin_.effect_linear_star, q, z + 0.002f, alpha, tint.r, tint.g, tint.b);
  }
  if (skin_.effect_circular) {
    batch.add_sprite(skin_.effect_circular, q, z + 0.003f, alpha, tint.r, tint.g, tint.b);
  }
}

void PlaybackPreviewView::draw_timing_effect(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  if (!show_judgment_text_ || !skin_.judge_auto || snapshot.last_judge_ms < 0) {
    return;
  }
  const float age =
      static_cast<float>(preview_now_sec(snapshot) -
                         static_cast<double>(snapshot.last_judge_ms) / 1000.0);
  const float duration = std::max(config_.timing_effect_duration, 1e-4f);
  if (age < 0.0f || age >= duration) {
    return;
  }

  // TimingEffect_anime: scale 0.4→1 @0.083s; alpha 1 until 0.333 then →0 @0.417.
  constexpr float kPopEndSec = 0.083333336f;
  constexpr float kFadeStartSec = 0.33333334f;
  constexpr float kFadeEndSec = 0.41666666f;
  float scale_mul = 1.0f;
  if (age < kPopEndSec) {
    const float u = age / kPopEndSec;
    scale_mul = 0.4f + 0.6f * ease_in_sine(u);
  }
  // Official fade window is only ~5 frames; cubic so it reads as nearly instant.
  float alpha = 1.0f;
  if (age >= kFadeEndSec) {
    return;
  }
  if (age > kFadeStartSec) {
    const float u = (age - kFadeStartSec) / (kFadeEndSec - kFadeStartSec);
    const float lin = 1.0f - std::clamp(u, 0.0f, 1.0f);
    alpha = lin * lin * lin;
  }
  if (alpha < 0.02f) {
    return;
  }

  const auto& stage = geometry_.stage();
  const auto& j = geometry_.judgeline();
  const float judgeline_y = (j.lb_y + j.lt_y) * 0.5f;
  const float tip_y = stage.t;
  const float cy = judgeline_y + (tip_y - judgeline_y) * config_.timing_effect_y_fraction;
  // Stage center (not PreviewUI's local -0.87, which is parent-relative).
  const float stage_cx = (stage.l + stage.r) * 0.5f;
  const float stage_half_w = std::max(0.5f * (stage.r - stage.l), 1e-4f);
  const float cx =
      stage_cx +
      (config_.timing_effect_unity_x / std::max(config_.timing_effect_unity_half_width, 1e-4f)) *
          stage_half_w;

  const float unit = geometry_.content_unit();
  const float H =
      config_.judge_text_height * config_.timing_effect_root_scale * scale_mul * unit;
  const float W = H * config_.judge_auto_ratio;
  Quad q{{cx - W * 0.5f, cy - H * 0.5f},
         {cx - W * 0.5f, cy + H * 0.5f},
         {cx + W * 0.5f, cy + H * 0.5f},
         {cx + W * 0.5f, cy - H * 0.5f}};
  batch.add_sprite(skin_.judge_auto, q, 0.97f, alpha);
}

void PlaybackPreviewView::draw_combo(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  if (snapshot.combo_count <= 0 || !skin_.combo_ap_text) {
    return;
  }
  for (int i = 0; i < 10; ++i) {
    if (!skin_.combo_ap_digit[i]) {
      return;
    }
  }

  const float anim_t = [&]() -> float {
    if (snapshot.last_judge_ms < 0) {
      return 1.0f;
    }
    const float age =
        static_cast<float>(snapshot.timeline_ms - snapshot.last_judge_ms) / 1000.0f;
    if (age < 0.0f) {
      return 1.0f;
    }
    return ease_in_sine(std::min(1.0f, age / config_.judge_text_duration));
  }();
  const float pop = 0.8f + 0.2f * anim_t;
  const float alpha = (0.8f + 0.2f * anim_t) * config_.combo_alpha;

  const float scale = config_.combo_scale * pop;
  float H = config_.combo_ap_number_height * scale;
  float H2 = config_.combo_ap_text_height * scale;
  const float digit_gap = config_.combo_ap_number_distance * scale;
  const float text_gap = config_.combo_ap_text_distance * scale;
  const float digit_ratio = config_.combo_ap_digit_ratio;
  const float text_ratio = config_.combo_ap_text_ratio;

  // Sirius drawCombo uses screen space with origin at center:
  //   cx = screen.w * 0.4, baseline y = 0.2, sizes in half-height units (=1 fullscreen).
  // Map that into the preview content rect so combo sits to the right of the lanes.
  const auto& content = geometry_.content();
  const float unit = geometry_.content_unit();
  H *= unit;
  H2 *= unit;
  const float digit_gap_u = digit_gap * unit;
  const float text_gap_u = text_gap * unit;

  int32_t tmp = snapshot.combo_count;
  float W = -digit_gap_u;
  while (tmp > 0) {
    W += H * digit_ratio + digit_gap_u;
    tmp /= 10;
  }
  const float W2 = H2 * text_ratio;

  const float content_cx = (content.l + content.r) * 0.5f;
  const float content_cy = (content.b + content.t) * 0.5f;
  const float cx = content_cx + content.w * config_.combo_center_x_factor;
  const float baseline = content_cy + unit * config_.combo_baseline_y;
  float R = cx + W * 0.5f;
  const float B = baseline - H * 0.5f;
  const float L2 = cx - W2 * 0.5f;
  const float B2 = baseline + H * 0.5f + text_gap_u;

  tmp = snapshot.combo_count;
  constexpr float kZ = 0.98f;
  while (tmp > 0) {
    const int digit = tmp % 10;
    const float digit_w = H * digit_ratio;
    const float L = R - digit_w;
    Quad q{{L, B}, {L, B + H}, {R, B + H}, {R, B}};
    batch.add_sprite(skin_.combo_ap_digit[digit], q, kZ, alpha);
    R = L - digit_gap_u;
    tmp /= 10;
  }

  Quad text_q{{L2, B2}, {L2, B2 + H2}, {L2 + W2, B2 + H2}, {L2 + W2, B2}};
  batch.add_sprite(skin_.combo_ap_text, text_q, kZ, alpha);
}

bool PlaybackPreviewView::mark_hit_sfx_event(uint64_t key) {
  return hit_sfx_played_.insert(key).second;
}

void PlaybackPreviewView::collect_due_hit_sfx(const PreviewSnapshot& snapshot, bool arm,
                                              int64_t clock_us) {
  // Clock: PreviewSnapshot::timeline_us ← Transport committed Timeline (µs).
  // With BGM, arm future hits via AudioEngine BASS_SYNC_POS → 1× ChannelPlay.
  // Without BGM, play on the tick where Timeline first reaches the hit time.
  const bool music_clock = hit_sfx_.has_music();
  const PreviewLookup lookup = build_preview_lookup(snapshot);

  auto transport_hit_ms = [&](int64_t chart_hit_ms) -> int64_t {
    return wds::chart_editor::EditLeadIn::transport_ms_for_chart_ms(
        chart_hit_ms, preview_lead_in_visible_ms_);
  };

  auto emit = [&](int32_t note_id, uint32_t kind, wds::audio::HitSfxClip clip, int64_t hit_ms) {
    if (clip == wds::audio::HitSfxClip::Count) {
      return;
    }
    const uint64_t key = (static_cast<uint64_t>(snapshot.revision) << 32) |
                         (static_cast<uint64_t>(static_cast<uint32_t>(note_id)) << 2) | kind;
    const int64_t when_ms = music_clock ? transport_hit_ms(hit_ms) : hit_ms;
    const int64_t when_us = when_ms * 1000;

    if (!arm) {
      // Seek/pause resync: remember hits already at/behind the playhead so resume
      // does not replay them. Leave future hits unmarked so they can be armed later.
      if (when_us <= clock_us) {
        mark_hit_sfx_event(key);
      }
      return;
    }

    if (music_clock) {
      if (when_us <= clock_us) {
        // Already behind the music playhead — skip (marked on seek/pause).
        mark_hit_sfx_event(key);
        return;
      }
      if (!mark_hit_sfx_event(key)) {
        return;
      }
      // Music byte sync (not display-frame quantized). Engine plays immediately if
      // decode already passed the target so SetSync cannot miss silently.
      // If arming/play fails (voice limit race, etc.), unmark so a later tick retries.
      if (!hit_sfx_.schedule_at(clip, wds::common::ms_to_us(std::max<int64_t>(0, when_ms)))) {
        hit_sfx_played_.erase(key);
      }
      return;
    }

    // No BGM: fire once when the Timeline reaches the hit (edge on each tick).
    // Do not play future notes — the snapshot window includes spawn-lead notes
    // that are still approaching the judgeline.
    if (when_us > clock_us) {
      return;
    }
    if (!mark_hit_sfx_event(key)) {
      return;
    }
    if (!hit_sfx_.play(clip)) {
      hit_sfx_played_.erase(key);
    }
  };

  constexpr uint32_t kHead = 0;
  constexpr uint32_t kTail = 1;
  constexpr uint32_t kStar = 2;

  for (const auto& note : snapshot.notes) {
    const bool hold_body = is_hold_body(note.note_type);
    const bool mid_star = is_hold_mid_star(note.note_type);
    const bool with_tail = is_hold_with_tail(note.note_type);
    const bool duration_hold = hold_body && !mid_star && note.end_ms > note.start_ms;

    // Head / tap / flick — arm once when the note is in the snapshot window.
    // Official HoldStart* paired with a duration body: body emits the start hit.
    if (!hold_body && !mid_star && !is_split_lane_gimmick(note.gimmick_type)) {
      if (is_hold_start(note.note_type) || note.note_type == NoteType::Normal ||
          note.note_type == NoteType::Critical ||
          note.note_type == NoteType::Flick || note.note_type == NoteType::BlueTap) {
        const bool paired = is_hold_start(note.note_type) &&
                            lookup.duration_hold_heads.count(note_span_key(
                                note.start_ms, note.lane, note.width)) != 0;
        if (!paired) {
          emit(note.note_id, kHead, hit_sfx_clip_for_head(note.note_type), note.start_ms);
        }
      }
    }

    // Hold body start — Perfect/Critical one-shot; Hold loop is handled separately.
    if (duration_hold) {
      emit(note.note_id, kHead, hit_sfx_clip_for_hold_body_start(note.note_type), note.start_ms);
    }

    if (with_tail && note.end_ms > note.start_ms) {
      emit(note.note_id, kTail, hit_sfx_clip_for_hold_tail(note.note_type), note.end_ms);
    }

    if (mid_star) {
      emit(note.note_id, kStar, hit_sfx_clip_for_mid_star(note.note_type), note.start_ms);
    }
  }
}

void PlaybackPreviewView::release_sfx_clock_control(const PreviewSnapshot& snapshot,
                                                    int64_t raw_us, bool playing) {
  // User/transport took the clock (pause, seek, scrub, play resync): mute, drop
  // armed keys, and release the monotonic filter so it re-latches at raw_us.
  hit_sfx_.stop_all();
  hit_sfx_played_.clear();
  sfx_mono_us_ = std::max<int64_t>(0, raw_us);
  sfx_position_generation_ = hit_sfx_.position_generation();
  collect_due_hit_sfx(snapshot, /*arm=*/false, sfx_mono_us_);
  if (playing) {
    collect_due_hit_sfx(snapshot, /*arm=*/true, sfx_mono_us_);
  }
  sfx_was_playing_ = playing;
  hit_sfx_.set_hold_looping(false);
}

void PlaybackPreviewView::update_hit_sfx(const PreviewSnapshot& snapshot) {
  if (!hit_sfx_.ready()) {
    return;
  }

  using wds::chart_editor::PreviewPlaybackState;
  const bool playing = snapshot.playback_state == PreviewPlaybackState::Playing;
  const bool music_clock = hit_sfx_.has_music();
  // Prefer BASS playtime when BGM is loaded; otherwise Transport Timeline µs.
  const int64_t timeline_us =
      (snapshot.timeline_us != 0 || snapshot.timeline_ms == 0)
          ? snapshot.timeline_us
          : snapshot.timeline_ms * 1000;
  const int64_t raw_us =
      music_clock ? hit_sfx_.music_position().count() : timeline_us;

  const uint64_t pos_gen = hit_sfx_.position_generation();
  const bool control_event = pos_gen != sfx_position_generation_;
  const bool pause_edge = !playing && sfx_was_playing_;
  const bool revision_changed = snapshot.revision != sfx_document_revision_;
  if (revision_changed) {
    hit_sfx_played_.clear();
    sfx_document_revision_ = snapshot.revision;
  }

  if (control_event || pause_edge) {
    release_sfx_clock_control(snapshot, raw_us, playing);
  } else if (!playing) {
    // Stay paused: keep mono latched; do not arm. Scrubs bump pos_gen above.
    if (sfx_mono_us_ < 0) {
      sfx_mono_us_ = std::max<int64_t>(0, raw_us);
    }
    collect_due_hit_sfx(snapshot, /*arm=*/false, sfx_mono_us_);
    sfx_was_playing_ = false;
    hit_sfx_.set_hold_looping(false);
  } else {
    // Playing: advance mono clock; filter occasional BASS/Timeline regressions so
    // they cannot un-mark hits and cause duplicate one-shots.
    constexpr int64_t kBassGlitchBackUs = 50000;  // 50ms — ignore as glitch
    if (sfx_mono_us_ < 0 || raw_us >= sfx_mono_us_) {
      sfx_mono_us_ = std::max<int64_t>(0, raw_us);
    } else if (sfx_mono_us_ - raw_us > kBassGlitchBackUs) {
      // Large backwards jump without control_event — treat as scrub fallback.
      release_sfx_clock_control(snapshot, raw_us, playing);
      // Hold / arm already handled inside release when playing.
      return;
    }
    // else: small backwards glitch — keep sfx_mono_us_, do not re-fire.

    collect_due_hit_sfx(snapshot, /*arm=*/true, sfx_mono_us_);
    sfx_was_playing_ = true;

    const int64_t clock_ms = sfx_mono_us_ / 1000;
    bool hold_active = false;
    for (const auto& note : snapshot.notes) {
      if (!is_hold_body(note.note_type) || is_hold_mid_star(note.note_type)) {
        continue;
      }
      if (clock_ms >= note.start_ms && clock_ms <= note.end_ms) {
        hold_active = true;
        break;
      }
    }
    hit_sfx_.set_hold_looping(hold_active && !mute_hold_body_sfx_);
  }
}

}  // namespace wds::ui

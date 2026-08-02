#include "wds/ui/regions/preview/playback_preview.hpp"
#include "wds/renderer/log.hpp"

#include <wds/chart_render/note_strips.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/ui/note_skin_mapping.hpp>

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
using wds::renderer::add_note_strips;

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

void split_boundaries_12(int32_t split_count, std::vector<int32_t>& out) {
  out.clear();
  switch (split_count) {
    case 2:
      out = {5};
      break;
    case 3:
      out = {3, 7};
      break;
    case 4:
      out = {2, 5, 8};
      break;
    case 5:
      out = {2, 4, 6, 8};
      break;
    case 6:
      out = {1, 3, 5, 7, 9};
      break;
    default:
      break;
  }
}

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
      return {1.0f, 0.82f, 0.25f};  // yellow critical (start heads only)
    case NoteType::HoldStart:
    case NoteType::Hold:
    case NoteType::CriticalHold:  // body/end use HoldNote blue, not gold
    case NoteType::NontailHold:
    case NoteType::NontailCriticalHold:
    case NoteType::BlueTap:
      return {0.25f, 0.85f, 1.0f};  // blue hold / tap
    case NoteType::Scratch:
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::Flick:
    case NoteType::SoundPurple:
      return {0.85f, 0.35f, 1.0f};  // purple flick / scratch-hold end
    case NoteType::Sound:
      return {0.35f, 0.95f, 0.55f};  // green tick
    case NoteType::HoldEighth:
      // Orphan path only — prefer resolving via parent hold body before calling tint_for.
      return {0.25f, 0.85f, 1.0f};
    case NoteType::ScratchHoldStart:  // Sirius: NormalNote head
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
  notes_by_start_desc_.clear();
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
  draw_split_lanes(batch_, snapshot);
  draw_concurrent_lines(batch_, snapshot);
  draw_notes(batch_, snapshot);
  draw_hit_effects(additive_batch_, snapshot);
  draw_combo(batch_, snapshot);
  merge_overlay(ui_overlay, false);

  const DrawBatch* post =
      (modal_overlay != nullptr && modal_overlay->vertex_count() > 0) ? modal_overlay : nullptr;
  const DrawBatch* post2 =
      (modal_chrome != nullptr && modal_chrome->vertex_count() > 0) ? modal_chrome : nullptr;
  vulkan_.draw_frame(batch_, geometry_.screen(), 0.05f, 0.05f, 0.08f, &additive_batch_, post,
                     post2);
}

void PlaybackPreviewView::draw_stage(DrawBatch& batch, const PreviewSnapshot& snapshot) {
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
  batch.add_sprite(skin_.stage_background, stage, -0.85f, config_.stage_opacity);

  const auto& j = geometry_.judgeline();
  Quad jq{{j.lb_x, j.lb_y}, {j.lt_x, j.lt_y}, {j.rt_x, j.rt_y}, {j.rb_x, j.rb_y}};
  batch.add_sprite(skin_.judgeline, jq, -0.7f, 1.0f);
}

void PlaybackPreviewView::draw_split_lanes(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  if (snapshot.split_lanes.empty()) {
    return;
  }
  const auto& split = snapshot.split_lanes.back();
  if (!split.should_show || split.split_count <= 0) {
    return;
  }

  // Official color id lives in scratch_length (Sirius SplitLine.color / getSplitLine).
  const int32_t color_id = split.scratch_length > 0 ? split.scratch_length : 1;
  const int32_t phase = split.split_anim_phase;

  auto fallback_tex = [&]() -> const TextureInfo* {
    if (phase == 0) {
      return skin_.split_line_trans1 ? &skin_.split_line_trans1
                                     : (skin_.split_line_1 ? &skin_.split_line_1 : nullptr);
    }
    if (phase == 2) {
      return skin_.split_line_trans2 ? &skin_.split_line_trans2
                                     : (skin_.split_line_1 ? &skin_.split_line_1 : nullptr);
    }
    return skin_.split_line_1 ? &skin_.split_line_1
                              : (skin_.split_line_2 ? &skin_.split_line_2 : nullptr);
  };

  const int32_t n = config_.lane_count;
  std::vector<int32_t> mids;
  split_boundaries_12(split.split_count, mids);

  const float p0 = split.split_percent_start;
  const float p1 = split.split_percent_end;
  const float line_a = split.split_line_alpha * config_.split_line_opacity;

  // Sonolus splitLineMemory: [0]=left, [1..split-1]=internals, [split]=right end.
  auto draw_slot = [&](int32_t memory_slot, const Quad& q) {
    const TextureInfo* line_tex = skin_.split_lines.texture_for(color_id, memory_slot, phase);
    if (line_tex == nullptr || !*line_tex) {
      line_tex = fallback_tex();
    }
    if (line_tex == nullptr || !*line_tex) {
      if (skin_.soft_split_line) {
        line_tex = &skin_.soft_split_line;
      } else {
        return;
      }
    }
    const float u0 = line_tex->u0;
    const float u1 = line_tex->u1;
    const float v_far = line_tex->v0 + (line_tex->v1 - line_tex->v0) * p0;
    const float v_near = line_tex->v0 + (line_tex->v1 - line_tex->v0) * p1;
    batch.add_quad(line_tex->id, q, -0.6f, line_a, u0, v_near, u1, v_far);
  };

  draw_slot(0, geometry_.split_line_quad(-1, p0, p1));
  int32_t mid_slot = 1;
  for (int32_t b12 : mids) {
    draw_slot(mid_slot++, geometry_.split_line_quad(scale_boundary(b12, n), p0, p1));
  }
  draw_slot(split.split_count, geometry_.split_end_line_quad(p0, p1));
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
      notes_by_start_desc_.size() != snapshot.notes.size()) {
    notes_by_start_desc_.resize(snapshot.notes.size());
    for (size_t i = 0; i < snapshot.notes.size(); ++i) {
      notes_by_start_desc_[i] = i;
    }
    std::sort(notes_by_start_desc_.begin(), notes_by_start_desc_.end(),
              [&](size_t a, size_t b) {
                return snapshot.notes[a].start_ms > snapshot.notes[b].start_ms;
              });
    notes_order_revision_ = snapshot.revision;
  }
  note_draw_order_.clear();
  note_draw_order_.reserve(snapshot.notes.size());
  for (size_t idx : notes_by_start_desc_) {
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

  // Layering (depthWrite off → draw order = visual order). Match Sirius z tiers:
  // hold connection (50k) < flat notes (100k) < ticks (200k) < arrows.
  // Pass 1: hold bodies only, always under overlapping taps / stars / flicks.
  for (const PreviewNoteInstance* note : order) {
    if (is_hold_body(note->note_type) && !is_hold_mid_star(note->note_type)) {
      draw_hold_body(batch, *note, now);
    }
  }
  // Pass 2: heads / tails / mid-stars (no hold connection).
  for (const PreviewNoteInstance* note : order) {
    draw_note(batch, *note, snapshot);
  }
  // Pass 3: flick arrows on top so translucent mid-stars cannot wash them out.
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
    // Mid-stars: Sound / SoundPurple are tick-only; HoldEighth has no sprite.
    if (is_hold_mid_star(note->note_type)) {
      continue;
    }
    // Hold heads are not flick ends — arrows belong on ScratchHoldEnd / Flick / Scratch.
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

void PlaybackPreviewView::draw_note(DrawBatch& batch, const PreviewNoteInstance& note,
                                    const PreviewSnapshot& snapshot) {
  const double now = preview_now_sec(snapshot);
  const NoteSprites sprites = sprites_for(skin_, note.note_type);

  // Hold bodies never invent a start head — only a real HoldStart* note draws one.
  // Start heads despawn at judgment like taps (no judgeline sticky).
  // Tailed holds still draw the approaching end cap (HoldEnd / ScratchHoldEnd).
  if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
    const double end = static_cast<double>(note.end_ms) / 1000.0;
    if (is_hold_with_tail(note.note_type) &&
        (note.visual_state == PreviewNoteVisualState::Approaching ||
         note.visual_state == PreviewNoteVisualState::Holding)) {
      draw_flat_note_at(batch, note, end, now, 0.05f, /*use_jump_lanes=*/true);
    }
  }

  // HoldStart* / taps: only while Approaching; Holding means already judged → gone.
  if (note.visual_state == PreviewNoteVisualState::Holding) {
    return;
  }

  if (sprites.is_tick) {
    draw_tick_note(batch, note, now);
  } else if (is_hold_start(note.note_type) || !is_hold_body(note.note_type)) {
    draw_flat_note(batch, note, now, 0.0f);
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

  auto alpha_at = [&](float p) {
    if (p <= fade_lo) return base_alpha;
    return base_alpha * std::clamp(1.0f - (p - fade_lo) / band, 0.0f, 1.0f);
  };

  // hold_body_quad: lb/rb at percent_near, lt/rt at percent_far.
  auto emit = [&](float lo, float hi) {
    if (hi <= lo) return;
    batch.add_sprite_vfade(sprites.connection,
                           geometry_.hold_body_quad(note.lane, note.end_lane, hi, lo), -0.25f,
                           alpha_at(hi), alpha_at(lo));
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
                                         double now_sec, float z_bias) {
  draw_flat_note_at(batch, note, static_cast<double>(note.start_ms) / 1000.0, now_sec, z_bias,
                    /*use_jump_lanes=*/false);
}

void PlaybackPreviewView::draw_flat_note_at(DrawBatch& batch, const PreviewNoteInstance& note,
                                            double beat_sec, double now_sec, float z_bias,
                                            bool use_jump_lanes) {
  const NoteSprites sprites = sprites_for(skin_, note.note_type);
  NoteSprites head = sprites;
  // Hold body end caps: scratch end uses purple; regular HoldEnd stays blue.
  // Gold is only on CriticalHoldStart / ScratchCriticalHoldStart note rows.
  if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
    if (is_scratch_hold_body(note.note_type) && use_jump_lanes) {
      head.left = skin_.note_purple_left;
      head.middle = skin_.note_purple_middle;
      head.right = skin_.note_purple_right;
    } else {
      head.left = skin_.note_blue_left;
      head.middle = skin_.note_blue_middle;
      head.right = skin_.note_blue_right;
    }
  }
  if (!head.middle) {
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

  Quad q = geometry_.note_quad(lane, end_lane, std::clamp(p, 0.0f, 1.0f));
  const float z = z_bias - static_cast<float>(beat_sec) * 1e-4f;
  const float alpha = note.is_grayed_out ? 0.55f : 1.0f;
  const int32_t span = std::max(1, end_lane - lane + 1);

  if (head.left && head.right) {
    add_note_strips(batch, head.left, head.middle, head.right, q, config_.note_border_percent,
                    span, z, alpha);
  } else {
    batch.add_sprite(head.middle, q, z, alpha);
  }
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
  const float w_ref = geometry_.lane_width(lane, 1.0f);
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

  // scratchLength: + right only, - left only, 0 both (outward) with a center gap.
  // Match Sirius utils.cpp: bidirectional uses half density; directional uses full.
  const float sonolus_num =
      w * static_cast<float>(end_lane - lane + 1) * config_.arrow_percent / W;
  const int32_t sl = note.scratch_length;
  const float num = std::max(1.0f, (sl == 0) ? sonolus_num * 0.5f : sonolus_num);
  const float n = num;

  auto arrow_alpha = [&](float i) {
    const float phase = std::fmod(i + static_cast<float>(anim_time_sec) * config_.arrow_speed, n);
    return 1.0f - 0.8f * phase / n;
  };

  if (sl <= 0) {
    for (float i = 1.0f; i < n; i += 1.0f) {
      const float x0 = L + (i - 1.0f) * W * 0.5f;
      const float x1 = L + (i + 1.0f) * W * 0.5f;
      // Stop before the far (start) end hangs past the note's right edge.
      if (x1 > R) {
        break;
      }
      batch.add_sprite(skin_.scratch_arrow,
                       Quad{{x0, c1.y}, {x0, c1.y + H * 0.5f}, {x1, c1.y + H * 0.5f}, {x1, c1.y}},
                       0.2f, arrow_alpha(i));
    }
  }
  if (sl >= 0) {
    for (float i = 1.0f; i < n; i += 1.0f) {
      // Reverse vertex X → horizontal UV flip (Sirius drawRightArrow).
      const float rx0 = R - (i - 1.0f) * W * 0.5f;
      const float rx1 = R - (i + 1.0f) * W * 0.5f;
      // Stop before the far (start) end hangs past the note's left edge.
      if (rx1 < L) {
        break;
      }
      batch.add_sprite(
          skin_.scratch_arrow,
          Quad{{rx0, c2.y}, {rx0, c2.y + H * 0.5f}, {rx1, c2.y + H * 0.5f}, {rx1, c2.y}}, 0.2f,
          arrow_alpha(i));
    }
  }
}

void PlaybackPreviewView::draw_hit_effects(DrawBatch& batch, const PreviewSnapshot& snapshot) {
  const double now = preview_now_sec(snapshot);
  const float duration = config_.effect_duration;
  const double duration_d = static_cast<double>(duration);
  const PreviewLookup lookup = build_preview_lookup(snapshot);

  auto lanes_overlap = [](const PreviewNoteInstance& a, const PreviewNoteInstance& b) {
    return a.lane <= b.end_lane && b.lane <= a.end_lane;
  };

  // Soft body VFX times = chart mid-stars only (same as combo; no synthetic eighths).
  auto collect_hold_soft = [&](const PreviewNoteInstance& hold, std::vector<int64_t>& times) {
    times.clear();
    if (hold.end_ms <= hold.start_ms) {
      return;
    }
    for (const PreviewNoteInstance* star : lookup.mid_stars) {
      if (star->start_ms <= hold.start_ms || star->start_ms >= hold.end_ms) {
        continue;
      }
      if (!lanes_overlap(hold, *star)) {
        continue;
      }
      times.push_back(star->start_ms);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
  };

  // HoldEighth has no family of its own — soft FX must follow the parent hold body
  // (blue Hold vs purple ScratchHold). Prefer scratch parent when both overlap.
  auto parent_hold_type_for_star = [&](const PreviewNoteInstance& star) -> NoteType {
    const PreviewNoteInstance* best = nullptr;
    for (const PreviewNoteInstance* hold : lookup.duration_holds) {
      if (star.start_ms <= hold->start_ms || star.start_ms >= hold->end_ms) {
        continue;
      }
      if (!lanes_overlap(*hold, star)) {
        continue;
      }
      if (best == nullptr || is_scratch_hold_body(hold->note_type)) {
        best = hold;
        if (is_scratch_hold_body(hold->note_type)) {
          break;
        }
      }
    }
    return best != nullptr ? best->note_type : star.note_type;
  };

  std::unordered_set<int32_t> absorbed_stars;
  std::vector<int64_t> soft_times;

  for (const auto& note : snapshot.notes) {
    const bool hold_body = is_hold_body(note.note_type);
    const bool mid_star = is_hold_mid_star(note.note_type);
    const bool with_tail = is_hold_with_tail(note.note_type);

    // Head / tap: exclude hold bodies and mid-stars (stars use soft body style).
    if (!hold_body && !mid_star && !is_split_lane_gimmick(note.gimmick_type)) {
      if (is_hold_start(note.note_type) || note.note_type == NoteType::Normal ||
          note.note_type == NoteType::Critical || note.note_type == NoteType::Scratch ||
          note.note_type == NoteType::Flick || note.note_type == NoteType::BlueTap) {
        const double age = now - static_cast<double>(note.start_ms) / 1000.0;
        if (age >= 0.0 && age < duration_d) {
          draw_hit_effect_at(batch, note.lane, note.end_lane, note.note_type,
                             static_cast<float>(age), 0.95f, 1.0f);
        }
      }
    }

    // Tail: tailed hold bodies only.
    if (with_tail && note.end_ms > note.start_ms) {
      const double age_end = now - static_cast<double>(note.end_ms) / 1000.0;
      if (age_end >= 0.0 && age_end < duration_d) {
        draw_hit_effect_at(batch, note.lane, note.end_lane, note.note_type,
                           static_cast<float>(age_end), 0.96f, 1.0f);
      }
    }

    // Hold body soft judgments: chart mid-stars in (head, tail), deduped.
    // Effect tint follows the hold body (ScratchHold → purple, Hold → blue).
    if (hold_body && !mid_star) {
      collect_hold_soft(note, soft_times);
      for (const PreviewNoteInstance* star : lookup.mid_stars) {
        if (star->start_ms > note.start_ms && star->start_ms < note.end_ms &&
            lanes_overlap(note, *star)) {
          absorbed_stars.insert(star->note_id);
        }
      }
      for (int64_t t : soft_times) {
        const double age = now - static_cast<double>(t) / 1000.0;
        if (age >= 0.0 && age < duration_d) {
          draw_hit_effect_at(batch, note.lane, note.end_lane, note.note_type,
                             static_cast<float>(age), 0.93f, config_.hold_body_effect_alpha);
        }
      }
    }
  }

  // Orphan mid-stars (not inside an active hold body in the snapshot).
  for (const PreviewNoteInstance* note : lookup.mid_stars) {
    if (absorbed_stars.count(note->note_id) != 0) {
      continue;
    }
    const double age = now - static_cast<double>(note->start_ms) / 1000.0;
    if (age >= 0.0 && age < duration_d) {
      const NoteType fx_type = note->note_type == NoteType::HoldEighth
                                   ? parent_hold_type_for_star(*note)
                                   : note->note_type;
      draw_hit_effect_at(batch, note->lane, note->end_lane, fx_type, static_cast<float>(age),
                         0.93f, config_.hold_body_effect_alpha);
    }
  }
}

void PlaybackPreviewView::draw_hit_effect_at(DrawBatch& batch, int32_t lane, int32_t end_lane,
                                             NoteType type, float age_sec, float z,
                                             float alpha_scale) {
  const float duration = std::max(config_.effect_duration, 1e-4f);
  const float t = std::clamp(age_sec / duration, 0.0f, 1.0f);
  const float alpha = (1.0f - t) * (1.0f - t) * alpha_scale;
  if (alpha < 0.02f) {
    return;
  }
  const EffectTint tint = tint_for(type);
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
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(note_id)) << 2) | kind;
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
          note.note_type == NoteType::Critical || note.note_type == NoteType::Scratch ||
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

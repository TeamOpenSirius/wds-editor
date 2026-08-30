#pragma once

#include <cstdint>
#include <unordered_set>
#include <utility>

namespace wds::ui {

// Music-clock SFX arm. `schedule` is invoked as a factory; callers must pass a
// callable (not a precomputed bool) so already-marked keys can skip schedule_at.
// Failure erases the key so TooFar / AtCapacity / SetSyncFailure stay retryable.
template <typename Schedule>
bool commit_hit_sfx_schedule(std::unordered_set<uint64_t>& played, uint64_t key,
                             Schedule&& schedule) {
  if (!played.insert(key).second) {
    return false;
  }
  if (!static_cast<bool>(std::forward<Schedule>(schedule)())) {
    played.erase(key);
    return false;
  }
  return true;
}

}  // namespace wds::ui

#ifndef WDS_UI_PLAYBACK_PREVIEW_HELPERS_ONLY

#include "wds/ui/regions/preview/hit_sfx_mapping.hpp"

#include "wds/renderer/draw_batch.hpp"
#include "wds/renderer/preview_visual_config.hpp"
#include "wds/renderer/skin_catalog.hpp"
#include "wds/renderer/stage_geometry.hpp"
#include "wds/renderer/texture.hpp"
#include "wds/renderer/vulkan_renderer.hpp"

#include <wds/core/preview_snapshot.hpp>

#include <wds/audio/hit_sfx.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

struct GLFWwindow;

namespace wds::ui {

// Chart playback preview surface. Builds a VulkanHostSurface from the GLFW window
// (WSI stays in ui) and owns VulkanRenderer. Consumes PreviewSnapshot each frame.
class PlaybackPreviewView {
 public:
  PlaybackPreviewView() = default;
  ~PlaybackPreviewView();

  PlaybackPreviewView(const PlaybackPreviewView&) = delete;
  PlaybackPreviewView& operator=(const PlaybackPreviewView&) = delete;

  bool initialize(GLFWwindow* window, const wds::renderer::PreviewVisualConfig& config = {});
  void shutdown();

  bool ready() const noexcept { return ready_; }

  void set_config(const wds::renderer::PreviewVisualConfig& config);
  const wds::renderer::PreviewVisualConfig& config() const noexcept { return config_; }
  // Persist preferred MSAA (1 / 2 / 4) and rebuild the swapchain when ready.
  void apply_msaa(int samples);

  void attach_audio(wds::audio::AudioEngine* audio) noexcept;
  // Read-only armed MIXTIME POS count (0 when no audio).
  size_t pending_sfx_sync_count() const noexcept;

  // Chart delay (MusicTiming::offset_ms): chart starts this many ms after music.
  // Note hit times already include the delay; SFX schedule at the same timeline ms
  // on the music stream (music t=0 == timeline 0).
  void set_chart_offset_ms(int64_t offset_ms) noexcept {
    chart_offset_ms_ = offset_ms < 0 ? 0 : offset_ms;
  }
  int64_t chart_offset_ms() const noexcept { return chart_offset_ms_; }

  // Edit visible_ms for lead-in SFX mapping (chart hit → transport time).
  void set_preview_lead_in_visible_ms(int64_t visible_ms) noexcept {
    preview_lead_in_visible_ms_ = visible_ms < 0 ? 0 : visible_ms;
  }

  // When true, mute Hold-body loop SFX only (head/tail/JumpScratch/stars unchanged).
  void set_mute_hold_body_sfx(bool mute) noexcept { mute_hold_body_sfx_ = mute; }
  bool mute_hold_body_sfx() const noexcept { return mute_hold_body_sfx_; }

  // When true, show TimingEffect Auto judgment text on auto-hit (persisted setting).
  void set_show_judgment_text(bool show) noexcept { show_judgment_text_ = show; }
  bool show_judgment_text() const noexcept { return show_judgment_text_; }

  void resize(int framebuffer_width, int framebuffer_height);

  // Update hit SFX from the latest snapshot. Call after apply_timeline; still
  // before Transport::start_pending_music() so the first playing frame sees a
  // paused BGM clock when marking past hits.
  void sync_hit_sfx(const wds::chart_editor::PreviewSnapshot& snapshot);

  // ui_solid_texture: 1×1 white used by UiPainter panels. Overlay solids are drawn
  // under the stage; overlay sprites (and edit-area skins) are drawn above it.
  // modal_overlay / modal_chrome are submitted via VulkanRenderer::draw_frame post
  // passes so sticky DrawBatch bucket indices cannot bury them. Chrome draws last.
  void render(const wds::chart_editor::PreviewSnapshot& snapshot,
              const wds::renderer::DrawBatch* ui_overlay = nullptr,
              wds::renderer::TextureId ui_solid_texture = wds::renderer::kInvalidTextureId,
              const wds::renderer::DrawBatch* modal_overlay = nullptr,
              const wds::renderer::DrawBatch* modal_chrome = nullptr,
              int64_t visual_lead_us = 0);

  wds::renderer::StageGeometry& geometry() noexcept { return geometry_; }
  const wds::renderer::StageGeometry& geometry() const noexcept { return geometry_; }
  wds::renderer::VulkanRenderer& vulkan() noexcept { return vulkan_; }
  wds::renderer::TextureCache& textures() noexcept { return textures_; }
  const wds::renderer::SkinCatalog& skin() const noexcept { return skin_; }

  // Last preview CPU build costs (µs). Always updated in render().
  struct PreviewBuildTimings {
    int64_t bg_us = 0;
    int64_t overlay_solid_us = 0;
    int64_t stage_us = 0;
    int64_t split_us = 0;
    int64_t concurrent_us = 0;
    int64_t notes_us = 0;
    int64_t hit_fx_us = 0;
    int64_t timing_us = 0;
    int64_t combo_us = 0;
    int64_t overlay_sprite_us = 0;
    int64_t total_us = 0;
    uint32_t note_count = 0;
    uint32_t verts = 0;
    uint32_t buckets = 0;
  };
  PreviewBuildTimings last_build_timings() const noexcept { return last_build_; }

 private:
  void draw_ingame_background(wds::renderer::DrawBatch& batch);
  void draw_stage(wds::renderer::DrawBatch& batch,
                  const wds::chart_editor::PreviewSnapshot& snapshot,
                  wds::renderer::TextureId solid_texture);
  void draw_hidden_line(wds::renderer::DrawBatch& batch);
  // Official LaneMask bottom percent (GetNoteVisiblePositionY). z=0 objects cull here.
  float spawn_clip_percent() const noexcept;
  void draw_split_lanes(wds::renderer::DrawBatch& batch, wds::renderer::DrawBatch& additive,
                        const wds::chart_editor::PreviewSnapshot& snapshot);
  void draw_concurrent_lines(wds::renderer::DrawBatch& batch,
                             const wds::chart_editor::PreviewSnapshot& snapshot);
  void draw_notes(wds::renderer::DrawBatch& batch,
                  const wds::chart_editor::PreviewSnapshot& snapshot);
  void draw_note(wds::renderer::DrawBatch& batch,
                 const wds::chart_editor::PreviewNoteInstance& note,
                 const wds::chart_editor::PreviewSnapshot& snapshot);
  void draw_hold_body(wds::renderer::DrawBatch& batch,
                      const wds::chart_editor::PreviewNoteInstance& note, double now_sec);
  void draw_flat_note(wds::renderer::DrawBatch& batch,
                      const wds::chart_editor::PreviewNoteInstance& note, double now_sec,
                      float z_bias, bool bottom_layer = false);
  void draw_flat_note_at(wds::renderer::DrawBatch& batch,
                         const wds::chart_editor::PreviewNoteInstance& note, double beat_sec,
                         double now_sec, float z_bias, bool use_jump_lanes = true,
                         bool bottom_layer = false);
  // Flat caps only (no ticks / hold ribbons) for bottom or top sandwich pass.
  void draw_note_flat_layer(wds::renderer::DrawBatch& batch,
                            const wds::chart_editor::PreviewNoteInstance& note,
                            const wds::chart_editor::PreviewSnapshot& snapshot,
                            bool bottom_layer);
  void draw_tick_note(wds::renderer::DrawBatch& batch,
                      const wds::chart_editor::PreviewNoteInstance& note, double now_sec);
  void draw_arrows(wds::renderer::DrawBatch& batch,
                   const wds::chart_editor::PreviewNoteInstance& note, double now_sec,
                   double anim_time_sec);
  void draw_arrows_at(wds::renderer::DrawBatch& batch,
                      const wds::chart_editor::PreviewNoteInstance& note, double beat_sec,
                      double now_sec, double anim_time_sec);
  void draw_hit_effects(wds::renderer::DrawBatch& batch,
                        const wds::chart_editor::PreviewSnapshot& snapshot);
  // role: 0=head/tap, 1=hold tail, 2=hold-body soft (mid-star / HoldEighth).
  // jump_scratch_flare: ScratchHold JumpScratch end uses ScratchBomb flare (same as Flick).
  void draw_hit_effect_at(wds::renderer::DrawBatch& batch, int32_t lane, int32_t end_lane,
                          wds::chart_editor::NoteType type, float age_sec, float z,
                          float alpha_scale = 1.0f, int hit_fx_role = 0,
                          bool jump_scratch_flare = false);
  void draw_timing_effect(wds::renderer::DrawBatch& batch,
                          const wds::chart_editor::PreviewSnapshot& snapshot);
  void draw_combo(wds::renderer::DrawBatch& batch,
                  const wds::chart_editor::PreviewSnapshot& snapshot);
  void update_hit_sfx(const wds::chart_editor::PreviewSnapshot& snapshot);
  bool mark_hit_sfx_event(uint64_t key);
  // arm=true: schedule future hits (music: BASS_SYNC_POS) or play when due.
  // arm=false: mark past hits only (seek/pause).
  // clock_us: filtered monotonic audible clock (µs).
  void collect_due_hit_sfx(const wds::chart_editor::PreviewSnapshot& snapshot, bool arm,
                           int64_t clock_us);
  // Mute SFX, clear played keys, latch mono clock to raw_us (pause / seek / scrub).
  void release_sfx_clock_control(const wds::chart_editor::PreviewSnapshot& snapshot,
                                 int64_t raw_us, bool playing);
  // Rebuild GenerateNoteId-reversed draw index for the current visible set.
  void prepare_note_draw_order(const wds::chart_editor::PreviewSnapshot& snapshot);

  wds::renderer::PreviewVisualConfig config_;
  wds::renderer::StageGeometry geometry_;
  wds::renderer::VulkanRenderer vulkan_;
  wds::renderer::TextureCache textures_;
  wds::renderer::SkinCatalog skin_;
  wds::audio::HitSfxPlayer hit_sfx_;
  wds::renderer::DrawBatch batch_;
  wds::renderer::DrawBatch additive_batch_;
  // Indices into snapshot.notes: GenerateNoteId order reversed (bottom-most first).
  std::vector<size_t> notes_draw_indices_;
  std::vector<const wds::chart_editor::PreviewNoteInstance*> note_draw_order_;
  PreviewBuildTimings last_build_{};
  bool ready_ = false;
  bool mute_hold_body_sfx_ = false;
  bool show_judgment_text_ = false;
  int64_t chart_offset_ms_ = 0;
  int64_t preview_lead_in_visible_ms_ = 0;
  // Present-only visual lead (µs). SFX / engine snapshot stay on the committed clock.
  int64_t visual_lead_us_ = 0;
  // Monotonic SFX clock (µs). Advances with BASS/Timeline; ignores small backwards
  // glitches. Released (reset) on pause / seek / scrub via control generation.
  int64_t sfx_mono_us_ = -1;
  // AudioEngine::position_generation() — seek / scrub / play set_position.
  uint64_t sfx_position_generation_ = std::numeric_limits<uint64_t>::max();
  uint64_t sfx_document_revision_ = std::numeric_limits<uint64_t>::max();
  bool sfx_was_playing_ = false;
  std::unordered_set<uint64_t> hit_sfx_played_;
};

}  // namespace wds::ui

#endif  // WDS_UI_PLAYBACK_PREVIEW_HELPERS_ONLY

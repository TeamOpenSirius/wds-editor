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
#include <wds/audio/hit_sfx_policy.hpp>

#include "wds/renderer/draw_batch.hpp"
#include "wds/renderer/preview_visual_config.hpp"
#include "wds/renderer/skin_catalog.hpp"
#include "wds/renderer/stage_geometry.hpp"
#include "wds/renderer/texture.hpp"
#include "wds/renderer/vulkan_renderer.hpp"

#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/core/preview_snapshot.hpp>

#include <wds/audio/hit_sfx.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wds::ui {

// Play-time split-lane plate cache: one projected quad per Line. The official
// SplitEffectSyuriken shading runs per fragment on the GPU; each frame only
// rewrites the fade-dependent V span and LineHight world Y scale.
struct PreviewLaneGeomCell {
  wds::renderer::Quad quad{};
  float pa = 0.0f;
  float pb = 0.0f;
  float sr = 1.0f;
  float sg = 1.0f;
  float sb = 1.0f;
  float sa = 1.0f;
  int32_t color_id = 1;
};

struct PreviewLaneGeomSplit {
  std::vector<PreviewLaneGeomCell> cells;
  float play_sec = 0.0f;
  // Last pump/draw seq that used this key. Seek keeps the pool; eviction
  // drops entries not touched for several seconds, or the oldest when
  // the pool is over the bounded cap.
  uint32_t last_hit_seq = 0;
};

// Chart playback preview surface. The UI host supplies the Vulkan instance/surface
// through VulkanHostSurface; this class owns the renderer and consumes snapshots.
class PlaybackPreviewView {
 public:
  PlaybackPreviewView();
  ~PlaybackPreviewView();

  PlaybackPreviewView(const PlaybackPreviewView&) = delete;
  PlaybackPreviewView& operator=(const PlaybackPreviewView&) = delete;

  bool initialize(const wds::renderer::VulkanHostSurface& host, const wds::renderer::PreviewVisualConfig& config = {});
  void shutdown();

  bool ready() const noexcept { return ready_; }

  void set_config(const wds::renderer::PreviewVisualConfig& config);
  const wds::renderer::PreviewVisualConfig& config() const noexcept { return config_; }
  // Persist preferred MSAA (1 / 2 / 4) and rebuild the swapchain when ready.
  void apply_msaa(int samples);

  void attach_audio(wds::audio::AudioEngine* audio) noexcept;
  // Read-only armed MIXTIME POS count (0 when no audio).
  size_t pending_sfx_sync_count() const noexcept;

  // Chart delay (MusicTiming::offset_ms): tick 0 maps to this music time.
  // Negative offset is a silent preroll; SFX still schedule on the music clock.
  void set_chart_offset_ms(int64_t offset_ms) noexcept { chart_offset_ms_ = offset_ms; }
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

  // Last draw_split_particles diagnostics (frame-diag / pause-memo).
  struct SplitFrameDiag {
    uint64_t revision = 0;
    int64_t now_us = 0;
    int quality = 0;
    int splits = 0;
    int particles = 0;
    uint32_t cache_epoch = 0;
    int pool_cap = 0;
    int pool_participants = 0;
    int pool_last = 0;
    int pool_p_cores = 0;
    int pool_e_cores = 0;
    const char* simd_path = "";
    uint32_t cache_entries = 0;
    uint64_t cache_lookup_ops = 0;
    uint64_t cache_lookup_hits = 0;
    uint64_t cache_lookup_contend = 0;
    uint64_t cache_store_ops = 0;
    uint64_t cache_store_contend = 0;
    int cleared = 0;
    int collect_memo = 0;
    int verts_memo = 0;
    int playing = 0;
    int64_t collect_us = 0;
    int64_t verts_us = 0;
    int64_t lanes_us = 0;
    int lanes_geom_miss = 0;
    int64_t tex_us = 0;
    int64_t upload_us = 0;
    int64_t submit_us = 0;
    int64_t total_us = 0;
  };
  const SplitFrameDiag& last_split_frame_diag() const noexcept { return split_frame_diag_; }

  // Decode UV/trail on a worker; GPU upload on pump/ensure (render thread).
  void request_split_effect_preload();
  void pump_split_effect_preload();
  // Idle-build split-lane plate corners (steady percents). First seek then
  // only recolors. Call from the panel tick when lanes/effects/view change.
  // Full clear: geometry / texture / document changed. Recycles cells.
  void invalidate_lane_geom_cache();
  // Seek: drop pause/playback memos and the current frame list. Keep
  // lane_geom_pool_ (keys are split-line parameters, not chart time).
  void reset_lane_geom_time_state();
  bool lane_geom_cache_empty() const noexcept { return lane_geom_pool_.empty(); }
  // High-water request (no touch). Idle pump commits pages off the edit frame.
  void request_buffer_precommit(const wds::chart_render::SplitEffectPrecommitPlan& plan,
                                bool fill_now);
  void pump_buffer_precommit(int budget_us);
  const wds::chart_render::SplitEffectPrecommitStats& last_precommit() const noexcept {
    return last_precommit_;
  }
  void pump_lane_geom_preload(
      const std::vector<wds::chart_editor::PreviewSplitLaneInstance>& splits,
      int64_t now_us = 0, bool retain_colored_batch = false);

  // No-op leftover: speculative collect pipeline was removed.
  void join_speculative_split_collect();

  // Update hit SFX from the latest snapshot. Call after apply_timeline; still
  // before Transport::start_pending_music() so the first playing frame sees a
  // paused BGM clock when marking past hits.
  void sync_hit_sfx(const wds::chart_editor::PreviewSnapshot& snapshot);

  // ui_solid_texture: 1×1 white used by UiPainter panels. Overlay solids are drawn
  // under the stage; overlay sprites (and edit-area skins) go through mid_overlay
  // with notes (sort 10). modal_overlay / modal_chrome use post passes so sticky
  // DrawBatch bucket indices cannot bury them. Chrome draws last. Combo / AUTO
  // HUD is a later playfield_hud pass (TimingEffect 99, then ScreenSpaceOverlay).
  void render(const wds::chart_editor::PreviewSnapshot& snapshot,
              const wds::renderer::DrawBatch* ui_overlay = nullptr,
              wds::renderer::TextureId ui_solid_texture = wds::renderer::kInvalidTextureId,
              const wds::renderer::DrawBatch* modal_overlay = nullptr,
              const wds::renderer::DrawBatch* modal_chrome = nullptr,
              int64_t visual_lead_us = 0, int64_t pulse_now_us = 0);

  // Upload processed 800×800 RGBA8 under a fixed session key, or unload when empty.
  bool load_jacket(const std::vector<unsigned char>& rgba);

  // Render an editor-only batch into this surface. This intentionally skips
  // preview stage, notes and timing effects so the editor viewport cannot
  // duplicate the adjacent realtime preview.
  void render_editor_only(const wds::renderer::DrawBatch& editor_batch);

  wds::renderer::StageGeometry& geometry() noexcept { return geometry_; }
  const wds::renderer::StageGeometry& geometry() const noexcept { return geometry_; }
  wds::renderer::VulkanRenderer& vulkan() noexcept { return vulkan_; }
  wds::renderer::TextureCache& textures() noexcept { return textures_; }
  const wds::renderer::SkinCatalog& skin() const noexcept { return skin_; }

 private:
  void draw_jacket(wds::renderer::DrawBatch& batch);
  void draw_ingame_background(wds::renderer::DrawBatch& batch);
  void draw_stage(wds::renderer::DrawBatch& batch,
                  const wds::chart_editor::PreviewSnapshot& snapshot,
                  wds::renderer::TextureId solid_texture);
  void draw_hidden_line(wds::renderer::DrawBatch& batch);
  // Official LaneMask bottom percent (GetNoteVisiblePositionY). z=0 objects cull here.
  float spawn_clip_percent() const noexcept;
  void draw_split_lanes(wds::renderer::DrawBatch& batch,
                        const wds::chart_editor::PreviewSnapshot& snapshot);
  void draw_split_particles(const wds::chart_editor::PreviewSnapshot& snapshot,
                             wds::renderer::TextureId solid_texture);
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
  void ensure_split_effect_textures();
  void assign_split_effect_texture(const std::string& path, const wds::renderer::TextureInfo& info);
  void finish_split_effect_preload_upload();
  void pump_draw_precommit_slice(bool all);
  void collect_hold_intervals(const wds::chart_editor::PreviewSnapshot& snapshot,
                              std::vector<wds::audio::HoldInterval>& out) const;
  void sync_hold_loop(const wds::chart_editor::PreviewSnapshot& snapshot, int64_t clock_us,
                      bool arm);
  // Mute SFX, clear played keys, latch mono clock to raw_us (pause / seek / scrub).
  void release_sfx_clock_control(const wds::chart_editor::PreviewSnapshot& snapshot,
                                 int64_t raw_us, bool playing);
  // Rebuild GenerateNoteId-reversed draw index for the current visible set.
  void prepare_note_draw_order(const wds::chart_editor::PreviewSnapshot& snapshot);
  // After present: copy mapped-VB / CPU parts into pause memo (not tess time).
  void persist_split_pause_vert_memo();

  wds::renderer::PreviewVisualConfig config_;
  wds::renderer::StageGeometry geometry_;
  wds::renderer::VulkanRenderer vulkan_;
  wds::renderer::TextureCache textures_;
  wds::renderer::SkinCatalog skin_;
  wds::renderer::TextureInfo jacket_{};
  wds::audio::HitSfxPlayer hit_sfx_;
  wds::renderer::DrawBatch batch_;
  wds::renderer::DrawBatch notes_batch_;
  wds::renderer::DrawBatch glow_batch_;
  // All split-effect particles (Alpha / Additive / Trail), ordered by
  // renderer sortingOrder. Blitted onto the resolved HDR scene (SortingGroup
  // 20 after notes 10 and line 0). Hits stay additive_batch_ (Bomb group 30)
  // and are not counted against kSplitEffectParticlePool. Combo / AUTO go to
  // hud_batch_ after bombs (TimingEffect 99, Combo ScreenSpaceOverlay).
  wds::renderer::DrawBatch split_late_batch_;
  wds::renderer::DrawBatch additive_batch_;
  wds::renderer::DrawBatch hud_batch_;
  // Indices into snapshot.notes: GenerateNoteId order reversed (bottom-most first).
  std::vector<size_t> notes_draw_indices_;
  std::vector<const wds::chart_editor::PreviewNoteInstance*> note_draw_order_;
  bool ready_ = false;
  bool mute_hold_body_sfx_ = false;
  bool show_judgment_text_ = false;
  int64_t chart_offset_ms_ = 0;
  int64_t preview_lead_in_visible_ms_ = 0;
  // Present-only visual lead (µs). SFX / engine snapshot stay on the committed clock.
  int64_t visual_lead_us_ = 0;
  // Syuriken pulse clock (µs). Independent of visual_lead extra. See pulse_now_us hop.
  int64_t pulse_now_us_ = 0;
  // Monotonic SFX clock (µs). Advances with BASS/Timeline; ignores small backwards
  // glitches. Released (reset) on pause / seek / scrub via control generation.
  int64_t sfx_mono_us_ = -1;
  // AudioEngine::position_generation() — seek / scrub / play set_position.
  uint64_t sfx_position_generation_ = std::numeric_limits<uint64_t>::max();
  uint64_t sfx_document_revision_ = std::numeric_limits<uint64_t>::max();
  bool sfx_was_playing_ = false;
  std::unordered_set<uint64_t> hit_sfx_played_;
  wds::audio::SfxPlayHistory sfx_history_;
  wds::audio::SfxInstantBudget sfx_budget_;
  uint64_t hold_gates_generation_ = std::numeric_limits<uint64_t>::max();
  uint64_t hold_gates_revision_ = std::numeric_limits<uint64_t>::max();
  // Continuous SplitEffect playback boundary. Paused/seek snapshots use the
  // stateless collector; this state is reset on Play/pause, seek, and edits.
  wds::chart_render::SplitEffectPlaybackState split_playback_state_;
  wds::chart_render::SplitEffectFrameCollect split_effect_collect_;
  bool split_playback_was_playing_ = false;
  uint64_t split_playback_revision_ = std::numeric_limits<uint64_t>::max();
  // Paused collect/vert memo: reuse last frame when every collect input (and
  // the viewport, for verts) is unchanged. Playing always recollects.
  // Glow/notes/late memos are particle-only — lane plates live in
  // split_pause_lanes_glow_ and must not be replayed with the particle glow.
  bool split_pause_collect_valid_ = false;
  bool split_pause_verts_valid_ = false;
  bool split_pause_verts_pending_ = false;
  uint64_t split_pause_revision_ = std::numeric_limits<uint64_t>::max();
  int64_t split_pause_now_us_ = std::numeric_limits<int64_t>::min();
  int split_pause_quality_ = -1;
  uint64_t split_pause_splits_hash_ = 0;
  uint64_t split_pause_view_hash_ = 0;
  uint32_t split_pause_seed_epoch_ = 0;
  int split_pause_drawn_ = 0;
  int64_t split_pause_vert_count_ = 0;
  wds::renderer::DrawBatch split_pause_notes_;
  wds::renderer::DrawBatch split_pause_glow_;
  wds::renderer::DrawBatch split_pause_late_;
  // Sticky particle-only emit dests + ordered-merge parts. clear() keeps
  // capacity so 10660-class late (~1.1M verts) does not realloc every frame.
  wds::renderer::DrawBatch split_emit_notes_;
  wds::renderer::DrawBatch split_emit_glow_;
  wds::renderer::DrawBatch split_emit_late_;
  struct SplitEmitPart {
    wds::renderer::DrawBatch notes;
    wds::renderer::DrawBatch glow;
    wds::renderer::DrawBatch late;
    int drawn = 0;
  };
  std::vector<SplitEmitPart> split_emit_parts_;
  size_t split_emit_reserve_hint_ = 0;
  // Split line plate geometry (Syuriken CPU tess) kept separate from particle glow.
  wds::renderer::DrawBatch split_lanes_glow_;
  bool split_pause_lanes_valid_ = false;
  uint64_t split_pause_lanes_hash_ = 0;
  wds::renderer::DrawBatch split_pause_lanes_glow_;
  // Per-split plate cache: one projected quad per Line (not now_us / fade).
  // Keyed by identity + percents + view. Idle preload fills steady plates;
  // each frame only rewrites the fade-dependent V span and world Y scale.
  std::unordered_map<uint64_t, PreviewLaneGeomSplit> lane_geom_pool_;
  std::vector<PreviewLaneGeomSplit> lane_geom_free_;
  std::vector<const PreviewLaneGeomSplit*> lane_geom_frame_;
  uint32_t lane_geom_hit_seq_ = 1;
  std::vector<wds::renderer::DrawBatch> lane_color_parts_;
  wds::renderer::DrawBatch lane_color_warmup_;
  uint64_t lane_color_warmup_hash_ = 0;
  bool lane_color_warmup_valid_ = false;
  std::vector<uint32_t> emit_notes_cap_;
  std::vector<uint32_t> emit_glow_cap_;
  std::vector<uint32_t> emit_late_cap_;
  // Particle tessellation parts written straight into the mapped VB (no
  // append_from into the main glow/notes/late batches). Fallback merges.
  bool split_emit_direct_ = false;
  size_t split_emit_direct_n_ = 0;
  bool split_emit_in_vb_ = false;
  uint32_t split_emit_vb_low_ = 0;
  std::vector<const wds::renderer::DrawBatch*> split_emit_early_ptrs_;
  std::vector<const wds::renderer::DrawBatch*> split_emit_mid_ptrs_;
  std::vector<const wds::renderer::DrawBatch*> split_emit_carve_ptrs_;
  SplitFrameDiag split_frame_diag_{};
  struct SplitTexPreloadState;
  std::unique_ptr<SplitTexPreloadState> split_tex_preload_;
  int64_t last_ensure_tex_us_ = 0;
  int64_t last_lanes_us_ = 0;
  int last_lanes_geom_miss_ = 0;
  wds::chart_render::SplitEffectPrecommitStats last_precommit_{};
  std::size_t draw_precommit_verts_ = 0;
  int draw_precommit_step_ = 0;
};

}  // namespace wds::ui

#endif  // WDS_UI_PLAYBACK_PREVIEW_HELPERS_ONLY

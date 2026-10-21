#pragma once

#include "wds/ui/regions/edit/chart_edit_renderer.hpp"
#include "wds/ui/toolbar_curve_selection.hpp"

#include <wds/core/edit_grid.hpp>
#include <wds/core/edit_history.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/types.hpp>

#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/gesture.hpp>
#include <wds/interaction/widget.hpp>
#include <wds/renderer/draw_batch.hpp>
#include <wds/renderer/skin_catalog.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wds::chart_editor {
class ChartEditorEngine;
}

namespace wds::ui {

// Id / tick set whose every mutation bumps a generation counter. visual_revision()
// hashes the counter instead of walking the set, and because the underlying set is
// only reachable through const access, no mutation site can forget to bump it.
class TrackedIdSet {
 public:
  using Set = std::unordered_set<int32_t>;

  TrackedIdSet() = default;
  TrackedIdSet(const TrackedIdSet&) = delete;
  TrackedIdSet& operator=(const TrackedIdSet&) = delete;

  TrackedIdSet& operator=(Set other) {
    set_ = std::move(other);
    ++generation_;
    return *this;
  }

  const Set& set() const noexcept { return set_; }
  operator const Set&() const noexcept { return set_; }
  uint64_t generation() const noexcept { return generation_; }

  bool empty() const noexcept { return set_.empty(); }
  std::size_t size() const noexcept { return set_.size(); }
  std::size_t count(int32_t id) const { return set_.count(id); }
  Set::const_iterator begin() const noexcept { return set_.begin(); }
  Set::const_iterator end() const noexcept { return set_.end(); }
  Set::const_iterator find(int32_t id) const { return set_.find(id); }

  bool insert(int32_t id) {
    const bool inserted = set_.insert(id).second;
    if (inserted) ++generation_;
    return inserted;
  }
  std::size_t erase(int32_t id) {
    const std::size_t removed = set_.erase(id);
    if (removed != 0) ++generation_;
    return removed;
  }
  void clear() {
    if (set_.empty()) return;
    set_.clear();
    ++generation_;
  }

 private:
  Set set_;
  uint64_t generation_ = 0;
};

struct ChartEditPanelTestAccess;

class ChartEditPanel final : public wds::interaction::Widget {
 public:
  friend struct ChartEditPanelTestAccess;  // tests reach hit_test_note / placement helpers
  explicit ChartEditPanel(wds::chart_editor::ChartEditorEngine& engine);

  void set_grid(wds::chart_editor::EditGridConfig grid);
  // Seek transport so edit scroll stays locked to preview playhead.
  void set_seek_ms(std::function<void(int64_t)> seek) { seek_ms_ = std::move(seek); }
  // Fired after exact Option+wheel changes visible_hectoms (sync toolbar + persist).
  void set_visible_range_changed_handler(std::function<void()> handler) {
    on_visible_range_changed_ = std::move(handler);
  }
  void set_extra_visible_lanes_changed_handler(std::function<void()> handler) {
    on_extra_visible_lanes_changed_ = std::move(handler);
  }
  void set_cursor_setter(std::function<void(wds::interaction::CursorKind)> setter) {
    cursor_setter_ = std::move(setter);
  }
  // Keep judgment line aligned with engine timeline (call after transport tick).
  // Prefer sub-ms (double) so scroll does not stair-step on floored int64 ms.
  void sync_to_timeline_ms(double timeline_ms) const;
  void sync_to_timeline_ms(int64_t timeline_ms) const {
    sync_to_timeline_ms(static_cast<double>(timeline_ms));
  }
  // Re-derive placement / gutter ghosts / live drags from the current pointer
  // after the viewport scrolls (playback or scrub). Ghosts and drag targets are
  // stored in tick space; without this they stick to the old tick and scroll
  // away under a stationary mouse.
  void resync_pointer_overlays();
  // Host feeds the true window pointer each frame so Idle ghosts can hide when
  // the cursor leaves the edit pane (move events stop once hover leaves).
  void sync_global_pointer(wds::interaction::Vec2 point);
  const EditViewport& viewport() const noexcept { return viewport_; }
  EditViewport& viewport() noexcept { return viewport_; }
  wds::interaction::CursorKind hover_cursor() const noexcept { return hover_cursor_; }
  wds::chart_editor::ChartEditorEngine& engine() const noexcept { return engine_; }
  // Cheap visual fingerprint: document generation + viewport + selection/hover/ghost.
  uint64_t visual_revision() const;

  const EditDrawDepthConfig& draw_depth() const noexcept { return renderer_.draw_depth(); }
  EditDrawDepthConfig& draw_depth() noexcept { return renderer_.draw_depth(); }
  void set_draw_depth(EditDrawDepthConfig depth) noexcept { renderer_.set_draw_depth(depth); }

  const std::unordered_set<int32_t>& selected() const noexcept { return selected_.set(); }
  const std::unordered_set<int32_t>& selected_speed_ticks() const noexcept {
    return selected_speed_ticks_.set();
  }
  void clear_selection() {
    selected_.clear();
    selected_speed_ticks_.clear();
    clear_hold_sel_focus();
  }
  void set_selected(std::unordered_set<int32_t> ids);
  void set_selected_speed_ticks(std::unordered_set<int32_t> ticks);

  int default_width() const noexcept { return default_width_; }
  void set_default_width(int width) noexcept;

  bool split_width_follow() const noexcept { return split_width_follow_; }
  void set_split_width_follow(bool on) noexcept { split_width_follow_ = on; }

  // When true: Space pauses in place; Shift+Space returns to play-start. When false (default):
  // Space returns to play-start; Shift+Space pauses in place.
  bool pause_at_current() const noexcept { return pause_at_current_; }
  void set_pause_at_current(bool on) noexcept { pause_at_current_ = on; }

  void set_skin(const wds::renderer::SkinCatalog* skin) noexcept { skin_ = skin; }
  void set_waveform(const wds::audio::WaveformOverview* waveform) noexcept {
    waveform_ = waveform;
  }
  const wds::audio::WaveformOverview* waveform() const noexcept { return waveform_; }
  void set_spectrogram(wds::renderer::TextureInfo spectrogram) noexcept {
    spectrogram_ = spectrogram;
  }
  void set_spectrum_mode(EditSpectrumMode mode) noexcept { spectrum_mode_ = mode; }
  EditSpectrumMode spectrum_mode() const noexcept { return spectrum_mode_; }
  void set_note_height_level(int level) noexcept { viewport_.set_note_height_level(level); }

  void set_extended_editing_active(bool enabled) noexcept;
  bool extended_editing_active() const noexcept { return extended_editing_active_; }
  void set_extra_visible_lanes(int extra) noexcept;
  int extra_visible_lanes() const noexcept { return extra_visible_lanes_session_; }
  void set_lane_slider_hovered(bool hovered) noexcept;
  bool lane_slider_hovered() const noexcept { return lane_slider_hovered_; }
  std::size_t gutter_ghost_count() const noexcept { return gutter_ghosts_.size(); }
  const wds::interaction::Rect& playfield() const noexcept { return playfield_; }
  const wds::interaction::Rect& left_gutter() const noexcept { return left_gutter_; }
  const wds::interaction::Rect& speed_gutter() const noexcept { return speed_gutter_; }

  // Toolbox (new-note-place) mode: left click places the current override type.
  // Tests call this directly; UiManager wires it from new_note_place_logic.
  void set_toolbox_place_mode(bool enabled) noexcept;
  bool toolbox_place_mode() const noexcept { return toolbox_place_mode_; }

  // Selected place type while toolbox mode is on. Tap is the default (not None).
  void set_place_intent_override(wds::interaction::PlaceIntent intent) noexcept {
    place_intent_override_ = intent;
  }
  wds::interaction::PlaceIntent place_intent_override() const noexcept {
    return place_intent_override_;
  }

  // scratch_length: Flick / ScratchHold direction intent (-1 left, 0 both, +1 right).
  // Flick is stored as the official signed span (0 / ±width). ScratchHold uses
  // ±width for equal-width tails. Pass nullopt for convert_note_type defaults.
  // Target Sound / ScratchSound converts selected hold children and selected
  // star bodies only; other targets skip star bodies, while Hold → non-hold
  // deletes all attached stars.
  bool convert_selected(wds::chart_editor::NoteType target,
                        std::optional<int32_t> scratch_length = std::nullopt);
  bool mirror_selected(bool about_center);
  bool nudge_selected(int32_t delta_tick, int32_t delta_lane);
  bool copy_selected();
  // `mirror`: same as paste then playfield-mirror, one history step. Splits skip the flip.
  bool paste_at_pointer(bool mirror = false);
  bool delete_selected();
  // Delete one note under the pointer; drops it from the selection if present.
  bool delete_note_at(wds::interaction::Vec2 point);

  void set_curve_fill_selection(CurveFillSelection selection);
  CurveFillSelection curve_fill_selection() const noexcept { return curve_fill_selection_; }
  bool curve_mode_active() const noexcept { return curve_mode_active_; }
  std::vector<wds::chart_editor::NotationNote> curve_ghost_notes() const;
  // Same placement / curve / hold-star ghosts as append_skinned_ghosts.
  std::vector<EditGhost> skinned_ghosts() const;

  // Playback tap / hold marks (session overlay; not part of the chart).
  // Taps are independent (every key-down leaves a red head). The pink trail is
  // shared: it lives while any record key is down.
  static constexpr float kClickRecordHoldThresholdMs = 120.0f;
  struct ClickTapMark {
    double ms = 0.0;
  };
  struct ClickHoldMark {
    double start_ms = 0.0;
    double end_ms = 0.0;
  };
  void click_record_tap(double now_ms);
  void click_record_hold_begin(double now_ms);
  void click_record_hold_end(double now_ms);
  void click_record_sync_playing(bool playing, double now_ms);
  void clear_click_marks();
  bool has_click_marks() const noexcept;
  const std::vector<ClickTapMark>& click_taps() const noexcept { return click_taps_; }
  const std::vector<ClickHoldMark>& click_holds() const noexcept { return click_holds_; }
  std::optional<ClickHoldMark> pending_click_hold(double now_ms) const;

  // Persistent overlap markers. Replaces the previous set; empty ticks clear.
  // Cleared when document content_generation differs from `content_generation`.
  void set_error_ticks(std::vector<int32_t> ticks, uint64_t content_generation);
  const std::vector<int32_t>& error_ticks() const noexcept;
  // Red filter on notes / split bodies after a rejected delay edit (2s solid, 1s fade).
  void flash_offset_violations(std::vector<int32_t> ids);

  bool wants_focus() const override { return true; }
  // Timing / split modals own the keyboard so Space/Delete/arrows do not hit global chords.
  bool captures_keys() const override { return has_modal_popup(); }

  void update(float delta_seconds) override;
  void paint(wds::interaction::UiPainter& painter) const override;
  // Selection / marquee overlay — call after append_skin_batch so it draws on top.
  void paint_overlays(wds::interaction::UiPainter& painter) const;
  // Qt canvas: split / BPM / meter / measure columns and chips (no form chrome).
  void paint_side_columns(wds::interaction::UiPainter& painter) const;
  // Hovered or dragged split-label pair; -1 if none. Hover card only — playfield
  // split lines bold from selected(), same as the gutter chips.
  int32_t active_split_highlight_id() const noexcept;
  struct SplitHoverHint {
    int32_t color_id = 1;
    int32_t split_count = 1;
    wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full;
    wds::interaction::Rect chip_bounds{};
  };
  std::optional<SplitHoverHint> active_split_hover() const;
  // Live Cmd/Ctrl drag-select rectangle in screen space; empty when idle.
  std::optional<wds::interaction::Rect> active_marquee_rect() const;

  struct SplitModalDraft {
    int32_t tick = 0;
    int32_t edit_id = -1;
    int32_t count = 2;
    int32_t color_id = 1;
    wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full;
  };
  struct TimingModalDraft {
    bool bpm_mode = true;
    int32_t tick = 0;
    double bpm = 120.0;
    int32_t numerator = 4;
    int32_t denominator = 4;
  };
  struct SpeedModalDraft {
    int32_t tick = 0;
    double multiplier = 1.0;
  };
  // Snapshot + close an in-panel modal so a Qt dialog can own the form.
  bool take_split_modal(SplitModalDraft& out);
  bool take_timing_modal(TimingModalDraft& out);
  bool take_speed_modal(SpeedModalDraft& out);
  bool apply_scroll_speed(int32_t tick, double multiplier);
  bool delete_scroll_speed(int32_t tick);
  bool add_split_effect(int32_t tick, int32_t count, int32_t color_id,
                        wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full);
  bool edit_split_effect(int32_t note_id, int32_t count, int32_t color_id,
                         wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full);
  bool apply_bpm(int32_t tick, double bpm);
  bool apply_meter(int32_t tick, int32_t numerator, int32_t denominator);

  // Modal dialogs (split picker / timing) — paint after overlays, above everything.
  bool has_modal_popup() const noexcept {
    return split_picker_open_ || timing_popup_open_ || speed_popup_open_;
  }
  bool is_interaction_modal() const override { return has_modal_popup(); }
  bool blocks_interaction_behind(wds::interaction::Vec2 point) const override;
  void paint_popups(wds::interaction::UiPainter& painter) const;
  // Footer buttons drawn in a later pass so list sprites cannot cover them.
  void paint_popup_chrome(wds::interaction::UiPainter& painter) const;
  // When a modal is open, capture hits across the whole window (not just the edit pane).
  wds::interaction::Widget* hit_test(wds::interaction::Vec2 point) override;
  void append_skin_batch(wds::renderer::DrawBatch& batch,
                         const wds::renderer::SkinCatalog& skin, int fb_w, int fb_h,
                         wds::renderer::ScreenBounds screen,
                         float stage_opacity = 0.8f) const;
  void on_pointer_down(const wds::interaction::PointerDownEvent& event) override;
  void on_pointer_move(const wds::interaction::PointerMoveEvent& event) override;
  void on_pointer_up(const wds::interaction::PointerUpEvent& event) override;
  void on_hover_leave() override;
  void on_double_click(const wds::interaction::DoubleClickEvent& event) override;
  void on_scroll(const wds::interaction::ScrollEvent& event) override;
  // Shared timeline scrub / visible-range zoom. PreviewHitWidget calls this
  // without the modal or edit-pane bounds guards in on_scroll.
  void handle_timeline_wheel(const wds::interaction::ScrollEvent& event);
  void on_key_down(const wds::interaction::KeyDownEvent& event) override;
  void on_key_up(const wds::interaction::KeyUpEvent& event) override;
  // Keep Shift/Cmd/Alt in sync when Qt drops them from mouse events.
  void sync_active_modifiers(wds::interaction::Modifiers mods);
  void on_text_input(const wds::interaction::TextInputEvent& event) override;

  const char* trace_name() const override { return "ChartEditPanel"; }
  void trace_snapshot(wds::common::CrashTraceSnap& snap) const override;
  std::uint8_t trace_drag_mode() const override { return static_cast<std::uint8_t>(mode_); }

 private:
  enum class Mode {
    Idle,
    PlaceGesture,
    Marquee,
    MoveSelection,
    ResizeWidth,
    AdjustHoldTime,
    PlaceHoldBody,
    DragSplitEdge,
    DragSpeedPoint,
  };

  struct GhostNote {
    wds::chart_editor::NotationNote note;
    bool visible = true;
  };

  void sync_viewport() const;
  wds::chart_editor::NotationNote make_base_note(wds::interaction::Vec2 point) const;
  int effective_placement_width(wds::interaction::Vec2 point) const;
  // Applies split-track lane/width when follow is on: union of all split
  // effects whose closed [start, end] covers the point's tick.
  void apply_placement_lane_width(wds::interaction::Vec2 point, int32_t& lane,
                                  int32_t& width) const;
  std::optional<wds::chart_editor::NotationNote> hit_test_note(wds::interaction::Vec2 point) const;
  // Longest tick spans (end - start, floored at 0) per note class, cached per
  // document content generation. They bound how far before a pointer's time an
  // overlapping hold / split can start, so scans can binary-search notes().
  struct NoteSpanCache {
    const void* doc = nullptr;
    uint64_t generation = 0;
    std::size_t note_count = 0;
    bool valid = false;
    int64_t hit_span = 0;    // hit-testable notes (not split / HoldEighth / mid-star)
    int64_t chain_span = 0;  // hold-chain bodies
    int64_t split_span = 0;  // split-lane gimmicks
  };
  const NoteSpanCache& note_spans() const;
  // Contiguous slice of document().notes() (same order) that can reach within
  // `reach` px of screen `y`; a conservative superset, callers re-test exactly.
  std::pair<const wds::chart_editor::NotationNote*, const wds::chart_editor::NotationNote*>
  notes_near_y(float y, float reach, int64_t max_span_ticks) const;
  float width_edge_px(const wds::chart_editor::NotationNote& note) const;
  float time_edge_px(const wds::chart_editor::NotationNote& note) const;
  bool near_left_edge(const wds::chart_editor::NotationNote& note, float x) const;
  bool near_right_edge(const wds::chart_editor::NotationNote& note, float x) const;
  bool near_start_time(const wds::chart_editor::NotationNote& note, float y) const;
  bool near_end_time(const wds::chart_editor::NotationNote& note, float y) const;
  // True when the body's paired hold head (official start note) covers screen x.
  // A body without a covering head has no start sprite there: only the ribbon
  // ends at the start tick, so the start hit / selection caps must stay tight.
  bool hold_body_head_covers_x(const wds::chart_editor::NotationNote& body, float x) const;
  // Extra screen-space tolerance past a headless body's start tick (no head cap).
  float headless_hold_start_pad_px() const;
  // ScratchHold end span edges (Sirius scratchLength), distinct from body edges.
  bool near_scratch_end_left(const wds::chart_editor::NotationNote& note, float x) const;
  bool near_scratch_end_right(const wds::chart_editor::NotationNote& note, float x) const;
  // Vertical band around the JumpScratch end-cap (same pad as hit_test end zone).
  bool near_scratch_end_cap(const wds::chart_editor::NotationNote& note, float y) const;
  void update_hover_cursor(wds::interaction::Vec2 point);
  void set_hover_cursor(wds::interaction::CursorKind kind);

  void hide_placement_ghost();
  void update_ghost(wds::interaction::Vec2 point);
  // True while placing a note (press-drag gesture or hold-body draft).
  bool is_note_drawing() const noexcept;
  // Edit pane plus a small leave slop — beyond this Idle ghosts must hide.
  bool pointer_in_edit_ghost_zone(wds::interaction::Vec2 point) const;
  // Live-update hold_draft_ end (and chain lane) from pointer under current scroll.
  // WriteLivePrevCover also pushes the previous JumpScratch cover into the document
  // (ordinary chain preview). LocalDraftOnly updates draft/ghost only.
  enum class HoldDraftSync { LocalDraftOnly, WriteLivePrevCover };
  void sync_hold_draft_to_pointer(
      HoldDraftSync sync = HoldDraftSync::WriteLivePrevCover);
  void finish_place_gesture(const wds::interaction::PointerUpEvent& event);
  void finish_marquee(wds::interaction::Vec2 end);
  void begin_marquee(wds::interaction::Vec2 pos);
  // Marquee in tick/lane space so scroll during drag can extend past the view.
  // Time snaps to the edit subdivision; X expands to whole tracks (split gutter,
  // each playfield lane, and the speed gutter when extended editing is on).
  wds::interaction::Rect marquee_screen_rect(wds::interaction::Vec2 end) const;
  // Left/right screen edges of the marquee track under `x` (split / lane / speed).
  std::pair<float, float> marquee_track_x_span(float x) const;
  void begin_move_selection_from_label(wds::interaction::Vec2 pos, int32_t open_split_id,
                                       int32_t open_speed_tick);
  void finish_move(bool refresh_eighths = true, bool prune_stars = false);
  void finish_resize();
  void finish_hold_adjust();
  void finish_hold_body(bool chain_next);
  // Map an out-of-window pointer to the equivalent in-window edge point for
  // hit/snap math only — does not mutate stored pointer state.
  wds::interaction::Vec2 pointer_as_in_host(wds::interaction::Vec2 point) const;
  // Live-update MoveSelection from a pointer (edit-area / window exit OK).
  void sync_move_selection_to_pointer(wds::interaction::Vec2 point);
  // Live-update DragSplitEdge from a pointer so scroll-without-move still tracks.
  void sync_split_edge_to_pointer(wds::interaction::Vec2 point);
  // Live-update AdjustHoldTime (hold tail / JumpScratch hinge) from a pointer
  // so wheel scrub and playback keep the grabbed edge under the cursor.
  void sync_hold_adjust_to_pointer(wds::interaction::Vec2 point);
  // Middle-button interrupt: drop in-progress place / hold draft. For chained
  // ScratchHold, discards only the current segment and keeps the previous as end.
  void cancel_placement();
  // Persist live covering-tail edits on the previous chain segment (undoable).
  void commit_hold_chain_prev_cover();
  // Revert live covering-tail preview without committing.
  void restore_hold_chain_prev_preview();

  bool commit_notes(std::vector<wds::chart_editor::NotationNote> notes, const std::string& label);
  bool commit_updates(const std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair>&
                          changes,
                      const std::string& label);

  void place_instant(wds::chart_editor::NoteType type, wds::interaction::Vec2 point,
                     int32_t scratch_length = 0);
  void begin_hold_body(bool scratch, wds::interaction::Vec2 point);
  // Continue a selected terminal hold from its end-cap. Requires
  // pending_chain_extend_id_ armed on pointer-down. Works for both the regular
  // and ScratchHold chain families.
  void begin_hold_chain_extend();
  // If point is on a selected terminal hold end-cap, arm pending_chain_extend_id_.
  // `scratch_family` restricts the search to one family (toolbox placement);
  // nullopt accepts either family (right-click on an already-selected tail).
  void try_arm_pending_chain_extend(wds::interaction::Vec2 point,
                                    std::optional<bool> scratch_family = std::nullopt);
  void clear_pending_chain_extend() { pending_chain_extend_id_ = -1; }
  // Enter ScratchHold placement: chain-extend when pending, else fresh begin_hold_body.
  void begin_scratch_hold_placement(wds::interaction::Vec2 point);
  void begin_regular_hold_placement(wds::interaction::Vec2 point);
  void add_hold_star_at(wds::interaction::Vec2 point);
  // Bindable hold body whose open tick span and lane span contain `point`.
  // Used by both the Ched-like star ghost and the actual placement so the two
  // cannot disagree. Returns nullopt when the pointer is not inside a hold.
  std::optional<wds::chart_editor::NotationNote> hold_body_under_pointer(
      wds::interaction::Vec2 point) const;
  static std::optional<wds::chart_editor::NoteType> star_type_for_place_intent(
      wds::interaction::PlaceIntent intent) noexcept;
  // Commit one Sound / ScratchSound into `hold` at `tick`, snapping lane/width to
  // the hold and regenerating its eighths in the same undo step.
  bool commit_star_on_hold(const wds::chart_editor::NotationNote& hold, int32_t tick,
                           wds::chart_editor::NoteType star_type, const char* label);
  // Ched-like placement: the hold under the pointer decides the lane span.
  bool place_star_under_pointer(wds::interaction::Vec2 point,
                                wds::chart_editor::NoteType star_type);
  // Place a Sound / ScratchSound on an already-selected existing hold body.
  // The requested star type is independent of the hold's color family.
  bool add_star_to_selected_hold(wds::interaction::Vec2 point,
                                 wds::chart_editor::NoteType star_type);
  // Keep placement ghost in sync with hold_draft_ (zero length → Tap / Flick).
  void sync_hold_placement_ghost();
  // First-segment gold head: Shift was down at mouse press, and this draft is
  // not a chain continuation. Release after press does not change the lock.
  bool want_gold_first_hold_segment() const noexcept;
  // Keep hold_draft_ tinted to the press-time gold-head lock.
  void sync_drawn_hold_body_type();
  void apply_gold_first_curve_body(std::vector<wds::chart_editor::NotationNote>& bodies) const;
  // Screen center of the placement note (gesture dx/dy / unlock origin).
  wds::interaction::Vec2 place_note_center() const;
  // Re-apply default_width_ to the locked placement note without chasing the pointer.
  void apply_width_to_locked_placement();
  // Live-sync previous chain segment's covering tail to hold_draft_; returns false
  // when the cover is not Sirius-representable.
  bool sync_chain_prev_tail_cover();
  // True when hold_draft_ + prev body can form a Sirius JumpScratch cover.
  bool chain_draft_cover_representable() const noexcept;
  // Snap hold_draft_.lane onto a JumpScratch-legal chain lane near desired_lane_f.
  // WriteLivePrevCover also writes/restores the previous cover in the document.
  // LocalDraftOnly only updates local draft + link-preview flags.
  // Returns true when the (local or live) chain cover is representable.
  bool snap_hold_draft_chain_lane_and_sync(
      float desired_lane_f, HoldDraftSync sync = HoldDraftSync::WriteLivePrevCover);
  wds::interaction::SwipeDirection update_place_swipe(wds::interaction::Vec2 pointer);

  bool handle_popup_pointer_down(const wds::interaction::PointerDownEvent& event);
  bool handle_left_gutter_pointer_down(const wds::interaction::PointerDownEvent& event);
  bool handle_speed_gutter_pointer_down(const wds::interaction::PointerDownEvent& event);
  bool handle_right_gutter_pointer_down(const wds::interaction::PointerDownEvent& event);
  void open_speed_popup(int32_t tick);
  void close_speed_popup();
  void sync_speed_point_to_pointer(wds::interaction::Vec2 point);
  bool execute_maybe_extended(std::unique_ptr<wds::chart_editor::IEditCommand> cmd,
                              bool introduces_extended, const std::string& label);
  bool try_convert_hold_tail_to_nontail(wds::interaction::Vec2 point);
  void paint_gutters(wds::interaction::UiPainter& painter) const;
  void paint_gutter_overlays(wds::interaction::UiPainter& painter) const;
  void paint_marquee(wds::interaction::UiPainter& painter) const;
  void open_split_picker(int32_t tick);
  void open_split_picker_for_edit(int32_t note_id);
  void scroll_split_picker_to_color(int32_t color_id);
  void remember_split_picker_count();
  void remember_split_picker_color();
  void remember_split_picker_type();
  void apply_split_search_text(const std::string& text);
  void sync_split_scrollbar_from_pointer(float y);
  void close_split_picker();
  void confirm_split_picker();
  std::vector<int32_t> split_picker_filtered_ids() const;
  enum class TimingPopupMode { Bpm, Meter };
  void open_bpm_popup(int32_t tick);
  void open_meter_popup(int32_t tick);
  void close_timing_popup();
  void commit_timing_popup();
  bool delete_timing_label(int32_t tick, TimingPopupMode kind);
  bool delete_split_note(int32_t note_id);
  void update_gutter_ghost(wds::interaction::Vec2 point);
  void hide_gutter_ghost();
  void update_split_label_hover(wds::interaction::Vec2 point);
  void clear_split_label_hover();
  bool active_split_highlight_is_end() const noexcept;
  void layout_popup_rects() const;
  wds::interaction::Rect overlay_host_bounds() const;
  void sync_error_ticks() const;
  int32_t first_legal_tick() const;
  float offset_violation_strength() const noexcept;

  wds::chart_editor::ChartEditorEngine& engine_;
  mutable EditViewport viewport_;
  ChartEditRenderer renderer_;
  const wds::renderer::SkinCatalog* skin_ = nullptr;
  const wds::audio::WaveformOverview* waveform_ = nullptr;
  wds::renderer::TextureInfo spectrogram_{};
  EditSpectrumMode spectrum_mode_ = EditSpectrumMode::Envelope;
  mutable std::vector<int32_t> error_ticks_;
  mutable uint64_t error_ticks_generation_ = 0;
  mutable bool error_ticks_armed_ = false;
  std::unordered_set<int32_t> offset_violation_ids_;
  float offset_violation_elapsed_ = 0.0f;
  mutable wds::interaction::Rect left_gutter_{};
  mutable wds::interaction::Rect speed_gutter_{};       // scroll speed (extended)
  mutable wds::interaction::Rect right_gutter_{};       // BPM / meter
  mutable wds::interaction::Rect measure_gutter_{};     // measure index (far right)
  mutable wds::interaction::Rect playfield_{};
  bool extended_editing_active_ = false;
  int extra_visible_lanes_session_ = 0;
  bool lane_slider_hovered_ = false;
  bool speed_popup_open_ = false;
  int32_t speed_edit_tick_ = 0;
  double speed_edit_multiplier_ = 1.0;
  int32_t drag_speed_tick_ = 0;
  int32_t drag_speed_press_tick_ = 0;
  bool speed_point_ever_moved_ = false;
  std::vector<wds::chart_editor::ScrollSpeedPoint> drag_speed_original_points_{};
  std::unordered_set<int32_t> drag_speed_selected_ticks_{};
  bool move_lock_lane_ = false;
  bool move_selection_ever_moved_ = false;
  int32_t move_click_opens_split_id_ = -1;
  int32_t move_click_opens_speed_tick_ = -1;
  mutable wds::interaction::Rect split_picker_bounds_{};
  mutable wds::interaction::Rect timing_popup_bounds_{};
  mutable std::vector<wds::interaction::Rect> split_count_buttons_{};
  mutable std::vector<wds::interaction::Rect> split_color_buttons_{};
  mutable wds::interaction::Rect split_search_label_{};
  mutable wds::interaction::Rect split_search_field_{};
  mutable wds::interaction::Rect split_color_list_{};
  mutable wds::interaction::Rect split_scrollbar_track_{};
  mutable wds::interaction::Rect split_scrollbar_thumb_{};
  mutable float split_color_row_h_ = 64.0f;
  mutable float split_color_max_scroll_ = 0.0f;
  static constexpr int kSplitColorCols = 3;
  mutable wds::interaction::Rect split_confirm_button_{};
  mutable wds::interaction::Rect split_cancel_button_{};
  mutable wds::interaction::Rect timing_confirm_button_{};
  mutable wds::interaction::Rect timing_cancel_button_{};
  mutable wds::interaction::Rect timing_bpm_field_{};
  mutable wds::interaction::Rect timing_num_field_{};
  mutable wds::interaction::Rect timing_den_field_{};
  std::function<void(int64_t)> seek_ms_;
  std::function<void()> on_visible_range_changed_;
  std::function<void()> on_extra_visible_lanes_changed_;
  std::function<void(wds::interaction::CursorKind)> cursor_setter_;
  wds::interaction::CursorKind hover_cursor_ = wds::interaction::CursorKind::Default;
  TrackedIdSet selected_;
  TrackedIdSet selected_speed_ticks_;
  mutable NoteSpanCache note_span_cache_{};
  mutable std::vector<int32_t> split_mids_scratch_{};
  mutable std::vector<int32_t> split_one_scratch_{};
  // Reused `out` buffers for ChartDocument index queries (note_indices_*/split range).
  // Never hold one across a call that can re-enter another user of the same buffer.
  mutable std::vector<std::size_t> note_index_scratch_{};
  mutable std::vector<int32_t> note_id_scratch_{};
  std::vector<wds::chart_editor::NotationNote> clipboard_;
  std::vector<wds::chart_editor::ScrollSpeedPoint> clipboard_speed_;
  int default_width_ = 3;
  bool split_width_follow_ = false;
  bool pause_at_current_ = false;

  bool split_picker_open_ = false;
  int32_t split_picker_tick_ = 0;
  int32_t split_picker_count_ = 2;
  int32_t split_picker_color_id_ = 1;
  wds::chart_editor::SplitLaneType split_picker_type_ = wds::chart_editor::SplitLaneType::Full;
  // >=0 while editing an existing split note; -1 when adding.
  int32_t split_picker_edit_id_ = -1;
  float split_color_scroll_ = 0.0f;
  std::string split_search_text_;
  bool split_search_focused_ = false;
  float split_search_caret_blink_t_ = 0.0f;
  bool split_scrollbar_dragging_ = false;
  float split_scrollbar_grab_offset_ = 0.0f;
  // Session-only: last confirmed/clicked count + color. Applied once on open
  // (select + scroll to the remembered ID).
  bool split_picker_memory_valid_ = false;
  int32_t split_picker_memory_count_ = 2;
  int32_t split_picker_memory_color_id_ = 1;
  wds::chart_editor::SplitLaneType split_picker_memory_type_ =
      wds::chart_editor::SplitLaneType::Full;

  bool timing_popup_open_ = false;
  TimingPopupMode timing_popup_mode_ = TimingPopupMode::Bpm;
  int32_t timing_edit_tick_ = 0;
  std::string timing_bpm_text_;
  std::string timing_num_text_;
  std::string timing_den_text_;
  std::string timing_bpm_committed_;
  std::string timing_num_committed_;
  std::string timing_den_committed_;
  int timing_focus_field_ = 0;
  float timing_caret_blink_t_ = 0.0f;

  int32_t drag_split_note_id_ = -1;
  bool drag_split_is_end_ = false;
  int32_t hovered_split_note_id_ = -1;
  bool hovered_split_is_end_ = false;
  // Pointer's snapped tick at press. The label sits off the grid line, so the
  // press tick may differ from the note; do not snap until this tick changes
  // (mouse move or wheel). True click = the note never left its original ticks.
  int32_t drag_split_press_tick_ = 0;
  bool split_edge_ever_moved_ = false;

  // Solid label previews on BPM / meter / split gutters (no text).
  struct GutterGhost {
    wds::interaction::Rect bounds{};
    wds::interaction::Color color{};
    float anchor_y = 0.0f;
    bool is_start = true;
    bool draw_leader = false;
    int column = 0;
  };
  std::vector<GutterGhost> gutter_ghosts_{};

  Mode mode_ = Mode::Idle;
  wds::interaction::GestureTracker gesture_;
  wds::interaction::PlaceSwipeTracker place_swipe_;
  // Note snapped at pointer-down; lane/tick stay fixed for the place gesture.
  wds::chart_editor::NotationNote place_anchor_{};
  wds::interaction::PointerButton active_button_ = wds::interaction::PointerButton::Left;
  wds::interaction::Modifiers active_mods_{};
  wds::interaction::Vec2 pointer_{};
  // Latest window pointer (may be outside this panel). Used to drop Idle ghosts.
  wds::interaction::Vec2 global_pointer_{};
  bool pointer_over_edit_ = false;
  // Marquee anchor in chart space (survives scroll while dragging).
  int32_t marquee_start_tick_ = 0;
  int32_t marquee_start_lane_ = 0;
  float marquee_start_x_ = 0.0f;

  GhostNote ghost_{};
  std::vector<GhostNote> hold_stars_;

  // Move / resize state
  int32_t anchor_note_id_ = -1;
  int32_t resize_side_ = 0;  // -1 left, +1 right
  // When true with ResizeWidth: edit ScratchHold end span (not body width).
  bool resize_scratch_end_ = false;
  // Single chained ScratchHold segment MoveSelection: lock time, keep chain joints.
  bool move_scratch_segment_ = false;
  // Chained neighbor involved in an unselected ScratchHold width edit (-1 = none).
  int32_t resize_chain_peer_id_ = -1;   // prev when editing body; next when editing end
  int32_t resize_chain_next_id_ = -1;   // next body for cover validation while editing body
  bool adjust_end_ = true;
  // Last geometry committed during ResizeWidth — skip recompute while the pointer holds still.
  int32_t resize_applied_end_l_ = std::numeric_limits<int32_t>::min();
  int32_t resize_applied_end_r_ = std::numeric_limits<int32_t>::min();
  int32_t resize_applied_body_lane_ = std::numeric_limits<int32_t>::min();
  int32_t resize_applied_body_width_ = std::numeric_limits<int32_t>::min();
  int32_t resize_applied_peer_lane_ = std::numeric_limits<int32_t>::min();
  int32_t resize_applied_peer_width_ = std::numeric_limits<int32_t>::min();
  std::unordered_map<int32_t, wds::chart_editor::NotationNote> drag_originals_;
  wds::interaction::Vec2 drag_start_pos_{};
  // MoveSelection: tick/lane at pointer-down (grab offset), not note origin.
  int32_t drag_start_tick_ = 0;
  int32_t drag_start_lane_ = 0;

  // Hold placement
  bool hold_scratch_ = false;
  wds::chart_editor::NotationNote hold_draft_{};
  // Continuous hold chain: select all segments; cover previous tail with next body.
  int32_t hold_chain_start_tick_ = 0;
  int32_t hold_chain_prev_id_ = -1;
  wds::chart_editor::NotationNote hold_chain_prev_body_{};
  std::unordered_set<int32_t> hold_chain_ids_;
  // Live JumpScratch cover applied to prev during chain preview. False when the
  // current width/lane cannot represent a cover — chain stays armed for width
  // changes, but ghost looks like a disconnected independent ScratchHold.
  bool hold_chain_link_preview_ = true;
  // Armed on RMB-down over a selected terminal hold end-cap; consumed when the
  // gesture resolves to HoldBody / ScratchHoldBody or finishes on right-up.
  int32_t pending_chain_extend_id_ = -1;
  // A right-click chain-extend starts placing immediately (no place-gesture
  // swipe), so its own right-up must finish for regular Hold too (whose finish
  // button is normally Left).
  bool finish_chain_extend_on_right_up_ = false;
  // Locked when PlaceGesture starts: Shift was already down at mouse press.
  // The same Shift still places stars; releasing it mid-draw does not retint.
  bool place_gold_head_ = false;
  // Toolbox lock. None means classic Tap-default / no lock; toolbox treats None as Tap.
  wds::interaction::PlaceIntent place_intent_override_ = wds::interaction::PlaceIntent::None;
  bool toolbox_place_mode_ = false;
  wds::interaction::PlaceIntent toolbox_place_intent() const noexcept;
  bool toolbox_hold_selected() const noexcept;
  // Classic ScratchHold swaps L/R; toolbox uses the regular-Hold map for both.
  bool hold_button_swap() const noexcept;
  wds::interaction::PlaceIntent effective_place_intent(
      wds::interaction::PlaceIntent intent) const noexcept;

  void clear_hold_chain_state();
  void select_hold_chain();
  // Select every ScratchHold body (+ heads) in the chained group containing seed.
  void select_scratch_hold_chain_from(const wds::chart_editor::NotationNote& seed);
  // Minimum hold body duration in ticks (one edit-grid subdivision).
  int32_t min_hold_duration_ticks() const;
  static void apply_hold_tail_cover(wds::chart_editor::NotationNote& prev,
                                    const wds::chart_editor::NotationNote& prev_body,
                                    const wds::chart_editor::NotationNote& next_body,
                                    bool scratch);

  // --- Multi-layer hold selection -------------------------------------------
  // A plain click on a hold enters its outermost available layer:
  //   Chain  when the hold has connected neighbours,
  //   Whole  for a lone segment with a paired head,
  //   Parts  for a lone headless segment (body-only is its top layer).
  // Double-click advances to the next available layer:
  //   Chain → Whole → Parts (only with a head) → Stars → wrap to the top layer.
  // Once a layer is active, a plain click switches the selected piece anywhere
  // inside the layer-1 chain scope without changing the layer. Parts: single
  // click picks the head or body. Stars: single click on a star stays in Stars
  // and picks that star; single click on body/head steps back one layer.
  // Exception: double-clicking a star keeps the direct star jump.
  enum class HoldSelLayer { None, Chain, Whole, Parts, Stars };

  void clear_hold_sel_focus();
  void sync_hold_sel_focus_to_selection();
  // Layers available for a hold body, outermost first. Stars is always last.
  // A one-segment chain has no Chain layer. A one-segment headless hold has
  // neither Chain nor Whole: Parts(body) is its top layer.
  std::vector<HoldSelLayer> hold_layer_order(
      const wds::chart_editor::NotationNote& body) const;
  // True when two bodies are the same body or belong to the same connected chain.
  bool hold_bodies_connected(const wds::chart_editor::NotationNote& a,
                             const wds::chart_editor::NotationNote& b) const;
  // True when every selected note resolves to a hold in `body`'s chain.
  bool selection_belongs_to_hold_chain(
      const wds::chart_editor::NotationNote& body) const;
  std::optional<wds::chart_editor::NotationNote> resolve_hold_body(
      const wds::chart_editor::NotationNote& hit) const;
  bool hold_bodies_same_focus(const wds::chart_editor::NotationNote& body) const;
  void select_hold_outer(const wds::chart_editor::NotationNote& body);
  void select_hold_segment(const wds::chart_editor::NotationNote& body);
  void select_hold_part(const wds::chart_editor::NotationNote& hit,
                        const wds::chart_editor::NotationNote& body);
  // Select every visible mid-star bound to `body` (Stars layer).
  void select_hold_stars(const wds::chart_editor::NotationNote& body);
  // Resolve which piece a double-click targeted inside `body`. With
  // `include_stars` (double-click shortcut) a bound star under the pointer wins;
  // otherwise the paired head wins when the pointer is over its sprite, else the
  // body. Geometry-based, so the selected-note hit-test tie bonus cannot hide the
  // head/star. Single-click Parts switching passes false so an overlapping star
  // canvas (also used for star placement) cannot hide the head.
  wds::chart_editor::NotationNote hold_drill_target(
      const wds::chart_editor::NotationNote& body, wds::interaction::Vec2 point,
      bool include_stars = true) const;
  // Double-click hold-family drill. Single click must not advance layers.
  void drill_hold_selection(const wds::chart_editor::NotationNote& hit);
  // Single click while a drill layer is active: switch the selected piece at the
  // current layer, anywhere inside the layer-1 chain scope. Returns true when the
  // click was consumed by the layer (so it must not reset the selection).
  bool switch_hold_selection_layer(const wds::chart_editor::NotationNote& hit,
                                   wds::interaction::Vec2 point);
  // True when a hold body is effectively selected for star placement.
  bool hold_body_selected_for_stars(const wds::chart_editor::NotationNote& body) const;

  HoldSelLayer hold_sel_layer_ = HoldSelLayer::None;
  int32_t hold_sel_body_id_ = -1;
  // Last hold body whose length/width edge gesture ran. A double-click anywhere
  // inside it drills that hold even if another note (e.g. a tap) covers its start.
  int32_t last_hold_edge_body_id_ = -1;

  void sync_curve_mode();
  void refresh_curve_ghosts();
  void cancel_curve_fill();
  bool commit_curve_fill();

  std::vector<ClickTapMark> click_taps_{};
  std::vector<ClickHoldMark> click_holds_{};
  bool click_record_hold_active_ = false;
  double click_record_hold_start_ms_ = 0.0;
  uint64_t click_marks_revision_ = 0;

  CurveFillSelection curve_fill_selection_{};
  bool curve_mode_active_ = false;
  bool curve_dismissed_ = false;
  int32_t curve_origin_tick_ = 0;
  int32_t curve_origin_lane_ = 0;
  std::vector<GhostNote> curve_ghosts_{};
};

}  // namespace wds::ui

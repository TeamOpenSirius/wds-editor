#include "wds/ui/regions/edit/chart_edit_panel.hpp"

#include "wds/ui/timeline_wheel.hpp"

#include <wds/chart_render/note_draw_order.hpp>
#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/edit_history.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/scratch_hold_curve.hpp>
#include <wds/core/timing_map.hpp>
#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <wds/interaction/caret.hpp>
#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/font_atlas.hpp>
#include <wds/interaction/theme.hpp>
#include <wds/interaction/widget_root.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wds::ui {
namespace {

using wds::chart_editor::NotationNote;
using wds::chart_editor::NoteType;
using wds::interaction::PlaceIntent;
using wds::interaction::SwipeDirection;

bool clamps_to_nonnegative_music(const NotationNote& note) noexcept {
  if (note.note_type == NoteType::HiSpeed) return false;
  if (note.note_type == NoteType::None &&
      !wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) {
    return false;
  }
  return true;
}

// Width / hold-time edge hit targets (logical px; flush scales to FB).
float width_edge_prefer_px() { return wds::interaction::theme::px(6.0f); }
float width_edge_max_px() { return wds::interaction::theme::px(7.0f); }
float time_edge_prefer_px() { return wds::interaction::theme::px(6.0f); }
float time_edge_max_px() { return wds::interaction::theme::px(7.0f); }
// Narrow gutters sized for kFontSizeGutter text: split ids (5 digits), BPM/meter,
// and a far-right measure index column (up to 4 digits). Total side width matches
// the previous left+right sum so the playfield width stays unchanged.
float left_gutter_w() { return wds::interaction::theme::px(60.0f); }
float right_gutter_w() { return wds::interaction::theme::px(52.0f); }
float measure_gutter_w() { return wds::interaction::theme::px(40.0f); }

float split_search_hint_width() {
  namespace th = wds::interaction::theme;
  constexpr const char* kHint = "分割线编号";
  float w = th::kFontSizeMd * 5.0f;
  auto& font = wds::interaction::FontAtlas::instance();
  if (font.atlas_width() > 0) {
    font.ensure_glyphs(kHint);
    const float measured = font.measure(kHint, th::kFontSizeMd).x;
    if (measured > 1.0f) w = measured;
  }
  return w;
}

bool is_visible_mid_star(NoteType type) noexcept {
  return type == NoteType::Sound || type == NoteType::ScratchSound;
}

std::optional<NotationNote> bound_parent_hold(
    const NotationNote& star, const std::unordered_map<int32_t, NotationNote>& next,
    const wds::chart_editor::ChartDocument& doc) {
  if (star.parent_hold_id < 0) return std::nullopt;
  if (auto it = next.find(star.parent_hold_id); it != next.end()) return it->second;
  return doc.find_note(star.parent_hold_id);
}

std::vector<wds::chart_editor::NoteUpdate> to_note_updates(
    const std::unordered_map<int32_t, NotationNote>& notes) {
  std::vector<wds::chart_editor::NoteUpdate> updates;
  updates.reserve(notes.size());
  for (const auto& [id, note] : notes) {
    updates.push_back({id, note});
  }
  return updates;
}

void append_unique_notes(std::vector<NotationNote>& out, const NotationNote& note) {
  for (const auto& existing : out) {
    if (existing.id == note.id) return;
  }
  out.push_back(note);
}

// Mid-stars / eighths follow hold-body delete and mirror. Paired heads/bodies
// never cascade. Copy strips HoldEighth separately — eighths are always derived.
std::vector<NotationNote> notes_with_hold_dependents(
    const wds::chart_editor::ChartDocument& doc, const std::vector<NotationNote>& roots) {
  std::vector<NotationNote> notes;
  for (const auto& note : roots) {
    append_unique_notes(notes, note);
    if (!wds::chart_editor::is_hold_with_tail(note.note_type)) continue;
    for (const auto& dep : wds::chart_editor::hold_attached_notes_for(doc, note)) {
      if (wds::chart_editor::is_hold_head_note(dep)) continue;
      append_unique_notes(notes, dep);
    }
  }
  return notes;
}

void strip_clipboard_hold_eighths(std::vector<NotationNote>& notes) {
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [](const NotationNote& n) {
                               return n.note_type == NoteType::HoldEighth;
                             }),
              notes.end());
}

std::vector<NotationNote> selected_with_hold_dependents(
    const wds::chart_editor::ChartDocument& doc, const std::unordered_set<int32_t>& selected) {
  std::vector<NotationNote> roots;
  roots.reserve(selected.size());
  for (const int32_t id : selected) {
    if (auto n = doc.find_note(id)) roots.push_back(*n);
  }
  return notes_with_hold_dependents(doc, roots);
}

bool apply_note_map(wds::chart_editor::ChartDocument& doc,
                    const std::unordered_map<int32_t, NotationNote>& notes) {
  return doc.apply_note_updates(to_note_updates(notes));
}

}  // namespace

ChartEditPanel::ChartEditPanel(wds::chart_editor::ChartEditorEngine& engine) : engine_(engine) {
}

uint64_t ChartEditPanel::visual_revision() const {
  auto mix = [](uint64_t h, uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  };
  auto mix_i32 = [&](uint64_t h, int32_t v) noexcept {
    return mix(h, static_cast<uint64_t>(static_cast<uint32_t>(v)));
  };

  uint64_t h = engine_.document().content_generation();
  h = mix(h, engine_.snapshot().revision);
  const float scroll = viewport_.scroll_ms();
  uint32_t scroll_bits = 0;
  static_assert(sizeof(scroll) == sizeof(scroll_bits));
  std::memcpy(&scroll_bits, &scroll, sizeof(scroll_bits));
  h = mix(h, scroll_bits);
  h = mix_i32(h, viewport_.visible_ms());
  h = mix_i32(h, viewport_.grid().visible_hectoms);
  h = mix_i32(h, viewport_.grid().lane_count);
  h = mix(h, static_cast<uint64_t>(std::lround(viewport_.bounds().w * 4.0f)));
  h = mix(h, static_cast<uint64_t>(std::lround(viewport_.bounds().h * 4.0f)));

  uint64_t sel = static_cast<uint64_t>(selected_.size());
  for (int32_t id : selected_) {
    sel ^= static_cast<uint64_t>(static_cast<uint32_t>(id)) * 0x9e3779b97f4a7c15ull;
  }
  h = mix(h, sel);

  h = mix(h, static_cast<uint64_t>(hover_cursor_));
  h = mix(h, ghost_.visible ? 1ull : 0ull);
  h = mix_i32(h, ghost_.note.id);
  h = mix_i32(h, ghost_.note.start_tick);
  h = mix_i32(h, ghost_.note.end_tick);
  h = mix_i32(h, ghost_.note.lane);
  h = mix_i32(h, ghost_.note.width);
  h = mix(h, static_cast<uint64_t>(mode_));
  h = mix_i32(h, hovered_split_note_id_);
  h = mix(h, hovered_split_is_end_ ? 1ull : 0ull);
  h = mix(h, pointer_over_edit_ ? 1ull : 0ull);
  h = mix(h, static_cast<uint64_t>(spectrum_mode_));
  h = mix(h, curve_mode_active_ ? 1ull : 0ull);
  h = mix(h, static_cast<uint64_t>(curve_ghosts_.size()));
  h = mix(h, static_cast<uint64_t>(gutter_ghosts_.size()));
  h = mix(h, static_cast<uint64_t>(hold_stars_.size()));
  h = mix_i32(h, hold_draft_.start_tick);
  h = mix_i32(h, hold_draft_.end_tick);
  h = mix_i32(h, hold_draft_.lane);
  h = mix_i32(h, marquee_start_tick_);
  if (waveform_ != nullptr) {
    h = mix(h, static_cast<uint64_t>(waveform_->size()));
  }
  if (!offset_violation_ids_.empty()) {
    h = mix(h, static_cast<uint64_t>(offset_violation_ids_.size()));
    h = mix(h, static_cast<uint64_t>(
                   std::lround(static_cast<double>(offset_violation_elapsed_) * 30.0)));
  }
  return h;
}

PlaceIntent ChartEditPanel::effective_place_intent(PlaceIntent intent) const noexcept {
  if (place_intent_override_ == PlaceIntent::None) return intent;
  if (intent == PlaceIntent::Tap) {
    switch (place_intent_override_) {
      case PlaceIntent::ExTap:
      case PlaceIntent::HoldStart:
      case PlaceIntent::Flick:
      case PlaceIntent::FlickLeft:
      case PlaceIntent::FlickRight:
        return place_intent_override_;
      default:
        // Hold-body locks keep the plain click as Tap; the drag defines the hold.
        return intent;
    }
  }
  // Scratch-hold lock: a left swipe-up draws the scratch hold body.
  if (intent == PlaceIntent::HoldBody &&
      place_intent_override_ == PlaceIntent::ScratchHoldBody) {
    return PlaceIntent::ScratchHoldBody;
  }
  return intent;
}

void ChartEditPanel::set_selected(std::unordered_set<int32_t> ids) {
  selected_.clear();
  for (const int32_t id : ids) {
    const auto note = engine_.document().find_note(id);
    if (note && note->note_type == NoteType::HoldEighth) continue;
    selected_.insert(id);
  }
  sync_hold_sel_focus_to_selection();
}

void ChartEditPanel::trace_snapshot(wds::common::CrashTraceSnap& snap) const {
  snap.mask = wds::common::kCrashSnapTimeline | wds::common::kCrashSnapHectoms |
              wds::common::kCrashSnapEditMode | wds::common::kCrashSnapGhost |
              wds::common::kCrashSnapRevision;
  snap.timeline_ms = engine_.timeline_ms();
  snap.visible_hectoms = viewport_.grid().visible_hectoms;
  snap.edit_mode = static_cast<std::uint8_t>(mode_);
  snap.ghost = ghost_.visible ? 1 : 0;
  snap.doc_revision = engine_.snapshot().revision;
}

void ChartEditPanel::set_grid(wds::chart_editor::EditGridConfig grid) {
  viewport_.set_grid(grid);
  if (curve_mode_active_) refresh_curve_ghosts();
}

void ChartEditPanel::sync_to_timeline_ms(double timeline_ms) const {
  sync_viewport();
  // Transport drives a wall-clock PLL toward audio; follow committed time as-is.
  viewport_.sync_scroll_to_playhead_ms(timeline_ms);
}

void ChartEditPanel::resync_pointer_overlays() {
  sync_viewport();
  if (has_modal_popup()) return;

  if (mode_ == Mode::MoveSelection) {
    sync_move_selection_to_pointer(global_pointer_);
    return;
  }
  if (mode_ == Mode::DragSplitEdge) {
    sync_split_edge_to_pointer(global_pointer_);
    return;
  }
  if (mode_ == Mode::Idle) {
    if (!pointer_over_edit_ || !pointer_in_edit_ghost_zone(global_pointer_)) {
      hide_placement_ghost();
      pointer_over_edit_ = false;
      return;
    }
    update_ghost(pointer_);
    return;
  }
  if (mode_ == Mode::PlaceGesture) {
    update_ghost(pointer_);
    return;
  }
  if (mode_ == Mode::PlaceHoldBody) {
    sync_curve_mode();
    sync_hold_draft_to_pointer();
    return;
  }
  if (mode_ == Mode::AdjustHoldTime) {
    sync_hold_adjust_to_pointer(global_pointer_);
    return;
  }
}

void ChartEditPanel::sync_global_pointer(wds::interaction::Vec2 point) {
  global_pointer_ = point;
  if (mode_ == Mode::MoveSelection) {
    sync_viewport();
    sync_move_selection_to_pointer(point);
    return;
  }
  if (mode_ == Mode::DragSplitEdge) {
    sync_viewport();
    sync_split_edge_to_pointer(point);
    return;
  }
  if (is_note_drawing()) {
    return;
  }
  if (!pointer_in_edit_ghost_zone(point)) {
    pointer_over_edit_ = false;
    hide_placement_ghost();
    set_hover_cursor(wds::interaction::CursorKind::Default);
  }
}

wds::interaction::Vec2 ChartEditPanel::pointer_as_in_host(wds::interaction::Vec2 point) const {
  const auto host = overlay_host_bounds();
  if (host.w <= 0.0f || host.h <= 0.0f) return point;
  // Out-of-window → treat as the nearest in-window edge (math only).
  point.x = std::min(std::max(point.x, host.x), host.right());
  point.y = std::min(std::max(point.y, host.y), host.bottom());
  return point;
}

bool ChartEditPanel::is_note_drawing() const noexcept {
  return mode_ == Mode::PlaceGesture || mode_ == Mode::PlaceHoldBody;
}

bool ChartEditPanel::pointer_in_edit_ghost_zone(wds::interaction::Vec2 point) const {
  // Leave slop: stay visible near the edge, hide once the cursor has clearly left.
  const float slop = wds::interaction::theme::px(32.0f);
  const auto b = absolute_bounds();
  return point.x >= b.x - slop && point.x < b.right() + slop &&
         point.y >= b.y - slop && point.y < b.bottom() + slop;
}

void ChartEditPanel::set_default_width(int width) noexcept {
  default_width_ = std::clamp(width, 1, viewport_.grid().lane_count);
  apply_width_to_locked_placement();
}

void ChartEditPanel::apply_width_to_locked_placement() {
  if (mode_ == Mode::PlaceGesture) {
    if (!ghost_.visible) return;
    apply_placement_lane_width(place_note_center(), ghost_.note.lane, ghost_.note.width);
    place_anchor_.lane = ghost_.note.lane;
    place_anchor_.width = ghost_.note.width;
    return;
  }
  if (mode_ == Mode::Idle) {
    if (ghost_.visible) {
      apply_placement_lane_width(pointer_, ghost_.note.lane, ghost_.note.width);
    }
    return;
  }
  if (mode_ != Mode::PlaceHoldBody) return;

  if (curve_mode_active_) {
    apply_placement_lane_width(place_note_center(), hold_draft_.lane, hold_draft_.width);
    curve_origin_lane_ = hold_draft_.lane;
    refresh_curve_ghosts();
    return;
  }

  // Hold body: first segment stays on the press anchor; chained next keeps its
  // current lane center (updated by pointer move) so width grows around it.
  const wds::interaction::Vec2 anchor = place_note_center();
  apply_placement_lane_width(anchor, hold_draft_.lane, hold_draft_.width);
  if (hold_chain_prev_id_ >= 0) {
    snap_hold_draft_chain_lane_and_sync(
        viewport_.lane_left_at_f(anchor.x, hold_draft_.width));
  }
  for (auto& star : hold_stars_) {
    star.note.lane = hold_draft_.lane;
    star.note.width = hold_draft_.width;
  }
  sync_hold_placement_ghost();
}

bool ChartEditPanel::chain_draft_cover_representable() const noexcept {
  if (hold_chain_prev_id_ < 0) return true;
  const int32_t cover_left = std::min(hold_chain_prev_body_.lane, hold_draft_.lane);
  const int32_t cover_right =
      std::max(hold_chain_prev_body_.end_lane(), hold_draft_.end_lane());
  return wds::chart_editor::scratch_hold_end_cover_representable(hold_chain_prev_body_,
                                                                 cover_left, cover_right);
}

bool ChartEditPanel::sync_chain_prev_tail_cover() {
  if (hold_chain_prev_id_ < 0) return true;
  auto prev = engine_.document().find_note(hold_chain_prev_id_);
  if (!prev) return true;
  if (!chain_draft_cover_representable()) return false;
  NotationNote updated = *prev;
  apply_hold_tail_cover(updated, hold_chain_prev_body_, hold_draft_, hold_scratch_);
  if (updated.gimmick_type != prev->gimmick_type ||
      updated.scratch_length != prev->scratch_length || updated.lane != prev->lane ||
      updated.width != prev->width) {
    engine_.document().update_note(hold_chain_prev_id_, updated);
    engine_.rebuild_snapshot();
  }
  return true;
}

bool ChartEditPanel::snap_hold_draft_chain_lane_and_sync(float desired_lane_f,
                                                        HoldDraftSync sync) {
  if (hold_chain_prev_id_ < 0) {
    hold_chain_link_preview_ = true;
    return true;
  }
  sync_viewport();
  hold_draft_.lane = wds::chart_editor::snap_scratch_chain_next_lane(
      hold_chain_prev_body_, hold_draft_.width, desired_lane_f, viewport_.grid().lane_count);
  if (sync == HoldDraftSync::LocalDraftOnly) {
    hold_chain_link_preview_ = chain_draft_cover_representable();
    if (!hold_chain_link_preview_) {
      hold_draft_.scratch_length = 0;
      hold_draft_.gimmick_type = wds::chart_editor::GimmickType::None;
    }
    return hold_chain_link_preview_;
  }
  if (sync_chain_prev_tail_cover()) {
    hold_chain_link_preview_ = true;
    return true;
  }
  // Keep chain armed (width hotkeys may make it representable again). Restore prev
  // and preview the draft as a disconnected independent ScratchHold.
  restore_hold_chain_prev_preview();
  hold_draft_.scratch_length = 0;
  hold_draft_.gimmick_type = wds::chart_editor::GimmickType::None;
  hold_chain_link_preview_ = false;
  return false;
}

void ChartEditPanel::sync_viewport() const {
  const auto panel = absolute_bounds();
  const float lgw = left_gutter_w();
  const float rgw = right_gutter_w();
  const float mgw = measure_gutter_w();
  left_gutter_ = {panel.x, panel.y, lgw, panel.h};
  measure_gutter_ = {panel.right() - mgw, panel.y, mgw, panel.h};
  right_gutter_ = {measure_gutter_.x - rgw, panel.y, rgw, panel.h};
  playfield_ = {left_gutter_.right(), panel.y,
                std::max(1.0f, panel.w - lgw - rgw - mgw), panel.h};
  viewport_.set_bounds(playfield_);
  viewport_.set_timing(engine_.document().timing());
}

void ChartEditPanel::apply_placement_lane_width(wds::interaction::Vec2 point, int32_t& lane,
                                                int32_t& width) const {
  sync_viewport();
  if (split_width_follow_) {
    const int32_t tick = viewport_.tick_at(point.y);
    std::vector<int32_t> mids;
    std::vector<int32_t> one;
    bool any = false;
    for (const auto& note : engine_.document().notes()) {
      if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
      const int32_t start = note.start_tick;
      const int32_t end = std::max(start, note.end_tick);
      // Closed interval [start, end] of every overlapping split effect.
      if (tick >= start && tick <= end) {
        any = true;
        split_boundaries_12(wds::chart_editor::get_split_count(note.gimmick_type), one);
        mids.insert(mids.end(), one.begin(), one.end());
      }
    }
    if (any) {
      const int32_t probe = viewport_.lane_at(point.x, 1);
      if (split_track_between_lines(mids, viewport_.grid().lane_count, probe, lane, width)) {
        return;
      }
    }
  }
  width = default_width_;
  lane = viewport_.lane_at(point.x, width);
}

int ChartEditPanel::effective_placement_width(wds::interaction::Vec2 point) const {
  int32_t lane = 0;
  int32_t width = default_width_;
  apply_placement_lane_width(point, lane, width);
  return width;
}

NotationNote ChartEditPanel::make_base_note(wds::interaction::Vec2 point) const {
  sync_viewport();
  NotationNote note;
  note.start_tick = viewport_.tick_at(point.y);
  note.end_tick = note.start_tick;
  apply_placement_lane_width(point, note.lane, note.width);
  note.note_type = NoteType::Normal;
  return note;
}

wds::interaction::Vec2 ChartEditPanel::place_note_center() const {
  sync_viewport();
  const NotationNote& note =
      mode_ == Mode::PlaceHoldBody ? hold_draft_ : place_anchor_;
  const float inset = viewport_.note_inset_px(note.width);
  const float width = std::max(4.0f, viewport_.lane_width(note.width) - inset * 2.0f);
  return {viewport_.x_at(note.lane) + inset + width * 0.5f, viewport_.y_at(note.start_tick)};
}

wds::interaction::SwipeDirection ChartEditPanel::update_place_swipe(
    wds::interaction::Vec2 pointer) {
  sync_viewport();
  // Horizontal place gestures require ≥ one lane of dx (same threshold unlocks).
  const float horizontal_min = viewport_.lane_width(1);
  // dx from press point; dy from the snapped note's time (not press Y / lane center).
  return place_swipe_.update(drag_start_pos_.x, place_note_center().y, pointer, horizontal_min,
                             wds::interaction::kEditorSwipeMinDistancePx);
}

std::optional<NotationNote> ChartEditPanel::hit_test_note(wds::interaction::Vec2 point) const {
  sync_viewport();
  std::optional<NotationNote> best;
  float best_dist = 1e9f;
  int best_priority = 0;  // higher wins ties (prefer JumpScratch end / head over body)
  int64_t best_start = 0;
  int32_t best_type = 0;

  auto consider = [&](float dist, int priority, const NotationNote& note) {
    // Keep the selected segment as the owner of an overlapping ScratchHold
    // joint, so the previous tail and next head can be adjusted independently.
    if (selected_.count(note.id) != 0) priority += 8;
    const int64_t start = static_cast<int64_t>(note.start_tick);
    const int32_t type = static_cast<int32_t>(note.note_type);
    bool better = dist < best_dist - 0.5f;
    if (!better && std::abs(dist - best_dist) <= 0.5f) {
      if (priority > best_priority) {
        better = true;
      } else if (priority == best_priority && best.has_value()) {
        // Same geometry priority: prefer the note that draws visually on top.
        better = wds::chart_render::is_visually_above(start, type, best_start, best_type);
      }
    }
    if (!better) return;
    best_dist = dist;
    best_priority = priority;
    best_start = start;
    best_type = type;
    best = note;
  };

  for (const auto& note : engine_.document().notes()) {
    if (wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.note_type == NoteType::HoldEighth) continue;

    // Mid-stars: tight square hitbox matching the drawn tick (not full hold width).
    if (is_visible_mid_star(note.note_type)) {
      const auto star = viewport_.mid_star_screen_rect(note);
      if (!star.contains(point)) continue;
      const float dist = std::hypot(point.x - (star.x + star.w * 0.5f),
                                    point.y - (star.y + star.h * 0.5f));
      constexpr int kStarPriority = 4;  // above body/head when pointer is on the star
      consider(dist, kStarPriority, note);
      continue;
    }

    const float y0 = viewport_.y_at(note.start_tick);
    const float y1 =
        note.end_tick > note.start_tick ? viewport_.y_at(note.end_tick) : y0;
    const float pad = viewport_.note_height_px() * 0.55f;
    const float top = std::min(y0, y1) - pad;
    const float bottom = std::max(y0, y1) + pad;
    if (point.y < top || point.y > bottom) continue;

    int32_t hit_lane = note.lane;
    int32_t hit_width = note.width;
    bool in_scratch_end_zone = false;
    // ScratchHold end cap may be wider than the body — include that span near the end.
    if (wds::chart_editor::is_hold_chain_body(note.note_type) &&
        note.end_tick > note.start_tick) {
      const auto end = wds::chart_editor::get_scratch_end_lane_range(note);
      const float end_y = viewport_.y_at(note.end_tick);
      if (std::abs(point.y - end_y) <= pad) {
        hit_lane = std::min(hit_lane, end.first);
        hit_width = std::max(note.end_lane(), end.second) - hit_lane + 1;
        in_scratch_end_zone = true;
      }
    }
    const float x0 = viewport_.x_at(hit_lane);
    const float x1 = x0 + viewport_.lane_width(hit_width);
    if (point.x < x0 || point.x > x1) continue;

    // At chain joints the next body start coincides with this end — prefer the
    // JumpScratch owner so side-edge width drags remain reachable.
    float dist;
    int priority;
    if (in_scratch_end_zone) {
      dist = std::abs(point.y - viewport_.y_at(note.end_tick));
      // A chain joint contains two independently editable pieces: the previous
      // segment's tail below the joint and the next segment's tap head above it.
      // Split the shared cap at its time line so the tail no longer permanently
      // wins the hit-test tie over the next head.
      const bool next_segment_side =
          point.y < viewport_.y_at(note.end_tick) &&
          wds::chart_editor::chained_next_scratch_hold(engine_.document(), note).has_value();
      // Cancel the selected-note tie bonus on the next-segment half as well;
      // otherwise a selected tail would make the head unreachable again.
      priority = next_segment_side ? -8 : 3;
    } else {
      dist = std::abs(point.y - y0);
      if (wds::chart_editor::is_hold_head_note(note)) {
        priority = 2;
        // Mirror the joint-half rule for a currently selected next head, so a
        // click on the previous-segment side can switch back to its tail.
        if (point.y > y0) {
          const auto body =
              wds::chart_editor::paired_hold_body_for(engine_.document(), note);
          if (body && wds::chart_editor::is_hold_chain_body(body->note_type) &&
              wds::chart_editor::chained_prev_scratch_hold(engine_.document(), *body)) {
            priority = -8;
          }
        }
      } else {
        priority = wds::chart_editor::is_hold_with_tail(note.note_type) ? 1 : 0;
      }
    }
    consider(dist, priority, note);
  }
  return best;
}

float ChartEditPanel::width_edge_px(const NotationNote& note) const {
  // Keep a broad selectable center even when a one-lane note is very narrow.
  // Handles may become thinner, but they never consume more than 20% per side.
  const float note_w = std::max(1.0f, viewport_.lane_width(std::max(1, note.width)));
  return std::max(1.0f, std::min({width_edge_prefer_px(), note_w * 0.20f,
                                 width_edge_max_px()}));
}

float ChartEditPanel::time_edge_px(const NotationNote& note) const {
  float edge = std::max(time_edge_prefer_px(), viewport_.note_height_px() * 0.55f);
  edge = std::min(edge, time_edge_max_px());
  if (note.end_tick > note.start_tick) {
    const float hold_h =
        std::abs(viewport_.y_at(note.end_tick) - viewport_.y_at(note.start_tick));
    // Keep a clear middle band for selecting/moving even for very short holds.
    edge = std::min(edge, hold_h * 0.20f);
  }
  return std::max(1.0f, edge);
}

bool ChartEditPanel::near_left_edge(const NotationNote& note, float x) const {
  return std::abs(x - viewport_.x_at(note.lane)) <= width_edge_px(note);
}
bool ChartEditPanel::near_right_edge(const NotationNote& note, float x) const {
  return std::abs(x - (viewport_.x_at(note.lane) + viewport_.lane_width(note.width))) <=
         width_edge_px(note);
}
bool ChartEditPanel::near_start_time(const NotationNote& note, float y) const {
  return std::abs(y - viewport_.y_at(note.start_tick)) <= time_edge_px(note);
}
bool ChartEditPanel::near_end_time(const NotationNote& note, float y) const {
  if (note.end_tick <= note.start_tick) return false;
  return std::abs(y - viewport_.y_at(note.end_tick)) <= time_edge_px(note);
}
bool ChartEditPanel::near_scratch_end_left(const NotationNote& note, float x) const {
  if (!wds::chart_editor::is_hold_chain_body(note.note_type)) return false;
  const auto range = wds::chart_editor::get_scratch_end_lane_range(note);
  NotationNote end = note;
  end.lane = range.first;
  end.width = std::max(1, range.second - range.first + 1);
  return std::abs(x - viewport_.x_at(end.lane)) <= width_edge_px(end);
}
bool ChartEditPanel::near_scratch_end_right(const NotationNote& note, float x) const {
  if (!wds::chart_editor::is_hold_chain_body(note.note_type)) return false;
  const auto range = wds::chart_editor::get_scratch_end_lane_range(note);
  NotationNote end = note;
  end.lane = range.first;
  end.width = std::max(1, range.second - range.first + 1);
  return std::abs(x - (viewport_.x_at(end.lane) + viewport_.lane_width(end.width))) <=
         width_edge_px(end);
}
bool ChartEditPanel::near_scratch_end_cap(const NotationNote& note, float y) const {
  if (!wds::chart_editor::is_hold_chain_body(note.note_type) ||
      note.end_tick <= note.start_tick) {
    return false;
  }
  const float pad = viewport_.note_height_px() * 0.55f;
  return std::abs(y - viewport_.y_at(note.end_tick)) <= pad;
}

void ChartEditPanel::set_hover_cursor(wds::interaction::CursorKind kind) {
  if (kind == hover_cursor_) return;
  hover_cursor_ = kind;
  if (cursor_setter_) cursor_setter_(kind);
}

void ChartEditPanel::update_hover_cursor(wds::interaction::Vec2 point) {
  using wds::interaction::CursorKind;
  if (mode_ == Mode::ResizeWidth) {
    set_hover_cursor(CursorKind::ResizeHorizontal);
    return;
  }
  if (mode_ == Mode::AdjustHoldTime) {
    set_hover_cursor(CursorKind::ResizeVertical);
    return;
  }
  if (mode_ != Mode::Idle && mode_ != Mode::PlaceGesture) {
    set_hover_cursor(CursorKind::Default);
    return;
  }
  if (!absolute_bounds().contains(point)) {
    set_hover_cursor(CursorKind::Default);
    return;
  }
  const auto hit = hit_test_note(point);
  if (!hit) {
    set_hover_cursor(CursorKind::Default);
    return;
  }
  // First click selects. Only expose resize handles for the selected note;
  // otherwise narrow notes can become all handle and no selectable center.
  if (selected_.count(hit->id) == 0) {
    set_hover_cursor(CursorKind::Default);
    return;
  }
  // Prefer JumpScratch side edges over hold time edges at chain joints.
  if (wds::chart_editor::is_hold_chain_body(hit->note_type) && near_scratch_end_cap(*hit, point.y) &&
      (near_scratch_end_left(*hit, point.x) || near_scratch_end_right(*hit, point.x))) {
    set_hover_cursor(CursorKind::ResizeHorizontal);
    return;
  }
  for (const auto& note : engine_.document().notes()) {
    if (!wds::chart_editor::is_hold_chain_body(note.note_type)) continue;
    if (!near_scratch_end_cap(note, point.y)) continue;
    if (near_scratch_end_left(note, point.x) || near_scratch_end_right(note, point.x)) {
      set_hover_cursor(CursorKind::ResizeHorizontal);
      return;
    }
  }
  if (wds::chart_editor::is_hold_chain_body(hit->note_type) &&
      near_scratch_end_cap(*hit, point.y)) {
    set_hover_cursor(CursorKind::ResizeVertical);
    return;
  }
  if (wds::chart_editor::is_hold_with_tail(hit->note_type) &&
      (near_start_time(*hit, point.y) || near_end_time(*hit, point.y))) {
    set_hover_cursor(CursorKind::ResizeVertical);
    return;
  }
  if (near_left_edge(*hit, point.x) || near_right_edge(*hit, point.x)) {
    set_hover_cursor(CursorKind::ResizeHorizontal);
    return;
  }
  set_hover_cursor(CursorKind::Default);
}

void ChartEditPanel::hide_placement_ghost() {
  if (mode_ == Mode::PlaceHoldBody) return;
  ghost_.visible = false;
  hide_gutter_ghost();
  clear_split_label_hover();
}

void ChartEditPanel::on_hover_leave() {
  // Drawing keeps the ghost even if hover leaves (capture may still drive moves).
  if (is_note_drawing()) return;
  pointer_over_edit_ = false;
  hide_placement_ghost();
  set_hover_cursor(wds::interaction::CursorKind::Default);
}

void ChartEditPanel::hide_gutter_ghost() { gutter_ghosts_.clear(); }

void ChartEditPanel::clear_split_label_hover() {
  if (mode_ == Mode::DragSplitEdge) return;
  hovered_split_note_id_ = -1;
  hovered_split_is_end_ = false;
}

void ChartEditPanel::update_split_label_hover(wds::interaction::Vec2 point) {
  hovered_split_note_id_ = -1;
  hovered_split_is_end_ = false;
  if (mode_ == Mode::DragSplitEdge) return;
  if (has_modal_popup()) return;
  if (!left_gutter_.contains(point)) return;
  const auto hits = build_split_label_hits(viewport_, left_gutter_, engine_.document().notes());
  for (const auto& hit : hits) {
    if (!split_label_hot_bounds(hit).contains(point)) continue;
    hovered_split_note_id_ = hit.note_id;
    hovered_split_is_end_ = !hit.is_start;
    return;
  }
}

int32_t ChartEditPanel::active_split_highlight_id() const noexcept {
  if (mode_ == Mode::DragSplitEdge && drag_split_note_id_ >= 0) return drag_split_note_id_;
  return hovered_split_note_id_;
}

bool ChartEditPanel::active_split_highlight_is_end() const noexcept {
  if (mode_ == Mode::DragSplitEdge && drag_split_note_id_ >= 0) return drag_split_is_end_;
  return hovered_split_is_end_;
}

void ChartEditPanel::update_gutter_ghost(wds::interaction::Vec2 point) {
  hide_gutter_ghost();
  update_split_label_hover(point);
  if (!engine_.is_editable() || has_modal_popup()) return;
  // Only while hovering idle — active place gestures keep the note ghost.
  if (mode_ != Mode::Idle) return;

  if (left_gutter_.contains(point)) {
    if (hovered_split_note_id_ >= 0) return;  // glowing label owns the gutter
    const auto hits = build_split_label_hits(viewport_, left_gutter_, engine_.document().notes());
    for (const auto& hit : hits) {
      if (split_label_hot_bounds(hit).contains(point)) return;  // existing label → edit, no add ghost
    }
    const int32_t tick = viewport_.tick_at(point.y);
    if (wds::chart_editor::tick_to_milliseconds(tick, engine_.document().timing()) < 0) {
      return;
    }
    const auto placed = split_start_placement(viewport_, left_gutter_, tick, hits);
    gutter_ghosts_.push_back(
        {placed.bounds, kSplitLabelGhostColor, placed.anchor_y, true, true, placed.column});
    return;
  }

  if (right_gutter_.contains(point)) {
    const auto& timing = engine_.document().timing();
    const auto hits = build_timing_label_hits(viewport_, right_gutter_, timing);
    for (const auto& hit : hits) {
      if (hit.bounds.contains(point)) return;
    }
    // BPM (above subdiv) and meter (below measure) can both ghost on a bar line.
    if (auto bpm_tick = timing_bpm_tick_at(viewport_, right_gutter_, timing, point)) {
      gutter_ghosts_.push_back(
          {bpm_label_bounds(viewport_, right_gutter_, *bpm_tick), kBpmLabelGhostColor});
    }
    if (auto measure = timing_measure_tick_at(viewport_, right_gutter_, timing, point)) {
      gutter_ghosts_.push_back(
          {meter_label_bounds(viewport_, right_gutter_, *measure), kMeterLabelGhostColor});
    }
  }
}

void ChartEditPanel::update_ghost(wds::interaction::Vec2 point) {
  update_split_label_hover(point);
  if (!engine_.is_editable()) {
    ghost_.visible = false;
    hide_gutter_ghost();
    return;
  }
  // Hold-body draft owns the ghost; other non-placement modes never show it.
  if (mode_ == Mode::PlaceHoldBody) return;
  if (mode_ != Mode::Idle && mode_ != Mode::PlaceGesture) {
    ghost_.visible = false;
    hide_gutter_ghost();
    return;
  }
  // Idle: hide once the cursor has left the edit pane by the leave slop.
  // PlaceGesture keeps the ghost while drawing, even outside the pane.
  if (mode_ == Mode::Idle && !pointer_in_edit_ghost_zone(point)) {
    pointer_over_edit_ = false;
    ghost_.visible = false;
    hide_gutter_ghost();
    clear_split_label_hover();
    return;
  }
  if (mode_ == Mode::Idle) {
    pointer_over_edit_ = true;
  }
  // Idle + Primary → prepare marquee; drawing keeps the ordinary placement ghost.
  if (wds::interaction::suppress_idle_placement_ghost(active_mods_, is_note_drawing())) {
    ghost_.visible = false;
    hide_gutter_ghost();
    return;
  }

  auto apply_intent = [&](PlaceIntent intent, wds::interaction::Vec2 anchor) {
    ghost_.note = make_base_note(anchor);
    ghost_.note.scratch_length = 0;
    ghost_.note.end_tick = ghost_.note.start_tick;
    switch (intent) {
      case PlaceIntent::Tap:
        ghost_.note.note_type = NoteType::Normal;
        break;
      case PlaceIntent::ExTap:
        ghost_.note.note_type = NoteType::Critical;
        break;
      case PlaceIntent::HoldStart:
        ghost_.note.note_type = NoteType::HoldStart;
        break;
      case PlaceIntent::HoldBody:
        ghost_.note.note_type = wds::chart_editor::drawn_hold_body_type(
            false, place_gold_head_);
        // Upward only: end cannot go earlier than the press tick.
        ghost_.note.end_tick =
            std::max(ghost_.note.start_tick, viewport_.tick_at(point.y));
        break;
      case PlaceIntent::Flick:
        ghost_.note.note_type = NoteType::Flick;
        break;
      case PlaceIntent::FlickLeft:
        ghost_.note.note_type = NoteType::Flick;
        ghost_.note.scratch_length = -1;
        break;
      case PlaceIntent::FlickRight:
        ghost_.note.note_type = NoteType::Flick;
        ghost_.note.scratch_length = 1;
        break;
      case PlaceIntent::ScratchHoldBody:
        ghost_.note.note_type = wds::chart_editor::drawn_hold_body_type(
            true, place_gold_head_);
        ghost_.note.end_tick =
            std::max(ghost_.note.start_tick, viewport_.tick_at(point.y));
        break;
      case PlaceIntent::None:
        // Non-place swipe (e.g. Down): keep the button's resting place type.
        ghost_.note.note_type =
            wds::interaction::is_right_button(active_button_) ? NoteType::Flick : NoteType::Normal;
        break;
    }
  };

  // Active place gesture: keep the note ghost on the press anchor even if the
  // swipe overshoots into BPM / split gutters or leaves the playfield.
  if (mode_ == Mode::PlaceGesture) {
    hide_gutter_ghost();
    const auto swipe = update_place_swipe(point);
    const PlaceIntent intent = effective_place_intent(wds::interaction::resolve_place_intent(
        active_button_, swipe, swipe == SwipeDirection::None));
    // Type/length may change; position stays on the snapped place anchor.
    // Feed note-center so lane/tick re-snap matches place_anchor_.
    apply_intent(intent, place_note_center());
    ghost_.visible = true;
    return;
  }

  // BPM / split gutters: solid label ghosts (no text). Measure index is display-only.
  if (left_gutter_.contains(point) || right_gutter_.contains(point)) {
    ghost_.visible = false;
    update_gutter_ghost(point);
    return;
  }
  if (measure_gutter_.contains(point)) {
    ghost_.visible = false;
    hide_gutter_ghost();
    return;
  }
  hide_gutter_ghost();
  // Hide when pointer leaves the playfield, or sits on a note / non-place target.
  if (!playfield_.contains(point) || hit_test_note(point).has_value()) {
    ghost_.visible = false;
    return;
  }
  // Idle: no placement ghost in music t<0. Active PlaceGesture returns earlier.
  if (mode_ == Mode::Idle && viewport_.ms_at_y(point.y) < 0.0f) {
    ghost_.visible = false;
    return;
  }

  ghost_.note = make_base_note(point);
  ghost_.note.note_type = NoteType::Normal;
  switch (effective_place_intent(PlaceIntent::Tap)) {
    case PlaceIntent::ExTap:
      ghost_.note.note_type = NoteType::Critical;
      break;
    case PlaceIntent::HoldStart:
      ghost_.note.note_type = NoteType::HoldStart;
      break;
    case PlaceIntent::Flick:
      ghost_.note.note_type = NoteType::Flick;
      break;
    case PlaceIntent::FlickLeft:
      ghost_.note.note_type = NoteType::Flick;
      ghost_.note.scratch_length = -1;
      break;
    case PlaceIntent::FlickRight:
      ghost_.note.note_type = NoteType::Flick;
      ghost_.note.scratch_length = 1;
      break;
    default:
      break;
  }
  ghost_.visible = true;
}

bool ChartEditPanel::commit_notes(std::vector<NotationNote> notes, const std::string& label) {
  if (!engine_.is_editable() || notes.empty()) return false;
  const auto& timing = engine_.document().timing();
  for (const auto& note : notes) {
    if (wds::chart_editor::note_intersects_negative_music_time(note, timing)) return false;
  }
  auto cmd = std::make_unique<wds::chart_editor::AddNotesCommand>(std::move(notes), label);
  if (!engine_.execute_command(std::move(cmd))) return false;
  selected_.clear();
  clear_hold_sel_focus();
  return true;
}

bool ChartEditPanel::commit_updates(
    const std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair>& changes,
    const std::string& label) {
  if (changes.empty()) return false;
  return engine_.execute_command(
      std::make_unique<wds::chart_editor::UpdateNotesCommand>(changes, label));
}

void ChartEditPanel::place_instant(NoteType type, wds::interaction::Vec2 point,
                                   int32_t scratch_length) {
  auto note = make_base_note(point);
  note.note_type = type;
  note.scratch_length = scratch_length;
  if (wds::chart_editor::note_intersects_negative_music_time(note, engine_.document().timing())) {
    return;
  }
  const auto before_ids = [&] {
    std::unordered_set<int32_t> ids;
    for (const auto& n : engine_.document().notes()) ids.insert(n.id);
    return ids;
  }();
  if (!engine_.execute_command(
          std::make_unique<wds::chart_editor::AddNotesCommand>(std::vector<NotationNote>{note},
                                                              "Place note"))) {
    return;
  }
  selected_.clear();
  for (const auto& n : engine_.document().notes()) {
    if (!before_ids.count(n.id)) selected_.insert(n.id);
  }
  clear_hold_sel_focus();
}

void ChartEditPanel::clear_hold_chain_state() {
  hold_chain_start_tick_ = 0;
  hold_chain_prev_id_ = -1;
  hold_chain_prev_body_ = {};
  hold_chain_ids_.clear();
  hold_chain_link_preview_ = true;
}

void ChartEditPanel::select_hold_chain() {
  selected_.clear();
  for (const int32_t id : hold_chain_ids_) {
    selected_.insert(id);
    if (auto note = engine_.document().find_note(id)) {
      if (auto head = wds::chart_editor::paired_hold_head_for(engine_.document(), *note)) {
        selected_.insert(head->id);
      }
    }
  }
}

int32_t ChartEditPanel::min_hold_duration_ticks() const {
  return std::max(1, wds::chart_editor::subdivision_tick_step(viewport_.grid()));
}

void ChartEditPanel::select_scratch_hold_chain_from(const NotationNote& seed) {
  NotationNote body = seed;
  if (wds::chart_editor::is_hold_head_note(seed)) {
    auto paired = wds::chart_editor::paired_hold_body_for(engine_.document(), seed);
    if (!paired || !wds::chart_editor::is_hold_chain_body(paired->note_type)) return;
    body = *paired;
  } else if (!wds::chart_editor::is_hold_chain_body(seed.note_type)) {
    return;
  }

  NotationNote cur = body;
  for (int guard = 0; guard < 64; ++guard) {
    auto prev = wds::chart_editor::chained_prev_scratch_hold(engine_.document(), cur);
    if (!prev) break;
    cur = *prev;
  }

  selected_.clear();
  for (int guard = 0; guard < 64; ++guard) {
    selected_.insert(cur.id);
    if (auto head = wds::chart_editor::paired_hold_head_for(engine_.document(), cur)) {
      selected_.insert(head->id);
    }
    auto next = wds::chart_editor::chained_next_scratch_hold(engine_.document(), cur);
    if (!next) break;
    cur = *next;
  }
}

void ChartEditPanel::clear_hold_sel_focus() {
  hold_sel_layer_ = HoldSelLayer::None;
  hold_sel_body_id_ = -1;
}

void ChartEditPanel::sync_hold_sel_focus_to_selection() {
  if (hold_sel_layer_ == HoldSelLayer::None || hold_sel_body_id_ < 0) {
    if (selected_.empty()) clear_hold_sel_focus();
    return;
  }
  if (selected_.empty()) {
    clear_hold_sel_focus();
    return;
  }
  bool keep = false;
  for (const int32_t id : selected_) {
    auto n = engine_.document().find_note(id);
    if (!n) continue;
    if (auto body = resolve_hold_body(*n)) {
      if (hold_bodies_same_focus(*body)) {
        keep = true;
        break;
      }
    }
  }
  if (!keep) clear_hold_sel_focus();
}

std::optional<NotationNote> ChartEditPanel::resolve_hold_body(const NotationNote& hit) const {
  if (wds::chart_editor::is_hold_with_tail(hit.note_type)) return hit;
  if (wds::chart_editor::is_hold_head_note(hit)) {
    return wds::chart_editor::paired_hold_body_for(engine_.document(), hit);
  }
  if (is_visible_mid_star(hit.note_type)) {
    return wds::chart_editor::parent_hold_for(engine_.document(), hit);
  }
  return std::nullopt;
}

bool ChartEditPanel::hold_bodies_same_focus(const NotationNote& body) const {
  if (hold_sel_body_id_ < 0) return false;
  if (body.id == hold_sel_body_id_) return true;
  auto focus = engine_.document().find_note(hold_sel_body_id_);
  if (!focus || !wds::chart_editor::is_hold_chain_body(focus->note_type) ||
      !wds::chart_editor::is_hold_chain_body(body.note_type)) {
    return false;
  }
  NotationNote cur = *focus;
  for (int guard = 0; guard < 64; ++guard) {
    auto prev = wds::chart_editor::chained_prev_scratch_hold(engine_.document(), cur);
    if (!prev) break;
    cur = *prev;
  }
  for (int guard = 0; guard < 64; ++guard) {
    if (cur.id == body.id) return true;
    auto next = wds::chart_editor::chained_next_scratch_hold(engine_.document(), cur);
    if (!next) break;
    cur = *next;
  }
  return false;
}

void ChartEditPanel::select_hold_outer(const NotationNote& body) {
  hold_sel_body_id_ = body.id;
  if (wds::chart_editor::is_hold_chain_body(body.note_type)) {
    select_scratch_hold_chain_from(body);
    hold_sel_layer_ = HoldSelLayer::Chain;
    return;
  }
  select_hold_segment(body);
}

void ChartEditPanel::select_hold_segment(const NotationNote& body) {
  selected_.clear();
  selected_.insert(body.id);
  if (auto head = wds::chart_editor::paired_hold_head_for(engine_.document(), body)) {
    selected_.insert(head->id);
  }
  hold_sel_body_id_ = body.id;
  hold_sel_layer_ = HoldSelLayer::Whole;
}

void ChartEditPanel::select_hold_part(const NotationNote& hit, const NotationNote& body) {
  selected_.clear();
  hold_sel_body_id_ = body.id;
  hold_sel_layer_ = HoldSelLayer::Parts;
  if (wds::chart_editor::is_hold_head_note(hit)) {
    selected_.insert(hit.id);
    return;
  }
  // Body, mid-star (drill), or body-area click → select body only.
  selected_.insert(body.id);
}

bool ChartEditPanel::hold_body_selected_for_stars(const NotationNote& body) const {
  if (selected_.count(body.id)) return true;
  // Star within this hold selected at Stars layer.
  if (hold_sel_layer_ == HoldSelLayer::Stars && hold_sel_body_id_ == body.id) return true;
  for (const int32_t id : selected_) {
    auto n = engine_.document().find_note(id);
    if (!n || !is_visible_mid_star(n->note_type)) continue;
    if (auto parent = wds::chart_editor::parent_hold_for(engine_.document(), *n)) {
      if (parent->id == body.id) return true;
    }
  }
  return false;
}

bool ChartEditPanel::apply_hold_layered_click(const NotationNote& hit) {
  auto body = resolve_hold_body(hit);
  if (!body) return false;

  const bool in_focus =
      hold_sel_layer_ != HoldSelLayer::None && hold_bodies_same_focus(*body);

  if (!in_focus) {
    select_hold_outer(*body);
    return true;
  }

  switch (hold_sel_layer_) {
    case HoldSelLayer::None:
      select_hold_outer(*body);
      return true;
    case HoldSelLayer::Chain:
      select_scratch_hold_chain_from(*body);
      hold_sel_body_id_ = body->id;
      return true;
    case HoldSelLayer::Whole:
      select_hold_segment(*body);
      return true;
    case HoldSelLayer::Parts:
      if (is_visible_mid_star(hit.note_type)) {
        // Single-click star does not enter Stars layer; keep current part selection.
        return true;
      }
      select_hold_part(hit, *body);
      return true;
    case HoldSelLayer::Stars:
      if (is_visible_mid_star(hit.note_type)) {
        auto parent = wds::chart_editor::parent_hold_for(engine_.document(), hit);
        if (parent && parent->id == hold_sel_body_id_) {
          selected_.clear();
          selected_.insert(hit.id);
          return true;
        }
      }
      // Body / head → back to Parts (no path to Whole/Chain from here).
      if (wds::chart_editor::is_hold_head_note(hit) ||
          wds::chart_editor::is_hold_with_tail(hit.note_type)) {
        select_hold_part(hit, *body);
        return true;
      }
      select_hold_outer(*body);
      return true;
  }
  return true;
}

void ChartEditPanel::drill_hold_selection(const NotationNote& hit) {
  // Any-layer double-click on a mid-star jumps straight into Stars.
  if (is_visible_mid_star(hit.note_type)) {
    auto parent = wds::chart_editor::parent_hold_for(engine_.document(), hit);
    if (!parent) return;
    selected_.clear();
    selected_.insert(hit.id);
    hold_sel_body_id_ = parent->id;
    hold_sel_layer_ = HoldSelLayer::Stars;
    return;
  }

  auto body = resolve_hold_body(hit);
  if (!body) return;

  if (hold_sel_layer_ == HoldSelLayer::None || !hold_bodies_same_focus(*body)) {
    select_hold_outer(*body);
    return;
  }

  if (hold_sel_layer_ == HoldSelLayer::Chain) {
    select_hold_segment(*body);
    return;
  }

  if (hold_sel_layer_ == HoldSelLayer::Whole) {
    select_hold_part(hit, *body);
    return;
  }

  // Parts: double-click head/body stays at Parts (single-click already switches).
  // Stars entry is handled above.
}

void ChartEditPanel::apply_hold_tail_cover(NotationNote& prev, const NotationNote& prev_body,
                                           const NotationNote& next_body, bool /*scratch*/) {
  // Body lane/width stay fixed; covering tail is scratch_length span (Sirius ScratchHoldEnd).
  prev.lane = prev_body.lane;
  prev.width = prev_body.width;
  wds::chart_editor::sync_scratch_chain_joint(prev, next_body);
}

void ChartEditPanel::begin_hold_body(bool scratch, wds::interaction::Vec2 /*point*/) {
  hold_scratch_ = scratch;
  hold_draft_ = place_anchor_;
  hold_draft_.note_type = wds::chart_editor::drawn_hold_body_type(scratch, place_gold_head_);
  // Upward only from the press tick; length follows current pointer (may be zero).
  hold_draft_.end_tick =
      std::max(hold_draft_.start_tick, viewport_.tick_at(pointer_.y));
  hold_stars_.clear();
  clear_hold_chain_state();
  hold_chain_start_tick_ = hold_draft_.start_tick;
  // Placement length is owned by hold_draft_; do not let place-swipe unlock abort it.
  place_swipe_.reset();
  curve_mode_active_ = false;
  curve_dismissed_ = false;
  curve_ghosts_.clear();
  mode_ = Mode::PlaceHoldBody;
  sync_hold_placement_ghost();
}

void ChartEditPanel::try_arm_pending_chain_extend(wds::interaction::Vec2 point,
                                                 bool scratch_family) {
  pending_chain_extend_id_ = -1;
  sync_viewport();
  std::optional<NotationNote> best;
  float best_dist = 1e9f;
  for (const auto& note : engine_.document().notes()) {
    if (!wds::chart_editor::is_hold_chain_body(note.note_type)) continue;
    if (wds::chart_editor::is_scratch_hold_body(note.note_type) != scratch_family) continue;
    if (note.end_tick <= note.start_tick) continue;
    if (!selected_.count(note.id)) continue;
    // Only the chain terminal (last JumpScratch) can be extended.
    if (wds::chart_editor::chained_next_scratch_hold(engine_.document(), note)) continue;
    if (!near_scratch_end_cap(note, point.y)) continue;
    const auto end = wds::chart_editor::get_scratch_end_lane_range(note);
    const int32_t hit_lane = std::min(note.lane, end.first);
    const int32_t hit_right = std::max(note.end_lane(), end.second);
    NotationNote span = note;
    span.lane = hit_lane;
    span.width = std::max(1, hit_right - hit_lane + 1);
    const float slop = width_edge_px(span);
    const float x0 = viewport_.x_at(hit_lane);
    const float x1 = x0 + viewport_.lane_width(span.width);
    if (point.x < x0 - slop || point.x > x1 + slop) continue;
    const float dist = std::abs(point.y - viewport_.y_at(note.end_tick));
    if (dist < best_dist) {
      best_dist = dist;
      best = note;
    }
  }
  if (best) pending_chain_extend_id_ = best->id;
}

void ChartEditPanel::begin_hold_chain_extend() {
  const int32_t prev_id = pending_chain_extend_id_;
  pending_chain_extend_id_ = -1;
  auto prev = prev_id >= 0 ? engine_.document().find_note(prev_id) : std::nullopt;
  const bool scratch =
      prev.has_value() && wds::chart_editor::is_scratch_hold_body(prev->note_type);
  if (prev_id < 0 || !prev || !wds::chart_editor::is_hold_chain_body(prev->note_type) ||
      !selected_.count(prev->id) ||
      wds::chart_editor::chained_next_scratch_hold(engine_.document(), *prev)) {
    begin_hold_body(scratch, place_note_center());
    return;
  }

  hold_scratch_ = scratch;
  hold_stars_.clear();
  clear_hold_chain_state();
  curve_mode_active_ = false;
  curve_dismissed_ = false;
  curve_ghosts_.clear();

  // Seed chain ids / start tick from the existing hold-chain group.
  NotationNote cur = *prev;
  for (int guard = 0; guard < 64; ++guard) {
    auto p = wds::chart_editor::chained_prev_scratch_hold(engine_.document(), cur);
    if (!p) break;
    cur = *p;
  }
  hold_chain_start_tick_ = cur.start_tick;
  for (int guard = 0; guard < 64; ++guard) {
    hold_chain_ids_.insert(cur.id);
    auto next = wds::chart_editor::chained_next_scratch_hold(engine_.document(), cur);
    if (!next) break;
    cur = *next;
  }

  hold_chain_prev_id_ = prev->id;
  hold_chain_prev_body_ = *prev;

  NotationNote next = *prev;
  next.id = wds::chart_editor::kAutoNoteId;
  next.note_type = prev->note_type;
  next.start_tick = prev->end_tick;
  // Same as chained-next after finish_hold_body(true): undrawn until vertical pull.
  next.end_tick = next.start_tick;
  next.width = default_width_;
  apply_placement_lane_width(pointer_, next.lane, next.width);
  next.lane = wds::chart_editor::snap_scratch_chain_next_lane(
      hold_chain_prev_body_, next.width, viewport_.lane_left_at_f(pointer_.x, next.width),
      viewport_.grid().lane_count);
  next.scratch_length = 0;
  next.gimmick_type = wds::chart_editor::GimmickType::None;
  hold_draft_ = next;

  place_swipe_.reset();
  mode_ = Mode::PlaceHoldBody;
  // Terminal JumpScratch → joint cover as soon as chain-extend starts. If the
  // current width cannot link, keep the chain armed and show a disconnected ghost.
  snap_hold_draft_chain_lane_and_sync(
      viewport_.lane_left_at_f(pointer_.x, hold_draft_.width));
  select_hold_chain();
  sync_hold_placement_ghost();
}

void ChartEditPanel::begin_scratch_hold_placement(wds::interaction::Vec2 point) {
  if (pending_chain_extend_id_ >= 0) {
    begin_hold_chain_extend();
  } else {
    begin_hold_body(true, point);
  }
}

void ChartEditPanel::begin_regular_hold_placement(wds::interaction::Vec2 point) {
  if (pending_chain_extend_id_ >= 0) {
    begin_hold_chain_extend();
  } else {
    begin_hold_body(false, point);
  }
}

bool ChartEditPanel::want_gold_first_hold_segment() const noexcept {
  return place_gold_head_ && hold_chain_prev_id_ < 0;
}

void ChartEditPanel::sync_drawn_hold_body_type() {
  if (mode_ != Mode::PlaceHoldBody) return;
  if (hold_chain_prev_id_ >= 0) return;
  hold_draft_.note_type =
      wds::chart_editor::drawn_hold_body_type(hold_scratch_, want_gold_first_hold_segment());
}

void ChartEditPanel::apply_gold_first_curve_body(
    std::vector<wds::chart_editor::NotationNote>& bodies) const {
  if (bodies.empty() || !want_gold_first_hold_segment()) return;
  bodies.front().note_type =
      wds::chart_editor::drawn_hold_body_type(hold_scratch_, true);
}

void ChartEditPanel::sync_hold_placement_ghost() {
  const int32_t min_end = hold_draft_.start_tick + min_hold_duration_ticks();
  // Linked chain next starts as a flick-tail adjuster only: no body until the
  // pointer pulls a vertical height. Unlinked preview (no representable cover)
  // still shows a disconnected-style ghost so width edits have a visual target.
  if (hold_draft_.end_tick < min_end && hold_chain_prev_id_ >= 0 &&
      hold_chain_link_preview_) {
    ghost_.visible = false;
    return;
  }
  if (hold_draft_.end_tick < min_end) {
    NotationNote flat = hold_draft_;
    flat.end_tick = flat.start_tick;
    flat.note_type = hold_scratch_ ? NoteType::Flick : NoteType::Normal;
    flat.scratch_length = 0;
    ghost_.note = flat;
  } else {
    ghost_.note = hold_draft_;
  }
  ghost_.visible = true;
}

void ChartEditPanel::sync_hold_draft_to_pointer(HoldDraftSync sync) {
  sync_drawn_hold_body_type();
  const int32_t start = hold_draft_.start_tick;
  const int32_t cur = viewport_.tick_at(pointer_.y);
  // Upward only: end may grow later in time, never earlier than the head.
  hold_draft_.start_tick = start;
  hold_draft_.end_tick = std::max(start, cur);
  if (curve_mode_active_) {
    hold_draft_.width = default_width_;
    ghost_.visible = false;
    refresh_curve_ghosts();
    return;
  }
  // First segment keeps the press lane; chained next follows pointer X so the
  // previous hold's covering tail can span both bodies. Illegal both-side
  // JumpScratch lanes adsorb to the nearest one-sided legal lane.
  if (hold_chain_prev_id_ >= 0) {
    hold_draft_.width = default_width_;
    apply_placement_lane_width(pointer_, hold_draft_.lane, hold_draft_.width);
    const float desired = viewport_.lane_left_at_f(pointer_.x, hold_draft_.width);
    snap_hold_draft_chain_lane_and_sync(desired, sync);
  }
  for (auto& star : hold_stars_) {
    star.note.lane = hold_draft_.lane;
    star.note.width = hold_draft_.width;
    star.visible = star.note.start_tick > hold_draft_.start_tick &&
                   star.note.start_tick < hold_draft_.end_tick;
  }
  sync_hold_placement_ghost();
}

void ChartEditPanel::add_hold_star_at(wds::interaction::Vec2 point) {
  if (mode_ != Mode::PlaceHoldBody) return;
  // Same lane span as the hold; the star sprite is drawn centered without stretch.
  NotationNote star;
  star.note_type = hold_scratch_ ? NoteType::ScratchSound : NoteType::Sound;
  star.width = hold_draft_.width;
  star.lane = hold_draft_.lane;
  star.start_tick = viewport_.tick_at(point.y);
  star.end_tick = star.start_tick;
  star.scratch_length = 0;
  for (const auto& g : hold_stars_) {
    if (g.note.start_tick == star.start_tick) return;
  }
  const int32_t head = std::min(hold_draft_.start_tick, hold_draft_.end_tick);
  const int32_t tail = std::max(hold_draft_.start_tick, hold_draft_.end_tick);
  GhostNote g{star, star.start_tick > head && star.start_tick < tail};
  hold_stars_.push_back(g);
}

bool ChartEditPanel::add_star_to_selected_hold(wds::interaction::Vec2 point, bool scratch_hold) {
  if (!engine_.is_editable() || selected_.empty()) return false;
  sync_viewport();
  if (!playfield_.contains(point)) return false;
  const int32_t tick = viewport_.tick_at(point.y);
  const int32_t lane = viewport_.lane_at(point.x, 1);
  std::optional<NotationNote> target;
  for (const int32_t id : selected_) {
    auto note = engine_.document().find_note(id);
    if (!note) continue;
    std::optional<NotationNote> body;
    if (scratch_hold) {
      if (wds::chart_editor::is_scratch_hold_body(note->note_type)) {
        body = *note;
      } else if (wds::chart_editor::is_hold_head_note(*note)) {
        body = wds::chart_editor::paired_hold_body_for(engine_.document(), *note);
        if (body && !wds::chart_editor::is_scratch_hold_body(body->note_type)) body.reset();
      } else if (is_visible_mid_star(note->note_type)) {
        body = wds::chart_editor::parent_hold_for(engine_.document(), *note);
        if (body && !wds::chart_editor::is_scratch_hold_body(body->note_type)) body.reset();
      }
    } else {
      if (wds::chart_editor::is_hold_with_tail(note->note_type) &&
          !wds::chart_editor::is_scratch_hold_body(note->note_type)) {
        body = *note;
      } else if (wds::chart_editor::is_hold_head_note(*note)) {
        body = wds::chart_editor::paired_hold_body_for(engine_.document(), *note);
        if (body && wds::chart_editor::is_scratch_hold_body(body->note_type)) body.reset();
      } else if (is_visible_mid_star(note->note_type)) {
        body = wds::chart_editor::parent_hold_for(engine_.document(), *note);
        if (body && wds::chart_editor::is_scratch_hold_body(body->note_type)) body.reset();
      }
    }
    if (!body || !hold_body_selected_for_stars(*body)) continue;
    if (tick <= body->start_tick || tick >= body->end_tick) continue;
    if (lane < body->lane || lane > body->end_lane()) continue;
    target = *body;
    break;
  }
  if (!target) return false;
  if (wds::chart_editor::hold_has_visible_star_at(engine_.document(), target->id, tick,
                                                  wds::chart_editor::kNoBoundHoldId)) {
    return false;
  }

  NotationNote star;
  star.note_type = scratch_hold ? NoteType::ScratchSound : NoteType::Sound;
  star.lane = target->lane;
  star.width = target->width;
  star.start_tick = tick;
  star.end_tick = tick;
  star.scratch_length = 0;
  star.id = wds::chart_editor::kAutoNoteId;
  star.parent_hold_id = target->id;
  const auto before = engine_.document().notes();
  auto after = before;
  after.push_back(star);
  after = wds::chart_editor::with_recomputed_hold_eighths(
      std::move(after), *target, engine_.document().timing().ticks_per_quarter);
  if (!engine_.execute_command(std::make_unique<wds::chart_editor::SetNotesCommand>(
          before, std::move(after), "Add hold star"))) {
    return false;
  }
  engine_.rebuild_snapshot();
  return true;
}

void ChartEditPanel::restore_hold_chain_prev_preview() {
  if (hold_chain_prev_id_ < 0) return;
  auto prev = engine_.document().find_note(hold_chain_prev_id_);
  if (!prev) return;
  NotationNote updated = *prev;
  updated.lane = hold_chain_prev_body_.lane;
  updated.width = hold_chain_prev_body_.width;
  updated.scratch_length = hold_chain_prev_body_.scratch_length;
  updated.gimmick_type = hold_chain_prev_body_.gimmick_type;
  if (updated.gimmick_type == prev->gimmick_type &&
      updated.scratch_length == prev->scratch_length && updated.lane == prev->lane &&
      updated.width == prev->width) {
    return;
  }
  engine_.document().update_note(hold_chain_prev_id_, updated);
  engine_.rebuild_snapshot();
}

void ChartEditPanel::commit_hold_chain_prev_cover() {
  if (hold_chain_prev_id_ < 0) return;
  auto cur = engine_.document().find_note(hold_chain_prev_id_);
  if (!cur) return;
  if (cur->gimmick_type == hold_chain_prev_body_.gimmick_type &&
      cur->scratch_length == hold_chain_prev_body_.scratch_length &&
      cur->lane == hold_chain_prev_body_.lane && cur->width == hold_chain_prev_body_.width) {
    return;
  }
  auto before = engine_.document().notes();
  for (auto& n : before) {
    if (n.id != hold_chain_prev_id_) continue;
    n.lane = hold_chain_prev_body_.lane;
    n.width = hold_chain_prev_body_.width;
    n.scratch_length = hold_chain_prev_body_.scratch_length;
    n.gimmick_type = hold_chain_prev_body_.gimmick_type;
    break;
  }
  auto after = engine_.document().notes();
  engine_.execute_command(
      std::make_unique<wds::chart_editor::SetNotesCommand>(before, after, "Adjust hold end"));
}

void ChartEditPanel::cancel_placement() {
  clear_pending_chain_extend();
  curve_mode_active_ = false;
  curve_dismissed_ = false;
  curve_ghosts_.clear();
  if (mode_ == Mode::PlaceHoldBody && hold_chain_prev_id_ >= 0) {
    // Abort only the in-progress next segment; previous remains the chain end.
    restore_hold_chain_prev_preview();
    select_hold_chain();
    mode_ = Mode::Idle;
    hold_stars_.clear();
    clear_hold_chain_state();
    update_ghost(pointer_);
    return;
  }
  if (mode_ == Mode::PlaceHoldBody || mode_ == Mode::PlaceGesture) {
    mode_ = Mode::Idle;
    hold_stars_.clear();
    clear_hold_chain_state();
    hide_placement_ghost();
    update_ghost(pointer_);
  }
}

void ChartEditPanel::finish_hold_body(bool chain_next) {
  curve_mode_active_ = false;
  curve_dismissed_ = false;
  curve_ghosts_.clear();
  sync_viewport();
  const int32_t min_dur = min_hold_duration_ticks();
  // Upward-only placement: never invert head/tail.
  if (hold_draft_.end_tick < hold_draft_.start_tick) {
    hold_draft_.end_tick = hold_draft_.start_tick;
  }
  // Below-minimum length: first segment degrades to Tap / Flick. Chained next with no
  // vertical pull is flick-tail-only — finish commits the previous end cover;
  // another chain click is ignored until a body height exists.
  if (hold_draft_.end_tick < hold_draft_.start_tick + min_dur) {
    if (hold_chain_prev_id_ >= 0) {
      if (chain_next) return;
      commit_hold_chain_prev_cover();
      select_hold_chain();
      mode_ = Mode::Idle;
      hold_stars_.clear();
      clear_hold_chain_state();
      update_ghost(pointer_);
      return;
    }
    NotationNote flat = hold_draft_;
    flat.id = wds::chart_editor::kAutoNoteId;
    flat.end_tick = flat.start_tick;
    flat.note_type = hold_scratch_ ? NoteType::Flick : NoteType::Normal;
    flat.scratch_length = 0;
    if (wds::chart_editor::note_intersects_negative_music_time(flat, engine_.document().timing())) {
      mode_ = Mode::Idle;
      hold_stars_.clear();
      clear_hold_chain_state();
      update_ghost(pointer_);
      return;
    }
    const auto before_ids = [&] {
      std::unordered_set<int32_t> ids;
      for (const auto& n : engine_.document().notes()) ids.insert(n.id);
      return ids;
    }();
    engine_.execute_command(std::make_unique<wds::chart_editor::AddNotesCommand>(
        std::vector<NotationNote>{flat}, hold_scratch_ ? "Place flick" : "Place note"));
    selected_.clear();
    for (const auto& n : engine_.document().notes()) {
      if (!before_ids.count(n.id)) selected_.insert(n.id);
    }
    mode_ = Mode::Idle;
    hold_stars_.clear();
    clear_hold_chain_state();
    clear_hold_sel_focus();
    update_ghost(pointer_);
    return;
  }

  // Place as a chain continuation only when JumpScratch cover is representable.
  // Otherwise auto-disconnect: independent hold + head, previous segment restored.
  const bool chain_link =
      hold_chain_prev_id_ >= 0 && chain_draft_cover_representable();
  if (!chain_link) {
    hold_draft_.note_type =
        wds::chart_editor::drawn_hold_body_type(hold_scratch_, place_gold_head_);
  }

  // Isolated / disconnected hold: no JumpScratch or OneDirection leftover.
  // A chain member (including the terminal) encodes official gimmick on the body.
  if (!chain_link) {
    hold_draft_.scratch_length = 0;
    hold_draft_.gimmick_type = wds::chart_editor::GimmickType::None;
  } else {
    wds::chart_editor::apply_hold_chain_gimmick(hold_draft_);
  }

  // Auto head: first segment of a chain, or a hold that disconnects on place.
  std::vector<NotationNote> to_add;
  int32_t next_id = engine_.document().next_note_id();
  if (!chain_link) {
    if (auto head = wds::chart_editor::make_auto_hold_head(engine_.document(), hold_draft_)) {
      head->id = next_id++;
      to_add.push_back(*head);
    }
  }
  hold_draft_.id = next_id++;
  to_add.push_back(hold_draft_);
  for (const auto& star : hold_stars_) {
    if (star.note.start_tick <= hold_draft_.start_tick ||
        star.note.start_tick >= hold_draft_.end_tick) {
      continue;
    }
    NotationNote note = star.note;
    note.id = next_id++;
    note.parent_hold_id = hold_draft_.id;
    note.lane = hold_draft_.lane;
    note.width = hold_draft_.width;
    to_add.push_back(note);
  }

  // Undo baseline restores the previous body before any live chain-tail preview.
  auto before = engine_.document().notes();
  if (hold_chain_prev_id_ >= 0) {
    for (auto& n : before) {
      if (n.id != hold_chain_prev_id_) continue;
      n.lane = hold_chain_prev_body_.lane;
      n.width = hold_chain_prev_body_.width;
      n.scratch_length = hold_chain_prev_body_.scratch_length;
      n.gimmick_type = hold_chain_prev_body_.gimmick_type;
      break;
    }
  }
  auto after = before;
  if (chain_link) {
    for (auto& n : after) {
      if (n.id != hold_chain_prev_id_) continue;
      apply_hold_tail_cover(n, hold_chain_prev_body_, hold_draft_, hold_scratch_);
      break;
    }
  }
  for (const auto& n : to_add) {
    after.push_back(n);
  }
  // Fold HoldEighths into the same SetNotesCommand so undo/redo stays transactional.
  {
    std::optional<NotationNote> new_hold;
    for (const auto& n : after) {
      if (n.id == hold_draft_.id && wds::chart_editor::is_hold_with_tail(n.note_type)) {
        new_hold = n;
        break;
      }
    }
    if (new_hold) {
      after = wds::chart_editor::with_recomputed_hold_eighths(
          std::move(after), *new_hold, engine_.document().timing().ticks_per_quarter);
    }
  }
  if (chain_link) {
    std::optional<NotationNote> prev_hold;
    for (const auto& n : after) {
      if (n.id != hold_chain_prev_id_) continue;
      prev_hold = n;
      break;
    }
    if (prev_hold) {
      after = wds::chart_editor::with_recomputed_hold_eighths(
          std::move(after), *prev_hold, engine_.document().timing().ticks_per_quarter);
    }
  }
  if (!engine_.execute_command(
          std::make_unique<wds::chart_editor::SetNotesCommand>(before, after, "Place hold"))) {
    mode_ = Mode::Idle;
    clear_hold_chain_state();
    return;
  }

  // Identify the hold just added by start tick + type (ids assigned on insert).
  std::optional<NotationNote> placed;
  for (auto it = engine_.document().notes().rbegin(); it != engine_.document().notes().rend();
       ++it) {
    if (!wds::chart_editor::is_hold_with_tail(it->note_type)) continue;
    if (it->start_tick != hold_draft_.start_tick) continue;
    if (it->lane != hold_draft_.lane || it->width != hold_draft_.width) continue;
    placed = *it;
    break;
  }
  if (chain_link) {
    if (placed) hold_chain_ids_.insert(placed->id);
  } else {
    // Auto-disconnect on place: start a fresh chain selection from the new hold.
    hold_chain_ids_.clear();
    if (placed) hold_chain_ids_.insert(placed->id);
  }
  engine_.rebuild_snapshot();
  // Select the new body and auto head as separate notes. Pairing is exact-span
  // only and must not hide a shortened auto head.
  std::unordered_set<int32_t> before_ids;
  for (const auto& n : before) before_ids.insert(n.id);
  clear_hold_sel_focus();
  selected_.clear();
  for (const int32_t id : hold_chain_ids_) selected_.insert(id);
  for (const auto& n : engine_.document().notes()) {
    if (before_ids.count(n.id)) continue;
    if (wds::chart_editor::is_hold_head_note(n) ||
        wds::chart_editor::is_hold_with_tail(n.note_type)) {
      selected_.insert(n.id);
    }
  }

  if (chain_next && placed) {
    hold_chain_prev_id_ = placed->id;
    hold_chain_prev_body_ = *placed;
    // Body span for later cover: use the just-placed body before any future widening.
    hold_chain_prev_body_.lane = hold_draft_.lane;
    hold_chain_prev_body_.width = hold_draft_.width;
    hold_chain_link_preview_ = true;

    NotationNote next = hold_draft_;
    next.id = wds::chart_editor::kAutoNoteId;
    next.start_tick = hold_draft_.end_tick;
    // Chained next starts undrawn: L/R only widens the previous flick tail until
    // the pointer pulls a vertical height for the next body.
    next.end_tick = next.start_tick;
    next.width = default_width_;
    apply_placement_lane_width(pointer_, next.lane, next.width);
    next.lane = wds::chart_editor::snap_scratch_chain_next_lane(
        hold_chain_prev_body_, next.width, viewport_.lane_left_at_f(pointer_.x, next.width),
        viewport_.grid().lane_count);
    next.scratch_length = 0;
    next.note_type = wds::chart_editor::drawn_hold_body_type(hold_scratch_, false);
    hold_draft_ = next;
    hold_stars_.clear();
    place_swipe_.reset();
    mode_ = Mode::PlaceHoldBody;
    // Immediately treat the pointer as the next flick-tail tip (no body yet).
    snap_hold_draft_chain_lane_and_sync(
        viewport_.lane_left_at_f(pointer_.x, hold_draft_.width));
    sync_hold_placement_ghost();
  } else {
    mode_ = Mode::Idle;
    hold_stars_.clear();
    clear_hold_chain_state();
    update_ghost(pointer_);
  }
}

void ChartEditPanel::finish_place_gesture(const wds::interaction::PointerUpEvent& event) {
  // Axis-locked place swipe (not angle classify). was_click only when there was
  // no axial swipe, so short left/right flicks are not collapsed into bidirectional.
  const auto swipe = update_place_swipe(event.position);
  const PlaceIntent intent = effective_place_intent(wds::interaction::resolve_place_intent(
      active_button_, swipe, swipe == SwipeDirection::None));
  if (pending_chain_extend_id_ >= 0 && intent == PlaceIntent::Tap) {
    clear_pending_chain_extend();
    mode_ = Mode::Idle;
    update_ghost(event.position);
    return;
  }
  // Commit on the snapped place anchor (swipe only selects type / hold end).
  const wds::interaction::Vec2 anchor = place_note_center();
  switch (intent) {
    case PlaceIntent::Tap:
      place_instant(NoteType::Normal, anchor);
      break;
    case PlaceIntent::ExTap:
      place_instant(NoteType::Critical, anchor);
      break;
    case PlaceIntent::HoldStart:
      place_instant(NoteType::HoldStart, anchor);
      break;
    case PlaceIntent::HoldBody: {
      begin_regular_hold_placement(anchor);
      const int32_t end = viewport_.tick_at(event.position.y);
      hold_draft_.end_tick = std::max(hold_draft_.start_tick, end);
      finish_hold_body(false);
      break;
    }
    case PlaceIntent::Flick:
      place_instant(NoteType::Flick, anchor, 0);
      break;
    case PlaceIntent::FlickLeft:
      place_instant(NoteType::Flick, anchor, -1);
      break;
    case PlaceIntent::FlickRight:
      place_instant(NoteType::Flick, anchor, 1);
      break;
    case PlaceIntent::ScratchHoldBody: {
      begin_scratch_hold_placement(anchor);
      const int32_t end = viewport_.tick_at(event.position.y);
      hold_draft_.end_tick = std::max(hold_draft_.start_tick, end);
      finish_hold_body(false);
      break;
    }
    case PlaceIntent::None:
      break;
  }
  clear_pending_chain_extend();
  mode_ = Mode::Idle;
  update_ghost(event.position);
}

wds::interaction::Rect ChartEditPanel::marquee_screen_rect(wds::interaction::Vec2 end) const {
  const int32_t t0 = marquee_start_tick_;
  const int32_t t1 = viewport_.tick_at(end.y);
  const int32_t l0 = marquee_start_lane_;
  const int32_t l1 = viewport_.lane_at(end.x, 1);
  const int32_t t_lo = std::min(t0, t1);
  const int32_t t_hi = std::max(t0, t1);
  const int32_t l_lo = std::min(l0, l1);
  const int32_t l_hi = std::max(l0, l1);
  // Later ticks are toward the top (smaller y). May extend past the visible panel
  // when the anchor tick was scrolled out of view.
  const float y_top = viewport_.y_at(t_hi);
  const float y_bot = viewport_.y_at(t_lo);
  const float x0 = viewport_.x_at(l_lo);
  const float x1 = viewport_.x_at(l_hi + 1);
  return {x0, y_top, std::max(0.0f, x1 - x0), std::max(0.0f, y_bot - y_top)};
}

void ChartEditPanel::finish_marquee(wds::interaction::Vec2 end) {
  sync_viewport();
  const int32_t t0 = marquee_start_tick_;
  const int32_t t1 = viewport_.tick_at(end.y);
  const int32_t l0 = marquee_start_lane_;
  const int32_t l1 = viewport_.lane_at(end.x, 1);
  selected_.clear();
  clear_hold_sel_focus();
  for (const auto& note : engine_.document().notes()) {
    if (note.note_type == NoteType::HoldEighth) continue;
    if (note.start_tick >= std::min(t0, t1) && note.start_tick <= std::max(t0, t1) &&
        note.lane <= std::max(l0, l1) && note.end_lane() >= std::min(l0, l1)) {
      selected_.insert(note.id);
    }
  }
  mode_ = Mode::Idle;
}

void ChartEditPanel::sync_move_selection_to_pointer(wds::interaction::Vec2 point) {
  if (mode_ != Mode::MoveSelection || !engine_.is_editable()) return;
  sync_viewport();
  // Keep the real pointer untouched; only snap math treats out-of-window as
  // the nearest in-window edge. Outside the edit pane, lane_at / tick_at still
  // resolve to the nearest in-range track / time.
  const wds::interaction::Vec2 mapped = pointer_as_in_host(point);

  const int32_t tick = viewport_.tick_at(mapped.y);
  const int32_t lane = viewport_.lane_at(mapped.x, 1);
  int32_t d_tick = tick - drag_start_tick_;
  int32_t d_lane = lane - drag_start_lane_;
  const int lane_count = viewport_.grid().lane_count;

  if (move_scratch_segment_) {
    const NotationNote* moving_orig = nullptr;
    for (const auto& [id, orig] : drag_originals_) {
      if (id == resize_chain_peer_id_ || id == resize_chain_next_id_) continue;
      if (wds::chart_editor::is_hold_chain_body(orig.note_type)) {
        moving_orig = &orig;
        break;
      }
    }
    if (!moving_orig) return;
    const NotationNote* prev_orig = nullptr;
    const NotationNote* next_orig = nullptr;
    if (resize_chain_peer_id_ >= 0) {
      if (auto it = drag_originals_.find(resize_chain_peer_id_); it != drag_originals_.end()) {
        prev_orig = &it->second;
      }
    }
    if (resize_chain_next_id_ >= 0) {
      if (auto it = drag_originals_.find(resize_chain_next_id_); it != drag_originals_.end()) {
        next_orig = &it->second;
      }
    }
    const float desired =
        static_cast<float>(moving_orig->lane + (lane - drag_start_lane_));
    const int32_t snapped = wds::chart_editor::snap_scratch_hold_segment_lane(
        prev_orig, *moving_orig, next_orig, desired, lane_count);
    d_lane = snapped - moving_orig->lane;

    std::unordered_map<int32_t, NotationNote> next;
    for (const auto& [id, orig] : drag_originals_) {
      if ((resize_chain_peer_id_ >= 0 && id == resize_chain_peer_id_) ||
          (resize_chain_next_id_ >= 0 && id == resize_chain_next_id_)) {
        next[id] = orig;
        continue;
      }
      NotationNote n = orig;
      if (id == moving_orig->id) {
        n.lane = snapped;
      } else {
        n.lane = wds::chart_editor::clamp_lane_for_width(orig.lane + d_lane, orig.width, lane_count);
      }
      next[id] = n;
    }
    auto body_it = next.find(moving_orig->id);
    if (body_it == next.end()) return;
    if (prev_orig) {
      auto prev_it = next.find(prev_orig->id);
      if (prev_it != next.end()) {
        wds::chart_editor::sync_scratch_chain_joint(prev_it->second, body_it->second);
      }
    }
    if (next_orig) {
      wds::chart_editor::sync_scratch_chain_joint(body_it->second, *next_orig);
    }
    for (auto& [id, n] : next) {
      const auto& orig = drag_originals_.at(id);
      if (!is_visible_mid_star(orig.note_type) && orig.note_type != NoteType::HoldEighth) continue;
      n.lane = body_it->second.lane;
      n.width = body_it->second.width;
    }
    if (!apply_note_map(engine_.document(), next)) return;
    engine_.rebuild_snapshot();
    return;
  }

  // Shrink the shared delta so every free-moving note stays in bounds, including
  // ScratchHold terminal JumpScratch overhang (do not snap the whole selection
  // back to the drag origin when the cursor overshoots).
  int32_t applied_d_tick = d_tick;
  if (auto anchor_it = drag_originals_.find(anchor_note_id_); anchor_it != drag_originals_.end()) {
    applied_d_tick = wds::chart_editor::selection_drag_tick_delta(
        anchor_it->second.start_tick, d_tick, viewport_.grid());
  }
  const int32_t min_tick = first_legal_tick();
  for (const auto& [id, orig] : drag_originals_) {
    if (is_visible_mid_star(orig.note_type) || orig.note_type == NoteType::HoldEighth) {
      continue;
    }
    const int32_t floor = clamps_to_nonnegative_music(orig) ? min_tick : 0;
    if (orig.start_tick + applied_d_tick < floor) {
      applied_d_tick = floor - orig.start_tick;
    }
    if (orig.width > 0 && lane_count > 0) {
      const auto [occ_lane, occ_width] = wds::chart_editor::occupied_lane_span(orig);
      const int32_t max_lane = lane_count - occ_width;
      if (occ_lane + d_lane < 0) {
        d_lane = -occ_lane;
      }
      if (occ_lane + d_lane > max_lane) {
        d_lane = max_lane - occ_lane;
      }
    }
  }

  std::unordered_map<int32_t, NotationNote> next;
  for (const auto& [id, orig] : drag_originals_) {
    NotationNote n = orig;
    // Grabbed note snaps to the subdivision grid; everyone else keeps their
    // offset from that note (same applied_d_tick, no per-note snap).
    const int32_t duration =
        orig.end_tick > orig.start_tick ? (orig.end_tick - orig.start_tick) : 0;
    const int32_t floor = clamps_to_nonnegative_music(orig) ? min_tick : 0;
    const int32_t new_start = std::max(floor, orig.start_tick + applied_d_tick);
    n.start_tick = new_start;
    n.end_tick = duration > 0 ? n.start_tick + duration : n.start_tick;
    n.lane = wds::chart_editor::clamp_lane_for_width(orig.lane + d_lane, orig.width, lane_count);
    next[id] = n;
  }
  // Mid-stars / eighths: never free-drag laterally; lock to parent hold span.
  for (auto& [id, n] : next) {
    const auto& orig = drag_originals_.at(id);
    if (!is_visible_mid_star(orig.note_type) && orig.note_type != NoteType::HoldEighth) continue;
    std::optional<NotationNote> parent =
        bound_parent_hold(orig, next, engine_.document());
    if (parent) {
      n.lane = parent->lane;
      n.width = parent->width;
      const int32_t lo = parent->start_tick + 1;
      const int32_t hi = parent->end_tick - 1;
      if (lo < hi) {
        n.start_tick = std::clamp(n.start_tick, lo, hi);
        n.end_tick = n.start_tick;
      } else if (lo == hi) {
        n.start_tick = lo;
        n.end_tick = n.start_tick;
      }
    } else {
      n.lane = orig.lane;
      n.width = orig.width;
    }
  }
  if (!apply_note_map(engine_.document(), next)) {
    return;
  }
  engine_.rebuild_snapshot();
}

void ChartEditPanel::sync_split_edge_to_pointer(wds::interaction::Vec2 point) {
  if (mode_ != Mode::DragSplitEdge || !engine_.is_editable()) return;
  sync_viewport();
  const wds::interaction::Vec2 mapped = pointer_as_in_host(point);
  const int32_t tick = viewport_.tick_at(mapped.y);
  // Hold still on the press tick so a click on the off-line label does not jump.
  if (!split_edge_ever_moved_ && tick == drag_split_press_tick_) return;
  const int32_t min_dur = min_hold_duration_ticks();
  const int32_t legal = first_legal_tick();
  auto note = engine_.document().find_note(drag_split_note_id_);
  if (!note) return;
  NotationNote updated = *note;
  if (drag_split_is_end_) {
    updated.end_tick = std::max({updated.start_tick + min_dur, tick, legal});
  } else {
    const int32_t hi = updated.end_tick - min_dur;
    if (legal <= hi) {
      updated.start_tick = std::clamp(tick, legal, hi);
    }
  }
  if (auto orig_it = drag_originals_.find(drag_split_note_id_); orig_it != drag_originals_.end()) {
    if (updated.start_tick != orig_it->second.start_tick ||
        updated.end_tick != orig_it->second.end_tick) {
      split_edge_ever_moved_ = true;
    }
  }
  if (updated.start_tick == note->start_tick && updated.end_tick == note->end_tick) return;
  engine_.document().update_note(drag_split_note_id_, updated);
  engine_.rebuild_snapshot();
}

void ChartEditPanel::sync_hold_adjust_to_pointer(wds::interaction::Vec2 point) {
  if (mode_ != Mode::AdjustHoldTime || !engine_.is_editable()) return;
  sync_viewport();
  const wds::interaction::Vec2 mapped = pointer_as_in_host(point);
  const int32_t tick = viewport_.tick_at(mapped.y);
  const int32_t min_dur = min_hold_duration_ticks();
  auto anchor_it = drag_originals_.find(anchor_note_id_);
  if (anchor_it == drag_originals_.end()) return;
  const NotationNote& anchor_orig = anchor_it->second;

  // Keep HoldEighth / mid-stars inside their parent hold after body edges move.
  auto collect_attached_updates = [&](std::unordered_map<int32_t, NotationNote>& pending) {
    for (const auto& [id, orig] : drag_originals_) {
      if (!is_visible_mid_star(orig.note_type) && orig.note_type != NoteType::HoldEighth) {
        continue;
      }
      auto parent = bound_parent_hold(orig, pending, engine_.document());
      if (!parent) continue;
      NotationNote n = orig;
      n.lane = parent->lane;
      n.width = parent->width;
      const int32_t lo = parent->start_tick + 1;
      const int32_t hi = parent->end_tick - 1;
      if (lo < hi) {
        n.start_tick = std::clamp(orig.start_tick, lo, hi);
        // Prefer preserving relative offset when the whole span shifts.
        if (auto hold_orig = drag_originals_.find(parent->id); hold_orig != drag_originals_.end()) {
          const int32_t delta = parent->start_tick - hold_orig->second.start_tick;
          n.start_tick = std::clamp(orig.start_tick + delta, lo, hi);
        }
        n.end_tick = n.start_tick;
        pending[id] = n;
      }
    }
  };

  // JumpScratch vertical hinge: move the joint tick; adjacent bodies change length.
  // Clamp so neither body collapses below one grid subdivision (cannot cross neighbors).
  if (adjust_end_ && wds::chart_editor::is_hold_chain_body(anchor_orig.note_type)) {
    int32_t lo = static_cast<int32_t>(anchor_orig.start_tick) + min_dur;
    int32_t hi = std::numeric_limits<int32_t>::max() / 4;
    const NotationNote* next_orig = nullptr;
    auto is_chained_next = [&](const NotationNote& n) {
      if (!wds::chart_editor::is_hold_chain_body(n.note_type) || n.id == anchor_orig.id) {
        return false;
      }
      if (!wds::chart_editor::same_hold_chain_family(anchor_orig.note_type, n.note_type)) {
        return false;
      }
      if (n.start_tick != anchor_orig.end_tick) return false;
      if (wds::chart_editor::paired_hold_head_for(engine_.document(), n)) return false;
      return wds::chart_editor::hold_chain_lanes_connected(anchor_orig, n);
    };
    if (resize_chain_next_id_ >= 0) {
      auto nit = drag_originals_.find(resize_chain_next_id_);
      if (nit != drag_originals_.end() && is_chained_next(nit->second)) {
        next_orig = &nit->second;
      }
    }
    if (!next_orig) {
      for (const auto& [id, n] : drag_originals_) {
        (void)id;
        if (!is_chained_next(n)) continue;
        next_orig = &n;
        break;
      }
    }
    if (next_orig) {
      hi = next_orig->end_tick - min_dur;
    }
    if (lo > hi) return;
    const int32_t joint = std::clamp(tick, lo, hi);

    std::unordered_map<int32_t, NotationNote> pending;
    NotationNote prev_n = anchor_orig;
    prev_n.end_tick = joint;
    pending[prev_n.id] = prev_n;
    if (next_orig) {
      NotationNote next_n = *next_orig;
      next_n.start_tick = joint;
      pending[next_n.id] = next_n;
    }
    collect_attached_updates(pending);
    if (!apply_note_map(engine_.document(), pending)) {
      return;
    }
    engine_.rebuild_snapshot();
    return;
  }

  std::unordered_map<int32_t, NotationNote> pending;
  int32_t new_start = -1;
  for (const auto& [id, orig] : drag_originals_) {
    NotationNote n = orig;
    if (!wds::chart_editor::is_hold_with_tail(n.note_type)) continue;
    // Only the anchor body changes for non-JumpScratch time edges (avoid collapsing a
    // multi-selected ScratchHold chain to one shared tick).
    if (wds::chart_editor::is_hold_chain_body(n.note_type) && id != anchor_note_id_) {
      continue;
    }
    if (adjust_end_) {
      n.end_tick = std::max({n.start_tick + min_dur, tick, first_legal_tick()});
    } else {
      const int32_t legal = first_legal_tick();
      const int32_t hi = n.end_tick - min_dur;
      if (legal <= hi) {
        n.start_tick = std::clamp(tick, legal, hi);
      }
      new_start = n.start_tick;
    }
    pending[id] = n;
  }
  // Paired head follows hold start when the start edge is dragged.
  if (!adjust_end_ && new_start >= 0) {
    for (const auto& [id, orig] : drag_originals_) {
      if (!wds::chart_editor::is_hold_head_note(orig)) continue;
      NotationNote n = orig;
      n.start_tick = new_start;
      n.end_tick = new_start;
      pending[id] = n;
    }
  }
  collect_attached_updates(pending);
  if (!apply_note_map(engine_.document(), pending)) {
    return;
  }
  engine_.rebuild_snapshot();
}

void ChartEditPanel::finish_move(bool refresh_eighths) {
  if (wds::chart_editor::visible_star_tick_conflicts(engine_.document().notes())) {
    std::vector<wds::chart_editor::NoteUpdate> revert;
    revert.reserve(drag_originals_.size());
    for (const auto& [id, before] : drag_originals_) {
      revert.push_back({id, before});
    }
    if (!revert.empty()) {
      engine_.document().apply_note_updates(revert);
      engine_.rebuild_snapshot();
    }
    drag_originals_.clear();
    move_scratch_segment_ = false;
    resize_chain_peer_id_ = -1;
    resize_chain_next_id_ = -1;
    mode_ = Mode::Idle;
    return;
  }
  std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair> changes;
  std::vector<int32_t> eighth_hold_ids;
  for (const auto& [id, before] : drag_originals_) {
    auto cur = engine_.document().find_note(id);
    if (!cur) continue;
    if (cur->start_tick != before.start_tick || cur->lane != before.lane ||
        cur->end_tick != before.end_tick || cur->width != before.width ||
        cur->scratch_length != before.scratch_length ||
        cur->gimmick_type != before.gimmick_type) {
      changes[id] = {before, *cur};
      if ((wds::chart_editor::is_hold_with_tail(before.note_type) ||
           wds::chart_editor::is_hold_with_tail(cur->note_type)) &&
          (cur->start_tick != before.start_tick || cur->lane != before.lane ||
           cur->end_tick != before.end_tick || cur->width != before.width)) {
        eighth_hold_ids.push_back(id);
      }
    }
  }
  // Revert live edits then apply command (document already mutated during drag).
  if (!changes.empty()) {
    std::vector<wds::chart_editor::NoteUpdate> revert;
    revert.reserve(changes.size());
    for (const auto& [id, pair] : changes) {
      revert.push_back({id, pair.first});
    }
    if (!engine_.document().apply_note_updates(revert)) {
      // Keep the live document; do not push a history entry from a failed revert.
      drag_originals_.clear();
      move_scratch_segment_ = false;
      resize_chain_peer_id_ = -1;
      resize_chain_next_id_ = -1;
      mode_ = Mode::Idle;
      return;
    }
    commit_updates(changes, "Move notes");
  }
  if (refresh_eighths && !eighth_hold_ids.empty()) {
    const auto before = engine_.document().notes();
    auto after = before;
    const int32_t tpq = engine_.document().timing().ticks_per_quarter;
    for (const int32_t id : eighth_hold_ids) {
      std::optional<NotationNote> body;
      for (const auto& n : after) {
        if (n.id == id && wds::chart_editor::is_hold_with_tail(n.note_type)) {
          body = n;
          break;
        }
      }
      if (!body) continue;
      after = wds::chart_editor::with_recomputed_hold_eighths(std::move(after), *body, tpq);
    }
    engine_.execute_command(std::make_unique<wds::chart_editor::SetNotesCommand>(
        before, std::move(after), "Refresh hold eighths"));
  }
  drag_originals_.clear();
  move_scratch_segment_ = false;
  resize_chain_peer_id_ = -1;
  resize_chain_next_id_ = -1;
  mode_ = Mode::Idle;
}

void ChartEditPanel::finish_resize() {
  resize_scratch_end_ = false;
  resize_chain_peer_id_ = -1;
  resize_chain_next_id_ = -1;
  resize_applied_end_l_ = std::numeric_limits<int32_t>::min();
  resize_applied_end_r_ = std::numeric_limits<int32_t>::min();
  resize_applied_body_lane_ = std::numeric_limits<int32_t>::min();
  resize_applied_body_width_ = std::numeric_limits<int32_t>::min();
  resize_applied_peer_lane_ = std::numeric_limits<int32_t>::min();
  resize_applied_peer_width_ = std::numeric_limits<int32_t>::min();
  finish_move();  // same snapshot pattern
}

void ChartEditPanel::finish_hold_adjust() {
  // Include chain neighbors that may have been hinged but were not selected.
  std::vector<int32_t> hold_ids;
  for (const auto& [id, before] : drag_originals_) {
    if (wds::chart_editor::is_hold_with_tail(before.note_type)) hold_ids.push_back(id);
  }
  resize_chain_peer_id_ = -1;
  resize_chain_next_id_ = -1;
  finish_move(false);
  if (!hold_ids.empty()) {
    const auto before = engine_.document().notes();
    for (const int32_t id : hold_ids) {
      auto body = engine_.document().find_note(id);
      if (!body || !wds::chart_editor::is_hold_with_tail(body->note_type)) continue;
      wds::chart_editor::prune_hold_mid_stars(engine_.document(), *body);
    }
    auto after = engine_.document().notes();
    const int32_t tpq = engine_.document().timing().ticks_per_quarter;
    for (const int32_t id : hold_ids) {
      std::optional<NotationNote> body;
      for (const auto& n : after) {
        if (n.id == id && wds::chart_editor::is_hold_with_tail(n.note_type)) {
          body = n;
          break;
        }
      }
      if (!body) continue;
      after = wds::chart_editor::with_recomputed_hold_eighths(std::move(after), *body, tpq);
    }
    // prune_hold_mid_stars mutated outside history — fold its result + eighths together.
    engine_.execute_command(std::make_unique<wds::chart_editor::SetNotesCommand>(
        before, std::move(after), "Adjust hold eighths"));
  }
  engine_.rebuild_snapshot();
}

bool ChartEditPanel::convert_selected(NoteType target, std::optional<int32_t> scratch_length) {
  if (!engine_.is_editable() || selected_.empty()) return false;
  const auto& doc = engine_.document();
  const auto planned =
      wds::chart_editor::convert_notes_in_selection(doc, selected_, target, scratch_length);

  std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair> changes;
  for (const auto& [id, after] : planned.updates) {
    auto before = doc.find_note(id);
    if (!before) continue;
    if (after.note_type != before->note_type || after.end_tick != before->end_tick ||
        after.scratch_length != before->scratch_length ||
        after.gimmick_type != before->gimmick_type ||
        after.parent_hold_id != before->parent_hold_id) {
      changes[id] = {*before, after};
    }
  }
  if (changes.empty() && planned.removals.empty()) return false;

  auto composite = std::make_unique<wds::chart_editor::CompositeCommand>("Convert notes");
  if (!changes.empty()) {
    composite->add(
        std::make_unique<wds::chart_editor::UpdateNotesCommand>(std::move(changes), "Convert notes"));
  }
  if (!planned.removals.empty()) {
    composite->add(std::make_unique<wds::chart_editor::RemoveNotesCommand>(
        planned.removals, "Convert cleanup"));
  }
  if (!engine_.execute_command(std::move(composite))) return false;

  for (const auto& removed : planned.removals) selected_.erase(removed.id);

  // Converted hold bodies stay headless by default (unlike place-hold). Still refresh
  // eighths for any body that remains / becomes a hold — inside one undoable command.
  {
    const auto before = engine_.document().notes();
    auto after = before;
    bool touched = false;
    for (const auto& [id, converted] : planned.updates) {
      if (!wds::chart_editor::is_hold_with_tail(converted.note_type)) continue;
      std::optional<NotationNote> body;
      for (const auto& n : after) {
        if (n.id == id) {
          body = n;
          break;
        }
      }
      if (!body) continue;
      after = wds::chart_editor::with_recomputed_hold_eighths(
          std::move(after), *body, engine_.document().timing().ticks_per_quarter);
      touched = true;
    }
    if (touched) {
      engine_.execute_command(std::make_unique<wds::chart_editor::SetNotesCommand>(
          before, std::move(after), "Refresh hold eighths"));
    }
  }
  engine_.rebuild_snapshot();
  sync_hold_sel_focus_to_selection();
  return true;
}

bool ChartEditPanel::mirror_selected(bool about_center) {
  if (!engine_.is_editable() || selected_.empty()) return false;
  auto notes = selected_with_hold_dependents(engine_.document(), selected_);
  auto before = notes;
  if (about_center) {
    wds::chart_editor::mirror_notes_about_center(notes);
  } else {
    wds::chart_editor::mirror_notes(notes, viewport_.grid().lane_count);
  }
  std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair> changes;
  for (size_t i = 0; i < notes.size(); ++i) changes[notes[i].id] = {before[i], notes[i]};
  return commit_updates(changes, about_center ? "Mirror center" : "Mirror");
}

bool ChartEditPanel::nudge_selected(int32_t delta_tick, int32_t delta_lane) {
  if (!engine_.is_editable() || selected_.empty()) return false;
  std::vector<NotationNote> notes;
  for (const int32_t id : selected_) {
    if (auto n = engine_.document().find_note(id)) notes.push_back(*n);
  }
  auto before = notes;
  if (delta_tick != 0 &&
      !wds::chart_editor::nudge_notes_time(notes, delta_tick, first_legal_tick())) {
    return false;
  }
  if (delta_lane != 0 &&
      !wds::chart_editor::nudge_notes_lane(notes, delta_lane, viewport_.grid().lane_count)) {
    return false;
  }
  std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair> changes;
  for (size_t i = 0; i < notes.size(); ++i) changes[notes[i].id] = {before[i], notes[i]};
  return commit_updates(changes, "Nudge");
}

bool ChartEditPanel::copy_selected() {
  clipboard_ = selected_with_hold_dependents(engine_.document(), selected_);
  // HoldEighth is derived from holds — clipboard must not carry authoritative copies.
  strip_clipboard_hold_eighths(clipboard_);
  return !clipboard_.empty();
}

bool ChartEditPanel::paste_at_pointer() {
  if (!engine_.is_editable() || clipboard_.empty()) return false;
  sync_viewport();
  const int32_t legal = first_legal_tick();
  const int32_t anchor = std::max(legal, viewport_.tick_at(pointer_.y));
  auto pasted = wds::chart_editor::paste_notes_aligned(clipboard_, anchor, viewport_.grid());
  // Defensive: old clipboards / callers may still include eighths.
  strip_clipboard_hold_eighths(pasted);
  int32_t shift = 0;
  for (const auto& n : pasted) {
    if (!clamps_to_nonnegative_music(n)) continue;
    if (n.start_tick < legal) shift = std::max(shift, legal - n.start_tick);
    if (n.end_tick > n.start_tick && n.end_tick < legal) {
      shift = std::max(shift, legal - n.end_tick);
    }
  }
  int32_t next_id = engine_.document().next_note_id();
  std::unordered_map<int32_t, int32_t> old_to_new;
  for (auto& n : pasted) {
    if (shift > 0) {
      n.start_tick += shift;
      if (n.end_tick > 0) n.end_tick += shift;
    }
    if (n.id >= 0) old_to_new[n.id] = next_id;
    n.id = next_id++;
  }
  for (auto& n : pasted) {
    if (n.parent_hold_id < 0) continue;
    if (auto it = old_to_new.find(n.parent_hold_id); it != old_to_new.end()) {
      n.parent_hold_id = it->second;
    }
  }
  if (wds::chart_editor::visible_star_tick_conflicts(pasted)) return false;
  for (const auto& n : pasted) {
    if (!is_visible_mid_star(n.note_type) || n.parent_hold_id < 0) continue;
    if (wds::chart_editor::hold_has_visible_star_at(engine_.document(), n.parent_hold_id,
                                                    n.start_tick,
                                                    wds::chart_editor::kNoBoundHoldId)) {
      return false;
    }
  }

  const auto before = engine_.document().notes();
  auto after = before;
  after.insert(after.end(), pasted.begin(), pasted.end());
  const int32_t tpq = engine_.document().timing().ticks_per_quarter;
  for (const auto& n : pasted) {
    if (!wds::chart_editor::is_hold_with_tail(n.note_type)) continue;
    after = wds::chart_editor::with_recomputed_hold_eighths(std::move(after), n, tpq);
  }
  if (!engine_.execute_command(
          std::make_unique<wds::chart_editor::SetNotesCommand>(before, std::move(after), "Paste"))) {
    return false;
  }

  const auto before_ids = [&] {
    std::unordered_set<int32_t> ids;
    for (const auto& n : before) ids.insert(n.id);
    return ids;
  }();
  selected_.clear();
  for (const auto& n : engine_.document().notes()) {
    // HoldEighth is always derived from holds — never enter the selection set.
    if (!before_ids.count(n.id) && n.note_type != NoteType::HoldEighth) {
      selected_.insert(n.id);
    }
  }
  clear_hold_sel_focus();
  return true;
}

namespace {

void collect_selected_originals(const wds::chart_editor::ChartDocument& doc,
                                const std::unordered_set<int32_t>& selected,
                                std::unordered_map<int32_t, NotationNote>& out) {
  out.clear();
  for (const int32_t id : selected) {
    if (auto n = doc.find_note(id)) out[id] = *n;
  }
}

// Time-adjust / coupled edits: expand selection with hold attachments + paired head/body.
void collect_drag_originals(const wds::chart_editor::ChartDocument& doc,
                            const std::unordered_set<int32_t>& selected,
                            std::unordered_map<int32_t, NotationNote>& out) {
  out.clear();
  for (const int32_t id : selected) {
    if (auto n = doc.find_note(id)) out[id] = *n;
  }
  for (const int32_t id : selected) {
    auto n = doc.find_note(id);
    if (!n) continue;
    if (wds::chart_editor::is_hold_with_tail(n->note_type)) {
      for (const auto& dep : wds::chart_editor::hold_attached_notes_for(doc, *n)) {
        if (dep.note_type == NoteType::HoldEighth) continue;
        out.emplace(dep.id, dep);
      }
      if (auto head = wds::chart_editor::paired_hold_head_for(doc, *n)) {
        out.emplace(head->id, *head);
      }
    } else if (wds::chart_editor::is_hold_head_note(*n)) {
      if (auto body = wds::chart_editor::paired_hold_body_for(doc, *n)) {
        out.emplace(body->id, *body);
        for (const auto& dep : wds::chart_editor::hold_attached_notes_for(doc, *body)) {
          if (dep.note_type == NoteType::HoldEighth) continue;
          out.emplace(dep.id, dep);
        }
      }
    }
  }
}

void append_hold_attached(const wds::chart_editor::ChartDocument& doc, const NotationNote& hold,
                          std::unordered_map<int32_t, NotationNote>& out) {
  if (!wds::chart_editor::is_hold_with_tail(hold.note_type)) return;
  for (const auto& dep : wds::chart_editor::hold_attached_notes_for(doc, hold)) {
    if (dep.note_type == NoteType::HoldEighth) continue;
    out.emplace(dep.id, dep);
  }
}

// Snapshot every ScratchHold body in the chain containing `seed` (plus attached notes /
// equal-width heads) so mid-drag backward sync never reads a prior frame's expanded body
// from the live document.
void append_scratch_hold_chain_snapshot(const wds::chart_editor::ChartDocument& doc,
                                        const NotationNote& seed,
                                        std::unordered_map<int32_t, NotationNote>& out) {
  if (!wds::chart_editor::is_hold_chain_body(seed.note_type)) return;
  NotationNote cur = seed;
  for (int guard = 0; guard < 64; ++guard) {
    auto prev = wds::chart_editor::chained_prev_scratch_hold(doc, cur);
    if (!prev) break;
    cur = *prev;
  }
  for (int guard = 0; guard < 64; ++guard) {
    out.emplace(cur.id, cur);
    append_hold_attached(doc, cur, out);
    if (auto head = wds::chart_editor::paired_hold_head_for(doc, cur)) {
      if (head->width == cur.width) out.emplace(head->id, *head);
    }
    auto next = wds::chart_editor::chained_next_scratch_hold(doc, cur);
    if (!next) break;
    cur = *next;
  }
}

std::optional<NotationNote> hold_width_pair_partner(const wds::chart_editor::ChartDocument& doc,
                                                    const NotationNote& note) {
  if (wds::chart_editor::is_hold_with_tail(note.note_type)) {
    return wds::chart_editor::paired_hold_head_for(doc, note);
  }
  if (wds::chart_editor::is_hold_head_note(note)) {
    return wds::chart_editor::paired_hold_body_for(doc, note);
  }
  return std::nullopt;
}

}  // namespace

bool ChartEditPanel::delete_selected() {
  if (!engine_.is_editable() || selected_.empty()) return false;
  const auto notes = selected_with_hold_dependents(engine_.document(), selected_);
  if (!engine_.execute_command(
          std::make_unique<wds::chart_editor::RemoveNotesCommand>(notes, "Delete"))) {
    return false;
  }
  selected_.clear();
  return true;
}

bool ChartEditPanel::delete_note_at(wds::interaction::Vec2 point) {
  if (!engine_.is_editable()) return false;
  const auto hit = hit_test_note(point);
  if (!hit) return false;
  // Middle-click deletes only the hit note (+ hold mid dependents). Never the
  // paired hold head/body — those stay independently deletable.
  const auto notes = notes_with_hold_dependents(engine_.document(), {*hit});
  if (!engine_.execute_command(
          std::make_unique<wds::chart_editor::RemoveNotesCommand>(notes, "Delete Note"))) {
    return false;
  }
  for (const auto& n : notes) selected_.erase(n.id);
  sync_hold_sel_focus_to_selection();
  return true;
}

void ChartEditPanel::set_error_ticks(std::vector<int32_t> ticks, uint64_t content_generation) {
  error_ticks_ = std::move(ticks);
  error_ticks_generation_ = content_generation;
  error_ticks_armed_ = true;
}

void ChartEditPanel::flash_offset_violations(std::vector<int32_t> ids) {
  offset_violation_ids_.clear();
  offset_violation_ids_.insert(ids.begin(), ids.end());
  offset_violation_elapsed_ = 0.0f;
}

int32_t ChartEditPanel::first_legal_tick() const {
  return wds::chart_editor::first_legal_note_tick(engine_.document().timing());
}

float ChartEditPanel::offset_violation_strength() const noexcept {
  if (offset_violation_ids_.empty()) return 0.0f;
  if (offset_violation_elapsed_ <= 2.0f) return 1.0f;
  return std::max(0.0f, 1.0f - (offset_violation_elapsed_ - 2.0f));
}

const std::vector<int32_t>& ChartEditPanel::error_ticks() const noexcept {
  sync_error_ticks();
  return error_ticks_;
}

void ChartEditPanel::sync_error_ticks() const {
  if (!error_ticks_armed_) return;
  if (engine_.document().content_generation() != error_ticks_generation_) {
    error_ticks_.clear();
    error_ticks_armed_ = false;
  }
}

void ChartEditPanel::paint(wds::interaction::UiPainter& painter) const {
  sync_viewport();
  sync_error_ticks();
  layout_popup_rects();
  const auto& timing = engine_.document().timing();
  const bool show_timing_grid = engine_.is_editable();
  paint_gutters(painter);
  renderer_.paint(painter, viewport_, timing, engine_.document().notes(),
                  engine_.preview_config(), selected_, std::nullopt, {}, std::nullopt, skin_,
                  show_timing_grid, active_split_highlight_id(), error_ticks_,
                  &offset_violation_ids_, offset_violation_strength(), waveform_,
                  spectrogram_ ? &spectrogram_ : nullptr, spectrum_mode_);
  // Modals are painted last via paint_popups() from UiManager (above skins).
}

void ChartEditPanel::paint_gutters(wds::interaction::UiPainter& painter) const {
  const auto& timing = engine_.document().timing();
  const auto range = viewport_.visible_tick_range();
  const bool show_timing_grid = engine_.is_editable();
  paint_split_gutter(painter, viewport_, left_gutter_, engine_.document().notes(), skin_,
                     show_timing_grid);
  paint_timing_gutter(painter, viewport_, right_gutter_, timing, range.first, range.second,
                      show_timing_grid);
  paint_measure_index_gutter(painter, viewport_, measure_gutter_, timing, range.first,
                             range.second);
  painter.fill_rect({left_gutter_.right() - 0.5f, left_gutter_.y, 1.0f, left_gutter_.h},
                    wds::interaction::theme::kOutline);
  painter.fill_rect({right_gutter_.x - 0.5f, right_gutter_.y, 1.0f, right_gutter_.h},
                    wds::interaction::theme::kOutline);
  painter.fill_rect({measure_gutter_.x - 0.5f, measure_gutter_.y, 1.0f, measure_gutter_.h},
                    wds::interaction::theme::kOutline);
}

void ChartEditPanel::layout_popup_rects() const {
  const auto host = overlay_host_bounds();
  namespace th = wds::interaction::theme;
  // Logical 1× sizes (via th::px) so Win 1× and Mac Retina keep the same visible row count.
  // Older Mac branch used raw fb constants (84–128 row) that only looked right at 2×; the
  // Win branch used th::px(72–108) and therefore showed roughly half as many rows.
  const float margin = th::px(12.0f);
  const float picker_w =
      std::min(th::px(540.0f), std::max(th::px(160.0f), host.w - margin * 2.0f));
  const float picker_h =
      std::min(th::px(630.0f), std::max(th::px(180.0f), host.h - margin * 2.0f));
  split_picker_bounds_ = {host.x + (host.w - picker_w) * 0.5f, host.y + (host.h - picker_h) * 0.5f,
                          picker_w, picker_h};

  // Compact timing dialog: BPM-only or meter-only (separate left/right gutter actions).
  const float kTimingPad = th::px(12.0f);
  const float kTimingLabelCol = th::kLabelW2;
  const float kTimingLabelGap = th::px(10.0f);
  const float kTimingFieldW = th::px(100.0f);
  const float kTimingSlashW = th::px(14.0f);
  const float kTimingSlashGap = th::px(5.0f);
  const float title_h = th::kFontSizeMd + th::px(5.0f);
  const float field_h = th::kControlHeight;
  const float tbtn_h = th::kControlHeight;
  const float timing_w = kTimingPad + kTimingLabelCol + kTimingLabelGap + kTimingFieldW + kTimingPad;
  const float timing_h = th::px(5.0f) + title_h + th::px(8.0f) + field_h + th::px(14.0f) + tbtn_h +
                         kTimingPad;
  timing_popup_bounds_ = {host.x + (host.w - timing_w) * 0.5f, host.y + (host.h - timing_h) * 0.5f,
                          timing_w, timing_h};

  const float pad = th::px(16.0f);
  const float count_y = split_picker_bounds_.y + title_h + th::px(10.0f);
  const float count_h = th::kControlHeight;
  const float count_gap = th::px(6.0f);
  const float count_w =
      (split_picker_bounds_.w - pad * 2.0f - count_gap * 5.0f) / 6.0f;
  split_count_buttons_.clear();
  split_count_buttons_.reserve(6);
  for (int i = 0; i < 6; ++i) {
    split_count_buttons_.push_back({split_picker_bounds_.x + pad + static_cast<float>(i) * (count_w + count_gap),
                                    count_y, count_w, count_h});
  }

  const float search_h = th::kControlHeight;
  const float search_y = count_y + count_h + th::px(8.0f);
  // Hug the painted glyphs and center the hint so leftover column width
  // does not sit between the text and the field.
  const float search_label_w = split_search_hint_width() + 4.0f;
  const float search_label_gap = th::px(6.0f);
  split_search_label_ = {split_picker_bounds_.x + pad, search_y, search_label_w, search_h};
  const float search_field_x = split_search_label_.right() + search_label_gap;
  split_search_field_ = {search_field_x, search_y,
                         std::max(th::px(48.0f), split_picker_bounds_.right() - pad - search_field_x),
                         search_h};

  const float section_label_h = th::kFontSizeSm + th::px(6.0f);
  const float footer_h = th::kControlHeight + th::px(22.0f);
  const float list_y = search_y + search_h + section_label_h + th::px(12.0f);
  const float list_h = split_picker_bounds_.h - (list_y - split_picker_bounds_.y) - footer_h - pad;
  const float scrollbar_w = th::px(10.0f);
  const float scrollbar_gap = th::px(8.0f);
  const float list_w =
      std::max(th::px(48.0f), split_picker_bounds_.w - pad * 2.0f - scrollbar_w - scrollbar_gap);
  split_color_list_ = {split_picker_bounds_.x + pad, list_y, list_w, std::max(0.0f, list_h)};
  split_scrollbar_track_ = {split_picker_bounds_.right() - pad - scrollbar_w, list_y, scrollbar_w,
                            std::max(0.0f, list_h)};
  const float col_gap = th::px(8.0f);
  const float row_gap = th::px(8.0f);
  // Cap matches prior Mac Retina look (84–128 fb @ 2× ≈ 42–64 logical).
  split_color_row_h_ = std::clamp(list_h / 3.2f, th::px(42.0f), th::px(64.0f));
  split_color_buttons_.clear();
  std::vector<int32_t> colors = split_picker_filtered_ids();
  const float cell_w =
      (list_w - col_gap * static_cast<float>(kSplitColorCols - 1)) /
      static_cast<float>(kSplitColorCols);
  const int visible_rows =
      std::max(1, static_cast<int>(std::floor((list_h + row_gap) / split_color_row_h_)));
  const int total_rows =
      std::max(1, (static_cast<int>(colors.size()) + kSplitColorCols - 1) / kSplitColorCols);
  split_color_max_scroll_ =
      std::max(0.0f, static_cast<float>(total_rows - visible_rows) * split_color_row_h_);
  const float scroll = std::clamp(split_color_scroll_, 0.0f, split_color_max_scroll_);
  const int first_row = std::max(0, static_cast<int>(scroll / split_color_row_h_));
  for (int r = 0; r < visible_rows; ++r) {
    for (int c = 0; c < kSplitColorCols; ++c) {
      const int idx = (first_row + r) * kSplitColorCols + c;
      if (idx >= static_cast<int>(colors.size())) break;
      split_color_buttons_.push_back(
          {split_color_list_.x + static_cast<float>(c) * (cell_w + col_gap),
           list_y + static_cast<float>(r) * split_color_row_h_, cell_w,
           std::max(th::px(24.0f), split_color_row_h_ - row_gap)});
    }
  }
  const float thumb_frac =
      static_cast<float>(visible_rows) / static_cast<float>(std::max(total_rows, 1));
  const float min_thumb = std::min(split_scrollbar_track_.h, th::px(22.0f));
  const float thumb_h =
      std::clamp(split_scrollbar_track_.h * thumb_frac, min_thumb, split_scrollbar_track_.h);
  const float thumb_travel = std::max(0.0f, split_scrollbar_track_.h - thumb_h);
  const float thumb_t =
      split_color_max_scroll_ <= 1e-4f ? 0.0f : scroll / split_color_max_scroll_;
  split_scrollbar_thumb_ = {split_scrollbar_track_.x, split_scrollbar_track_.y + thumb_travel * thumb_t,
                            split_scrollbar_track_.w, thumb_h};

  const float btn_w = std::min(th::px(120.0f), (split_picker_bounds_.w - pad * 3.0f) * 0.5f);
  const float btn_h = wds::interaction::theme::kControlHeight;
  const float btn_y = split_picker_bounds_.bottom() - pad - btn_h;
  split_cancel_button_ = {split_picker_bounds_.x + pad, btn_y, btn_w, btn_h};
  split_confirm_button_ = {split_picker_bounds_.right() - pad - btn_w, btn_y, btn_w, btn_h};

  const float fields_x =
      timing_popup_bounds_.x + kTimingPad + kTimingLabelCol + kTimingLabelGap;
  const float field_y = timing_popup_bounds_.y + title_h + th::px(8.0f);
  timing_bpm_field_ = {fields_x, field_y, kTimingFieldW, field_h};
  const float sig_field_w =
      (kTimingFieldW - kTimingSlashW - kTimingSlashGap * 2.0f) * 0.5f;
  timing_num_field_ = {fields_x, field_y, sig_field_w, field_h};
  timing_den_field_ = {timing_num_field_.right() + kTimingSlashGap + kTimingSlashW + kTimingSlashGap,
                       timing_num_field_.y, sig_field_w, field_h};
  const float tbtn_w = th::px(60.0f);
  timing_cancel_button_ = {timing_popup_bounds_.x + kTimingPad,
                           timing_popup_bounds_.bottom() - kTimingPad - tbtn_h, tbtn_w, tbtn_h};
  timing_confirm_button_ = {timing_popup_bounds_.right() - kTimingPad - tbtn_w,
                            timing_popup_bounds_.bottom() - kTimingPad - tbtn_h, tbtn_w, tbtn_h};
}

wds::interaction::Rect ChartEditPanel::overlay_host_bounds() const {
  const wds::interaction::Widget* root = this;
  while (root->parent() != nullptr) root = root->parent();
  return root->absolute_bounds();
}

bool ChartEditPanel::blocks_interaction_behind(wds::interaction::Vec2 point) const {
  return has_modal_popup() && visible() && enabled() && overlay_host_bounds().contains(point);
}

wds::interaction::Widget* ChartEditPanel::hit_test(wds::interaction::Vec2 point) {
  if (!visible() || !enabled()) return nullptr;
  // Modal must own the whole window so toolbar/settings cannot steal clicks.
  if (has_modal_popup() && overlay_host_bounds().contains(point)) {
    return this;
  }
  return wds::interaction::Widget::hit_test(point);
}

void ChartEditPanel::paint_popups(wds::interaction::UiPainter& painter) const {
  using wds::interaction::Color;
  layout_popup_rects();
  const auto host = overlay_host_bounds();
  // Keep modal draw depth at the top of the UI stack.
  constexpr float kZDim = 0.9990f;
  constexpr float kZPanel = 0.9992f;
  constexpr float kZCtrl = 0.9994f;
  constexpr float kZText = 0.9996f;
  if (split_picker_open_) {
    namespace th = wds::interaction::theme;
    painter.fill_rect(host, {0.0f, 0.0f, 0.0f, 0.72f}, 0.0f, kZDim);
    painter.fill_rect(split_picker_bounds_, {0.16f, 0.17f, 0.20f, 1.0f}, 10.0f, kZPanel);

    const float title_h = th::kFontSizeMd + th::px(5.0f);
    const float pad_x = th::px(16.0f);
    painter.label({split_picker_bounds_.x + pad_x, split_picker_bounds_.y + th::px(5.0f),
                   split_picker_bounds_.w - pad_x * 2.0f, title_h},
                  "分割轨道数", {0.96f, 0.96f, 0.98f, 1.0f}, kZText);
    for (size_t i = 0; i < split_count_buttons_.size(); ++i) {
      const bool sel = static_cast<int32_t>(i + 1) == split_picker_count_;
      painter.fill_rect(split_count_buttons_[i],
                        sel ? Color{0.28f, 0.52f, 0.90f, 1.0f} : Color{0.24f, 0.26f, 0.30f, 1.0f},
                        8.0f, kZCtrl);
      painter.label(split_count_buttons_[i], std::to_string(i + 1), {1, 1, 1, 1}, kZText);
    }

    // Match TextField (谱面延迟): darker fill + purple outline when focused.
    // fill_rect_outline bumps outline z by 0.001, which clips past the far plane
    // in this modal band — draw the 1px frame at a safe depth instead.
    const auto field_bg = [&](const wds::interaction::Rect& r, bool focused, bool invalid) {
      const Color fill = focused ? th::kSurface : th::kSurfaceVariant;
      const Color outline = invalid ? th::kError : focused ? th::kPrimary : th::kOutline;
      painter.fill_rect(r, fill, th::kCornerRadiusSm, kZCtrl);
      const float zo = kZCtrl + 0.00005f;
      const float t = 1.0f;
      painter.fill_rect({r.x, r.y, r.w, t}, outline, 0.0f, zo);
      painter.fill_rect({r.x, r.bottom() - t, r.w, t}, outline, 0.0f, zo);
      painter.fill_rect({r.x, r.y, t, r.h}, outline, 0.0f, zo);
      painter.fill_rect({r.right() - t, r.y, t, r.h}, outline, 0.0f, zo);
    };
    painter.label(split_search_label_, "分割线编号", th::kOnSurfaceMuted, kZText);
    const bool search_invalid = !is_split_picker_search_text_valid(split_search_text_);
    field_bg(split_search_field_, split_search_focused_, search_invalid);
    painter.label(split_search_field_, split_search_text_, th::kOnSurface, kZText);
    if (split_search_focused_) {
      const float px = th::kFontSizeMd;
      const auto size = painter.measure_text(split_search_text_, px);
      const float text_x = split_search_field_.x + std::max(0.0f, (split_search_field_.w - size.x) * 0.5f);
      wds::interaction::caret::paint(painter, split_search_field_, text_x + size.x, kZText,
                                     split_search_caret_blink_t_);
    }

    const float section_h = th::kFontSizeSm + th::px(4.0f);
    const float appearance_y = split_search_field_.bottom() + th::px(6.0f);
    painter.label({split_picker_bounds_.x + pad_x, appearance_y, split_picker_bounds_.w - pad_x * 2.0f,
                   section_h},
                  "分割线外观", {0.90f, 0.90f, 0.93f, 1.0f}, kZText);

    std::vector<int32_t> colors = split_picker_filtered_ids();
    const float scroll = std::clamp(split_color_scroll_, 0.0f, split_color_max_scroll_);
    const int first_row = std::max(0, static_cast<int>(scroll / split_color_row_h_));
    for (size_t i = 0; i < split_color_buttons_.size(); ++i) {
      const int idx = first_row * kSplitColorCols + static_cast<int>(i);
      if (idx < 0 || idx >= static_cast<int>(colors.size())) continue;
      const int32_t color_id = colors[static_cast<size_t>(idx)];
      const bool sel = color_id == split_picker_color_id_;
      const auto& cell = split_color_buttons_[i];
      painter.fill_rect(cell,
                        sel ? Color{0.26f, 0.38f, 0.60f, 1.0f} : Color{0.20f, 0.21f, 0.25f, 1.0f},
                        8.0f, kZCtrl);
      const float id_h = th::kFontSizeSm;
      // Narrower thumbnail: keep side margins so 3-up cells stay readable.
      const float preview_w = cell.w * 0.70f;
      const float preview_pad = th::px(3.0f);
      const float preview_h =
          std::max(th::px(11.0f), cell.h - id_h - preview_pad * 2.0f - th::px(2.0f));
      const wds::interaction::Rect preview{cell.x + (cell.w - preview_w) * 0.5f,
                                           cell.y + preview_pad, preview_w, preview_h};
      paint_split_lane_preview(painter, preview, split_picker_count_, color_id, skin_);
      painter.label({cell.x + th::px(3.0f), cell.bottom() - id_h - th::px(1.5f),
                     cell.w - th::px(6.0f), id_h},
                    std::to_string(color_id), {0.96f, 0.97f, 0.99f, 1.0f}, kZText);
    }
    if (split_scrollbar_track_.h > 0.5f) {
      painter.fill_rect(split_scrollbar_track_, {0.12f, 0.13f, 0.16f, 1.0f}, 4.0f, kZCtrl);
      painter.fill_rect(split_scrollbar_thumb_, {0.42f, 0.44f, 0.50f, 1.0f}, 4.0f, kZText);
    }
  }
  if (timing_popup_open_) {
    painter.fill_rect(host, {0.0f, 0.0f, 0.0f, 0.72f}, 0.0f, kZDim);
    painter.fill_rect(timing_popup_bounds_, {0.16f, 0.17f, 0.20f, 1.0f}, 10.0f, kZPanel);
    const float title_h = wds::interaction::theme::kFontSizeMd + wds::interaction::theme::px(5.0f);
    const float kTimingPad = wds::interaction::theme::px(12.0f);
    const float kTimingLabelCol = wds::interaction::theme::kLabelW2;
    const bool bpm_mode = timing_popup_mode_ == TimingPopupMode::Bpm;
    painter.label({timing_popup_bounds_.x + kTimingPad,
                   timing_popup_bounds_.y + wds::interaction::theme::px(5.0f),
                   timing_popup_bounds_.w - kTimingPad * 2.0f, title_h},
                  bpm_mode ? "BPM 编辑" : "拍号编辑", {0.96f, 0.96f, 0.98f, 1.0f}, kZText);
    painter.label({timing_popup_bounds_.x + kTimingPad,
                   bpm_mode ? timing_bpm_field_.y : timing_num_field_.y, kTimingLabelCol,
                   bpm_mode ? timing_bpm_field_.h : timing_num_field_.h},
                  bpm_mode ? "BPM" : "拍号", {0.90f, 0.90f, 0.93f, 1.0f}, kZText);
    const auto field_bg = [&](const wds::interaction::Rect& r, int field) {
      const bool focused = timing_focus_field_ == field;
      const Color fill =
          focused ? Color{0.32f, 0.36f, 0.44f, 1.0f} : Color{0.22f, 0.23f, 0.27f, 1.0f};
      const Color outline = focused ? wds::interaction::theme::kPrimary : Color{0.38f, 0.38f, 0.42f, 1.0f};
      painter.fill_rect_outline(r, fill, outline, 6.0f, kZCtrl);
    };
    if (bpm_mode) {
      field_bg(timing_bpm_field_, 0);
      painter.label(timing_bpm_field_, timing_bpm_text_, {1, 1, 1, 1}, kZText);
    } else {
      field_bg(timing_num_field_, 1);
      field_bg(timing_den_field_, 2);
      painter.label(timing_num_field_, timing_num_text_, {1, 1, 1, 1}, kZText);
      painter.label({timing_num_field_.right(), timing_num_field_.y,
                     timing_den_field_.x - timing_num_field_.right(), timing_num_field_.h},
                    "/", {0.90f, 0.90f, 0.93f, 1.0f}, kZText);
      painter.label(timing_den_field_, timing_den_text_, {1, 1, 1, 1}, kZText);
    }

    const auto paint_caret = [&](const wds::interaction::Rect& r, const std::string& text) {
      const float px = wds::interaction::theme::kFontSizeMd;
      const auto size = painter.measure_text(text, px);
      const float text_x = r.x + std::max(0.0f, (r.w - size.x) * 0.5f);
      // Keep z < ortho far (1.0); kZText+epsilon was clipped away.
      wds::interaction::caret::paint(painter, r, text_x + size.x, kZText, timing_caret_blink_t_);
    };
    if (bpm_mode) paint_caret(timing_bpm_field_, timing_bpm_text_);
    else if (timing_focus_field_ == 1) paint_caret(timing_num_field_, timing_num_text_);
    else paint_caret(timing_den_field_, timing_den_text_);
  }
}

void ChartEditPanel::paint_popup_chrome(wds::interaction::UiPainter& painter) const {
  using wds::interaction::Color;
  layout_popup_rects();
  constexpr float kZCtrl = 0.9994f;
  constexpr float kZText = 0.9996f;
  if (split_picker_open_) {
    // Opaque strip so list sprites cannot bleed over the action buttons.
    const float bar_top = std::min(split_cancel_button_.y, split_confirm_button_.y) - 12.0f;
    painter.fill_rect({split_picker_bounds_.x + 8.0f, bar_top,
                       split_picker_bounds_.w - 16.0f, split_picker_bounds_.bottom() - bar_top - 8.0f},
                      {0.16f, 0.17f, 0.20f, 1.0f}, 0.0f, kZCtrl);
    painter.fill_rect(split_cancel_button_, {0.32f, 0.34f, 0.38f, 1.0f}, 8.0f, kZCtrl);
    painter.label(split_cancel_button_, "取消", {1, 1, 1, 1}, kZText);
    painter.fill_rect(split_confirm_button_, {0.20f, 0.58f, 0.36f, 1.0f}, 8.0f, kZCtrl);
    painter.label(split_confirm_button_, "确认", {1, 1, 1, 1}, kZText);
  }
  if (timing_popup_open_) {
    painter.fill_rect(timing_cancel_button_, {0.32f, 0.34f, 0.38f, 1.0f}, 8.0f, kZCtrl);
    painter.label(timing_cancel_button_, "取消", {1, 1, 1, 1}, kZText);
    painter.fill_rect(timing_confirm_button_, {0.20f, 0.58f, 0.36f, 1.0f}, 8.0f, kZCtrl);
    painter.label(timing_confirm_button_, "确认", {1, 1, 1, 1}, kZText);
  }
}

void ChartEditPanel::open_split_picker(int32_t tick) {
  if (wds::chart_editor::tick_to_milliseconds(tick, engine_.document().timing()) < 0) return;
  split_picker_open_ = true;
  split_picker_edit_id_ = -1;
  split_picker_tick_ = tick;
  split_search_text_.clear();
  split_search_focused_ = false;
  split_search_caret_blink_t_ = 0.0f;
  split_scrollbar_dragging_ = false;
  split_color_scroll_ = 0.0f;
  if (split_picker_memory_valid_) {
    split_picker_count_ = std::clamp(split_picker_memory_count_, 1, 6);
    split_picker_color_id_ = split_picker_memory_color_id_;
    scroll_split_picker_to_color(split_picker_color_id_);
  } else {
    split_picker_count_ = 2;
    split_picker_color_id_ = 1;
  }
  close_timing_popup();
  hide_gutter_ghost();
  if (auto* root = find_root()) {
    root->close_exclusive_popup_outside(this);
  }
}

std::vector<int32_t> ChartEditPanel::split_picker_filtered_ids() const {
  return filter_split_picker_color_ids(split_search_text_);
}

void ChartEditPanel::remember_split_picker_count() {
  split_picker_memory_valid_ = true;
  split_picker_memory_count_ = split_picker_count_;
}

void ChartEditPanel::remember_split_picker_color() {
  split_picker_memory_valid_ = true;
  split_picker_memory_color_id_ = split_picker_color_id_;
}

void ChartEditPanel::apply_split_search_text(const std::string& text) {
  if (text == split_search_text_) return;
  if (!is_split_picker_search_text_valid(text)) return;
  split_search_text_ = text;
  // Keep the current selection in view when it still matches; otherwise jump to top.
  scroll_split_picker_to_color(split_picker_color_id_);
}

void ChartEditPanel::sync_split_scrollbar_from_pointer(float y) {
  layout_popup_rects();
  if (split_color_max_scroll_ <= 1e-4f || split_scrollbar_track_.h <= 1e-3f) return;
  const float thumb_h = split_scrollbar_thumb_.h;
  const float usable = std::max(1e-3f, split_scrollbar_track_.h - thumb_h);
  const float t = std::clamp((y - split_scrollbar_grab_offset_ - split_scrollbar_track_.y) / usable,
                             0.0f, 1.0f);
  split_color_scroll_ = t * split_color_max_scroll_;
}

void ChartEditPanel::scroll_split_picker_to_color(int32_t color_id) {
  layout_popup_rects();
  std::vector<int32_t> colors = split_picker_filtered_ids();
  int idx = -1;
  for (size_t i = 0; i < colors.size(); ++i) {
    if (colors[i] == color_id) {
      idx = static_cast<int>(i);
      break;
    }
  }
  if (idx < 0) {
    split_color_scroll_ = 0.0f;
    return;
  }
  const int row = idx / kSplitColorCols;
  // Keep the remembered / edited effect near the top of the visible list.
  split_color_scroll_ =
      std::clamp(static_cast<float>(row) * split_color_row_h_, 0.0f, split_color_max_scroll_);
}

void ChartEditPanel::open_split_picker_for_edit(int32_t note_id) {
  auto note = engine_.document().find_note(note_id);
  if (!note || !wds::chart_editor::is_split_lane_gimmick(note->gimmick_type)) return;
  split_picker_open_ = true;
  split_picker_edit_id_ = note_id;
  split_picker_tick_ = note->start_tick;
  split_search_text_.clear();
  split_search_focused_ = false;
  split_search_caret_blink_t_ = 0.0f;
  split_scrollbar_dragging_ = false;
  split_picker_count_ = std::clamp(wds::chart_editor::get_split_count(note->gimmick_type), 1, 6);
  split_picker_color_id_ = note->scratch_length;
  scroll_split_picker_to_color(split_picker_color_id_);
  close_timing_popup();
  hide_gutter_ghost();
  if (auto* root = find_root()) {
    root->close_exclusive_popup_outside(this);
  }
}

void ChartEditPanel::close_split_picker() {
  split_picker_open_ = false;
  split_picker_edit_id_ = -1;
  split_search_focused_ = false;
  split_scrollbar_dragging_ = false;
}

bool ChartEditPanel::take_split_modal(SplitModalDraft& out) {
  if (!split_picker_open_) return false;
  out.tick = split_picker_tick_;
  out.edit_id = split_picker_edit_id_;
  out.count = split_picker_count_;
  out.color_id = split_picker_color_id_;
  close_split_picker();
  return true;
}

bool ChartEditPanel::take_timing_modal(TimingModalDraft& out) {
  if (!timing_popup_open_) return false;
  out.bpm_mode = timing_popup_mode_ == TimingPopupMode::Bpm;
  out.tick = timing_edit_tick_;
  const auto& timing = engine_.document().timing();
  if (out.bpm_mode) {
    try {
      out.bpm = std::stod(timing_bpm_text_);
    } catch (...) {
      out.bpm = wds::chart_editor::timing_point_at(timing, timing_edit_tick_).bpm;
    }
  } else {
    try {
      out.numerator = std::stoi(timing_num_text_);
      out.denominator = std::stoi(timing_den_text_);
    } catch (...) {
      const auto& p = wds::chart_editor::timing_meter_at(timing, timing_edit_tick_);
      out.numerator = p.numerator;
      out.denominator = p.denominator;
    }
  }
  close_timing_popup();
  return true;
}

bool ChartEditPanel::add_split_effect(int32_t tick, int32_t count, int32_t color_id) {
  if (!engine_.is_editable()) return false;
  if (wds::chart_editor::tick_to_milliseconds(tick, engine_.document().timing()) < 0) return false;
  const int32_t tpq = std::max(1, engine_.document().timing().ticks_per_quarter);
  NotationNote note;
  note.note_type = NoteType::None;
  note.gimmick_type = wds::chart_editor::split_gimmick_for_count(std::clamp(count, 1, 6));
  note.scratch_length = color_id;
  note.start_tick = tick;
  note.end_tick = note.start_tick + tpq;
  note.lane = 0;
  note.width = 12;
  if (!commit_notes({note}, "Add split")) return false;
  split_picker_count_ = std::clamp(count, 1, 6);
  split_picker_color_id_ = color_id;
  remember_split_picker_count();
  remember_split_picker_color();
  return true;
}

bool ChartEditPanel::edit_split_effect(int32_t note_id, int32_t count, int32_t color_id) {
  if (!engine_.is_editable()) return false;
  auto prev = engine_.document().find_note(note_id);
  if (!prev || !wds::chart_editor::is_split_lane_gimmick(prev->gimmick_type)) return false;
  NotationNote updated = *prev;
  updated.gimmick_type = wds::chart_editor::split_gimmick_for_count(std::clamp(count, 1, 6));
  updated.scratch_length = color_id;
  if (updated.gimmick_type != prev->gimmick_type ||
      updated.scratch_length != prev->scratch_length) {
    std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair> changes;
    changes[note_id] = {*prev, updated};
    if (!commit_updates(changes, "Edit split")) return false;
  }
  split_picker_count_ = std::clamp(count, 1, 6);
  split_picker_color_id_ = color_id;
  remember_split_picker_count();
  remember_split_picker_color();
  return true;
}

bool ChartEditPanel::apply_bpm(int32_t tick, double bpm) {
  if (!engine_.is_editable() || !std::isfinite(bpm) || !(bpm > 0.0)) return false;
  const auto before = engine_.document().timing();
  auto timing = before;
  wds::chart_editor::TimingPoint* existing = nullptr;
  for (auto& p : timing.points) {
    if (p.tick == tick) {
      existing = &p;
      break;
    }
  }
  if (existing) {
    existing->bpm = bpm;
    existing->has_bpm = true;
  } else {
    wds::chart_editor::TimingPoint point;
    point.tick = tick;
    point.bpm = bpm;
    point.has_bpm = true;
    point.has_meter = false;
    timing.points.push_back(point);
  }
  return engine_.execute_command(std::make_unique<wds::chart_editor::SetTimingCommand>(
      before, std::move(timing), existing ? "Edit BPM" : "Add BPM"));
}

bool ChartEditPanel::apply_meter(int32_t tick, int32_t numerator, int32_t denominator) {
  if (!engine_.is_editable() || numerator < 1 || denominator < 1) return false;
  const auto before = engine_.document().timing();
  auto timing = before;
  wds::chart_editor::TimingPoint* existing = nullptr;
  for (auto& p : timing.points) {
    if (p.tick == tick) {
      existing = &p;
      break;
    }
  }
  if (existing) {
    existing->numerator = numerator;
    existing->denominator = denominator;
    existing->has_meter = true;
  } else {
    wds::chart_editor::TimingPoint point;
    point.tick = tick;
    point.numerator = numerator;
    point.denominator = denominator;
    point.has_bpm = false;
    point.has_meter = true;
    timing.points.push_back(point);
  }
  wds::chart_editor::normalize_timing_points(timing);
  wds::chart_editor::prune_orphaned_meter_changes(timing, tick);
  return engine_.execute_command(std::make_unique<wds::chart_editor::SetTimingCommand>(
      before, std::move(timing), existing ? "Edit meter" : "Add meter"));
}

void ChartEditPanel::confirm_split_picker() {
  if (split_picker_edit_id_ >= 0) {
    (void)edit_split_effect(split_picker_edit_id_, split_picker_count_, split_picker_color_id_);
  } else {
    (void)add_split_effect(split_picker_tick_, split_picker_count_, split_picker_color_id_);
  }
  close_split_picker();
}

void ChartEditPanel::open_bpm_popup(int32_t tick) {
  timing_popup_open_ = true;
  timing_popup_mode_ = TimingPopupMode::Bpm;
  timing_edit_tick_ = tick;
  timing_focus_field_ = 0;
  timing_caret_blink_t_ = 0.0f;
  const auto& timing = engine_.document().timing();
  const auto& p = wds::chart_editor::timing_point_at(timing, tick);
  timing_bpm_text_ = format_bpm_label(p.bpm);
  timing_bpm_committed_ = timing_bpm_text_;
  close_split_picker();
  hide_gutter_ghost();
  if (auto* root = find_root()) {
    root->close_exclusive_popup_outside(this);
  }
}

void ChartEditPanel::open_meter_popup(int32_t tick) {
  timing_popup_open_ = true;
  timing_popup_mode_ = TimingPopupMode::Meter;
  timing_edit_tick_ = tick;
  timing_focus_field_ = 1;
  timing_caret_blink_t_ = 0.0f;
  const auto& timing = engine_.document().timing();
  const auto& p = wds::chart_editor::timing_meter_at(timing, tick);
  timing_num_text_ = std::to_string(p.numerator);
  timing_den_text_ = std::to_string(p.denominator);
  timing_num_committed_ = timing_num_text_;
  timing_den_committed_ = timing_den_text_;
  close_split_picker();
  hide_gutter_ghost();
  if (auto* root = find_root()) {
    root->close_exclusive_popup_outside(this);
  }
}

void ChartEditPanel::close_timing_popup() { timing_popup_open_ = false; }

void ChartEditPanel::commit_timing_popup() {
  auto parse_positive = [](const std::string& text) -> std::optional<double> {
    if (text.empty()) return std::nullopt;
    try {
      std::size_t n = 0;
      const double v = std::stod(text, &n);
      if (n != text.size() || !std::isfinite(v) || !(v > 0.0)) return std::nullopt;
      return v;
    } catch (...) {
      return std::nullopt;
    }
  };
  auto parse_positive_int = [](const std::string& text) -> std::optional<int> {
    if (text.empty()) return std::nullopt;
    try {
      std::size_t n = 0;
      const int v = std::stoi(text, &n);
      if (n != text.size() || v < 1) return std::nullopt;
      return v;
    } catch (...) {
      return std::nullopt;
    }
  };

  if (timing_popup_mode_ == TimingPopupMode::Bpm) {
    const auto bpm = parse_positive(timing_bpm_text_);
    if (!bpm) {
      timing_bpm_text_ = timing_bpm_committed_;
      return;
    }
    (void)apply_bpm(timing_edit_tick_, *bpm);
  } else {
    const auto num = parse_positive_int(timing_num_text_);
    const auto den = parse_positive_int(timing_den_text_);
    if (!num || !den) {
      timing_num_text_ = timing_num_committed_;
      timing_den_text_ = timing_den_committed_;
      return;
    }
    (void)apply_meter(timing_edit_tick_, *num, *den);
  }
  close_timing_popup();
}

bool ChartEditPanel::delete_timing_label(int32_t tick, TimingPopupMode kind) {
  if (!engine_.is_editable() || tick == 0) return false;
  const auto before = engine_.document().timing();
  auto timing = before;
  auto it = std::find_if(timing.points.begin(), timing.points.end(),
                         [&](const wds::chart_editor::TimingPoint& p) { return p.tick == tick; });
  if (it == timing.points.end()) return false;
  if (kind == TimingPopupMode::Bpm) {
    if (!it->has_bpm) return false;
    it->has_bpm = false;
  } else {
    if (!it->has_meter) return false;
    it->has_meter = false;
  }
  wds::chart_editor::normalize_timing_points(timing);
  if (kind == TimingPopupMode::Meter) {
    wds::chart_editor::prune_orphaned_meter_changes(timing, tick);
  }
  const char* label = kind == TimingPopupMode::Bpm ? "Delete BPM" : "Delete meter";
  return engine_.execute_command(
      std::make_unique<wds::chart_editor::SetTimingCommand>(before, std::move(timing), label));
}

bool ChartEditPanel::delete_split_note(int32_t note_id) {
  if (!engine_.is_editable()) return false;
  auto note = engine_.document().find_note(note_id);
  if (!note || !wds::chart_editor::is_split_lane_gimmick(note->gimmick_type)) return false;
  return engine_.execute_command(
      std::make_unique<wds::chart_editor::RemoveNotesCommand>(std::vector<NotationNote>{*note},
                                                              "Delete split"));
}

bool ChartEditPanel::handle_popup_pointer_down(const wds::interaction::PointerDownEvent& event) {
  layout_popup_rects();
  if (split_picker_open_) {
    if (split_cancel_button_.contains(event.position)) {
      close_split_picker();
      return true;
    }
    if (split_confirm_button_.contains(event.position)) {
      confirm_split_picker();
      return true;
    }
    for (size_t i = 0; i < split_count_buttons_.size(); ++i) {
      if (split_count_buttons_[i].contains(event.position)) {
        split_picker_count_ = static_cast<int32_t>(i + 1);
        remember_split_picker_count();
        split_search_focused_ = false;
        return true;
      }
    }
    if (split_search_field_.contains(event.position)) {
      split_search_focused_ = true;
      split_search_caret_blink_t_ = 0.0f;
      split_scrollbar_dragging_ = false;
      return true;
    }
    if (split_scrollbar_track_.contains(event.position) && split_color_max_scroll_ > 1e-4f) {
      split_search_focused_ = false;
      if (split_scrollbar_thumb_.contains(event.position)) {
        split_scrollbar_dragging_ = true;
        split_scrollbar_grab_offset_ = event.position.y - split_scrollbar_thumb_.y;
      } else {
        split_scrollbar_dragging_ = true;
        split_scrollbar_grab_offset_ = split_scrollbar_thumb_.h * 0.5f;
        sync_split_scrollbar_from_pointer(event.position.y);
      }
      return true;
    }
    std::vector<int32_t> colors = split_picker_filtered_ids();
    for (size_t i = 0; i < split_color_buttons_.size(); ++i) {
      if (!split_color_buttons_[i].contains(event.position)) continue;
      const float scroll = std::clamp(split_color_scroll_, 0.0f, split_color_max_scroll_);
      const int first_row = std::max(0, static_cast<int>(scroll / split_color_row_h_));
      const int idx = first_row * kSplitColorCols + static_cast<int>(i);
      if (idx >= 0 && idx < static_cast<int>(colors.size())) {
        split_picker_color_id_ = colors[static_cast<size_t>(idx)];
        remember_split_picker_color();
      }
      split_search_focused_ = false;
      return true;
    }
    split_search_focused_ = false;
    if (!split_picker_bounds_.contains(event.position)) close_split_picker();
    return true;
  }
  if (timing_popup_open_) {
    if (timing_cancel_button_.contains(event.position)) {
      close_timing_popup();
      return true;
    }
    if (timing_confirm_button_.contains(event.position)) {
      commit_timing_popup();
      return true;
    }
    if (timing_popup_mode_ == TimingPopupMode::Bpm) {
      if (timing_bpm_field_.contains(event.position)) {
        timing_focus_field_ = 0;
        timing_caret_blink_t_ = 0.0f;
        return true;
      }
    } else {
      if (timing_num_field_.contains(event.position)) {
        timing_focus_field_ = 1;
        timing_caret_blink_t_ = 0.0f;
        return true;
      }
      if (timing_den_field_.contains(event.position)) {
        timing_focus_field_ = 2;
        timing_caret_blink_t_ = 0.0f;
        return true;
      }
    }
    if (!timing_popup_bounds_.contains(event.position)) close_timing_popup();
    return true;
  }
  return false;
}

bool ChartEditPanel::handle_left_gutter_pointer_down(const wds::interaction::PointerDownEvent& event) {
  if (!left_gutter_.contains(event.position)) return false;
  sync_viewport();
  const auto hits = build_split_label_hits(viewport_, left_gutter_, engine_.document().notes());
  for (const auto& hit : hits) {
    if (!split_label_hot_bounds(hit).contains(event.position)) continue;
    if (event.button == wds::interaction::PointerButton::Middle) {
      delete_split_note(hit.note_id);
      return true;
    }
    if (wds::interaction::is_left_button(event.button)) {
      // Press starts a live drag immediately (wheel-without-move still tracks).
      // Pointer-up opens the picker only if the label never left its original ticks.
      if (!engine_.is_editable()) return true;
      if (auto note = engine_.document().find_note(hit.note_id)) {
        drag_originals_.clear();
        drag_originals_[hit.note_id] = *note;
      }
      drag_split_note_id_ = hit.note_id;
      drag_split_is_end_ = !hit.is_start;
      drag_split_press_tick_ = viewport_.tick_at(pointer_as_in_host(event.position).y);
      split_edge_ever_moved_ = false;
      mode_ = Mode::DragSplitEdge;
      hide_gutter_ghost();
      return true;
    }
    return true;
  }
  if (event.button == wds::interaction::PointerButton::Middle) return true;
  if (wds::interaction::is_left_button(event.button) && engine_.is_editable()) {
    const int32_t tick = viewport_.tick_at(event.position.y);
    if (wds::chart_editor::tick_to_milliseconds(tick, engine_.document().timing()) >= 0) {
      open_split_picker(tick);
    }
    return true;
  }
  return true;
}

bool ChartEditPanel::handle_right_gutter_pointer_down(const wds::interaction::PointerDownEvent& event) {
  // Measure-index column is display-only; absorb clicks so they do not hit the playfield.
  if (measure_gutter_.contains(event.position)) return true;
  if (!right_gutter_.contains(event.position)) return false;
  // Official charts have no BPM/meter authoring — ignore timing-gutter clicks.
  if (!engine_.is_editable()) return true;
  sync_viewport();
  const auto& timing = engine_.document().timing();
  const auto hits = build_timing_label_hits(viewport_, right_gutter_, timing);
  for (const auto& hit : hits) {
    if (!hit.bounds.contains(event.position)) continue;
    const auto kind =
        hit.kind == TimingLabelKind::Bpm ? TimingPopupMode::Bpm : TimingPopupMode::Meter;
    if (event.button == wds::interaction::PointerButton::Middle) {
      delete_timing_label(hit.point_tick, kind);
      return true;
    }
    if (kind == TimingPopupMode::Bpm && wds::interaction::is_left_button(event.button)) {
      open_bpm_popup(hit.point_tick);
      return true;
    }
    // Meter: right-click (authoring) or left-click on an existing label to edit.
    if (kind == TimingPopupMode::Meter && (wds::interaction::is_right_button(event.button) ||
                                          wds::interaction::is_left_button(event.button))) {
      open_meter_popup(hit.point_tick);
      return true;
    }
    return true;
  }
  if (event.button == wds::interaction::PointerButton::Middle) return true;
  if (wds::interaction::is_left_button(event.button)) {
    if (auto bpm_tick = timing_bpm_tick_at(viewport_, right_gutter_, timing, event.position)) {
      open_bpm_popup(*bpm_tick);
    }
    return true;
  }
  if (wds::interaction::is_right_button(event.button)) {
    if (auto measure = timing_measure_tick_at(viewport_, right_gutter_, timing, event.position)) {
      open_meter_popup(*measure);
    }
    return true;
  }
  return true;
}

void ChartEditPanel::paint_side_columns(wds::interaction::UiPainter& painter) const {
  sync_viewport();
  sync_error_ticks();
  paint_gutters(painter);
  paint_gutter_overlays(painter);
}

void ChartEditPanel::paint_overlays(wds::interaction::UiPainter& painter) const {
  sync_viewport();
  renderer_.paint_overlays(painter, viewport_, engine_.document().notes(), selected_,
                           active_marquee_rect());
  paint_gutter_overlays(painter);
}

std::optional<wds::interaction::Rect> ChartEditPanel::active_marquee_rect() const {
  if (mode_ != Mode::Marquee) return std::nullopt;
  return marquee_screen_rect(pointer_);
}

void ChartEditPanel::paint_gutter_overlays(wds::interaction::UiPainter& painter) const {
  sync_viewport();
  // Compact split chips above skinned notes. Color ID only on hover/drag.
  // Hits are time-ascending; paint reverse so earlier labels stay on top.
  namespace th = wds::interaction::theme;
  const auto hits = build_split_label_hits(viewport_, left_gutter_, engine_.document().notes());
  const int32_t highlight_id = active_split_highlight_id();
  const bool highlight_is_end = active_split_highlight_is_end();
  const GutterLabelHit* active_hit = nullptr;
  auto paint_leader = [&](const wds::interaction::Rect& bounds, float anchor_y, bool is_start,
                          int column, const wds::interaction::Color& c, float z) {
    const float natural_y =
        is_start ? anchor_y - bounds.h - th::px(1.0f) : anchor_y + th::px(1.0f);
    if (std::abs(bounds.y - natural_y) <= 0.5f) return;
    const float line_w = th::px(2.0f);
    const float inner_x = column == 0 ? bounds.right() : bounds.x;
    const float leader_x = column == 0 ? bounds.right() + line_w * 0.5f : bounds.x - line_w * 0.5f;
    const float connect_y = is_start ? bounds.bottom() - 0.5f : bounds.y + 0.5f;
    const float hx = std::min(inner_x, leader_x);
    const float hw = std::max(line_w, std::abs(leader_x - inner_x));
    painter.fill_rect({hx, connect_y - line_w * 0.5f, hw, line_w}, c, 0.0f, z);
    const float top = std::min(connect_y, anchor_y);
    const float line_h = std::abs(anchor_y - connect_y);
    if (line_h > 0.5f) {
      painter.fill_rect({leader_x - line_w * 0.5f, top, line_w, line_h}, c, 0.0f, z);
    }
  };
  for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
    const auto& hit = *it;
    auto note = engine_.document().find_note(hit.note_id);
    if (!note) continue;
    const bool active = hit.note_id == highlight_id && hit.is_start == !highlight_is_end;
    if (active) active_hit = &hit;
    const wds::interaction::Color c =
        hit.is_start ? wds::interaction::Color{0.22f, 0.48f, 0.95f, 1.0f}
                     : wds::interaction::Color{0.92f, 0.28f, 0.28f, 1.0f};
    paint_leader(hit.bounds, hit.anchor_y, hit.is_start, hit.column, c, 0.977f);
    if (active) {
      painter.fill_rect(hit.bounds.inset(-th::px(1.0f), -th::px(1.0f)),
                        {1.0f, 1.0f, 1.0f, 0.92f}, th::kCornerRadiusSm, 0.978f);
    }
    painter.fill_rect(hit.bounds, c, th::kCornerRadiusSm, 0.979f);
    painter.fill_rect(hit.bounds.inset(th::px(0.5f), th::px(0.5f)), {0.08f, 0.08f, 0.10f, 0.40f},
                      th::px(1.0f), 0.98f);
  }
  if (active_hit) {
    if (auto note = engine_.document().find_note(active_hit->note_id)) {
      const std::string id_text = std::to_string(note->scratch_length);
      const float card_px = th::kFontSizeGutter;
      const float card_h = th::px(26.0f);
      const float card_w = std::max(th::px(56.0f), card_px * 0.62f * static_cast<float>(id_text.size()) +
                                                       th::px(10.0f));
      float card_y = active_hit->bounds.y + (active_hit->bounds.h - card_h) * 0.5f;
      card_y = std::clamp(card_y, playfield_.y, std::max(playfield_.y, playfield_.bottom() - card_h));
      const wds::interaction::Rect card{playfield_.x + th::px(2.0f), card_y, card_w, card_h};
      painter.fill_rect(card, {0.10f, 0.11f, 0.14f, 0.94f}, th::kCornerRadiusMd, 0.982f);
      painter.fill_rect(card.inset(th::px(0.5f), th::px(0.5f)), {0.22f, 0.24f, 0.28f, 0.9f},
                        th::kCornerRadiusSm, 0.983f);
      painter.label(card, id_text, {1.0f, 1.0f, 1.0f, 1.0f}, 0.984f, false, card_px);
    }
  }
  for (const auto& ghost : gutter_ghosts_) {
    if (ghost.draw_leader) {
      paint_leader(ghost.bounds, ghost.anchor_y, ghost.is_start, ghost.column, ghost.color, 0.976f);
    }
    painter.fill_rect(ghost.bounds, ghost.color, th::kCornerRadiusSm, 0.977f);
  }
}

std::vector<EditGhost> ChartEditPanel::skinned_ghosts() const {
  std::vector<EditGhost> ghosts;
  const float alpha = mode_ == Mode::PlaceHoldBody ? 0.5f : 0.45f;
  if (curve_mode_active_) {
    for (const auto& s : curve_ghosts_) {
      if (s.visible) ghosts.push_back({s.note, true, alpha});
    }
    return ghosts;
  }
  if (ghost_.visible &&
      !wds::interaction::suppress_idle_placement_ghost(active_mods_, is_note_drawing())) {
    // PlaceHoldBody may degrade zero-length holds to Tap/Flick in ghost_.note.
    ghosts.push_back({ghost_.note, true, alpha});
    for (const auto& s : hold_stars_) {
      if (s.visible) ghosts.push_back({s.note, true, 0.4f});
    }
    // Preview auto head for first-chain / disconnected-preview drafts (same rules as
    // finish_hold_body). Linked chain continuations stay headless.
    const bool preview_auto_head = hold_chain_prev_id_ < 0 || !hold_chain_link_preview_;
    if (wds::chart_editor::is_hold_with_tail(ghost_.note.note_type) &&
        ghost_.note.end_tick > ghost_.note.start_tick && preview_auto_head) {
      if (auto head = wds::chart_editor::make_auto_hold_head(engine_.document(), ghost_.note)) {
        ghosts.push_back({*head, true, alpha});
      }
    }
  }
  return ghosts;
}

void ChartEditPanel::append_skin_batch(wds::renderer::DrawBatch& batch,
                                       const wds::renderer::SkinCatalog& skin, int fb_w, int fb_h,
                                       wds::renderer::ScreenBounds screen,
                                       float stage_opacity) const {
  sync_viewport();
  renderer_.append_skinned_backdrop(batch, skin, viewport_, fb_w, fb_h, screen, stage_opacity);
  renderer_.append_skinned_notes(batch, skin, viewport_, engine_.document().notes(), selected_,
                                 fb_w, fb_h, screen, &offset_violation_ids_,
                                 offset_violation_strength());
  renderer_.append_skinned_ghosts(batch, skin, viewport_, std::nullopt, skinned_ghosts(), fb_w,
                                 fb_h, screen);
}

void ChartEditPanel::update(float delta_seconds) {
  wds::interaction::Widget::update(delta_seconds);
  sync_error_ticks();
  if (!offset_violation_ids_.empty()) {
    offset_violation_elapsed_ += delta_seconds;
    if (offset_violation_strength() <= 0.0f) {
      offset_violation_ids_.clear();
      offset_violation_elapsed_ = 0.0f;
    }
  }
  if (timing_popup_open_) {
    timing_caret_blink_t_ += delta_seconds;
  }
  if (split_picker_open_ && split_search_focused_) {
    split_search_caret_blink_t_ += delta_seconds;
  }
  // Prefer global pointer: after leaving the pane, local pointer_ freezes on the
  // last in-bounds sample and must not keep Idle ghosts alive.
  if (!is_note_drawing() &&
      (!pointer_over_edit_ || !pointer_in_edit_ghost_zone(global_pointer_))) {
    hide_placement_ghost();
    pointer_over_edit_ = false;
    set_hover_cursor(wds::interaction::CursorKind::Default);
  }
}

void ChartEditPanel::on_pointer_down(const wds::interaction::PointerDownEvent& event) {
  // Modal captures the whole host; do not require the edit-pane bounds.
  if (has_modal_popup()) {
    pointer_ = event.position;
    global_pointer_ = event.position;
    active_button_ = event.button;
    active_mods_ = event.mods;
    handle_popup_pointer_down(event);
    return;
  }
  if (!absolute_bounds().contains(event.position)) return;
  sync_viewport();
  pointer_ = event.position;
  global_pointer_ = event.position;
  pointer_over_edit_ = true;
  active_button_ = event.button;
  active_mods_ = event.mods;
  drag_start_pos_ = event.position;
  gesture_.on_pointer_down(event);

  if (handle_left_gutter_pointer_down(event)) return;
  if (handle_right_gutter_pointer_down(event)) return;
  if (!playfield_.contains(event.position)) return;

  // Middle interrupts in-progress placement before delete-on-note handling.
  if ((mode_ == Mode::PlaceHoldBody || mode_ == Mode::PlaceGesture) &&
      wds::interaction::is_cancel_placement(event)) {
    cancel_placement();
    return;
  }

  if (mode_ == Mode::PlaceHoldBody && curve_mode_active_ &&
      wds::interaction::is_curve_fill_confirm(event, hold_scratch_)) {
    commit_curve_fill();
    return;
  }

  if (mode_ == Mode::PlaceHoldBody &&
      wds::interaction::is_place_hold_star(event, hold_scratch_)) {
    add_hold_star_at(event.position);
    return;
  }
  if (mode_ == Mode::PlaceHoldBody &&
      wds::interaction::is_chain_hold_body(event, hold_scratch_)) {
    finish_hold_body(true);
    return;
  }
  // Shift on the star-side button with extra modifiers is not a star and must
  // not start a nested placement or hit-test while the hold is still drawing.
  if (mode_ == Mode::PlaceHoldBody && event.mods.shift) {
    const bool star_side = hold_scratch_ ? wds::interaction::is_left_button(event.button)
                                         : wds::interaction::is_right_button(event.button);
    if (star_side) return;
  }

  // Selected existing hold: Shift-only RMB (normal) / LMB (scratch) places a star.
  if (mode_ == Mode::Idle) {
    if (wds::interaction::is_place_hold_star(event, false) &&
        add_star_to_selected_hold(event.position, false)) {
      return;
    }
    if (wds::interaction::is_place_hold_star(event, true) &&
        add_star_to_selected_hold(event.position, true)) {
      return;
    }
  }

  if (auto hit = hit_test_note(event.position)) {
    hide_placement_ghost();
    if (wds::interaction::is_delete_single_note(event)) {
      delete_note_at(event.position);
      mode_ = Mode::Idle;
      return;
    }
    if (wds::interaction::is_toggle_select(event)) {
      clear_hold_sel_focus();
      if (selected_.count(hit->id)) selected_.erase(hit->id);
      else selected_.insert(hit->id);
      return;
    }
    if (wds::interaction::is_select_or_edit_note(event)) {
      // JumpScratch side edges win over the next body's start-time hit at chain joints.
      std::optional<NotationNote> scratch_edge_owner;
      float scratch_edge_dist = 1e9f;
      for (const auto& note : engine_.document().notes()) {
        if (selected_.count(note.id) == 0) continue;
        if (!wds::chart_editor::is_hold_chain_body(note.note_type)) continue;
        if (!near_scratch_end_cap(note, event.position.y)) continue;
        if (!near_scratch_end_left(note, event.position.x) &&
            !near_scratch_end_right(note, event.position.x)) {
          continue;
        }
        const float d = std::abs(event.position.y - viewport_.y_at(note.end_tick));
        if (d < scratch_edge_dist) {
          scratch_edge_dist = d;
          scratch_edge_owner = note;
        }
      }
      const NotationNote* hit_note = scratch_edge_owner ? &*scratch_edge_owner : &*hit;
      bool alt_chain_part = false;
      if (event.mods.alt) {
        if (auto body = resolve_hold_body(*hit_note);
            body && wds::chart_editor::is_hold_chain_body(body->note_type)) {
          // Alt opts out of the normal chain-layer selection and isolates the
          // clicked ScratchHold segment (plus its paired head).
          select_hold_segment(*body);
          hold_sel_layer_ = HoldSelLayer::Whole;
          alt_chain_part = true;
        }
      }
      bool anchor_already_selected = selected_.count(hit_note->id) != 0;
      if (alt_chain_part) anchor_already_selected = true;
      const auto pair_partner = hold_width_pair_partner(engine_.document(), *hit_note);
      const bool pair_already_selected =
          pair_partner.has_value() && selected_.count(pair_partner->id) != 0;

      const bool hitting_scratch_end =
          anchor_already_selected && (scratch_edge_owner.has_value() ||
          (wds::chart_editor::is_hold_chain_body(hit_note->note_type) &&
           near_scratch_end_cap(*hit_note, event.position.y) &&
           (near_scratch_end_left(*hit_note, event.position.x) ||
            near_scratch_end_right(*hit_note, event.position.x))));
      // JumpScratch end-cap: vertical adjust or side-edge width only — never free-drag.
      const bool in_jump_scratch_cap =
          anchor_already_selected && !hitting_scratch_end &&
          wds::chart_editor::is_hold_chain_body(hit_note->note_type) &&
          near_scratch_end_cap(*hit_note, event.position.y);
      const bool mid_star_hit =
          is_visible_mid_star(hit_note->note_type) || hit_note->note_type == NoteType::HoldEighth;
      const bool hitting_width_edge =
          anchor_already_selected && !mid_star_hit &&
          (hitting_scratch_end || near_left_edge(*hit_note, event.position.x) ||
           near_right_edge(*hit_note, event.position.x));
      const bool hitting_time_edge =
          anchor_already_selected && !mid_star_hit && !hitting_scratch_end &&
          (in_jump_scratch_cap ||
           (wds::chart_editor::is_hold_with_tail(hit_note->note_type) &&
            (near_start_time(*hit_note, event.position.y) ||
             near_end_time(*hit_note, event.position.y))));

      // Width / time edge drags must not change the current selection set.
      if (!alt_chain_part && !hitting_width_edge && !hitting_time_edge) {
        const bool direct_hold_part =
            wds::chart_editor::is_hold_head_note(*hit_note) ||
            (wds::chart_editor::is_hold_with_tail(hit_note->note_type) &&
             (near_start_time(*hit_note, event.position.y) ||
              near_end_time(*hit_note, event.position.y)));
        if (direct_hold_part) {
          clear_hold_sel_focus();
          selected_.clear();
          selected_.insert(hit_note->id);
        } else if (!apply_hold_layered_click(*hit_note)) {
          clear_hold_sel_focus();
          if (!selected_.count(hit_note->id)) {
            selected_.clear();
            selected_.insert(hit_note->id);
          }
        }
      }
      // Mid-stars: move in time only (lane locked to parent); no independent resize.
      if (mid_star_hit) {
        mode_ = Mode::MoveSelection;
        anchor_note_id_ = hit_note->id;
        resize_scratch_end_ = false;
        move_scratch_segment_ = false;
        collect_selected_originals(engine_.document(), selected_, drag_originals_);
      } else if (hitting_scratch_end) {
        // ScratchHold end width: grab the end-cap side edges (not the body).
        // Width edits always target the grabbed note only — never the selection set.
        mode_ = Mode::ResizeWidth;
        resize_scratch_end_ = true;
        resize_chain_peer_id_ = -1;
        resize_chain_next_id_ = -1;
        resize_applied_end_l_ = std::numeric_limits<int32_t>::min();
        resize_applied_end_r_ = std::numeric_limits<int32_t>::min();
        resize_applied_body_lane_ = std::numeric_limits<int32_t>::min();
        resize_applied_body_width_ = std::numeric_limits<int32_t>::min();
        resize_applied_peer_lane_ = std::numeric_limits<int32_t>::min();
        resize_applied_peer_width_ = std::numeric_limits<int32_t>::min();
        resize_side_ = near_scratch_end_left(*hit_note, event.position.x) ? -1 : 1;
        anchor_note_id_ = hit_note->id;
        drag_originals_.clear();
        append_scratch_hold_chain_snapshot(engine_.document(), *hit_note, drag_originals_);
        drag_originals_[hit_note->id] = *hit_note;
        append_hold_attached(engine_.document(), *hit_note, drag_originals_);
        if (auto head = wds::chart_editor::paired_hold_head_for(engine_.document(), *hit_note)) {
          if (head->width == hit_note->width) {
            drag_originals_.emplace(head->id, *head);
          }
        }
        if (auto next =
                wds::chart_editor::chained_next_scratch_hold(engine_.document(), *hit_note)) {
          resize_chain_peer_id_ = next->id;
        }
      } else if (hitting_time_edge) {
        mode_ = Mode::AdjustHoldTime;
        adjust_end_ = in_jump_scratch_cap || near_end_time(*hit_note, event.position.y);
        anchor_note_id_ = hit_note->id;
        resize_scratch_end_ = false;
        resize_chain_peer_id_ = -1;
        resize_chain_next_id_ = -1;
        // Edge drags keep selection unchanged; if the hit hold is not selected, snapshot
        // only that hold (+ paired head) instead of the current selection set.
        if (anchor_already_selected) {
          collect_drag_originals(engine_.document(), selected_, drag_originals_);
        } else {
          drag_originals_.clear();
          drag_originals_[hit_note->id] = *hit_note;
          if (wds::chart_editor::is_hold_with_tail(hit_note->note_type)) {
            append_hold_attached(engine_.document(), *hit_note, drag_originals_);
            if (auto head =
                    wds::chart_editor::paired_hold_head_for(engine_.document(), *hit_note)) {
              drag_originals_.emplace(head->id, *head);
            }
          }
        }
        // JumpScratch hinge: snapshot adjacent bodies so vertical drag can resize both.
        if (wds::chart_editor::is_hold_chain_body(hit_note->note_type)) {
          if (!adjust_end_) {
            // Start of a chained segment is the previous JumpScratch joint.
            if (auto prev = wds::chart_editor::chained_prev_scratch_hold(engine_.document(),
                                                                        *hit_note)) {
              adjust_end_ = true;
              resize_chain_next_id_ = hit_note->id;
              anchor_note_id_ = prev->id;
              drag_originals_.emplace(prev->id, *prev);
              append_hold_attached(engine_.document(), *prev, drag_originals_);
            }
          }
          if (adjust_end_) {
            auto joint_owner = engine_.document().find_note(anchor_note_id_);
            if (joint_owner) {
              drag_originals_.emplace(joint_owner->id, *joint_owner);
              append_hold_attached(engine_.document(), *joint_owner, drag_originals_);
              if (resize_chain_next_id_ < 0) {
                if (auto next = wds::chart_editor::chained_next_scratch_hold(engine_.document(),
                                                                            *joint_owner)) {
                  resize_chain_next_id_ = next->id;
                  drag_originals_.emplace(next->id, *next);
                  append_hold_attached(engine_.document(), *next, drag_originals_);
                }
              } else if (auto next = engine_.document().find_note(resize_chain_next_id_)) {
                drag_originals_.emplace(next->id, *next);
                append_hold_attached(engine_.document(), *next, drag_originals_);
              }
            }
          }
        }
      } else if (anchor_already_selected &&
                 (near_left_edge(*hit_note, event.position.x) ||
                  near_right_edge(*hit_note, event.position.x))) {
        // Width edits always target the grabbed note only — never the selection set.
        // Move / copy / mirror remain the multi-select batch operations.
        mode_ = Mode::ResizeWidth;
        resize_scratch_end_ = false;
        resize_chain_peer_id_ = -1;
        resize_chain_next_id_ = -1;
        resize_applied_end_l_ = std::numeric_limits<int32_t>::min();
        resize_applied_end_r_ = std::numeric_limits<int32_t>::min();
        resize_applied_body_lane_ = std::numeric_limits<int32_t>::min();
        resize_applied_body_width_ = std::numeric_limits<int32_t>::min();
        resize_applied_peer_lane_ = std::numeric_limits<int32_t>::min();
        resize_applied_peer_width_ = std::numeric_limits<int32_t>::min();
        resize_side_ = near_left_edge(*hit_note, event.position.x) ? -1 : 1;
        anchor_note_id_ = hit_note->id;
        std::optional<NotationNote> sync_partner;
        // Head↔body equal-width sync only when neither was already selected.
        if (pair_partner && hit_note->width == pair_partner->width && !anchor_already_selected &&
            !pair_already_selected) {
          sync_partner = pair_partner;
        }
        if (wds::chart_editor::is_hold_chain_body(hit_note->note_type)) {
          // Snapshot full chain for backward JumpScratch sync; only the grabbed
          // body receives the edge delta (peers stay on the drag-start snapshot).
          drag_originals_.clear();
          append_scratch_hold_chain_snapshot(engine_.document(), *hit_note, drag_originals_);
          drag_originals_[hit_note->id] = *hit_note;
          append_hold_attached(engine_.document(), *hit_note, drag_originals_);
          if (sync_partner) {
            drag_originals_.emplace(sync_partner->id, *sync_partner);
          }
          if (auto prev = wds::chart_editor::chained_prev_scratch_hold(engine_.document(),
                                                                      *hit_note)) {
            resize_chain_peer_id_ = prev->id;
          }
          if (auto next = wds::chart_editor::chained_next_scratch_hold(engine_.document(),
                                                                      *hit_note)) {
            resize_chain_next_id_ = next->id;
          }
        } else {
          drag_originals_.clear();
          drag_originals_[hit_note->id] = *hit_note;
          append_hold_attached(engine_.document(), *hit_note, drag_originals_);
          if (sync_partner) {
            drag_originals_.emplace(sync_partner->id, *sync_partner);
            append_hold_attached(engine_.document(), *sync_partner, drag_originals_);
          }
        }
      } else {
        mode_ = Mode::MoveSelection;
        anchor_note_id_ = hit_note->id;
        resize_scratch_end_ = false;
        move_scratch_segment_ = false;
        resize_chain_peer_id_ = -1;
        resize_chain_next_id_ = -1;
        // Include bound mid-stars / paired head so they track the body.
        // HoldEighth is regenerated locally after the move, not dragged.
        collect_drag_originals(engine_.document(), selected_, drag_originals_);
        const NotationNote* segment = hit_note;
        std::optional<NotationNote> resolved_body;
        if (!wds::chart_editor::is_hold_chain_body(hit_note->note_type) &&
            wds::chart_editor::is_hold_head_note(*hit_note)) {
          resolved_body = wds::chart_editor::paired_hold_body_for(engine_.document(), *hit_note);
          if (resolved_body && wds::chart_editor::is_hold_chain_body(resolved_body->note_type)) {
            segment = &*resolved_body;
          }
        }
        if (segment && wds::chart_editor::is_hold_chain_body(segment->note_type)) {
          int body_count = 0;
          for (const auto& [id, n] : drag_originals_) {
            (void)id;
            if (wds::chart_editor::is_hold_chain_body(n.note_type)) ++body_count;
          }
          if (body_count == 1) {
            auto prev = wds::chart_editor::chained_prev_scratch_hold(engine_.document(), *segment);
            auto next = wds::chart_editor::chained_next_scratch_hold(engine_.document(), *segment);
            if (prev || next) {
              move_scratch_segment_ = true;
              if (prev) {
                resize_chain_peer_id_ = prev->id;
                drag_originals_.emplace(prev->id, *prev);
              }
              if (next) {
                resize_chain_next_id_ = next->id;
                drag_originals_.emplace(next->id, *next);
              }
            }
          }
        }
      }
      // Follow the press point (not the note origin) so an off-center grab
      // does not snap the note under the cursor on the first move.
      drag_start_tick_ = viewport_.tick_at(event.position.y);
      drag_start_lane_ = viewport_.lane_at(event.position.x, 1);
      return;
    }
  }

  if (wds::interaction::is_marquee_select(event)) {
    hide_placement_ghost();
    mode_ = Mode::Marquee;
    marquee_start_tick_ = viewport_.tick_at(event.position.y);
    marquee_start_lane_ = viewport_.lane_at(event.position.x, 1);
    return;
  }

  if (!engine_.is_editable()) return;
  if (!wds::interaction::is_place_button(event.button)) return;
  if (viewport_.ms_at_y(event.position.y) < 0.0f) return;

  // Empty area: start placement. Right-click on a selected purple terminal
  // end-cap (including outer-edge slop that hit_test misses) arms chain-extend.
  clear_pending_chain_extend();
  if (wds::interaction::is_right_button(event.button)) {
    try_arm_pending_chain_extend(event.position, true);
  }

  place_anchor_ = make_base_note(event.position);
  place_swipe_.reset();
  // Gold vs ordinary head is decided at this press. The same Shift still
  // places stars; releasing it after this does not retint the hold.
  place_gold_head_ = event.mods.shift;
  mode_ = Mode::PlaceGesture;
  update_ghost(event.position);
}

void ChartEditPanel::on_pointer_move(const wds::interaction::PointerMoveEvent& event) {
  if (has_modal_popup()) {
    pointer_ = event.position;
    global_pointer_ = event.position;
    active_mods_ = event.mods;
    if (split_picker_open_ && split_scrollbar_dragging_) {
      sync_split_scrollbar_from_pointer(event.position.y);
    }
    return;
  }
  pointer_ = event.position;
  global_pointer_ = event.position;
  active_mods_ = event.mods;
  gesture_.on_pointer_move(event);
  sync_viewport();
  update_hover_cursor(event.position);

  if (mode_ == Mode::Idle) {
    if (!pointer_in_edit_ghost_zone(event.position)) {
      pointer_over_edit_ = false;
      hide_placement_ghost();
      return;
    }
    pointer_over_edit_ = true;
    update_ghost(event.position);
    return;
  }
  if (mode_ == Mode::PlaceGesture) {
    const auto swipe = update_place_swipe(event.position);
    if (swipe == SwipeDirection::Up) {
      const PlaceIntent intent =
          wds::interaction::resolve_place_intent(active_button_, swipe, false);
      if (intent == PlaceIntent::HoldBody) {
        begin_regular_hold_placement(place_note_center());
      } else if (intent == PlaceIntent::ScratchHoldBody) {
        begin_scratch_hold_placement(place_note_center());
      } else {
        update_ghost(event.position);
      }
    } else {
      update_ghost(event.position);
    }
  }
  if (mode_ == Mode::PlaceHoldBody) {
    sync_curve_mode();
    sync_hold_draft_to_pointer();
  }
  if (mode_ == Mode::Marquee) return;
  if (mode_ == Mode::MoveSelection) {
    sync_move_selection_to_pointer(event.position);
  }
  if (mode_ == Mode::ResizeWidth && engine_.is_editable()) {
    auto orig_it = drag_originals_.find(anchor_note_id_);
    if (orig_it == drag_originals_.end()) return;
    const int32_t lane = viewport_.lane_at(event.position.x, 1);
    const int lane_count = viewport_.grid().lane_count;
    const NotationNote& anchor_orig = orig_it->second;

    // Illegal resize: keep the last accepted document state (do not snap back to drag start).

    auto apply_body_delta = [&](const NotationNote& orig) -> std::optional<NotationNote> {
      NotationNote n = orig;
      if (resize_side_ > 0) {
        n.width = std::max(1, orig.width + (lane - orig.end_lane()));
      } else {
        const int32_t width_delta = orig.lane - lane;
        const int32_t new_lane = orig.lane - width_delta;
        const int32_t new_width = orig.width + width_delta;
        if (new_width < 1 || new_lane < 0) return std::nullopt;
        n.lane = new_lane;
        n.width = new_width;
      }
      if (!wds::chart_editor::lane_in_bounds(n.lane, n.width, lane_count)) return std::nullopt;
      return n;
    };

    auto sync_mid_stars = [&](std::unordered_map<int32_t, NotationNote>& next) {
      for (const auto& [id, orig] : drag_originals_) {
        if (!is_visible_mid_star(orig.note_type) && orig.note_type != NoteType::HoldEighth) {
          continue;
        }
        auto parent = bound_parent_hold(orig, next, engine_.document());
        NotationNote n = orig;
        if (parent) {
          n.lane = parent->lane;
          n.width = parent->width;
        } else if (const auto updated = apply_body_delta(orig)) {
          n = *updated;
        }
        if (!wds::chart_editor::lane_in_bounds(n.lane, n.width, lane_count)) {
          next.clear();
          return false;
        }
        next[id] = n;
      }
      return true;
    };

    auto cover_ok = [&](const NotationNote& body, const std::optional<NotationNote>& next_body) {
      int32_t cover_left = body.lane;
      int32_t cover_right = body.end_lane();
      if (next_body) {
        cover_left = std::min(cover_left, next_body->lane);
        cover_right = std::max(cover_right, next_body->end_lane());
      }
      return wds::chart_editor::scratch_hold_end_cover_representable(body, cover_left, cover_right);
    };

    // Move one body edge; the opposite edge stays fixed.
    auto move_body_left_edge = [&](NotationNote& b, int32_t new_left) -> bool {
      const int32_t right = b.end_lane();
      if (new_left > right || new_left < 0) return false;
      b.lane = new_left;
      b.width = right - new_left + 1;
      return wds::chart_editor::lane_in_bounds(b.lane, b.width, lane_count);
    };
    auto move_body_right_edge = [&](NotationNote& b, int32_t new_right) -> bool {
      if (new_right < b.lane || new_right >= lane_count) return false;
      b.width = new_right - b.lane + 1;
      return wds::chart_editor::lane_in_bounds(b.lane, b.width, lane_count);
    };

    // Chain walk direction: forward = toward later segments, backward = earlier.
    // Recording the walk prevents forward↔backward repair cycles that discard edits.
    enum class ChainDir : int8_t { Backward = -1, Forward = 1 };
    std::unordered_set<int32_t> pinned_body_ids;

    auto pin_body = [&](const NotationNote& b) { pinned_body_ids.insert(b.id); };

    // JumpScratch edge drag: only push body edges inward when the cover narrows past
    // them. Expanding an equal-width cover must NOT drag the body (allows last-segment
    // scratch overhang after they were once equal).
    auto sync_body_dragged_edge = [&](NotationNote& b, int32_t /*orig_cover_l*/,
                                      int32_t /*orig_cover_r*/, int32_t new_left,
                                      int32_t new_right) -> bool {
      if (resize_side_ > 0) {
        if (new_right < b.end_lane()) {
          if (!move_body_right_edge(b, new_right)) return false;
          pin_body(b);
        }
      } else {
        if (new_left > b.lane) {
          if (!move_body_left_edge(b, new_left)) return false;
          pin_body(b);
        }
      }
      return true;
    };

    // Scratch drag would need both-sided hang: sync THIS side of the body to the cover
    // (not the opposite side). Left drag → body.left = cover.left; right likewise.
    auto sync_body_same_side_to_cover = [&](NotationNote& b, int32_t cover_l,
                                            int32_t cover_r) -> bool {
      if (wds::chart_editor::scratch_hold_end_cover_representable(b, cover_l, cover_r)) {
        return true;
      }
      const bool ext_left = cover_l < b.lane;
      const bool ext_right = cover_r > b.end_lane();
      if (!(ext_left && ext_right)) return false;
      bool moved = false;
      if (resize_side_ < 0) {
        moved = move_body_left_edge(b, cover_l);
      } else if (resize_side_ > 0) {
        moved = move_body_right_edge(b, cover_r);
      } else if (ext_left) {
        moved = move_body_left_edge(b, cover_l);
      } else {
        moved = move_body_right_edge(b, cover_r);
      }
      if (!moved) return false;
      pin_body(b);
      return wds::chart_editor::scratch_hold_end_cover_representable(b, cover_l, cover_r);
    };

    // Chain membership is decided on drag-start geometry: same family, time
    // abutment, and prev's then-current tail exactly covering both bodies.
    auto original_of = [&](int32_t id, const NotationNote& fallback) -> const NotationNote& {
      if (auto it = drag_originals_.find(id); it != drag_originals_.end()) return it->second;
      return fallback;
    };
    auto originally_chained = [&](const NotationNote& prev, const NotationNote& next) {
      const NotationNote& p = original_of(prev.id, prev);
      const NotationNote& n = original_of(next.id, next);
      if (!wds::chart_editor::is_hold_chain_body(p.note_type) ||
          !wds::chart_editor::is_hold_chain_body(n.note_type)) {
        return false;
      }
      if (!wds::chart_editor::same_hold_chain_family(p.note_type, n.note_type)) return false;
      if (n.start_tick != p.end_tick) return false;
      return wds::chart_editor::hold_chain_lanes_connected(p, n);
    };
    auto later_starts_new_chain = [&](const NotationNote& later) {
      const NotationNote& later_orig = original_of(later.id, later);
      if (wds::chart_editor::paired_hold_head_for(engine_.document(), later_orig)) return true;
      for (const auto& [id, n] : drag_originals_) {
        (void)id;
        if (wds::chart_editor::hold_head_pairs_with_body(n, later_orig)) return true;
      }
      return false;
    };

    // Resolve hold-chain neighbors (may load into `working`).
    auto resolve_chained_next = [&](const NotationNote& body,
                                    std::unordered_map<int32_t, NotationNote>& working)
        -> std::optional<NotationNote*> {
      auto is_candidate = [&](const NotationNote& n) {
        if (n.id == body.id) return false;
        if (!originally_chained(body, n)) return false;
        if (later_starts_new_chain(n)) return false;
        return true;
      };
      for (auto& [id, n] : working) {
        (void)id;
        if (is_candidate(n)) return &n;
      }
      for (const auto& [id, n] : drag_originals_) {
        if (!is_candidate(n)) continue;
        auto [it, inserted] = working.emplace(id, n);
        (void)inserted;
        return &it->second;
      }
      for (const auto& note : engine_.document().notes()) {
        if (!is_candidate(note)) continue;
        auto [it, inserted] = working.emplace(note.id, note);
        (void)inserted;
        return &it->second;
      }
      return std::nullopt;
    };

    auto resolve_chained_prev = [&](const NotationNote& body,
                                    std::unordered_map<int32_t, NotationNote>& working)
        -> std::optional<NotationNote*> {
      auto is_candidate = [&](const NotationNote& n) {
        if (n.id == body.id) return false;
        if (!originally_chained(n, body)) return false;
        if (later_starts_new_chain(body)) return false;
        return true;
      };
      for (auto& [id, n] : working) {
        (void)id;
        if (is_candidate(n)) return &n;
      }
      for (const auto& [id, n] : drag_originals_) {
        if (!is_candidate(n)) continue;
        auto [it, inserted] = working.emplace(id, n);
        (void)inserted;
        return &it->second;
      }
      for (const auto& note : engine_.document().notes()) {
        if (!is_candidate(note)) continue;
        auto [it, inserted] = working.emplace(note.id, note);
        (void)inserted;
        return &it->second;
      }
      return std::nullopt;
    };

    auto cover_need_span = [&](const NotationNote& body,
                               std::unordered_map<int32_t, NotationNote>& working) {
      int32_t need_l = body.lane;
      int32_t need_r = body.end_lane();
      if (auto next_ptr = resolve_chained_next(body, working)) {
        need_l = std::min(need_l, (*next_ptr)->lane);
        need_r = std::max(need_r, (*next_ptr)->end_lane());
      }
      return std::pair<int32_t, int32_t>{need_l, need_r};
    };

    // Active working map for joint-direction encode inside try_set_cover.
    std::unordered_map<int32_t, NotationNote>* cover_working = nullptr;

    auto try_set_cover = [&](NotationNote& body, int32_t L, int32_t R, int32_t need_l,
                             int32_t need_r) -> bool {
      L = std::max(0, L);
      R = std::min(lane_count - 1, R);
      if (R < L) return false;
      if (!wds::chart_editor::scratch_hold_end_cover_representable(body, L, R)) return false;
      wds::chart_editor::set_scratch_hold_end_lanes(body, L, R);
      // Joint direction is solely prev↔next body edge score — always recompute.
      if (cover_working) {
        if (auto next_ptr = resolve_chained_next(body, *cover_working)) {
          wds::chart_editor::apply_scratch_chain_joint_direction(body, **next_ptr);
        }
      }
      const auto got = wds::chart_editor::get_scratch_end_lane_range(body);
      if (got.first > need_l || got.second < need_r) return false;
      if (!wds::chart_editor::scratch_hold_end_cover_representable(body, got.first, got.second)) {
        return false;
      }
      return true;
    };

    // Body → scratch same-side sync when cover would hang on both sides: glue the
    // dragged-side cover edge to the body edge; keep the opposite prefer/need span.
    auto sync_cover_same_side_to_body = [&](NotationNote& body, int32_t prefer_l,
                                            int32_t prefer_r, int32_t need_l,
                                            int32_t need_r) -> bool {
      int32_t L = std::min(prefer_l, need_l);
      int32_t R = std::max(prefer_r, need_r);
      if (resize_side_ < 0) {
        L = body.lane;
        R = std::max({need_r, prefer_r, body.end_lane()});
      } else if (resize_side_ > 0) {
        R = body.end_lane();
        L = std::min({need_l, prefer_l, body.lane});
      } else {
        return false;
      }
      return try_set_cover(body, L, R, need_l, need_r);
    };

    // Cover `need` while preserving prefer overhang when still one-sided — never
    // collapse to body-equal just because the opposite side became illegal.
    auto set_cover_preserving = [&](NotationNote& body, int32_t prefer_l, int32_t prefer_r,
                                    int32_t need_l, int32_t need_r) -> bool {
      int32_t L = std::min(prefer_l, need_l);
      int32_t R = std::max(prefer_r, need_r);
      if (try_set_cover(body, L, R, need_l, need_r)) return true;

      const bool req_left = need_l < body.lane;
      const bool req_right = need_r > body.end_lane();
      if (req_left && req_right) {
        // Both-sided hang: sync scratch on the dragged side to the body (not absorb).
        return sync_cover_same_side_to_body(body, prefer_l, prefer_r, need_l, need_r);
      }

      if (req_right) {
        L = body.lane;
        R = std::max({need_r, prefer_r, body.end_lane()});
      } else if (req_left) {
        R = body.end_lane();
        L = std::min({need_l, prefer_l, body.lane});
      } else {
        const int32_t left_amt = std::max(0, body.lane - L);
        const int32_t right_amt = std::max(0, R - body.end_lane());
        if (left_amt == 0 && right_amt == 0) {
          L = body.lane;
          R = body.end_lane();
        } else if (left_amt >= right_amt) {
          R = body.end_lane();
          L = std::min(L, body.lane);
        } else {
          L = body.lane;
          R = std::max(R, body.end_lane());
        }
      }
      return try_set_cover(body, L, R, need_l, need_r);
    };

    std::function<bool(NotationNote&, int32_t, int32_t, std::unordered_map<int32_t, NotationNote>&,
                       int, bool, ChainDir)>
        repair_scratch_cover_chain;
    repair_scratch_cover_chain =
        [&](NotationNote& body, int32_t prefer_left, int32_t prefer_right,
            std::unordered_map<int32_t, NotationNote>& working, int depth,
            bool allow_expand_body, ChainDir dir) -> bool {
      if (depth > 24) return false;
      cover_working = &working;
      const auto need = cover_need_span(body, working);
      const int32_t need_l = need.first;
      const int32_t need_r = need.second;

      if (set_cover_preserving(body, prefer_left, prefer_right, need_l, need_r)) return true;
      if (try_set_cover(body, need_l, need_r, need_l, need_r)) return true;

      // Body edit path: same-side body sync to the needed cover on the dragged edge.
      if (sync_body_same_side_to_cover(body, need_l, need_r)) {
        working[body.id] = body;
        if (try_set_cover(body, need_l, need_r, need_l, need_r) ||
            set_cover_preserving(body, prefer_left, prefer_right, need_l, need_r)) {
          working[body.id] = body;
          return true;
        }
      }

      auto try_expand_body_same_side = [&]() -> bool {
        if (!allow_expand_body) return false;
        const int32_t saved_lane = body.lane;
        const int32_t saved_width = body.width;
        bool moved = false;
        if (resize_side_ < 0 && need_l < body.lane) {
          moved = move_body_left_edge(body, need_l);
        } else if (resize_side_ > 0 && need_r > body.end_lane()) {
          moved = move_body_right_edge(body, need_r);
        }
        if (!moved) {
          body.lane = saved_lane;
          body.width = saved_width;
          return false;
        }
        pin_body(body);
        if (try_set_cover(body, need_l, need_r, need_l, need_r) ||
            set_cover_preserving(body, prefer_left, prefer_right, need_l, need_r)) {
          working[body.id] = body;
          return true;
        }
        body.lane = saved_lane;
        body.width = saved_width;
        return false;
      };
      if (try_expand_body_same_side()) return true;

      // Forward-only: if next sticks out on the dragged side, pull that edge in, then
      // repair next forward. Never recurse back onto `body` (avoids repair cycles).
      if (dir != ChainDir::Forward) return false;
      if (auto next_ptr = resolve_chained_next(body, working)) {
        NotationNote& nb = **next_ptr;
        if (pinned_body_ids.count(nb.id)) return false;
        const int32_t left_over = body.lane - nb.lane;
        const int32_t right_over = nb.end_lane() - body.end_lane();
        bool moved = false;
        if (resize_side_ < 0 && left_over > 0) {
          moved = move_body_left_edge(nb, body.lane);
        } else if (resize_side_ > 0 && right_over > 0) {
          moved = move_body_right_edge(nb, body.end_lane());
        }
        if (!moved) return false;
        pin_body(nb);
        working[nb.id] = nb;
        auto nb_orig_it = drag_originals_.find(nb.id);
        const NotationNote nb_prefer_src =
            nb_orig_it != drag_originals_.end() ? nb_orig_it->second : nb;
        const auto nb_prefer = wds::chart_editor::get_scratch_end_lane_range(nb_prefer_src);
        if (!repair_scratch_cover_chain(nb, nb_prefer.first, nb_prefer.second, working, depth + 1,
                                       allow_expand_body, ChainDir::Forward)) {
          return false;
        }
        if (auto it = working.find(nb.id); it != working.end()) nb = it->second;
        const auto need2 = cover_need_span(body, working);
        return set_cover_preserving(body, prefer_left, prefer_right, need2.first, need2.second) ||
               try_set_cover(body, need2.first, need2.second, need2.first, need2.second) ||
               sync_cover_same_side_to_body(body, prefer_left, prefer_right, need2.first,
                                            need2.second);
      }
      return false;
    };

    // Own JumpScratch after this body moved: preserve one-sided overhang; cover next if any.
    // Walk forward only so reverse cover sync remains a separate Backward pass.
    auto apply_own_cover_after_body_change =
        [&](NotationNote& body, const NotationNote& body_orig,
            std::unordered_map<int32_t, NotationNote>& working) -> bool {
      const auto prefer = wds::chart_editor::get_scratch_end_lane_range(body_orig);
      pin_body(body);
      return repair_scratch_cover_chain(body, prefer.first, prefer.second, working, 0,
                                       /*allow_expand_body=*/false, ChainDir::Forward);
    };

    // Walk backward only: each previous JumpScratch becomes the union of that body + next.
    // Unpinned prev bodies rebaseline from drag_originals so transient expands shrink back.
    // Pinned bodies (edited by this drag / same-side sync) keep their live geometry.
    std::function<bool(int32_t, std::unordered_map<int32_t, NotationNote>&, int)>
        propagate_backward_cover_sync;
    propagate_backward_cover_sync =
        [&](int32_t body_id, std::unordered_map<int32_t, NotationNote>& working,
            int depth) -> bool {
      if (depth > 24) return false;
      cover_working = &working;
      auto body_it = working.find(body_id);
      if (body_it == working.end()) return false;
      auto prev_ptr = resolve_chained_prev(body_it->second, working);
      if (!prev_ptr) return true;

      NotationNote& prev = **prev_ptr;
      auto prev_orig_it = drag_originals_.find(prev.id);
      if (prev_orig_it != drag_originals_.end() && !pinned_body_ids.count(prev.id)) {
        prev.lane = prev_orig_it->second.lane;
        prev.width = prev_orig_it->second.width;
        prev.scratch_length = prev_orig_it->second.scratch_length;
      }
      const int32_t baseline_lane = prev.lane;
      const int32_t baseline_width = prev.width;
      const int32_t baseline_end = prev.end_lane();
      const int32_t union_l = std::min(prev.lane, body_it->second.lane);
      const int32_t union_r = std::max(prev.end_lane(), body_it->second.end_lane());

      auto restore_prev_baseline = [&] {
        prev.lane = baseline_lane;
        prev.width = baseline_width;
        if (prev_orig_it != drag_originals_.end()) {
          prev.scratch_length = prev_orig_it->second.scratch_length;
        }
      };

      if (!try_set_cover(prev, union_l, union_r, union_l, union_r)) {
        // Both-sided hang vs prev body: sync THIS side of prev body to the union edge
        // matching the drag (scratch/body same-side), never absorb the opposite side.
        const int32_t left_over = baseline_lane - union_l;
        const int32_t right_over = union_r - baseline_end;
        bool expanded = false;

        auto try_sync_left = [&]() -> bool {
          if (left_over <= 0) return false;
          const int32_t new_width = baseline_end - union_l + 1;
          if (new_width < 1 ||
              !wds::chart_editor::lane_in_bounds(union_l, new_width, lane_count)) {
            return false;
          }
          prev.lane = union_l;
          prev.width = new_width;
          if (!try_set_cover(prev, union_l, union_r, union_l, union_r)) return false;
          pin_body(prev);
          return true;
        };
        auto try_sync_right = [&]() -> bool {
          if (right_over <= 0) return false;
          const int32_t new_width = union_r - baseline_lane + 1;
          if (new_width < 1 ||
              !wds::chart_editor::lane_in_bounds(baseline_lane, new_width, lane_count)) {
            return false;
          }
          prev.lane = baseline_lane;
          prev.width = new_width;
          if (!try_set_cover(prev, union_l, union_r, union_l, union_r)) return false;
          pin_body(prev);
          return true;
        };

        if (resize_side_ < 0) {
          expanded = try_sync_left() || try_sync_right();
        } else if (resize_side_ > 0) {
          expanded = try_sync_right() || try_sync_left();
        } else {
          expanded = (left_over <= right_over) ? (try_sync_left() || try_sync_right())
                                              : (try_sync_right() || try_sync_left());
        }

        if (!expanded) {
          restore_prev_baseline();
          return false;
        }
      }
      working[prev.id] = prev;

      const bool prev_body_changed =
          prev.lane != baseline_lane || prev.width != baseline_width;
      if (prev_orig_it != drag_originals_.end() && prev_body_changed) {
        const auto& prev_for_head = prev_orig_it->second;
        for (const auto& [hid, head_orig] : drag_originals_) {
          if (!wds::chart_editor::is_hold_head_note(head_orig)) continue;
          if (head_orig.start_tick != prev_for_head.start_tick) continue;
          if (head_orig.width != prev_for_head.width) continue;
          if (!(head_orig.lane <= prev_for_head.end_lane() &&
                prev_for_head.lane <= head_orig.end_lane())) {
            continue;
          }
          NotationNote head = head_orig;
          head.lane = prev.lane;
          head.width = prev.width;
          if (!wds::chart_editor::lane_in_bounds(head.lane, head.width, lane_count)) {
            return false;
          }
          working[hid] = head;
        }
      }
      // Continue backward only — never turn around into a forward repair from here.
      return propagate_backward_cover_sync(prev.id, working, depth + 1);
    };

    // Write the gesture snapshot back — used when the pointer returns to drag-start
    // geometry so mid-drag tip/direction edits do not linger in the document.
    auto restore_drag_originals = [&]() -> bool {
      if (!apply_note_map(engine_.document(), drag_originals_)) {
        return false;
      }
      engine_.rebuild_snapshot();
      return true;
    };

    // Keep equal-width hold heads matched to a resized body.
    auto sync_equal_width_heads = [&](std::unordered_map<int32_t, NotationNote>& next) {
      for (const auto& [id, body_next] : next) {
        if (!wds::chart_editor::is_hold_with_tail(body_next.note_type)) continue;
        auto body_orig_it = drag_originals_.find(id);
        if (body_orig_it == drag_originals_.end()) continue;
        const auto& body_orig = body_orig_it->second;
        for (const auto& [hid, head_orig] : drag_originals_) {
          if (!wds::chart_editor::is_hold_head_note(head_orig)) continue;
          if (head_orig.start_tick != body_orig.start_tick) continue;
          if (head_orig.width != body_orig.width) continue;
          if (!(head_orig.lane <= body_orig.end_lane() && body_orig.lane <= head_orig.end_lane())) {
            continue;
          }
          NotationNote head = head_orig;
          head.lane = body_next.lane;
          head.width = body_next.width;
          if (!wds::chart_editor::lane_in_bounds(head.lane, head.width, lane_count)) {
            return false;
          }
          next[hid] = head;
        }
      }
      return true;
    };

    // --- ScratchHold end-cap (JumpScratch) resize ---
    if (resize_scratch_end_ && wds::chart_editor::is_hold_chain_body(anchor_orig.note_type)) {
      const auto orig_end = wds::chart_editor::get_scratch_end_lane_range(anchor_orig);
      int32_t end_left = orig_end.first;
      int32_t end_right = orig_end.second;
      if (resize_side_ > 0) {
        end_right = std::clamp(lane, end_left, lane_count - 1);
      } else {
        end_left = std::clamp(lane, 0, end_right);
      }

      // Pointer back at drag-start cover: restore snapshot (cheap; fixes lingering tip dirs).
      if (end_left == orig_end.first && end_right == orig_end.second) {
        if (resize_applied_end_l_ != orig_end.first || resize_applied_end_r_ != orig_end.second ||
            resize_applied_body_lane_ != anchor_orig.lane ||
            resize_applied_body_width_ != anchor_orig.width) {
          if (!restore_drag_originals()) {
            return;
          }
          resize_applied_end_l_ = orig_end.first;
          resize_applied_end_r_ = orig_end.second;
          resize_applied_body_lane_ = anchor_orig.lane;
          resize_applied_body_width_ = anchor_orig.width;
          resize_applied_peer_lane_ = std::numeric_limits<int32_t>::min();
          resize_applied_peer_width_ = std::numeric_limits<int32_t>::min();
          default_width_ = std::max(1, orig_end.second - orig_end.first + 1);
        }
        return;
      }

      std::unordered_map<int32_t, NotationNote> next;
      NotationNote body = anchor_orig;
      std::optional<NotationNote> next_body;
      if (resize_chain_peer_id_ >= 0) {
        auto peer_it = drag_originals_.find(resize_chain_peer_id_);
        if (peer_it != drag_originals_.end()) next_body = peer_it->second;
      }

      const int32_t body_lane0 = body.lane;
      const int32_t body_width0 = body.width;
      if (!sync_body_dragged_edge(body, orig_end.first, orig_end.second, end_left, end_right)) {
        return;
      }
      int32_t next_lane0 = -1;
      int32_t next_width0 = -1;
      if (next_body) {
        next_lane0 = next_body->lane;
        next_width0 = next_body->width;
        if (!sync_body_dragged_edge(*next_body, orig_end.first, orig_end.second, end_left,
                                   end_right)) {
          return;
        }
      }

      if (end_left > body.lane || end_right < body.end_lane() ||
          (next_body &&
           (end_left > next_body->lane || end_right < next_body->end_lane()))) {
        return;
      }
      // Both-sided hang: sync this side of the body to the scratch (not absorb opposite).
      if (!sync_body_same_side_to_cover(body, end_left, end_right)) {
        return;
      }
      if (!wds::chart_editor::scratch_hold_end_cover_representable(body, end_left, end_right)) {
        return;
      }
      if (next_body && !cover_ok(body, next_body)) {
        const int32_t need_l = std::min({body.lane, next_body->lane, end_left});
        const int32_t need_r = std::max({body.end_lane(), next_body->end_lane(), end_right});
        if (!sync_body_same_side_to_cover(body, need_l, need_r) || !cover_ok(body, next_body)) {
          return;
        }
        if (end_left > body.lane || end_right < body.end_lane()) {
          return;
        }
      }

      // Hold still at the same committed geometry — skip cover/direction work.
      const int32_t peer_lane = next_body ? next_body->lane : std::numeric_limits<int32_t>::min();
      const int32_t peer_width = next_body ? next_body->width : std::numeric_limits<int32_t>::min();
      if (end_left == resize_applied_end_l_ && end_right == resize_applied_end_r_ &&
          body.lane == resize_applied_body_lane_ && body.width == resize_applied_body_width_ &&
          peer_lane == resize_applied_peer_lane_ && peer_width == resize_applied_peer_width_) {
        return;
      }

      pin_body(body);
      if (next_body) pin_body(*next_body);
      wds::chart_editor::set_scratch_hold_end_lanes(body, end_left, end_right);
      {
        const auto got = wds::chart_editor::get_scratch_end_lane_range(body);
        if (got.first != end_left || got.second != end_right) {
          return;
        }
      }
      next[body.id] = body;
      if (next_body) {
        next[next_body->id] = *next_body;
        wds::chart_editor::apply_scratch_chain_joint_direction(next[body.id], next[next_body->id]);
        auto next_orig_it = drag_originals_.find(next_body->id);
        if (next_orig_it == drag_originals_.end()) return;
        // Forward-only own-cover repair on next; backward sync is a separate pass.
        if (!apply_own_cover_after_body_change(next[next_body->id], next_orig_it->second, next)) {
          return;
        }
      } else if (auto next_ptr = resolve_chained_next(next[body.id], next)) {
        wds::chart_editor::apply_scratch_chain_joint_direction(next[body.id], **next_ptr);
      }

      // Backward cover sync only when bodies moved (joint score inputs changed).
      const bool body_changed =
          next[body.id].lane != body_lane0 || next[body.id].width != body_width0;
      const bool next_changed =
          next_body &&
          (next[next_body->id].lane != next_lane0 || next[next_body->id].width != next_width0);
      if (next_changed && !propagate_backward_cover_sync(next_body->id, next, 0)) {
        return;
      }
      if (body_changed && !propagate_backward_cover_sync(body.id, next, 0)) {
        return;
      }
      // Re-apply the dragged cover on the anchor after backward sync may have touched it.
      if (auto ait = next.find(body.id); ait != next.end()) {
        if (!sync_body_dragged_edge(ait->second, orig_end.first, orig_end.second, end_left,
                                   end_right)) {
          return;
        }
        if (!sync_body_same_side_to_cover(ait->second, end_left, end_right)) {
          return;
        }
        if (end_left > ait->second.lane || end_right < ait->second.end_lane()) {
          return;
        }
        std::optional<NotationNote*> joint_next;
        if (next_body) {
          auto nit = next.find(next_body->id);
          if (nit == next.end()) return;
          if (!sync_body_dragged_edge(nit->second, orig_end.first, orig_end.second, end_left,
                                     end_right)) {
            return;
          }
          if (end_left > nit->second.lane || end_right < nit->second.end_lane()) {
            return;
          }
          if (!cover_ok(ait->second, nit->second)) return;
          joint_next = &nit->second;
        } else {
          joint_next = resolve_chained_next(ait->second, next);
        }
        wds::chart_editor::set_scratch_hold_end_lanes(ait->second, end_left, end_right);
        {
          const auto got = wds::chart_editor::get_scratch_end_lane_range(ait->second);
          if (got.first != end_left || got.second != end_right) return;
        }
        if (joint_next) {
          wds::chart_editor::apply_scratch_chain_joint_direction(ait->second, **joint_next);
        }
      }

      if (!sync_equal_width_heads(next) || !sync_mid_stars(next)) {
        return;
      }
      if (!apply_note_map(engine_.document(), next)) {
        return;
      }
      resize_applied_end_l_ = end_left;
      resize_applied_end_r_ = end_right;
      resize_applied_body_lane_ = next[body.id].lane;
      resize_applied_body_width_ = next[body.id].width;
      if (next_body) {
        resize_applied_peer_lane_ = next[next_body->id].lane;
        resize_applied_peer_width_ = next[next_body->id].width;
      } else {
        resize_applied_peer_lane_ = std::numeric_limits<int32_t>::min();
        resize_applied_peer_width_ = std::numeric_limits<int32_t>::min();
      }
      if (auto n = engine_.document().find_note(anchor_note_id_)) {
        const auto end = wds::chart_editor::get_scratch_end_lane_range(*n);
        default_width_ = std::max(1, end.second - end.first + 1);
      }
      engine_.rebuild_snapshot();
      return;
    }

    // --- Body edge resize ---
    const int32_t width_delta =
        resize_side_ > 0 ? (lane - anchor_orig.end_lane()) : (anchor_orig.lane - lane);

    // Pointer back at drag-start body width: restore snapshot (incl. prior tip directions).
    if (width_delta == 0) {
      if (resize_applied_body_lane_ != anchor_orig.lane ||
          resize_applied_body_width_ != anchor_orig.width ||
          resize_applied_end_l_ != std::numeric_limits<int32_t>::min()) {
        if (!restore_drag_originals()) {
          return;
        }
        resize_applied_end_l_ = std::numeric_limits<int32_t>::min();
        resize_applied_end_r_ = std::numeric_limits<int32_t>::min();
        resize_applied_body_lane_ = anchor_orig.lane;
        resize_applied_body_width_ = anchor_orig.width;
        resize_applied_peer_lane_ = std::numeric_limits<int32_t>::min();
        resize_applied_peer_width_ = std::numeric_limits<int32_t>::min();
        default_width_ = anchor_orig.width;
      }
      return;
    }

    int32_t expect_lane = anchor_orig.lane;
    int32_t expect_width = anchor_orig.width;
    if (resize_side_ > 0) {
      expect_width = std::max(1, anchor_orig.width + width_delta);
    } else {
      expect_lane = anchor_orig.lane - width_delta;
      expect_width = anchor_orig.width + width_delta;
      if (expect_width < 1 || expect_lane < 0) return;
    }
    if (!wds::chart_editor::lane_in_bounds(expect_lane, expect_width, lane_count)) return;
    // Same committed body geometry as last move — skip cover/direction work.
    if (expect_lane == resize_applied_body_lane_ && expect_width == resize_applied_body_width_) {
      return;
    }

    std::unordered_map<int32_t, NotationNote> next_map;
    for (const auto& [id, orig] : drag_originals_) {
      if (is_visible_mid_star(orig.note_type) || orig.note_type == NoteType::HoldEighth) {
        continue;
      }
      // Only the dragged body receives the edge delta. Chain peers (and their heads)
      // stay on the drag-start snapshot; covers / peer bodies are updated by
      // apply_own_cover + propagate_backward. Applying the same delta to peer heads
      // would push them out of bounds (e.g. middle body left-drag stuck at prev end).
      if (wds::chart_editor::is_hold_chain_body(anchor_orig.note_type) &&
          id != anchor_note_id_) {
        next_map[id] = orig;
        continue;
      }
      if (wds::chart_editor::is_hold_chain_body(orig.note_type) &&
          (id == resize_chain_peer_id_ || id == resize_chain_next_id_)) {
        next_map[id] = orig;
        continue;
      }

      NotationNote n = orig;
      if (resize_side_ > 0) {
        n.width = std::max(1, orig.width + width_delta);
      } else {
        const int32_t new_lane = orig.lane - width_delta;
        const int32_t new_width = orig.width + width_delta;
        if (new_width < 1 || new_lane < 0) {
          return;
        }
        n.lane = new_lane;
        n.width = new_width;
      }
      if (!wds::chart_editor::lane_in_bounds(n.lane, n.width, lane_count)) {
        return;
      }

      if (wds::chart_editor::is_hold_chain_body(orig.note_type)) {
        pin_body(n);
      }
      if (wds::chart_editor::is_hold_chain_body(orig.note_type) && id == anchor_note_id_) {
        next_map[id] = n;
        if (!apply_own_cover_after_body_change(next_map[id], orig, next_map)) {
          return;
        }
        continue;
      }
      next_map[id] = n;
    }

    // Sync previous JumpScratch to the new body (exact union) and chain further back.
    if (wds::chart_editor::is_hold_chain_body(anchor_orig.note_type) && !resize_scratch_end_) {
      if (!propagate_backward_cover_sync(anchor_note_id_, next_map, 0)) {
        return;
      }
    } else if (!wds::chart_editor::is_hold_chain_body(anchor_orig.note_type) ||
               resize_scratch_end_) {
      for (auto& [id, n] : next_map) {
        auto oit = drag_originals_.find(id);
        if (oit == drag_originals_.end()) continue;
        if (wds::chart_editor::is_hold_chain_body(oit->second.note_type) &&
            n.scratch_length == oit->second.scratch_length) {
          const auto old_end = wds::chart_editor::get_scratch_end_lane_range(oit->second);
          wds::chart_editor::set_scratch_hold_end_lanes(n, old_end.first, old_end.second);
        }
      }
    }

    // Direction for dirty tips is already applied inside try_set_cover / own-cover;
    // do not blanket-refresh the whole chain every move.
    if (!sync_equal_width_heads(next_map) || !sync_mid_stars(next_map)) {
      return;
    }
    if (!apply_note_map(engine_.document(), next_map)) {
      return;
    }
    if (auto ait = next_map.find(anchor_note_id_); ait != next_map.end()) {
      resize_applied_body_lane_ = ait->second.lane;
      resize_applied_body_width_ = ait->second.width;
    }
    resize_applied_end_l_ = std::numeric_limits<int32_t>::min();
    resize_applied_end_r_ = std::numeric_limits<int32_t>::min();
    if (auto n = engine_.document().find_note(anchor_note_id_)) {
      default_width_ = n->width;
    }
    engine_.rebuild_snapshot();
  }
  if (mode_ == Mode::AdjustHoldTime) {
    sync_hold_adjust_to_pointer(event.position);
  }
  if (mode_ == Mode::DragSplitEdge) {
    sync_split_edge_to_pointer(event.position);
  }
}

void ChartEditPanel::on_pointer_up(const wds::interaction::PointerUpEvent& event) {
  if (has_modal_popup()) {
    pointer_ = event.position;
    split_scrollbar_dragging_ = false;
    return;
  }
  pointer_ = event.position;
  global_pointer_ = event.position;
  active_mods_ = event.mods;
  gesture_.on_pointer_up(event);
  sync_viewport();

  if (mode_ == Mode::Marquee) {
    finish_marquee(event.position);
    return;
  }
  if (mode_ == Mode::MoveSelection) {
    finish_move();
    return;
  }
  if (mode_ == Mode::ResizeWidth) {
    finish_resize();
    return;
  }
  if (mode_ == Mode::AdjustHoldTime) {
    finish_hold_adjust();
    return;
  }
  if (mode_ == Mode::DragSplitEdge) {
    const int32_t note_id = drag_split_note_id_;
    const bool ever_moved = split_edge_ever_moved_;
    auto orig_it = drag_originals_.find(note_id);
    auto cur = engine_.document().find_note(note_id);
    if (ever_moved && orig_it != drag_originals_.end() && cur &&
        (cur->start_tick != orig_it->second.start_tick ||
         cur->end_tick != orig_it->second.end_tick)) {
      std::unordered_map<int32_t, wds::chart_editor::UpdateNotesCommand::NotePair> changes;
      changes[note_id] = {orig_it->second, *cur};
      if (!engine_.document().update_note(note_id, orig_it->second)) {
        drag_originals_.clear();
        drag_split_note_id_ = -1;
        split_edge_ever_moved_ = false;
        mode_ = Mode::Idle;
        update_split_label_hover(event.position);
        return;
      }
      commit_updates(changes, "Resize split");
    }
    drag_originals_.clear();
    drag_split_note_id_ = -1;
    split_edge_ever_moved_ = false;
    mode_ = Mode::Idle;
    update_split_label_hover(event.position);
    if (!ever_moved && wds::interaction::is_left_button(event.button) && engine_.is_editable()) {
      open_split_picker_for_edit(note_id);
    }
    return;
  }
  if (mode_ == Mode::PlaceHoldBody) {
    // Chain/star use the opposite button (down); only the hold's primary button
    // release finishes. ScratchHold finishes on right-up; normal Hold on left-up.
    if (curve_mode_active_ &&
        (wds::interaction::is_curve_fill_confirm(event, hold_scratch_) ||
         wds::interaction::is_finish_hold_body(event.button, hold_scratch_))) {
      if (commit_curve_fill()) return;
    }
    if (wds::interaction::is_finish_hold_body(event.button, hold_scratch_)) {
      finish_hold_body(false);
    }
    return;
  }
  if (mode_ == Mode::PlaceGesture) {
    finish_place_gesture(event);
    return;
  }

  // Click empty to clear selection
  if (!hit_test_note(event.position) && wds::interaction::is_clear_selection_click(event)) {
    selected_.clear();
    clear_hold_sel_focus();
  }
  mode_ = Mode::Idle;
}

void ChartEditPanel::on_double_click(const wds::interaction::DoubleClickEvent& event) {
  if (has_modal_popup()) return;
  if (!absolute_bounds().contains(event.position)) return;
  if (!playfield_.contains(event.position)) return;
  sync_viewport();
  auto hit = hit_test_note(event.position);
  if (!hit) return;
  drill_hold_selection(*hit);
  mode_ = Mode::Idle;
  drag_originals_.clear();
}

void ChartEditPanel::on_scroll(const wds::interaction::ScrollEvent& event) {
  if (has_modal_popup()) {
    if (!overlay_host_bounds().contains(event.position)) return;
    if (split_picker_open_) {
      layout_popup_rects();
      split_color_scroll_ = std::clamp(
          split_color_scroll_ - event.delta_y * split_color_row_h_ * 0.45f, 0.0f,
          split_color_max_scroll_);
    }
    return;
  }
  if (!absolute_bounds().contains(event.position)) return;
  handle_timeline_wheel(event);
}

void ChartEditPanel::handle_timeline_wheel(const wds::interaction::ScrollEvent& event) {
  active_mods_ = event.mods;

  // Fixed gesture: Option+wheel (Alt+wheel on Windows/Linux) adjusts visible
  // range only when that is the sole modifier (not in shortcut settings).
  // Shift+Ctrl/Cmd is curve fill and must not zoom.
  // Default: scroll up shrinks the window (zoom in).
  if (wds::interaction::is_visible_range_wheel_modifiers(event.mods)) {
    auto grid = viewport_.grid();
    const int32_t before = grid.visible_hectoms;
    const int32_t next = visible_range_after_wheel(
        event.delta_y, before, wds::interaction::invert_scroll_wheel(),
        wds::interaction::invert_visible_range_scroll());
    if (next == before) return;
    pointer_ = event.position;
    global_pointer_ = event.position;
    grid.visible_hectoms = next;
    set_grid(grid);
    viewport_.sync_scroll_to_playhead_ms(static_cast<double>(engine_.timeline_ms()));
    if (on_visible_range_changed_) {
      on_visible_range_changed_();
    }
    resync_pointer_overlays();
    return;
  }

  // Scroll = scrub preview timeline by fixed time so motion stays BPM-independent.
  // Floor is the chart start (negative delay), not music t=0.
  const int64_t next_ms = clamp_scrub_ms(
      engine_.timeline_ms() +
          timeline_scrub_delta_ms(event.delta_y, viewport_.grid().visible_hectoms,
                                  wds::interaction::scroll_wheel_speed()),
      engine_.document().timing().offset_ms);
  if (seek_ms_) {
    seek_ms_(next_ms);
  } else {
    viewport_.set_timing(engine_.document().timing());
    viewport_.sync_scroll_to_playhead_ms(next_ms);
  }
  // Wheel scrub updates scroll without a pointer-move; keep ghosts under the cursor.
  resync_pointer_overlays();
}

void ChartEditPanel::sync_active_modifiers(wds::interaction::Modifiers mods) {
  active_mods_ = mods;
  if (wds::interaction::suppress_idle_placement_ghost(active_mods_, is_note_drawing())) {
    hide_placement_ghost();
  }
  sync_curve_mode();
  if (mode_ == Mode::Idle && pointer_over_edit_ &&
      !wds::interaction::suppress_idle_placement_ghost(active_mods_, is_note_drawing())) {
    update_ghost(pointer_);
  }
}

void ChartEditPanel::on_key_down(const wds::interaction::KeyDownEvent& event) {
  active_mods_ = event.mods;
  if (timing_popup_open_) {
    auto* field = timing_popup_mode_ == TimingPopupMode::Bpm ? &timing_bpm_text_
                  : timing_focus_field_ == 1                 ? &timing_num_text_
                                                             : &timing_den_text_;
    if (event.key == wds::interaction::KeyCode::Backspace) {
      if (!field->empty()) field->pop_back();
      timing_caret_blink_t_ = 0.0f;
      return;
    }
    if (event.key == wds::interaction::KeyCode::Enter) {
      commit_timing_popup();
      return;
    }
    if (event.key == wds::interaction::KeyCode::Escape) {
      close_timing_popup();
      return;
    }
    if (event.key == wds::interaction::KeyCode::Tab) {
      if (timing_popup_mode_ == TimingPopupMode::Meter) {
        timing_focus_field_ = timing_focus_field_ == 1 ? 2 : 1;
        timing_caret_blink_t_ = 0.0f;
      }
      return;
    }
    return;
  }
  if (split_picker_open_) {
    if (split_search_focused_ && event.key == wds::interaction::KeyCode::Backspace) {
      if (!split_search_text_.empty()) {
        apply_split_search_text(split_search_text_.substr(0, split_search_text_.size() - 1));
      }
      split_search_caret_blink_t_ = 0.0f;
      return;
    }
    if (event.key == wds::interaction::KeyCode::Escape) {
      close_split_picker();
    }
    return;
  }
  // Esc clears selection and resets hold drill layer to outer (None), so the next
  // hold click starts at Chain/Whole instead of residual Parts/Stars.
  if (event.key == wds::interaction::KeyCode::Escape) {
    clear_selection();
    return;
  }
  if (wds::interaction::suppress_idle_placement_ghost(event.mods, is_note_drawing())) {
    hide_placement_ghost();
  }
  sync_curve_mode();
  const auto resolved = wds::interaction::resolve_edit_key(event);
  switch (resolved.action) {
    case wds::interaction::EditKeyAction::SetDefaultWidth:
      set_default_width(resolved.width);
      break;
    case wds::interaction::EditKeyAction::DeleteSelection:
      delete_selected();
      break;
    case wds::interaction::EditKeyAction::None:
      break;
  }
}

void ChartEditPanel::on_key_up(const wds::interaction::KeyUpEvent& event) {
  active_mods_ = event.mods;
  const bool was_curve = curve_mode_active_;
  sync_curve_mode();
  if (was_curve && !curve_mode_active_ && mode_ == Mode::PlaceHoldBody) {
    sync_hold_draft_to_pointer(HoldDraftSync::LocalDraftOnly);
  }
  if (mode_ == Mode::Idle) {
    update_ghost(pointer_);
  }
}

void ChartEditPanel::on_text_input(const wds::interaction::TextInputEvent& event) {
  if (split_picker_open_ && split_search_focused_ && !event.text.empty()) {
    std::string next = split_search_text_;
    bool changed = false;
    for (char c : event.text) {
      if (c >= '0' && c <= '9') {
        next.push_back(c);
        changed = true;
      }
    }
    if (changed) {
      apply_split_search_text(next);
      split_search_caret_blink_t_ = 0.0f;
    }
    return;
  }
  if (!timing_popup_open_ || event.text.empty()) return;
  const bool bpm_mode = timing_popup_mode_ == TimingPopupMode::Bpm;
  std::string* field = bpm_mode                  ? &timing_bpm_text_
                       : timing_focus_field_ == 1 ? &timing_num_text_
                                                  : &timing_den_text_;
  bool changed = false;
  for (char c : event.text) {
    if ((c >= '0' && c <= '9') || (bpm_mode && c == '.')) {
      field->push_back(c);
      changed = true;
    }
  }
  if (changed) timing_caret_blink_t_ = 0.0f;
}

void ChartEditPanel::set_curve_fill_selection(CurveFillSelection selection) {
  curve_fill_selection_ = selection;
  if (curve_mode_active_) refresh_curve_ghosts();
}

std::vector<wds::chart_editor::NotationNote> ChartEditPanel::curve_ghost_notes() const {
  std::vector<wds::chart_editor::NotationNote> notes;
  notes.reserve(curve_ghosts_.size());
  for (const auto& ghost : curve_ghosts_) {
    if (ghost.visible) notes.push_back(ghost.note);
  }
  return notes;
}

void ChartEditPanel::sync_curve_mode() {
  if (!wds::interaction::is_curve_fill_modifiers(active_mods_)) {
    curve_dismissed_ = false;
  }
  const bool allowed = wds::interaction::is_curve_fill_placement_allowed(
      mode_ == Mode::PlaceHoldBody, hold_scratch_);
  const bool want = allowed && wds::interaction::is_curve_fill_modifiers(active_mods_) &&
                    !curve_dismissed_;
  if (!want) {
    if (curve_mode_active_) {
      curve_mode_active_ = false;
      curve_ghosts_.clear();
    }
    return;
  }
  if (!curve_mode_active_) {
    curve_origin_tick_ = hold_draft_.start_tick;
    curve_origin_lane_ = hold_draft_.lane;
    curve_mode_active_ = true;
    ghost_.visible = false;
  }
  refresh_curve_ghosts();
}

void ChartEditPanel::refresh_curve_ghosts() {
  curve_ghosts_.clear();
  if (!curve_mode_active_ || mode_ != Mode::PlaceHoldBody) return;
  sync_viewport();
  wds::chart_editor::ScratchHoldCurveRequest req;
  const int32_t width = std::max(1, default_width_);
  const int32_t lane_count = viewport_.grid().lane_count;
  const int32_t origin_lane =
      wds::chart_editor::clamp_lane_for_width(curve_origin_lane_, width, lane_count);
  req.start_tick = curve_origin_tick_;
  req.end_tick = viewport_.tick_at(pointer_.y);
  req.start_center = wds::chart_editor::scratch_hold_lane_center(origin_lane, width);
  req.end_center = static_cast<double>(viewport_.lane_left_at_f(pointer_.x, width)) +
                   0.5 * static_cast<double>(width - 1);
  req.width = width;
  req.lane_count = lane_count;
  req.subdivisions_per_beat = viewport_.grid().subdivisions_per_beat;
  req.algorithm = curve_fill_selection_.easing.algorithm;
  req.direction = curve_fill_selection_.easing.direction;
  req.parameter = curve_fill_selection_.easing.parameter;
  req.note_type = hold_scratch_ ? NoteType::ScratchHold : NoteType::Hold;
  auto bodies = wds::chart_editor::generate_scratch_hold_curve(req, engine_.document().timing());
  apply_gold_first_curve_body(bodies);
  if (bodies.empty()) return;
  if (hold_chain_prev_id_ >= 0) {
    NotationNote prev = hold_chain_prev_body_;
    wds::chart_editor::sync_scratch_chain_joint(prev, bodies.front());
    curve_ghosts_.push_back(GhostNote{prev, true});
  } else if (auto head = wds::chart_editor::make_auto_hold_head(engine_.document(), bodies.front())) {
    curve_ghosts_.push_back(GhostNote{*head, true});
  }
  for (const auto& body : bodies) {
    curve_ghosts_.push_back(GhostNote{body, true});
  }
}

void ChartEditPanel::cancel_curve_fill() {
  curve_dismissed_ = true;
  curve_mode_active_ = false;
  curve_ghosts_.clear();
  if (mode_ == Mode::PlaceHoldBody) {
    sync_hold_draft_to_pointer(HoldDraftSync::LocalDraftOnly);
  }
}

bool ChartEditPanel::commit_curve_fill() {
  if (!curve_mode_active_ || mode_ != Mode::PlaceHoldBody) return false;
  sync_viewport();
  refresh_curve_ghosts();
  wds::chart_editor::ScratchHoldCurveRequest req;
  const int32_t width = std::max(1, default_width_);
  const int32_t lane_count = viewport_.grid().lane_count;
  const int32_t origin_lane =
      wds::chart_editor::clamp_lane_for_width(curve_origin_lane_, width, lane_count);
  req.start_tick = curve_origin_tick_;
  req.end_tick = viewport_.tick_at(pointer_.y);
  req.start_center = wds::chart_editor::scratch_hold_lane_center(origin_lane, width);
  req.end_center = static_cast<double>(viewport_.lane_left_at_f(pointer_.x, width)) +
                   0.5 * static_cast<double>(width - 1);
  req.width = width;
  req.lane_count = lane_count;
  req.subdivisions_per_beat = viewport_.grid().subdivisions_per_beat;
  req.algorithm = curve_fill_selection_.easing.algorithm;
  req.direction = curve_fill_selection_.easing.direction;
  req.parameter = curve_fill_selection_.easing.parameter;
  req.note_type = hold_scratch_ ? NoteType::ScratchHold : NoteType::Hold;
  auto bodies = wds::chart_editor::generate_scratch_hold_curve(req, engine_.document().timing());
  apply_gold_first_curve_body(bodies);
  wds::chart_editor::ScratchHoldCurveCommitInput input;
  input.notes = engine_.document().notes();
  input.generated_bodies = bodies;
  input.previous_body_id = hold_chain_prev_id_;
  if (hold_chain_prev_id_ >= 0) input.previous_body_original = hold_chain_prev_body_;
  input.include_head = hold_chain_prev_id_ < 0;
  input.ticks_per_quarter = engine_.document().timing().ticks_per_quarter;
  input.next_id = engine_.document().next_note_id();
  const auto commit = wds::chart_editor::build_scratch_hold_curve_commit(input);
  if (!commit) return false;
  if (!engine_.execute_command(std::make_unique<wds::chart_editor::SetNotesCommand>(
          commit->before, commit->after, "Place curve hold"))) {
    return false;
  }
  engine_.rebuild_snapshot();

  std::unordered_set<int32_t> before_ids;
  for (const auto& note : commit->before) before_ids.insert(note.id);
  clear_hold_sel_focus();
  selected_.clear();
  for (const int32_t id : hold_chain_ids_) selected_.insert(id);
  for (const auto& note : engine_.document().notes()) {
    if (before_ids.count(note.id)) continue;
    if (wds::chart_editor::is_hold_head_note(note) ||
        wds::chart_editor::is_hold_with_tail(note.note_type)) {
      selected_.insert(note.id);
    }
  }

  curve_mode_active_ = false;
  curve_dismissed_ = false;
  curve_ghosts_.clear();
  mode_ = Mode::Idle;
  hold_stars_.clear();
  clear_hold_chain_state();
  update_ghost(pointer_);
  return true;
}

}  // namespace wds::ui

#pragma once

#include <wds/core/edit_grid.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/official_playfield.hpp>

#include <wds/interaction/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace wds::ui {
// Time-linear edit view (official chart fall speed): Y maps wall-clock ms, not
// ticks. Higher BPM packs equal tick gaps denser on screen while the playhead
// still moves at constant time speed.
class EditViewport {
 public:
  static constexpr float kJudgelineMarginBottom =
      wds::chart_editor::EditLeadIn::kJudgelineMarginBottom;
  // Matches PreviewVisualConfig::note_border_percent.
  static constexpr float kNoteBorderPercent = 0.02f;
  // Vertical stretch for flattened judgeline skins.
  static constexpr float kVerticalStretch = 2.0f;  // was 1.5; ×4/3 thicker edit notes/judgeline
  // dest_h matches official preview notes at the judgeline on the default
  // editor preview (1280×800 → stage 584.7552×328.9248). Pane size does not
  // scale dest_h; 「note厚度」 is GetNoteHeight Rx (Tap only). Mid-stars stay
  // square; their side follows edit width at the preview's star/lane ratio.
  static constexpr float kPreviewRefContentHeight = 328.9248f;
  static constexpr int32_t kMsPerHectom = wds::chart_editor::EditLeadIn::kMsPerHectom;

  void set_bounds(wds::interaction::Rect bounds) { bounds_ = bounds; }
  void set_grid(wds::chart_editor::EditGridConfig grid) { grid_ = grid; }
  void set_timing(wds::chart_editor::MusicTiming timing) { timing_ = std::move(timing); }
  void set_note_height_level(int level) {
    note_height_level_ = wds::chart_editor::official_clamp_note_height_level(level);
  }
  int note_height_level() const noexcept { return note_height_level_; }

  int32_t visible_ms() const noexcept {
    return wds::chart_editor::EditLeadIn::visible_ms_from_hectoms(grid_.visible_hectoms);
  }

  // Earliest scroll: chart start on the judgeline (blank = band below the line).
  // Positive delay still scrolls into music-before-chart blank; negative delay
  // can park tick 0 (music_ms = offset) on the judgeline.
  float min_scroll_ms() const noexcept {
    const int64_t tick0_ms = wds::chart_editor::tick_to_milliseconds(0, timing_);
    const float chart_start = static_cast<float>(std::min<int64_t>(0, tick0_ms));
    return chart_start - static_cast<float>(visible_ms()) * kJudgelineMarginBottom;
  }
  void set_scroll_ms(float ms) { scroll_ms_ = std::max(min_scroll_ms(), ms); }
  void scroll_by_ms(float delta_ms) { set_scroll_ms(scroll_ms_ + delta_ms); }
  float scroll_ms() const noexcept { return scroll_ms_; }

  const wds::interaction::Rect& bounds() const noexcept { return bounds_; }
  const wds::chart_editor::MusicTiming& timing() const noexcept { return timing_; }

  float ms_at_y(float y) const {
    const float visible = static_cast<float>(visible_ms());
    const float from_top = (y - bounds_.y) / std::max(bounds_.h, 1.0f) * visible;
    return scroll_ms_ + visible - from_top;
  }

  float y_at_ms(float ms) const {
    const float visible = static_cast<float>(visible_ms());
    return bounds_.y + (scroll_ms_ + visible - ms) * bounds_.h / visible;
  }

  int32_t tick_at(float y) const {
    const float ms = ms_at_y(y);
    const int32_t raw = wds::chart_editor::milliseconds_to_tick(
        static_cast<int64_t>(std::llround(ms)), timing_);
    return wds::chart_editor::snap_tick(static_cast<float>(std::max(0, raw)), grid_);
  }

  float y_at(int32_t tick) const {
    return y_at_ms(static_cast<float>(wds::chart_editor::tick_to_milliseconds(tick, timing_)));
  }

  // Tick span that intersects the current time window (for grid / note culling).
  std::pair<int32_t, int32_t> visible_tick_range() const {
    const float vis = static_cast<float>(visible_ms());
    const float ms_lo = scroll_ms_;
    const float ms_hi = std::max(ms_lo, scroll_ms_ + vis);
    const int32_t start = wds::chart_editor::milliseconds_to_tick(
        static_cast<int64_t>(std::llround(ms_lo)), timing_);
    const int32_t end = wds::chart_editor::milliseconds_to_tick(
        static_cast<int64_t>(std::llround(ms_hi)), timing_);
    return {std::max(0, start), std::max(std::max(0, start), end)};
  }

  // Continuous left-edge lane under `x` before integer snap (width-centered).
  float lane_left_at_f(float x, int32_t width = 1) const {
    const int32_t w = std::max(1, width);
    const float p =
        (x - bounds_.x) * static_cast<float>(grid_.lane_count) / std::max(bounds_.w, 1.0f);
    return p - static_cast<float>(w) * 0.5f;
  }
  // Left lane so the note's horizontal center snaps to the nearest track under `x`.
  int32_t lane_at(float x, int32_t width = 1) const {
    const int32_t w = std::max(1, width);
    const int32_t lane = wds::chart_editor::round_to_int_tick(lane_left_at_f(x, w));
    return wds::chart_editor::clamp_lane_for_width(lane, w, grid_.lane_count);
  }
  float x_at(int32_t lane) const { return bounds_.x + lane * bounds_.w / grid_.lane_count; }
  float lane_width(int32_t width) const { return width * bounds_.w / grid_.lane_count; }
  float note_height_px() const {
    return wds::chart_editor::official_preview_note_height_px(note_height_level_,
                                                             kPreviewRefContentHeight);
  }
  float mid_star_size_px() const {
    return lane_width(1) *
           wds::chart_editor::official_preview_sound_note_to_lane_width_ratio();
  }
  float judgeline_y() const {
    return bounds_.y + bounds_.h * (1.0f - kJudgelineMarginBottom);
  }
  float judgeline_height_px() const {
    // ~60% of prior thickness (notes keep kVerticalStretch unchanged).
    return std::clamp(bounds_.h * 0.133f * 0.35f * kVerticalStretch * 0.6f, 7.0f, 26.0f);
  }
  float tap_visual_world_width(int32_t width) const {
    return wds::chart_editor::official_tap_visual_width(
        wds::chart_editor::official_note_width(std::max(1, width)));
  }
  float hold_visual_world_width(int32_t width) const {
    return wds::chart_editor::official_hold_line_visual_width(
        wds::chart_editor::official_note_width(std::max(1, width)));
  }
  float note_inset_px(int32_t width) const { return visual_inset_px(width, tap_visual_world_width(width)); }
  float hold_inset_px(int32_t width) const {
    return visual_inset_px(width, hold_visual_world_width(width));
  }

  // Screen rect of a Sound / ScratchSound mid-star. Side scales with one lane
  // (edit width / lane count) at the preview judgeline ratio, and stays square.
  // 厚度 does not apply; center follows the hold span / tick.
  wds::interaction::Rect mid_star_screen_rect(const wds::chart_editor::NotationNote& note) const {
    const float inset = note_inset_px(note.width);
    const float x = x_at(note.lane) + inset;
    const float width = std::max(4.0f, lane_width(note.width) - inset * 2.0f);
    const float y0 = y_at(note.start_tick);
    const float side = mid_star_size_px();
    const float cx = x + width * 0.5f;
    return {cx - side * 0.5f, y0 - side * 0.5f, side, side};
  }

  // Playhead always on the judgeline at 1:1 — no lead-in ease / speed change.
  float scroll_ms_for_playhead(float now_ms) const {
    return now_ms - static_cast<float>(visible_ms()) * kJudgelineMarginBottom;
  }

  void sync_scroll_to_playhead_ms(double now_ms) {
    set_scroll_ms(scroll_ms_for_playhead(static_cast<float>(now_ms)));
  }

  void sync_scroll_to_playhead(int32_t now_tick) {
    sync_scroll_to_playhead_ms(static_cast<double>(
        wds::chart_editor::tick_to_milliseconds(now_tick, timing_)));
  }

  const wds::chart_editor::EditGridConfig& grid() const noexcept { return grid_; }

 private:
  float visual_inset_px(int32_t width, float visual_world) const {
    const float notation = wds::chart_editor::official_note_width(std::max(1, width));
    const float full = lane_width(width);
    const float visual_px = full * (visual_world / std::max(notation, 1e-6f));
    return std::max(0.0f, (full - visual_px) * 0.5f);
  }

  wds::interaction::Rect bounds_{};
  wds::chart_editor::EditGridConfig grid_{};
  wds::chart_editor::MusicTiming timing_{};
  int note_height_level_ = wds::chart_editor::kOfficialDefaultNoteHeightLevel;
  // May be negative within [-judgeline_margin, +∞) for blank below the line.
  float scroll_ms_ = 0.0f;
};
}  // namespace wds::ui

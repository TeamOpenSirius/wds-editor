#pragma once

#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>
#include <wds/core/timing_map.hpp>

#include <wds/interaction/types.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/renderer/skin_catalog.hpp>
#include <wds/renderer/split_line_skins.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wds::ui {

class EditViewport;

// Lane boundary indices for a 12-lane split (matches playback_preview.cpp).
void split_boundaries_12(int32_t split_count, std::vector<int32_t>& out);

// Track that contains probe_lane under a split_count partitioning of lane_count.
// out_lane / out_width are the inclusive start and span of that track.
bool split_track_for_lane(int32_t split_count, int32_t lane_count, int32_t probe_lane,
                          int32_t& out_lane, int32_t& out_width) noexcept;

wds::interaction::Color split_color_for_id(int32_t color_id) noexcept;

// Per-line tint for multi-suffix color ids (falls back to split_color_for_id).
wds::interaction::Color split_slot_color(int32_t color_id, int32_t line_slot,
                                         const wds::renderer::SkinCatalog* skin) noexcept;

// Split coverage for default-lane-guide hiding. Fade windows are wall-clock
// seconds (PreviewConfig), never BPM/subdivision ticks — edit and official share
// the same real-time behavior.
struct SplitCoverageMs {
  int64_t fade_start_ms = 0;
  int64_t fade_end_ms = 0;
  int64_t steady_start_ms = 0;
  int64_t steady_end_ms = 0;
  int32_t split_count = 1;
  int32_t color_id = 0;
};

std::vector<SplitCoverageMs> collect_split_coverage_ms(
    const std::vector<wds::chart_editor::NotationNote>& notes,
    const wds::chart_editor::MusicTiming& timing,
    const wds::chart_editor::PreviewConfig& preview);

// Merge fade windows intersecting [view_ms_lo, view_ms_hi] into sorted, non-overlapping
// segments used to punch holes in default vertical lane guides.
std::vector<std::pair<float, float>> merged_split_hide_ranges_ms(
    const std::vector<wds::chart_editor::NotationNote>& notes,
    const wds::chart_editor::MusicTiming& timing,
    const wds::chart_editor::PreviewConfig& preview, float view_ms_lo, float view_ms_hi);

// Opacity along a split effect: 0 at outer fade edge → 1 at steady → 0 at outer fade edge.
// Prefer ms sampling in the edit view (time-linear Y); tick helper converts via timing.
float split_line_opacity_at_ms(const wds::chart_editor::NotationNote& note,
                               const wds::chart_editor::MusicTiming& timing,
                               const wds::chart_editor::PreviewConfig& preview,
                               int64_t time_ms) noexcept;
float split_line_opacity_at_tick(const wds::chart_editor::NotationNote& note,
                                 const wds::chart_editor::MusicTiming& timing,
                                 const wds::chart_editor::PreviewConfig& preview,
                                 int32_t tick) noexcept;

// True when wall-clock time at `tick` falls inside any split fade/steady window.
const wds::chart_editor::NotationNote* split_note_covering_tick(
    const std::vector<wds::chart_editor::NotationNote>& notes,
    const wds::chart_editor::MusicTiming& timing,
    const wds::chart_editor::PreviewConfig& preview, int32_t tick);

struct GutterLabelHit {
  int32_t note_id = -1;
  bool is_start = true;
  // Anchor tick for paint/hit priority (earlier = on top / first to receive input).
  int32_t anchor_tick = 0;
  wds::interaction::Rect bounds{};
};

enum class TimingLabelKind { Bpm, Meter };

struct TimingLabelHit {
  int32_t point_tick = 0;
  TimingLabelKind kind = TimingLabelKind::Bpm;
  wds::interaction::Rect bounds{};
};

// Hits are sorted by time ascending: earlier labels sit on top and win hit-tests.
// Paint callers should draw in reverse so later labels are underneath.

// Subdivision + beat + measure lines (uses viewport timing + grid).
void paint_horizontal_grid(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                           const wds::interaction::Rect& area);

void paint_timing_gutter(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                         const wds::interaction::Rect& gutter,
                         const wds::chart_editor::MusicTiming& timing, int32_t view_start_tick,
                         int32_t view_end_tick, bool show_timing_marks = true);

// Far-right column: 1-based measure indices centered on each measure line.
void paint_measure_index_gutter(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                                const wds::interaction::Rect& gutter,
                                const wds::chart_editor::MusicTiming& timing,
                                int32_t view_start_tick, int32_t view_end_tick);

void paint_split_gutter(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                      const wds::interaction::Rect& gutter,
                      const std::vector<wds::chart_editor::NotationNote>& notes,
                      const wds::renderer::SkinCatalog* skin, bool show_beat_grid = true);

// Mini playfield thumbnail: 12 lanes + split-line skins at official boundaries.
void paint_split_lane_preview(wds::interaction::UiPainter& painter,
                              const wds::interaction::Rect& area, int32_t split_count,
                              int32_t color_id, const wds::renderer::SkinCatalog* skin);

std::vector<GutterLabelHit> build_split_label_hits(
    const EditViewport& viewport, const wds::interaction::Rect& gutter,
    const std::vector<wds::chart_editor::NotationNote>& notes);

// Format BPM for gutter / timing popup (keeps decimals, strips trailing zeros).
std::string format_bpm_label(double bpm);

std::vector<TimingLabelHit> build_timing_label_hits(const EditViewport& viewport,
                                                    const wds::interaction::Rect& gutter,
                                                    const wds::chart_editor::MusicTiming& timing);

// Snap pointer to a subdivision line for BPM placement (nullopt if too far / occupied).
std::optional<int32_t> timing_bpm_tick_at(const EditViewport& viewport,
                                          const wds::interaction::Rect& gutter,
                                          const wds::chart_editor::MusicTiming& timing,
                                          wds::interaction::Vec2 point);

// Snap pointer to a measure line for meter placement (nullopt if too far / occupied).
std::optional<int32_t> timing_measure_tick_at(const EditViewport& viewport,
                                              const wds::interaction::Rect& gutter,
                                              const wds::chart_editor::MusicTiming& timing,
                                              wds::interaction::Vec2 point);

// Placement-preview band geometry (same as real start / BPM / meter labels).
wds::interaction::Rect split_start_label_bounds(const EditViewport& viewport,
                                                const wds::interaction::Rect& gutter,
                                                int32_t tick);
wds::interaction::Rect bpm_label_bounds(const EditViewport& viewport,
                                        const wds::interaction::Rect& gutter, int32_t tick);
wds::interaction::Rect meter_label_bounds(const EditViewport& viewport,
                                          const wds::interaction::Rect& gutter, int32_t tick);

// Solid placement ghosts for gutter labels (no text yet).
inline constexpr wds::interaction::Color kSplitLabelGhostColor{0.22f, 0.48f, 0.95f, 0.45f};
inline constexpr wds::interaction::Color kBpmLabelGhostColor{0.48f, 0.30f, 0.14f, 0.45f};
inline constexpr wds::interaction::Color kMeterLabelGhostColor{0.10f, 0.38f, 0.24f, 0.45f};

}  // namespace wds::ui

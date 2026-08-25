#include "wds/ui/regions/edit/edit_gutters.hpp"

#include "wds/ui/regions/edit/edit_viewport.hpp"

#include <wds/chart_render/preview_visual_config.hpp>
#include <wds/chart_render/split_line_official_colors.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>

#include <wds/interaction/theme.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wds::ui {
namespace {

using wds::chart_editor::NotationNote;
using wds::interaction::Color;
using wds::interaction::Rect;
using wds::interaction::Vec2;

float gutter_font_px() { return wds::interaction::theme::kFontSizeGutter; }
float split_label_h() { return wds::interaction::theme::px(28.0f); }
float timing_label_h() { return wds::interaction::theme::px(28.0f); }
float split_band_gap() { return wds::interaction::theme::px(1.0f); }
float gutter_label_pad() { return wds::interaction::theme::px(2.0f); }
constexpr float kLineSnapPx = 8.0f;

bool rects_overlap(const Rect& a, const Rect& b) noexcept {
  return a.x < b.right() && a.right() > b.x && a.y < b.bottom() && a.bottom() > b.y;
}

constexpr Color kSplitStartColor{0.22f, 0.48f, 0.95f, 1.0f};
constexpr Color kSplitEndColor{0.92f, 0.28f, 0.28f, 1.0f};
constexpr Color kBpmLabelColor{0.48f, 0.30f, 0.14f, 0.96f};
constexpr Color kMeterLabelColor{0.10f, 0.38f, 0.24f, 0.96f};
constexpr Color kSubdivLineColor{0.28f, 0.30f, 0.34f, 0.75f};
constexpr Color kBeatLineColor{0.50f, 0.52f, 0.56f, 0.88f};
constexpr Color kMeasureLineColor{0.72f, 0.74f, 0.78f, 0.95f};

}  // namespace

std::string format_bpm_label(double bpm) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", bpm);
  std::string s(buf);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
      s.pop_back();
    }
  }
  return s.empty() ? "0" : s;
}

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

bool split_track_for_lane(int32_t split_count, int32_t lane_count, int32_t probe_lane,
                          int32_t& out_lane, int32_t& out_width) noexcept {
  const int32_t n = std::max(1, lane_count);
  const int32_t lane = std::clamp(probe_lane, 0, n - 1);
  std::vector<int32_t> mids;
  split_boundaries_12(split_count, mids);
  // Boundaries store the last lane index of each track before the final one;
  // vertical lines are drawn at mid + 1 (see draw_split_boundaries).
  int32_t start = 0;
  for (const int32_t mid : mids) {
    const int32_t next = mid + 1;
    if (lane < next) {
      out_lane = start;
      out_width = next - start;
      return out_width > 0;
    }
    start = next;
  }
  out_lane = start;
  out_width = n - start;
  return out_width > 0;
}

Color split_color_for_id(int32_t color_id) noexcept {
  const uint32_t h = static_cast<uint32_t>(color_id) * 2654435761u;
  const float hue = (h % 360) / 360.0f;
  const float s = 0.72f;
  const float v = 0.88f;
  const float c = v * s;
  const float x = c * (1.0f - std::fabs(std::fmod(hue * 6.0f, 2.0f) - 1.0f));
  const float m = v - c;
  float r = 0, g = 0, b = 0;
  const int sector = static_cast<int>(hue * 6.0f) % 6;
  switch (sector) {
    case 0:
      r = c;
      g = x;
      break;
    case 1:
      r = x;
      g = c;
      break;
    case 2:
      g = c;
      b = x;
      break;
    case 3:
      g = x;
      b = c;
      break;
    case 4:
      r = x;
      b = c;
      break;
    default:
      r = c;
      b = x;
      break;
  }
  return {r + m, g + m, b + m, 1.0f};
}

Color split_slot_color(int32_t color_id, int32_t world_index, int32_t split_count,
                       const wds::renderer::SkinCatalog* /*skin*/) noexcept {
  const int32_t official =
      wds::chart_editor::split_color_slot(color_id, split_count, world_index);
  float sr = 1.0f, sg = 1.0f, sb = 1.0f, sa = 1.0f;
  if (wds::chart_render::official_split_line_color(color_id, official, sr, sg, sb, sa)) {
    return {sr, sg, sb, sa};
  }
  return split_color_for_id(color_id);
}

void apply_official_split_rgb_opacity(wds::interaction::Color& c) noexcept {
  const float k = wds::renderer::PreviewVisualConfig{}.split_line_opacity;
  wds::chart_render::apply_split_line_opacity(c.r, c.g, c.b, c.a, k, 1.0f);
}

std::vector<int32_t> split_picker_color_ids() {
  return wds::chart_render::official_split_color_ids();
}

namespace {
int64_t split_fade_ms(float seconds) noexcept {
  return std::max<int64_t>(
      1, static_cast<int64_t>(std::llround(static_cast<double>(seconds) * 1000.0)));
}
}  // namespace

std::vector<SplitCoverageMs> collect_split_coverage_ms(
    const std::vector<NotationNote>& notes, const wds::chart_editor::MusicTiming& timing,
    const wds::chart_editor::PreviewConfig& preview) {
  std::vector<SplitCoverageMs> out;
  const int64_t appear_ms = split_fade_ms(preview.split_line_animation_start_sec);
  const int64_t disappear_ms = split_fade_ms(preview.split_line_animation_end_sec);
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    SplitCoverageMs range;
    range.steady_start_ms = note.start_ms(timing);
    range.steady_end_ms = std::max(range.steady_start_ms, note.end_ms(timing));
    range.fade_start_ms = range.steady_start_ms - appear_ms;
    range.fade_end_ms = range.steady_end_ms + disappear_ms;
    range.split_count = wds::chart_editor::get_split_count(note.gimmick_type);
    range.color_id = note.scratch_length;
    out.push_back(range);
  }
  return out;
}

std::vector<std::pair<float, float>> merged_split_hide_ranges_ms(
    const std::vector<NotationNote>& notes, const wds::chart_editor::MusicTiming& timing,
    const wds::chart_editor::PreviewConfig& preview, float view_ms_lo, float view_ms_hi) {
  std::vector<std::pair<float, float>> covered;
  for (const auto& range : collect_split_coverage_ms(notes, timing, preview)) {
    const float lo = std::max(view_ms_lo, static_cast<float>(range.fade_start_ms));
    const float hi = std::min(view_ms_hi, static_cast<float>(range.fade_end_ms));
    if (hi > lo) covered.emplace_back(lo, hi);
  }
  std::sort(covered.begin(), covered.end());
  std::vector<std::pair<float, float>> merged;
  for (const auto& seg : covered) {
    if (merged.empty() || seg.first > merged.back().second) {
      merged.push_back(seg);
    } else {
      merged.back().second = std::max(merged.back().second, seg.second);
    }
  }
  return merged;
}

float split_line_opacity_at_ms(const NotationNote& note,
                               const wds::chart_editor::MusicTiming& timing,
                               const wds::chart_editor::PreviewConfig& preview,
                               int64_t time_ms) noexcept {
  if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) return 0.0f;
  const int64_t start_ms = note.start_ms(timing);
  const int64_t end_ms = std::max(start_ms, note.end_ms(timing));
  const int64_t appear_ms = split_fade_ms(preview.split_line_animation_start_sec);
  const int64_t disappear_ms = split_fade_ms(preview.split_line_animation_end_sec);
  const int64_t fade_start = start_ms - appear_ms;
  const int64_t fade_end = end_ms + disappear_ms;
  if (time_ms < fade_start || time_ms > fade_end) return 0.0f;
  if (time_ms < start_ms) {
    return std::clamp(static_cast<float>(time_ms - fade_start) / static_cast<float>(appear_ms),
                      0.0f, 1.0f);
  }
  if (time_ms > end_ms) {
    return std::clamp(static_cast<float>(fade_end - time_ms) / static_cast<float>(disappear_ms),
                      0.0f, 1.0f);
  }
  return 1.0f;
}

float split_line_opacity_at_tick(const NotationNote& note,
                                 const wds::chart_editor::MusicTiming& timing,
                                 const wds::chart_editor::PreviewConfig& preview,
                                 int32_t tick) noexcept {
  return split_line_opacity_at_ms(
      note, timing, preview,
      wds::chart_editor::tick_to_milliseconds(tick, timing));
}

const NotationNote* split_note_covering_tick(const std::vector<NotationNote>& notes,
                                             const wds::chart_editor::MusicTiming& timing,
                                             const wds::chart_editor::PreviewConfig& preview,
                                             int32_t tick) {
  // Cover check is wall-clock (same as preview fade), not BPM→tick conversion /
  // subdivision banding — otherwise edit-mode hide edges snap to 拍内分割.
  const int64_t time_ms =
      wds::chart_editor::tick_to_milliseconds(tick, timing);
  for (const auto& note : notes) {
    if (split_line_opacity_at_ms(note, timing, preview, time_ms) > 0.0f) return &note;
  }
  return nullptr;
}

void paint_horizontal_grid(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                           const Rect& area) {
  const auto& grid = viewport.grid();
  const auto& timing = viewport.timing();
  const auto range = viewport.visible_tick_range();
  const int32_t start = range.first;
  const int32_t end = range.second;
  if (end < start) return;

  // Screen-space LOD: when consecutive ticks land < ~1.25px apart, further lines alias.
  constexpr float kMinLineGapPx = 1.25f;
  auto draw_h = [&](int32_t tick, float thickness, const Color& color, float& last_y,
                    bool& have_last) {
    const float y = viewport.y_at(tick);
    if (y < area.y || y > area.bottom()) return;
    if (have_last && std::abs(y - last_y) < kMinLineGapPx) return;
    painter.fill_rect({area.x, y - thickness * 0.5f, area.w, thickness}, color, 0.0f, 0.905f);
    last_y = y;
    have_last = true;
  };

  const auto measure_ticks =
      wds::chart_editor::measure_ticks_in_range(start, end, timing);
  const auto beat_ticks = wds::chart_editor::beat_ticks_in_range(start, end, timing);
  auto is_in = [](const std::vector<int32_t>& ticks, int32_t tick) {
    return std::binary_search(ticks.begin(), ticks.end(), tick);
  };

  // Subdivision ticks: exact in-beat offsets (i*beat)/subdivs so odd subdivs
  // (7/9/11/13) still land flush with the next beat line.
  std::vector<int32_t> subdiv_ticks;
  {
    const int32_t tpq = std::max(1, timing.ticks_per_quarter);
    const int32_t subdivs = std::max(1, grid.subdivisions_per_beat);
    std::vector<const wds::chart_editor::TimingPoint*> meters;
    for (const auto& p : timing.points) {
      if (p.has_meter) meters.push_back(&p);
    }
    if (meters.empty()) {
      // Fallback: constant TPQ grid when timing is empty/unnormalized.
      wds::chart_editor::EditGridConfig cfg = grid;
      cfg.ticks_per_quarter = tpq;
      cfg.subdivisions_per_beat = subdivs;
      subdiv_ticks =
          wds::chart_editor::subdivision_ticks_in_range(start, end, cfg);
    } else {
      subdiv_ticks = wds::chart_editor::subdivision_ticks_in_range(
          start, end, timing, subdivs);
    }
  }

  painter.reserve_rects(subdiv_ticks.size() + beat_ticks.size() + measure_ticks.size() + 8);

  float last_sub_y = 0.0f;
  bool have_sub = false;
  for (int32_t tick : subdiv_ticks) {
    if (is_in(beat_ticks, tick) || is_in(measure_ticks, tick)) continue;
    draw_h(tick, 1.0f, kSubdivLineColor, last_sub_y, have_sub);
  }

  float last_beat_y = 0.0f;
  bool have_beat = false;
  for (int32_t tick : beat_ticks) {
    if (is_in(measure_ticks, tick)) continue;
    draw_h(tick, 1.5f, kBeatLineColor, last_beat_y, have_beat);
  }

  float last_measure_y = 0.0f;
  bool have_measure = false;
  for (int32_t tick : measure_ticks) {
    draw_h(tick, 2.5f, kMeasureLineColor, last_measure_y, have_measure);
  }
}

void paint_timing_gutter(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                         const Rect& gutter, const wds::chart_editor::MusicTiming& timing,
                         int32_t /*view_start_tick*/, int32_t /*view_end_tick*/,
                         bool show_timing_marks) {
  painter.fill_rect(gutter, {0.06f, 0.07f, 0.09f, 1.0f}, 0.0f, 0.85f);
  if (!show_timing_marks) {
    return;  // Official charts have no authored BPM/meter — hide grid + labels.
  }
  paint_horizontal_grid(painter, viewport, gutter);

  const auto hits = build_timing_label_hits(viewport, gutter, timing);
  // Later ticks first so earlier labels paint on top.
  for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
    const auto& hit = *it;
    const auto& p =
        *std::find_if(timing.points.begin(), timing.points.end(),
                      [&](const wds::chart_editor::TimingPoint& tp) {
                        return tp.tick == hit.point_tick;
                      });
    const float gutter_px = gutter_font_px();
    if (hit.kind == TimingLabelKind::Bpm) {
      painter.fill_rect(hit.bounds, kBpmLabelColor, 2.0f, 0.93f);
      painter.label(hit.bounds, format_bpm_label(p.bpm), {0.98f, 0.93f, 0.84f, 1.0f}, 0.931f,
                    false, gutter_px);
    } else {
      const std::string meter =
          std::to_string(p.numerator) + "/" + std::to_string(p.denominator);
      painter.fill_rect(hit.bounds, kMeterLabelColor, 2.0f, 0.93f);
      painter.label(hit.bounds, meter, {0.86f, 0.96f, 0.90f, 1.0f}, 0.931f, false, gutter_px);
    }
  }
}

void paint_measure_index_gutter(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                                const Rect& gutter, const wds::chart_editor::MusicTiming& timing,
                                int32_t view_start_tick, int32_t view_end_tick) {
  painter.fill_rect(gutter, {0.05f, 0.06f, 0.08f, 1.0f}, 0.0f, 0.85f);
  if (view_end_tick < view_start_tick) return;

  // Number from the chart start so indices stay correct when scrolled mid-song.
  const auto ticks =
      wds::chart_editor::measure_ticks_in_range(0, view_end_tick, timing);
  const float font_px = gutter_font_px();
  const float label_h = timing_label_h();
  for (size_t i = 0; i < ticks.size(); ++i) {
    const int32_t tick = ticks[i];
    if (tick < view_start_tick) continue;
    const float y = viewport.y_at(tick);
    if (y < gutter.y - label_h || y > gutter.bottom() + label_h) continue;
    const Rect bounds{gutter.x, y - label_h * 0.5f, gutter.w, label_h};
    painter.label(bounds, std::to_string(static_cast<int>(i) + 1),
                  {0.78f, 0.80f, 0.84f, 0.95f}, 0.932f, false, font_px);
  }
}

void paint_split_gutter(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                        const Rect& gutter, const std::vector<NotationNote>& notes,
                        const wds::renderer::SkinCatalog* /*skin*/, bool show_beat_grid) {
  painter.fill_rect(gutter, {0.05f, 0.06f, 0.08f, 1.0f}, 0.0f, 0.85f);
  if (show_beat_grid) {
    paint_horizontal_grid(painter, viewport, gutter);
  }

  const auto hits = build_split_label_hits(viewport, gutter, notes);
  for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
    const auto& hit = *it;
    const Color c = hit.is_start ? kSplitStartColor : kSplitEndColor;
    // Bands only — text is painted in ChartEditPanel::paint_overlays so it stays
    // above skinned notes (avoids a second mismatched font size).
    painter.fill_rect(hit.bounds, c, 3.0f, 0.94f);
    painter.fill_rect(hit.bounds.inset(1.0f, 1.0f), {0.08f, 0.08f, 0.10f, 0.40f}, 2.0f, 0.941f);
  }
}

void paint_split_lane_preview(wds::interaction::UiPainter& painter, const Rect& area,
                              int32_t split_count, int32_t color_id,
                              const wds::renderer::SkinCatalog* skin) {
  constexpr int32_t kLanes = 12;
  // Only effect split boundaries — no gray default lane dividers.
  std::vector<int32_t> mids;
  split_boundaries_12(split_count, mids);
  const float line_w = std::clamp(area.w / static_cast<float>(kLanes) * 0.14f, 1.5f, 2.5f);
  // Edge lines are centered on area.x / area.right; widen the black plate so the
  // soft sprite does not composite over the gray cell.
  const float bg_pad_x = line_w * 0.5f + 1.0f;
  const Rect bg{area.x - bg_pad_x, area.y, area.w + bg_pad_x * 2.0f, area.h};
  painter.fill_rect(bg, {0.02f, 0.03f, 0.05f, 1.0f}, 2.0f, 0.996f);

  auto draw_edge = [&](int32_t edge_lane, int32_t slot) {
    const float x = std::floor(area.x + static_cast<float>(edge_lane) * area.w /
                                            static_cast<float>(kLanes) +
                                0.5f);
    const Rect line{x - line_w * 0.5f, area.y + 2.0f, line_w, area.h - 4.0f};
    auto c = split_slot_color(color_id, slot, split_count, skin);
    if (c.a < 0.02f) return;
    apply_official_split_rgb_opacity(c);
    if (skin != nullptr && skin->soft_split_line) {
      painter.sprite(line, skin->soft_split_line, {c.r, c.g, c.b, 1.0f}, 0.997f);
      return;
    }
    painter.fill_rect(line, c, 1.0f, 0.997f);
  };
  draw_edge(0, 0);
  int32_t slot = 1;
  for (int32_t b : mids) draw_edge(b + 1, slot++);
  draw_edge(kLanes, std::max(1, split_count));
}

std::vector<GutterLabelHit> build_split_label_hits(const EditViewport& viewport, const Rect& gutter,
                                                   const std::vector<NotationNote>& notes) {
  const float label_w = std::max(1.0f, gutter.w - gutter_label_pad() * 2.0f);
  const float label_x = gutter.x + gutter_label_pad();
  std::vector<GutterLabelHit> out;
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    const int32_t start_tick = note.start_tick;
    const int32_t end_tick = std::max(note.start_tick, note.end_tick);
    const float y0 = viewport.y_at(note.start_tick);
    const float y1 = viewport.y_at(end_tick);
    // Upper band = start, lower band = end. No anti-overlap offsets.
    if (y0 >= gutter.y - split_label_h() * 2.0f && y0 <= gutter.bottom() + split_label_h()) {
      GutterLabelHit hit;
      hit.note_id = note.id;
      hit.is_start = true;
      hit.anchor_tick = start_tick;
      hit.bounds = {label_x, y0 - split_label_h() - split_band_gap(), label_w, split_label_h()};
      out.push_back(hit);
    }
    if (y1 >= gutter.y - split_label_h() && y1 <= gutter.bottom() + split_label_h() * 2.0f &&
        note.end_tick > note.start_tick) {
      GutterLabelHit hit;
      hit.note_id = note.id;
      hit.is_start = false;
      hit.anchor_tick = end_tick;
      hit.bounds = {label_x, y1 + split_band_gap(), label_w, split_label_h()};
      out.push_back(hit);
    }
  }
  // Ascending time: hit-test prefers earlier; paint should reverse-iterate.
  std::sort(out.begin(), out.end(), [](const GutterLabelHit& a, const GutterLabelHit& b) {
    if (a.anchor_tick != b.anchor_tick) return a.anchor_tick < b.anchor_tick;
    if (a.is_start != b.is_start) return a.is_start;  // start before end at same tick
    return a.note_id < b.note_id;
  });
  return out;
}

std::vector<TimingLabelHit> build_timing_label_hits(const EditViewport& viewport, const Rect& gutter,
                                                    const wds::chart_editor::MusicTiming& timing) {
  std::vector<TimingLabelHit> out;
  for (const auto& p : timing.points) {
    // Visible label ⇒ clickable: include by painted bounds, not only anchor-line Y.
    if (p.has_bpm) {
      TimingLabelHit hit;
      hit.point_tick = p.tick;
      hit.kind = TimingLabelKind::Bpm;
      hit.bounds = bpm_label_bounds(viewport, gutter, p.tick);
      if (rects_overlap(hit.bounds, gutter)) out.push_back(hit);
    }
    if (p.has_meter) {
      TimingLabelHit hit;
      hit.point_tick = p.tick;
      hit.kind = TimingLabelKind::Meter;
      hit.bounds = meter_label_bounds(viewport, gutter, p.tick);
      if (rects_overlap(hit.bounds, gutter)) out.push_back(hit);
    }
  }
  std::sort(out.begin(), out.end(), [](const TimingLabelHit& a, const TimingLabelHit& b) {
    if (a.point_tick != b.point_tick) return a.point_tick < b.point_tick;
    return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  });
  return out;
}

std::optional<int32_t> timing_bpm_tick_at(const EditViewport& viewport, const Rect& gutter,
                                          const wds::chart_editor::MusicTiming& timing,
                                          Vec2 point) {
  if (!gutter.contains(point)) return std::nullopt;
  const int32_t raw = viewport.tick_at(point.y);
  const int32_t snapped = wds::chart_editor::snap_to_subdivision(
      raw, timing, viewport.grid().subdivisions_per_beat);
  const float y = viewport.y_at(snapped);
  if (std::abs(point.y - y) > kLineSnapPx) return std::nullopt;
  for (const auto& p : timing.points) {
    if (p.tick == snapped && p.has_bpm) return std::nullopt;
  }
  return snapped;
}

std::optional<int32_t> timing_measure_tick_at(const EditViewport& viewport, const Rect& gutter,
                                              const wds::chart_editor::MusicTiming& timing,
                                              Vec2 point) {
  if (!gutter.contains(point)) return std::nullopt;
  const int tick = viewport.tick_at(point.y);
  const int32_t measure = wds::chart_editor::snap_to_measure(tick, timing);
  const float y = viewport.y_at(measure);
  if (std::abs(point.y - y) > kLineSnapPx) return std::nullopt;
  for (const auto& p : timing.points) {
    if (p.tick == measure && p.has_meter) return std::nullopt;
  }
  return measure;
}

Rect split_start_label_bounds(const EditViewport& viewport, const Rect& gutter, int32_t tick) {
  const float label_w = std::max(1.0f, gutter.w - gutter_label_pad() * 2.0f);
  const float label_x = gutter.x + gutter_label_pad();
  const float y = viewport.y_at(tick);
  return {label_x, y - split_label_h() - split_band_gap(), label_w, split_label_h()};
}

Rect bpm_label_bounds(const EditViewport& viewport, const Rect& gutter, int32_t tick) {
  const float label_w = std::max(1.0f, gutter.w - gutter_label_pad() * 2.0f);
  const float label_x = gutter.x + gutter_label_pad();
  const float y = viewport.y_at(tick);
  return {label_x, y - timing_label_h() - split_band_gap(), label_w, timing_label_h()};
}

Rect meter_label_bounds(const EditViewport& viewport, const Rect& gutter, int32_t tick) {
  const float label_w = std::max(1.0f, gutter.w - gutter_label_pad() * 2.0f);
  const float label_x = gutter.x + gutter_label_pad();
  const float y = viewport.y_at(tick);
  return {label_x, y + split_band_gap(), label_w, timing_label_h()};
}

}  // namespace wds::ui

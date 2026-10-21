#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/qt_input_adapter.hpp"
#include "wds/ui/qt/split_effect_snapshot.hpp"
#include "wds/ui/qt/split_picker_dialog.hpp"
#include "wds/ui/qt/speed_edit_dialog.hpp"
#include "wds/ui/qt/timing_edit_dialog.hpp"
#include <wds/audio/waveform_overview.hpp>
#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/split_fade.hpp>
#include <wds/core/split_effect_gate.hpp>
#include <wds/chart_render/note_draw_order.hpp>
#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/common/crash_input_journal.hpp>
#include <wds/interaction/theme.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/ui/regions/edit/edit_gutters.hpp>

#include <QCursor>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVarLengthArray>
#include <QShowEvent>
#include <QWheelEvent>
#include <QDir>
#include <QFont>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <cstring>
#include <string>

namespace wds::ui {
namespace {

QColor qcolor(const wds::interaction::Color& c) {
  return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f),
                          std::clamp(c.b, 0.0f, 1.0f), std::clamp(c.a, 0.0f, 1.0f));
}

QRectF qrect(const wds::interaction::Rect& r) { return {r.x, r.y, r.w, r.h}; }

std::uint8_t pack_mods(const wds::interaction::Modifiers& mods) {
  return static_cast<std::uint8_t>((mods.shift ? 1 : 0) | (mods.control ? 2 : 0) |
                                   (mods.alt ? 4 : 0) | (mods.super ? 8 : 0));
}

struct ChartEditJournal {
  ChartEditJournal(wds::common::CrashInputKind kind, float x, float y, float dx, float dy,
                   std::int32_t key, std::uint8_t mods, std::uint8_t button,
                   std::uint8_t repeat) {
    wds::common::journal_begin_event(kind, x, y, dx, dy, key, mods, button, repeat);
    wds::common::journal_set_route("ChartEditPanel", wds::common::CrashRouteVia::Host);
  }
  ~ChartEditJournal() { wds::common::journal_end_event(); }
  ChartEditJournal(const ChartEditJournal&) = delete;
  ChartEditJournal& operator=(const ChartEditJournal&) = delete;
};

QPixmap pixmap_from_hold_bake(bool scratch) {
  const auto px = wds::chart_render::bake_hold_long_rgba(scratch);
  const QImage img(px.data(), wds::chart_render::kHoldLongTexW, wds::chart_render::kHoldLongTexH,
                   wds::chart_render::kHoldLongTexW * 4, QImage::Format_RGBA8888);
  return QPixmap::fromImage(img.copy());
}

// Horizontal 3-slice (official Sprite.border L/R). Caps are fractions of
// dest_world_w so a wider edit pane scales dest_w; dest_h matches preview 厚度.
// Only a 1-wide tap (caps > dest) shrinks the sides and drops the middle.
void paint_sliced_sprite(QPainter& p, const QPixmap& tex, const QRectF& dest, float border_l_px,
                         float border_r_px, float dest_world_w) {
  if (tex.isNull() || dest.width() < 1.0 || dest.height() < 0.5) return;
  const float tex_w = static_cast<float>(tex.width());
  const float tex_h = static_cast<float>(tex.height());
  float u_bl = std::clamp(border_l_px / tex_w, 0.0f, 0.49f);
  float u_br = std::clamp(border_r_px / tex_w, 0.0f, 0.49f);
  if (u_bl + u_br > 0.98f) {
    const float s = 0.98f / (u_bl + u_br);
    u_bl *= s;
    u_br *= s;
  }
  const auto layout = wds::chart_render::sliced_cap_layout(border_l_px, border_r_px, dest_world_w);
  const qreal src_l = static_cast<qreal>(tex_w * u_bl);
  const qreal src_r = static_cast<qreal>(tex_w * u_br);
  const qreal dst_l = dest.width() * static_cast<qreal>(layout.bl);
  const qreal dst_r = dest.width() * static_cast<qreal>(layout.br);
  const qreal src_mid = std::max<qreal>(1.0, tex_w - src_l - src_r);
  const qreal dst_mid = dest.width() - dst_l - dst_r;
  const auto draw = [&](const QRectF& d, const QRectF& s) {
    if (d.width() > 0.5 && d.height() > 0.5 && s.width() > 0.5 && s.height() > 0.5) {
      p.drawPixmap(d, tex, s);
    }
  };
  draw({dest.x(), dest.y(), dst_l, dest.height()}, {0.0, 0.0, src_l, tex_h});
  if (layout.emit_middle) {
    draw({dest.x() + dst_l, dest.y(), dst_mid, dest.height()}, {src_l, 0.0, src_mid, tex_h});
  }
  draw({dest.x() + dest.width() - dst_r, dest.y(), dst_r, dest.height()},
       {tex_w - src_r, 0.0, src_r, tex_h});
}

void paint_sliced_hold(QPainter& p, const QPixmap& tex, const QRectF& dest, float dest_world_w) {
  paint_sliced_sprite(p, tex, dest, wds::chart_render::kHoldLongSliceBorderL,
                      wds::chart_render::kHoldLongSliceBorderR, dest_world_w);
}

void paint_sliced_note(QPainter& p, const QPixmap& tex, const QRectF& dest, float dest_world_w) {
  paint_sliced_sprite(p, tex, dest, wds::chart_render::kNoteSliceBorderL,
                      wds::chart_render::kNoteSliceBorderR, dest_world_w);
}

struct ScopedAA {
  QPainter& p;
  explicit ScopedAA(QPainter& painter) : p(painter) { p.setRenderHint(QPainter::Antialiasing, true); }
  ~ScopedAA() { p.setRenderHint(QPainter::Antialiasing, false); }
};

bool note_in_tick_window(const wds::chart_editor::NotationNote& n, int32_t lo, int32_t hi) {
  return n.start_tick <= hi && std::max(n.end_tick, n.start_tick) >= lo;
}

// Calls f(index) for every note satisfying note_in_tick_window(note, lo, hi), ascending index.
// `sorted` = notes ascending by start_tick and `max_span` >= max(end_tick, start_tick) - start_tick
// for every candidate note: a note starting before lo - max_span cannot reach lo, and one starting
// after hi cannot match, so only [lower_bound(lo - max_span), first start_tick > hi) is visited.
// Same set and order as a full scan; falls back to the full scan when not sorted.
template <typename F>
void for_each_note_in_tick_window(const std::vector<wds::chart_editor::NotationNote>& notes,
                                  bool sorted, int64_t max_span, int32_t lo, int32_t hi, F&& f) {
  std::size_t i = 0;
  if (sorted) {
    const int64_t min_start = static_cast<int64_t>(lo) - std::max<int64_t>(0, max_span);
    i = static_cast<std::size_t>(
        std::lower_bound(notes.begin(), notes.end(), min_start,
                         [](const wds::chart_editor::NotationNote& n, int64_t v) {
                           return static_cast<int64_t>(n.start_tick) < v;
                         }) -
        notes.begin());
  }
  for (const std::size_t n = notes.size(); i < n; ++i) {
    const auto& note = notes[i];
    if (sorted && note.start_tick > hi) break;
    if (note_in_tick_window(note, lo, hi)) f(i);
  }
}


void fill_ui_rect(QPainter& p, const wds::interaction::UiPaintRect& r) {
  p.setPen(Qt::NoPen);
  p.setBrush(qcolor(r.color));
  if (r.corner_radius > 0.5f) {
    ScopedAA aa(p);
    p.drawRoundedRect(qrect(r.bounds), r.corner_radius, r.corner_radius);
  } else {
    p.drawRect(qrect(r.bounds));
  }
}

void draw_ui_label(QPainter& p, const wds::interaction::UiPaintLabel& label) {
  // Same face as the Qt chrome. Vulkan label pixel sizes (22–26) are atlas
  // bake metrics and must not drive QPainter.
  (void)label.pixel_size;
  p.setPen(qcolor(label.color));
  int flags = Qt::AlignVCenter;
  flags |= label.left_align ? Qt::AlignLeft : Qt::AlignHCenter;
  if (label.wrap) flags |= Qt::TextWordWrap;
  p.drawText(qrect(label.bounds), flags, QString::fromUtf8(label.text.c_str()));
}

void flush_ui_painter(QPainter& p, const wds::interaction::UiPainter& ui) {
  auto rects = ui.rects();
  std::stable_sort(rects.begin(), rects.end(),
                   [](const auto& a, const auto& b) { return a.z < b.z; });
  for (const auto& r : rects) fill_ui_rect(p, r);
  auto labels = ui.labels();
  std::stable_sort(labels.begin(), labels.end(),
                   [](const auto& a, const auto& b) { return a.z < b.z; });
  for (const auto& label : labels) draw_ui_label(p, label);
  auto front = ui.front_rects();
  std::stable_sort(front.begin(), front.end(),
                   [](const auto& a, const auto& b) { return a.z < b.z; });
  for (const auto& r : front) fill_ui_rect(p, r);
}

void paint_click_marks(QPainter& p, const EditViewport& v, const ChartEditPanel& panel) {
  const auto& b = v.bounds();
  const float cx = b.x + b.w * 0.5f;
  const double now_ms = static_cast<double>(panel.engine().timeline_us()) / 1000.0;
  const bool light = wds::interaction::theme::color_scheme_is_light();
  const QColor tap_fill = light ? QColor(198, 40, 40, 230) : QColor(226, 58, 58, 220);
  const QColor tap_stroke = light ? QColor(110, 18, 22, 235) : QColor(255, 232, 232, 235);
  const QColor tap_halo = light ? QColor(226, 58, 58, 38) : QColor(255, 90, 90, 58);
  const QColor trail_glow = light ? QColor(232, 90, 168, 40) : QColor(255, 122, 195, 50);
  const QColor trail_core = light ? QColor(210, 64, 148, 185) : QColor(255, 150, 214, 205);
  constexpr qreal kDotR = 4.5;
  constexpr qreal kHaloR = 7.0;
  constexpr qreal kCoreW = 2.0;
  constexpr qreal kGlowW = 7.5;

  auto in_view = [&](float y0, float y1) {
    const float top = std::min(y0, y1);
    const float bot = std::max(y0, y1);
    return bot >= b.y - 14.0f && top <= b.bottom() + 14.0f;
  };

  ScopedAA aa(p);
  auto draw_band = [&](qreal top, qreal bot, qreal width, const QColor& color) {
    if (bot <= top) return;
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(cx - width * 0.5, top, width, bot - top), width * 0.5, width * 0.5);
  };
  auto draw_trail = [&](double t0, double t1, bool pending) {
    const float y0 = v.y_at_ms(static_cast<float>(t0));
    const float y1 = v.y_at_ms(static_cast<float>(t1));
    if (!in_view(y0, y1)) return;
    qreal top = static_cast<qreal>(std::min(y0, y1));
    qreal bot = static_cast<qreal>(std::max(y0, y1));
    // Keep the red head clear; do not inflate short holds into pills.
    const qreal inset = kDotR * 0.85;
    if (bot - top <= inset * 2.0 + 1.0) return;
    top += inset;
    bot -= inset;
    QColor glow = trail_glow;
    QColor core = trail_core;
    if (pending) {
      glow.setAlpha(std::max(18, glow.alpha() * 3 / 4));
      core.setAlpha(std::max(90, core.alpha() * 3 / 4));
    }
    draw_band(top, bot, kGlowW, glow);
    draw_band(top, bot, kCoreW, core);
  };
  auto draw_dot = [&](double t) {
    const float y = v.y_at_ms(static_cast<float>(t));
    if (!in_view(y, y)) return;
    const QPointF c(cx, y);
    p.setPen(Qt::NoPen);
    p.setBrush(tap_halo);
    p.drawEllipse(c, kHaloR, kHaloR);
    p.setBrush(tap_fill);
    p.drawEllipse(c, kDotR, kDotR);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(tap_stroke, 1.15));
    p.drawEllipse(c, kDotR, kDotR);
  };

  for (const auto& mark : panel.click_holds()) draw_trail(mark.start_ms, mark.end_ms, false);
  if (const auto pending = panel.pending_click_hold(now_ms)) {
    draw_trail(pending->start_ms, pending->end_ms, true);
  }
  for (const auto& mark : panel.click_taps()) draw_dot(mark.ms);
}

// Builds, once per frame, everything the split passes draw: the windowed split items with their
// line segments / ramp stops / edge colors, and the default lane guides with the spans not hidden
// by a split. The main pass and the two edge-spill strips only draw from this (see draw_* below).
void build_split_frame(ChartEditSplitFrame& f, const EditViewport& v,
                       const wds::interaction::Rect& b,
                       const std::vector<wds::chart_editor::NotationNote>& notes,
                       const wds::chart_editor::MusicTiming& timing,
                       const wds::chart_editor::PreviewConfig& preview,
                       const std::unordered_set<int32_t>& selected) {
  constexpr float kSplitLineW = 2.0f;
  constexpr float kSplitLineHighlightW = 4.0f;
  constexpr int kRampStops = kChartEditSplitRampStops;
  f.items.clear();
  f.edges.clear();
  f.guide_lines.clear();
  f.guide_spans.clear();
  f.cover_scratch.clear();
  const int lanes = std::max(1, v.grid().lane_count);
  const float view_ms_lo = v.scroll_ms();
  const float view_ms_hi = view_ms_lo + static_cast<float>(v.visible_ms());
  const int64_t appear =
      wds::chart_editor::split_fade_sec_to_ms(preview.split_line_animation_start_sec);
  const int64_t fade_out_ms =
      wds::chart_editor::split_fade_sec_to_ms(preview.split_line_animation_end_sec);

  for (const std::size_t index : f.note_indices) {
    const auto& note = notes[index];
    ChartEditSplitItem it;
    it.note = &note;
    it.selected = selected.count(note.id) != 0;
    it.color_id = note.scratch_length;
    it.split_count = wds::chart_editor::get_split_count(note.gimmick_type);
    it.start_ms = note.start_ms(timing);
    it.end_ms = std::max(it.start_ms, note.end_ms(timing));
    it.appear_ms = appear;
    it.fade_start_ms = it.start_ms - appear;
    it.fade_end_ms = wds::chart_editor::official_split_destroy_ms(it.start_ms, it.end_ms);
    it.line_w = it.selected ? kSplitLineHighlightW : kSplitLineW;

    // Lane-guide hide coverage (fade-in start .. destroy), clipped to the view.
    {
      const float lo = std::max(view_ms_lo, static_cast<float>(it.fade_start_ms));
      const float hi = std::min(view_ms_hi, static_cast<float>(it.fade_end_ms));
      if (hi > lo) f.cover_scratch.emplace_back(lo, hi);
    }

    const float fade0 = static_cast<float>(it.fade_start_ms);
    const float fade1 = static_cast<float>(it.fade_end_ms);
    it.lines_visible = !(fade1 < view_ms_lo || fade0 > view_ms_hi);
    if (it.lines_visible) {
      // Opacity curve of split_line_opacity_at_ms with the per-note terms hoisted.
      const int64_t expire_ms = wds::chart_editor::official_split_expire_ms(it.start_ms, it.end_ms);
      auto opacity_at = [&](float ms) {
        const int64_t t = static_cast<int64_t>(std::llround(ms));
        if (t < it.fade_start_ms || t >= it.fade_end_ms) return 0.0f;
        if (t < it.start_ms) {
          const float u = std::clamp(
              static_cast<float>(t - it.fade_start_ms) / static_cast<float>(it.appear_ms), 0.0f,
              1.0f);
          return wds::chart_editor::official_split_fade_in_scale(u);
        }
        if (t >= expire_ms) {
          const float u = std::clamp(
              static_cast<float>(t - expire_ms) / static_cast<float>(fade_out_ms), 0.0f, 1.0f);
          return wds::chart_editor::official_split_fade_out_alpha(u);
        }
        return 1.0f;
      };
      // `ramp` segments (fade-in / fade-out) carry a vertical alpha gradient sampled from the opacity
      // curve, so opacity varies along the line from the fade start to the fade end.
      auto make_seg = [&](ChartEditSplitSegment& s, float ms0, float ms1, bool ramp) {
        s.valid = false;
        if (ms1 <= ms0) return;
        const float y0 = v.y_at_ms(ms0);
        const float y1 = v.y_at_ms(ms1);
        const float top = std::max(std::min(y0, y1), b.y);
        const float bot = std::min(std::max(y0, y1), b.bottom());
        if (bot <= top) return;
        const bool use_ramp = ramp && std::abs(y1 - y0) >= 1.0f;
        float max_op = 0.0f;
        if (use_ramp) {
          for (int i = 0; i <= kRampStops; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kRampStops);
            s.stops[i] = opacity_at(ms0 + (ms1 - ms0) * t);
            max_op = std::max(max_op, s.stops[i]);
          }
        } else {
          max_op = opacity_at((ms0 + ms1) * 0.5f);
        }
        if (max_op <= 0.001f) return;
        s.valid = true;
        s.ramp = use_ramp;
        s.y0 = y0;
        s.y1 = y1;
        s.top = top;
        s.bot = bot;
        s.max_op = max_op;
      };
      const float start_f = static_cast<float>(it.start_ms);
      const float end_f = static_cast<float>(it.end_ms);
      make_seg(it.seg[0], std::max(fade0, view_ms_lo), std::min(start_f, view_ms_hi), true);
      make_seg(it.seg[1], std::max(start_f, view_ms_lo), std::min(end_f, view_ms_hi), false);
      make_seg(it.seg[2], std::max(end_f, view_ms_lo), std::min(fade1, view_ms_hi), true);

      it.edge_begin = static_cast<uint32_t>(f.edges.size());
      if (it.seg[0].valid || it.seg[1].valid || it.seg[2].valid) {
        auto add_edge = [&](int32_t edge, int32_t slot) {
          auto c = split_slot_color(it.color_id, slot, it.split_count, nullptr);
          if (c.a < 0.02f) return;
          apply_official_split_rgb_opacity(c);
          if (it.selected) {
            c = c.lerp({1.0f, 1.0f, 1.0f, c.a}, 0.28f);
          }
          f.edges.push_back({std::floor(v.x_at(edge) + 0.5f), c});
        };
        split_boundaries_12(it.split_count, f.mids_scratch);
        add_edge(0, 0);
        int32_t slot = 1;
        for (int32_t mid : f.mids_scratch) add_edge(mid + 1, slot++);
        add_edge(lanes, std::max(1, it.split_count));
      }
      it.edge_end = static_cast<uint32_t>(f.edges.size());
    }
    f.items.push_back(it);
  }

  // Default lane guides, punched out where a split fade window covers the view.
  std::sort(f.cover_scratch.begin(), f.cover_scratch.end());
  std::size_t merged = 0;
  for (std::size_t i = 0; i < f.cover_scratch.size(); ++i) {
    const auto seg = f.cover_scratch[i];
    if (merged == 0 || seg.first > f.cover_scratch[merged - 1].second) {
      f.cover_scratch[merged++] = seg;
    } else {
      f.cover_scratch[merged - 1].second = std::max(f.cover_scratch[merged - 1].second, seg.second);
    }
  }
  f.cover_scratch.resize(merged);
  auto add_span = [&](float y0, float y1) {
    const float top = std::max(std::min(y0, y1), b.y);
    const float bot = std::min(std::max(y0, y1), b.bottom());
    if (bot <= top) return;
    f.guide_spans.emplace_back(top, bot);
  };
  float cursor = view_ms_lo;
  for (const auto& seg : f.cover_scratch) {
    if (seg.first > cursor) add_span(v.y_at_ms(cursor), v.y_at_ms(seg.first));
    cursor = std::max(cursor, seg.second);
  }
  if (view_ms_hi > cursor) add_span(v.y_at_ms(cursor), v.y_at_ms(view_ms_hi));
  if (f.guide_spans.empty()) return;
  const int first = v.first_visible_lane();
  const int guide_lanes = v.visible_lane_count();
  const float lane_px = v.lane_width(1);
  namespace th = wds::interaction::theme;
  for (int i = first; i <= first + guide_lanes; ++i) {
    if (!lane_guide_visible(i, lane_px)) continue;
    const bool edge = i == 0 || i == 12;
    f.guide_lines.push_back(
        {v.x_at(i), QPen(qcolor(edge ? th::kEditLaneEdge : th::kEditLaneInner),
                         static_cast<qreal>(lane_guide_thickness(i)))});
  }
}

// Draws the lane guides from `f`. Lines wholly outside [x_lo, x_hi] (plus pen/AA slack) are skipped:
// they could not touch a clip confined to that range, so the pixels match an unfiltered draw.
void draw_lane_guides(QPainter& p, const ChartEditSplitFrame& f, float x_lo, float x_hi) {
  for (const auto& span : f.guide_spans) {
    for (const auto& line : f.guide_lines) {
      const float slack = static_cast<float>(line.pen.widthF()) * 0.5f + 1.5f;
      if (line.x + slack < x_lo || line.x - slack > x_hi) continue;
      p.setPen(line.pen);
      p.drawLine(QPointF(line.x, span.first), QPointF(line.x, span.second));
    }
  }
}

// Draws the split lines from `f`: unselected splits first, then the selected (highlighted) ones.
// x_lo / x_hi cull edges exactly like draw_lane_guides.
void draw_split_lines(QPainter& p, const ChartEditSplitFrame& f, float x_lo, float x_hi) {
  constexpr int kRampStops = kChartEditSplitRampStops;
  const auto draw_item = [&](const ChartEditSplitItem& it) {
    if (!it.lines_visible) return;
    const float slack = it.line_w * 0.5f + 1.5f;
    for (const auto& s : it.seg) {
      if (!s.valid) continue;
      for (uint32_t e = it.edge_begin; e < it.edge_end; ++e) {
        const auto& edge = f.edges[e];
        if (edge.x + slack < x_lo || edge.x - slack > x_hi) continue;
        const QRectF r(edge.x - static_cast<qreal>(it.line_w) * 0.5, s.top, it.line_w,
                       s.bot - s.top);
        if (s.ramp) {
          QLinearGradient grad(QPointF(edge.x, s.y0), QPointF(edge.x, s.y1));
          QGradientStops stops;
          stops.reserve(kRampStops + 1);
          for (int i = 0; i <= kRampStops; ++i) {
            auto stop_c = edge.color;
            stop_c.a *= s.stops[i];
            stops.push_back({static_cast<qreal>(i) / kRampStops, qcolor(stop_c)});
          }
          grad.setStops(stops);
          p.fillRect(r, QBrush(grad));
        } else {
          auto c = edge.color;
          c.a *= s.max_op;
          p.fillRect(r, qcolor(c));
        }
      }
    }
  };
  for (const auto& it : f.items) {
    if (!it.selected) draw_item(it);
  }
  for (const auto& it : f.items) {
    if (it.selected) draw_item(it);
  }
}

// Static particle indicators at the start (fade0, bottom end on screen) of each split line's fade-in
// segment. The picker's composition is scaled proportionally (lane_px / kSplitIndicatorRefLanePx) and anchored
// there; it is not stretched over the segment. Only cached clusters are drawn per frame; a miss is requested from the
// background builder (the GUI thread never builds) and simply skipped until `on_ready` repaints. Painted after notes
// (preview draws split particles above notes). Opacity is 1: baked particles already carry their mid-fade alpha.

float split_lane_px(const EditViewport& v) {
  // Quantized like the cluster cache (0.25 px) so band_h and keys stay stable.
  return std::round(std::abs(v.x_at(1) - v.x_at(0)) * 4.0f) / 4.0f;
}

SplitIndicatorClusterKey split_cluster_key(int32_t color_id, int32_t split_count,
                                           wds::chart_editor::SplitLaneType type, float lane_px,
                                           int band_h) {
  // Single source of the editor's cluster key: both the visible paint and the prefetch go through here, so their
  // cache keys always agree. Only the edit area differs from the picker: sprite edge length and strip width are
  // halved (kEditorIndicatorSizeFactor), the band follows the time axis (see split_band_q, the
  // caller passes the quantized value in band_h) and it shows denser clusters (rank < kEditorIndicatorMaxRank). Lane mapping is the
  // picker's; the picker / hover card are unchanged.
  const float s = lane_px / kSplitIndicatorRefLanePx;
  SplitIndicatorClusterKey k;
  k.color_id = color_id;
  k.split_count = split_count;
  k.type = type;
  k.lane_px = lane_px;
  k.band_h = band_h;
  k.size_scale = kSplitIndicatorSizeScale * kEditorIndicatorSizeFactor;
  k.min_sprite_px = kSplitIndicatorRefMinSpritePx * s * kEditorIndicatorSizeFactor;
  k.max_rank = kEditorIndicatorMaxRank;
  return k;
}

// Fade-in duration exactly as build_split_frame computes it.
int64_t split_appear_ms(const wds::chart_editor::PreviewConfig& preview) {
  return std::max<int64_t>(
      1, static_cast<int64_t>(std::llround(static_cast<double>(preview.split_line_animation_start_sec) * 1000.0)));
}

// Editor band height in px: independent of lane width, follows the time axis (fraction of the fade-in duration
// starting at fade0). Returns the quantized band (0 = too short, skip).
int split_band_q(const EditViewport& v, float fade0, int64_t appear) {
  const float band_ms = static_cast<float>(appear) * kEditorIndicatorFadeFraction;
  const float band_px = std::abs(v.y_at_ms(fade0 + band_ms) - v.y_at_ms(fade0));
  return quantize_editor_indicator_band(band_px);
}

void paint_split_particles(QPainter& p, const EditViewport& v, const wds::interaction::Rect& b,
                           const ChartEditSplitFrame& frame, QObject* notify,
                           const std::function<void()>& on_ready) {
  constexpr float kMinBandPx = 16.0f;
  const float view_ms_lo = v.scroll_ms();
  const float view_ms_hi = view_ms_lo + static_cast<float>(v.visible_ms());
  const float lane_px = split_lane_px(v);
  if (lane_px <= 0.5f) return;
  const float x0 = std::floor(v.x_at(0) + 0.5f);
  auto paint_one = [&](const ChartEditSplitItem& item) {
    const auto& note = *item.note;
    const int64_t start_ms = item.start_ms;
    const int64_t appear = item.appear_ms;
    const float fade0 = static_cast<float>(item.fade_start_ms);
    if (static_cast<float>(start_ms) < view_ms_lo || fade0 > view_ms_hi) return;
    const float y_bottom = v.y_at_ms(fade0);  // fade0 = earliest time = bottom end of the segment on screen
    const int band_q = split_band_q(v, fade0, appear);
    if (band_q < static_cast<int>(kMinBandPx)) return;
    const float band_top = y_bottom - static_cast<float>(band_q);
    if (y_bottom < b.y || band_top > b.bottom()) return;
    const int32_t color_id = note.scratch_length;
    const int32_t split_count = wds::chart_editor::get_split_count(note.gimmick_type);
    const auto type = wds::chart_editor::get_split_lane_type(note.gimmick_type);
    const auto key = split_cluster_key(color_id, split_count, type, lane_px, band_q);
    // nullptr = queued on the worker (most recent request builds first). c is drawn immediately and never held
    // across event processing (the LRU may evict when results are delivered).
    const SplitIndicatorCluster* c = split_effect_indicator_cluster_request(key, notify, on_ready);
    if (c) {
      if (!c->empty) draw_split_effect_indicator_cluster(p, *c, QPointF(x0, band_top));
      return;
    }
    // Not built yet: draw the nearest READY band variant stretched vertically to band_q around the bottom anchor
    // (transient; the exact one replaces it when its build finishes).
    const int step = editor_indicator_band_step(band_q);
    for (int d = 1; d <= 6; ++d) {
      for (int sgn = -1; sgn <= 1; sgn += 2) {
        const int vb = editor_indicator_band_for_step(step + sgn * d);
        if (vb < static_cast<int>(kMinBandPx) || vb == band_q) continue;
        const SplitIndicatorCluster* alt = split_effect_indicator_cluster_if_ready(
            split_cluster_key(color_id, split_count, type, lane_px, vb));
        if (!alt) continue;
        if (!alt->empty) {
          p.save();
          p.translate(x0, y_bottom);
          p.scale(1.0, static_cast<qreal>(band_q) / static_cast<qreal>(vb));
          draw_split_effect_indicator_cluster(p, *alt, QPointF(0.0, -static_cast<qreal>(vb)));
          p.restore();
        }
        return;
      }
    }
  };
  for (const auto& item : frame.items) {
    if (!item.selected) paint_one(item);
  }
  for (const auto& item : frame.items) {
    if (item.selected) paint_one(item);
  }
}

// Groove is thin so QSlider only starts a drag on the handle. Any press on
// this control jumps to that x and keeps tracking, matching the playback seek bar.
class ExtraLaneSlider final : public QSlider {
 public:
  using QSlider::QSlider;

  QRect handle_rect() const {
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    return style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
  }

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton) {
      QSlider::mousePressEvent(event);
      return;
    }
    groove_drag_ = true;
    setSliderDown(true);
    setValue(value_from_pos(event->position().toPoint()));
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (!groove_drag_) {
      QSlider::mouseMoveEvent(event);
      return;
    }
    setValue(value_from_pos(event->position().toPoint()));
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (!groove_drag_) {
      QSlider::mouseReleaseEvent(event);
      return;
    }
    setValue(value_from_pos(event->position().toPoint()));
    groove_drag_ = false;
    setSliderDown(false);
    event->accept();
  }

 private:
  int value_from_pos(const QPoint& pos) const {
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
    const int slider_length = std::max(1, handle.width());
    const int slider_min = groove.isValid() ? groove.x() : 0;
    const int slider_max = groove.isValid() ? groove.right() - slider_length + 1
                                            : std::max(0, width() - slider_length);
    const int span = std::max(1, slider_max - slider_min);
    return QStyle::sliderValueFromPosition(minimum(), maximum(), pos.x() - slider_min, span,
                                           opt.upsideDown);
  }

  bool groove_drag_ = false;
};

}  // namespace

ChartEditWidget::ChartEditWidget(ChartEditPanel* panel, QWidget* parent)
    : QWidget(parent), panel_(panel) {
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setAutoFillBackground(false);
  // Timing / split pickers are Qt dialogs. IME on the canvas swallows Shift/Cmd
  // letter chords under CJK input methods.
  setAttribute(Qt::WA_InputMethodEnabled, false);
  hold_blue_ = pixmap_from_hold_bake(false);
  hold_purple_ = pixmap_from_hold_bake(true);
  extra_lane_slider_ = new ExtraLaneSlider(Qt::Horizontal, this);
  extra_lane_slider_->setRange(0, 20);
  extra_lane_slider_->setValue(0);
  extra_lane_slider_->setVisible(false);
  extra_lane_slider_->setFocusPolicy(Qt::NoFocus);
  extra_lane_slider_->setMinimumHeight(22);
  extra_lane_slider_opacity_ = new QGraphicsOpacityEffect(extra_lane_slider_);
  extra_lane_slider_opacity_->setOpacity(0.3);
  extra_lane_slider_->setGraphicsEffect(extra_lane_slider_opacity_);
  extra_lane_slider_fade_ = new QPropertyAnimation(extra_lane_slider_opacity_, "opacity", this);
  extra_lane_slider_fade_->setDuration(100);
  extra_lane_slider_->installEventFilter(this);
  extra_lane_slider_label_ = new QLabel(this);
  extra_lane_slider_label_->setVisible(false);
  extra_lane_slider_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
  extra_lane_slider_label_->setAlignment(Qt::AlignCenter);
  extra_lane_slider_label_->setStyleSheet(
      QStringLiteral("background: rgba(26, 28, 36, 240); color: white; padding: 2px 8px; "
                     "border-radius: 4px;"));
  connect(extra_lane_slider_, &QSlider::valueChanged, this, [this](int value) {
    if (panel_ == nullptr) return;
    panel_->set_extra_visible_lanes(value);
    update_extra_lane_slider_label();
    mark_dirty();
    update();
  });
  connect(extra_lane_slider_, &QSlider::sliderMoved, this, [this](int) {
    update_extra_lane_slider_label();
  });
  connect(extra_lane_slider_, &QSlider::sliderPressed, this, [this] {
    if (panel_ != nullptr) panel_->set_lane_slider_hovered(true);
    fade_lane_slider(true);
    update_extra_lane_slider_label();
  });
  connect(extra_lane_slider_, &QSlider::sliderReleased, this, [this] {
    update_extra_lane_slider_label();
    fade_lane_slider(extra_lane_slider_ != nullptr && extra_lane_slider_->underMouse());
  });
}

void ChartEditWidget::set_skins_directory(const QString& directory) {
  skins_dir_ = directory;
  const QDir dir(directory);
  const auto load = [&](const char* name) { return QPixmap(dir.filePath(QString::fromUtf8(name))); };
  background_ = load("ingame_bg.png");
  judgment_ = load("img_ingame_judgment_area3.png");
  red_ = load("Sirius Note Red Top.png");
  yellow_ = load("Sirius Note Yellow Top.png");
  blue_ = load("Sirius Note Blue Top.png");
  purple_ = load("Sirius Note Purple Top.png");
  tick_blue_ = load("Sirius Note Tick Blue.png");
  tick_purple_ = load("Sirius Note Tick Purple.png");
  arrow_ = load("Sirius Scratch Arrow.png");
  if (arrow_.isNull()) {
    arrow_mirrored_ = {};
  } else {
    arrow_mirrored_ = QPixmap::fromImage(arrow_.toImage().mirrored(true, false));
    arrow_mirrored_.setDevicePixelRatio(arrow_.devicePixelRatio());
  }
  ensure_split_effect_uv_atlas(directory);
  prefetch_rev_ = ~uint64_t{0};  // requests made before the atlas existed were refused: redo the prefetch
  mark_dirty();
  update();
}

void ChartEditWidget::schedule_split_prefetch(
    const std::vector<wds::chart_editor::NotationNote>& notes, float lane_px, int band_q) {
  if (!panel_ || lane_px <= 0.5f || band_q < kEditorIndicatorMinBandPx) return;
  // Keyed by the document content (not visual_revision(), which mixes in the scroll position and therefore
  // changes on every frame during playback and would restart the prefetch constantly).
  const uint64_t rev = panel_->engine().document().content_generation();
  if (rev == prefetch_rev_ && lane_px == prefetch_lane_px_ && band_q == prefetch_band_q_) return;
  prefetch_rev_ = rev;
  prefetch_lane_px_ = lane_px;
  prefetch_band_q_ = band_q;
  // Distinct (color, count, type) of the whole chart at the reference band, requested once per change. Capped so a
  // chart with very many distinct splits cannot push the visible clusters out of the byte-bounded cache. Requests
  // are cheap (queue push); the worker builds them. Callers run this BEFORE the visible paint requests: the worker
  // is LIFO, so what is on screen then builds first.
  constexpr std::size_t kMaxPrefetch = 32;
  struct Item {
    int32_t color_id;
    int32_t split_count;
    wds::chart_editor::SplitLaneType type;
  };
  std::vector<Item> items;
  for (const auto& n : notes) {
    if (items.size() >= kMaxPrefetch) break;
    if (!wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) continue;
    const Item it{n.scratch_length, wds::chart_editor::get_split_count(n.gimmick_type),
                  wds::chart_editor::get_split_lane_type(n.gimmick_type)};
    const bool dup = std::any_of(items.begin(), items.end(), [&](const Item& q) {
      return q.color_id == it.color_id && q.split_count == it.split_count && q.type == it.type;
    });
    if (!dup) items.push_back(it);
  }
  // Reverse: the first chart entry becomes the most recent request.
  for (auto it = items.rbegin(); it != items.rend(); ++it) {
    const auto key = split_cluster_key(it->color_id, it->split_count, it->type, lane_px, band_q);
    (void)split_effect_indicator_cluster_request(key, this, [this] { request_split_repaint(); });
  }
}

void ChartEditWidget::request_split_repaint() {
  mark_dirty();
  if (split_repaint_pending_) return;  // coalesce: one queued update serves every cluster finished before it paints
  split_repaint_pending_ = true;
  update();
}

void ChartEditWidget::set_split_effect_quality(int quality) {
  quality = wds::chart_editor::official_clamp_sprit_effect_setting(quality);
  if (quality == split_effect_quality_) return;
  split_effect_quality_ = quality;
  mark_dirty();
  update();
}

bool ChartEditWidget::take_dirty_for_frame(bool playing) {
  if (!isVisible()) return false;
  if (panel_) {
    const uint64_t rev = panel_->visual_revision();
    if (rev != last_visual_revision_) {
      last_visual_revision_ = rev;
      dirty_ = true;
    }
  }
  if (playing) dirty_ = true;
  const auto now = std::chrono::steady_clock::now();
  if (now - last_paint_at_ >= std::chrono::milliseconds(250)) dirty_ = true;
  if (!dirty_) return false;
  dirty_ = false;
  return true;
}

wds::interaction::Modifiers ChartEditWidget::mods(Qt::KeyboardModifiers m) const {
  return qt_modifiers(m | QGuiApplication::queryKeyboardModifiers());
}
wds::interaction::Vec2 ChartEditWidget::point(const QPointF& p) const {
  return {static_cast<float>(p.x()), static_cast<float>(p.y())};
}
static wds::interaction::PointerButton button(Qt::MouseButton b) {
  if (b == Qt::RightButton) return wds::interaction::PointerButton::Right;
  if (b == Qt::MiddleButton) return wds::interaction::PointerButton::Middle;
  return wds::interaction::PointerButton::Left;
}
void ChartEditWidget::sync_extra_lane_slider() {
  if (extra_lane_slider_ == nullptr || panel_ == nullptr) return;
  const bool on = panel_->extended_editing_active();
  extra_lane_slider_->setVisible(on);
  if (!on) {
    panel_->set_lane_slider_hovered(false);
    if (extra_lane_slider_label_ != nullptr) extra_lane_slider_label_->hide();
    return;
  }
  {
    const QSignalBlocker blocker(extra_lane_slider_);
    extra_lane_slider_->setValue(panel_->extra_visible_lanes());
  }
  const auto& pf = panel_->playfield();
  const int w = std::max(220, static_cast<int>(pf.w) - 8);
  const int h = 22;
  extra_lane_slider_->setGeometry(static_cast<int>(pf.x + (pf.w - static_cast<float>(w)) * 0.5f),
                                  static_cast<int>(pf.bottom() - static_cast<float>(h) - 6.0f), w,
                                  h);
  extra_lane_slider_->raise();
  update_extra_lane_slider_label();
}

void ChartEditWidget::update_extra_lane_slider_label() {
  if (extra_lane_slider_label_ == nullptr || extra_lane_slider_ == nullptr) return;
  if (!extra_lane_slider_->isVisible() || !extra_lane_slider_->isSliderDown()) {
    extra_lane_slider_label_->hide();
    return;
  }
  extra_lane_slider_label_->setText(QString::number(extra_lane_slider_->value()));
  extra_lane_slider_label_->adjustSize();
  const auto* slider = static_cast<const ExtraLaneSlider*>(extra_lane_slider_);
  const QRect local_handle = slider->handle_rect();
  const QPoint handle_top_center =
      extra_lane_slider_->mapToParent(QPoint(local_handle.center().x(), local_handle.top()));
  const int x = handle_top_center.x() - extra_lane_slider_label_->width() / 2;
  const int y = handle_top_center.y() - extra_lane_slider_label_->height() - 4;
  extra_lane_slider_label_->move(std::max(0, x), std::max(0, y));
  extra_lane_slider_label_->show();
  extra_lane_slider_label_->raise();
}

void ChartEditWidget::fade_lane_slider(bool highlighted) {
  if (extra_lane_slider_opacity_ == nullptr || extra_lane_slider_fade_ == nullptr) return;
  extra_lane_slider_fade_->stop();
  extra_lane_slider_fade_->setStartValue(extra_lane_slider_opacity_->opacity());
  extra_lane_slider_fade_->setEndValue(highlighted ? 1.0 : 0.3);
  extra_lane_slider_fade_->start();
}

bool ChartEditWidget::eventFilter(QObject* watched, QEvent* event) {
  if (watched == extra_lane_slider_ && extra_lane_slider_ != nullptr) {
    if (event->type() == QEvent::Enter) {
      if (panel_ != nullptr) panel_->set_lane_slider_hovered(true);
      fade_lane_slider(true);
    } else if (event->type() == QEvent::Leave &&
               (extra_lane_slider_ == nullptr || !extra_lane_slider_->isSliderDown())) {
      if (panel_ != nullptr) panel_->set_lane_slider_hovered(false);
      fade_lane_slider(false);
    }
  }
  return QWidget::eventFilter(watched, event);
}

void ChartEditWidget::resizeEvent(QResizeEvent*) {
  if (panel_) panel_->set_bounds({0, 0, static_cast<float>(width()), static_cast<float>(height())});
  if (panel_) panel_->viewport();
  sync_extra_lane_slider();
  mark_dirty();
  update();
}
void ChartEditWidget::showEvent(QShowEvent* e) {
  mark_dirty();
  QWidget::showEvent(e);
}
void ChartEditWidget::enterEvent(QEnterEvent* e) {
  mark_dirty();
  QWidget::enterEvent(e);
  update();
}
void ChartEditWidget::leaveEvent(QEvent* e) {
  if (panel_) panel_->on_hover_leave();
  mark_dirty();
  QWidget::leaveEvent(e);
  update();
}
void ChartEditWidget::focusInEvent(QFocusEvent* e) {
  mark_dirty();
  QWidget::focusInEvent(e);
}
void ChartEditWidget::focusOutEvent(QFocusEvent* e) {
  mark_dirty();
  QWidget::focusOutEvent(e);
}
void ChartEditWidget::paintEvent(QPaintEvent*) {
  dirty_ = false;
  split_repaint_pending_ = false;  // the queued update (if any) is being served by this paint
  const auto paint_t0 = std::chrono::steady_clock::now();
  struct PaintCost {
    ChartEditWidget* self;
    std::chrono::steady_clock::time_point t0;
    ~PaintCost() {
      const auto now = std::chrono::steady_clock::now();
      self->last_paint_us_ = std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count();
      self->last_paint_at_ = now;
      ++self->paint_count_;
    }
  } paint_cost{this, paint_t0};

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, false);
  p.fillRect(rect(), qcolor(wds::interaction::theme::kEditChrome));
  if (!panel_) return;
  // Viewport is synced in UiManager::update after the transport tick. Ghosts
  // and drag targets live in tick space; rematerialize them from the last
  // pointer so a stationary mouse is not scrolled away with the playhead.
  panel_->resync_pointer_overlays();
  wds::interaction::UiPainter chrome;
  chrome.set_defer_glyphs(true);
  panel_->paint_side_columns(chrome);
  sync_extra_lane_slider();
  const auto& v = panel_->viewport(); const auto b = v.bounds();
  const auto& notes = panel_->engine().document().notes();
  const auto& timing = panel_->engine().document().timing();
  const auto& preview = panel_->engine().preview_config();
  const QRectF playfield(b.x, b.y, b.w, b.h);
  p.save(); p.setClipRect(playfield);
  // The editor intentionally does not use the preview's ingame background.
  // Its neutral authoring surface keeps lane contrast and waveform readable.
  p.fillRect(playfield, qcolor(wds::interaction::theme::kEditCanvas));

  paint_waveform(p, v, b);

  // Same subdiv / beat / measure strokes as the side gutters so the thick
  // bar lines continue across the playfield.
  {
    wds::interaction::UiPainter grid;
    paint_horizontal_grid(grid, v, b);
    flush_ui_painter(p, grid);
  }

  // Note art hangs half a note-height (plus selection pad) past its tick.
  // Convert that pixel pad to ticks; long holds still pass the span test.
  const float pad_ms =
      (v.note_height_px() + 8.0f) * static_cast<float>(v.visible_ms()) / std::max(b.h, 1.0f);
  const float view_ms_lo_cull = v.scroll_ms();
  const float view_ms_hi_cull = view_ms_lo_cull + static_cast<float>(v.visible_ms());
  const int32_t cull_lo = wds::chart_editor::milliseconds_to_tick(
      static_cast<int64_t>(std::llround(static_cast<double>(view_ms_lo_cull - pad_ms))), timing);
  const int32_t cull_hi = wds::chart_editor::milliseconds_to_tick(
      static_cast<int64_t>(std::llround(static_cast<double>(view_ms_hi_cull + pad_ms))), timing);
  {
    // y_at() is fractional; AA keeps lane strokes pixel-identical to the
    // previous whole-paint hint.
    ScopedAA aa(p);
    const int64_t appear_ms = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(
               static_cast<double>(preview.split_line_animation_start_sec) * 1000.0)));
    const int64_t disappear_ms = wds::chart_editor::official_split_hide_tail_ms();
    const float view_ms_lo = v.scroll_ms();
    const float view_ms_hi = view_ms_lo + static_cast<float>(v.visible_ms());
    const int32_t fade_lo = wds::chart_editor::milliseconds_to_tick(
        static_cast<int64_t>(std::llround(static_cast<double>(view_ms_lo) -
                                          static_cast<double>(disappear_ms))),
        timing);
    const int32_t fade_hi = wds::chart_editor::milliseconds_to_tick(
        static_cast<int64_t>(
            std::llround(static_cast<double>(view_ms_hi) + static_cast<double>(appear_ms))),
        timing);
    const int32_t split_lo = std::min(cull_lo, fade_lo);
    const int32_t split_hi = std::max(cull_hi, fade_hi);
    // Windowed split gimmicks: binary-search the sorted notes (split_max_span_ticks_ bounds how far
    // back a long split can start and still reach the window), then compute all split geometry once.
    ensure_note_span_cache(notes);
    split_frame_.note_indices.clear();
    for_each_note_in_tick_window(notes, note_sorted_, split_max_span_ticks_, split_lo, split_hi,
                                 [&](std::size_t i) {
                                   if (wds::chart_editor::is_split_lane_gimmick(notes[i].gimmick_type)) {
                                     split_frame_.note_indices.push_back(i);
                                   }
                                 });
    build_split_frame(split_frame_, v, b, notes, timing, preview, panel_->selected());
    draw_lane_guides(p, split_frame_, -std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity());
  }
  draw_split_lines(p, split_frame_, -std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::infinity());
  // 12-cell judgeline tracks authored lanes 0..N as extra columns shrink them.
  if (!judgment_.isNull()) {
    const float h = v.judgeline_height_px();
    p.drawPixmap(QRectF(v.judgeline_x(), v.judgeline_y() - h * .5f, v.judgeline_width_px(), h),
                 judgment_, judgment_.rect());
  }

  sync_visible_notes(notes, cull_lo, cull_hi);
  paint_notes(p, notes, visible_note_indices_, 1.0f, true);
  {
    // Prefetch first, visible requests second: the builder is LIFO so the visible clusters are built first.
    {
      // Every split shares the same fade-in duration, so the band at the current zoom is one value (px per ms).
      const int64_t appear = split_appear_ms(preview);
      schedule_split_prefetch(notes, split_lane_px(v), split_band_q(v, 0.0f, appear));
    }
    paint_split_particles(p, v, b, split_frame_, this, [this] { request_split_repaint(); });
  }
  paint_click_marks(p, v, *panel_);
  for (const auto& ghost : panel_->skinned_ghosts()) {
    if (!ghost.visible) continue;
    // Ghosts are not part of the document: own one-note list, never the visible-note cache.
    if (!note_in_tick_window(ghost.note, cull_lo, cull_hi)) continue;
    ghost_scratch_.clear();
    ghost_scratch_.push_back(ghost.note);
    ghost_indices_.assign(1, 0);
    paint_notes(p, ghost_scratch_, ghost_indices_, ghost.alpha, false);
  }
  p.restore();
  flush_ui_painter(p, chrome);
  // Edge lane guides / split lines are centered on the playfield border; their outer half falls on the side
  // gutters. Repaint just that spill strip above the gutter layer (inside the playfield lines stay under notes).
  {
    constexpr qreal kEdgeSpillPx = 4.0;  // >= half the 4 px highlighted split line + pixel snap
    const QRectF strips[2] = {QRectF(b.x - kEdgeSpillPx, b.y, kEdgeSpillPx, b.h),
                              QRectF(b.x + b.w, b.y, kEdgeSpillPx, b.h)};
    for (const QRectF& strip : strips) {
      p.save();
      p.setClipRect(strip);
      // Geometry was built once above; the clip does the cutting, the x range just skips lines that
      // cannot reach the strip.
      const float x_lo = static_cast<float>(strip.left());
      const float x_hi = static_cast<float>(strip.right());
      {
        ScopedAA aa(p);
        draw_lane_guides(p, split_frame_, x_lo, x_hi);
      }
      draw_split_lines(p, split_frame_, x_lo, x_hi);
      p.restore();
    }
  }
  if (const auto marquee = panel_->active_marquee_rect()) {
    const QRectF box = qrect(*marquee);
    if (box.width() > 0.5 && box.height() > 0.5) {
      p.fillRect(box, qcolor(wds::interaction::theme::kEditSelectionGlow));
      ScopedAA aa(p);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(qcolor(wds::interaction::theme::kEditSelection), 2.0));
      p.drawRect(box);
    }
  }
  if (const auto hover = panel_->active_split_hover()) {
    // Text only: color id + lane type (no particle preview).
    const QString text =
        QString::number(hover->color_id) + QLatin1Char(' ') +
        QString::fromUtf8(split_fade_arrow_utf8(hover->color_id)) + QLatin1Char(' ') +
        QString::fromLatin1(wds::chart_editor::split_lane_type_name(hover->type));
    const QFontMetrics fm(font());
    const int text_w = fm.horizontalAdvance(text) + 4;
    constexpr int kPad = 8;
    const int card_w = kPad + text_w + kPad;
    const int card_h = fm.height() + kPad * 2;
    float card_y = hover->chip_bounds.y + (hover->chip_bounds.h - static_cast<float>(card_h)) * 0.5f;
    card_y = std::clamp(card_y, b.y, std::max(b.y, b.y + b.h - static_cast<float>(card_h)));
    const QRectF card(b.x + 2.0, static_cast<qreal>(card_y), card_w, card_h);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QColor(56, 61, 71, 230));
    p.setBrush(QColor(26, 28, 36, 240));
    p.drawRoundedRect(card, 6, 6);
    p.setPen(QColor(255, 255, 255));
    p.setBrush(Qt::NoBrush);
    p.drawText(QRectF(card.x() + kPad, card.y(), text_w, card.height()),
               Qt::AlignVCenter | Qt::AlignLeft, text);
    p.restore();
  }
}

void ChartEditWidget::fill_audio_backdrop_rows(const EditViewport& v,
                                               const wds::audio::WaveformOverview& waveform,
                                               EditSpectrumMode mode, int row0, int row1) {
  if (audio_backdrop_.isNull()) return;
  const int rows = audio_backdrop_.height();
  const int width = audio_backdrop_.width();
  row0 = std::clamp(row0, 0, rows);
  row1 = std::clamp(row1, row0, rows);
  if (row1 <= row0 || width <= 0) return;
  if (mode == EditSpectrumMode::Spectrogram) {
    for (int i = row0; i < row1; ++i) {
      const int y = backdrop_y0_ + i;
      unsigned char* line = audio_backdrop_.scanLine(i);
      if (!waveform.rasterize_row_rgba(v.ms_at_y(static_cast<float>(y)),
                                       v.ms_at_y(static_cast<float>(y + 1)), line, width)) {
        std::memset(line, 0, static_cast<std::size_t>(width) * 4u);
      }
    }
    return;
  }
  for (int i = row0; i < row1; ++i) {
    std::memset(audio_backdrop_.scanLine(i), 0, static_cast<std::size_t>(width) * 4u);
  }
  QPainter wp(&audio_backdrop_);
  wp.setPen(Qt::NoPen);
  wp.setBrush(qcolor(wds::interaction::theme::kEditWaveform));
  wp.setClipRect(QRect(0, row0, width, row1 - row0));
  const float cx = static_cast<float>(width) * 0.5f;
  const float max_w = static_cast<float>(width) * 0.46f;
  float run_y = 0.0f;
  float run_h = 0.0f;
  float run_w = -1.0f;
  auto flush_run = [&]() {
    if (run_w <= 0.5f || run_h <= 0.0f) return;
    wp.drawRect(QRectF(cx - run_w * 0.5f, run_y, run_w, run_h));
  };
  for (int i = row0; i < row1; ++i) {
    const int y = backdrop_y0_ + i;
    const float peak = waveform.peak_in_range(v.ms_at_y(static_cast<float>(y)),
                                              v.ms_at_y(static_cast<float>(y + 1)));
    const float w = std::sqrt(std::clamp(peak, 0.0f, 1.0f)) * max_w;
    if (w <= 0.5f) {
      flush_run();
      run_w = -1.0f;
      continue;
    }
    if (run_w >= 0.0f && std::abs(w - run_w) < 1.0f) {
      run_h += 1.0f;
      run_w = std::max(run_w, w);
    } else {
      flush_run();
      run_y = static_cast<float>(i);
      run_h = 1.0f;
      run_w = w;
    }
  }
  flush_run();
}

void ChartEditWidget::sync_audio_backdrop(const EditViewport& v,
                                          const wds::interaction::Rect& b) {
  if (!panel_) {
    audio_backdrop_ = {};
    backdrop_mode_ = EditSpectrumMode::None;
    return;
  }
  const auto mode = panel_->spectrum_mode();
  const auto* waveform = panel_->waveform();
  if (mode == EditSpectrumMode::None || waveform == nullptr || waveform->empty() ||
      b.w <= 1.0f || b.h <= 1.0f ||
      (mode == EditSpectrumMode::Spectrogram && !waveform->has_spectrogram())) {
    audio_backdrop_ = {};
    backdrop_mode_ = EditSpectrumMode::None;
    backdrop_wave_ = nullptr;
    return;
  }

  const float vis = static_cast<float>(std::max(1, v.visible_ms()));
  const float view_lo = v.scroll_ms();
  const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
  const float H = std::max(b.h, 1.0f);
  const int x0 = std::max(0, static_cast<int>(std::floor(b.x)));
  const int x1 = std::min(width(), static_cast<int>(std::ceil(b.x + b.w)));
  const int y0 = std::max(0, static_cast<int>(std::floor(b.y)));
  const int y1 = std::min(height(), static_cast<int>(std::ceil(b.bottom())));
  if (x1 <= x0 || y1 < y0) return;
  const int cols = x1 - x0;
  const int rows = y1 - y0 + 1;
  const float ms_per_px = vis / H;

  const bool geom_ok = !audio_backdrop_.isNull() && mode == backdrop_mode_ &&
                       waveform == backdrop_wave_ && waveform->size() == backdrop_peaks_ &&
                       waveform->spec_frames() == backdrop_frames_ &&
                       std::abs(backdrop_pw_ - b.w) < 0.5f &&
                       std::abs(backdrop_ph_ - b.h) < 0.5f &&
                       backdrop_visible_ms_ == v.visible_ms() &&
                       std::abs(backdrop_dpr_ - dpr) < 0.01 && backdrop_x0_ == x0 &&
                       backdrop_y0_ == y0 && audio_backdrop_.width() == cols &&
                       audio_backdrop_.height() == rows;

  auto remember_geom = [&]() {
    backdrop_mode_ = mode;
    backdrop_wave_ = waveform;
    backdrop_peaks_ = waveform->size();
    backdrop_frames_ = waveform->spec_frames();
    backdrop_pw_ = b.w;
    backdrop_ph_ = b.h;
    backdrop_dpr_ = dpr;
    backdrop_visible_ms_ = v.visible_ms();
    backdrop_x0_ = x0;
    backdrop_y0_ = y0;
  };

  if (!geom_ok) {
    audio_backdrop_ = QImage(cols, rows, QImage::Format_RGBA8888);
    remember_geom();
    fill_audio_backdrop_rows(v, *waveform, mode, 0, rows);
    backdrop_scroll_ms_ = view_lo;
    return;
  }

  const int drow =
      static_cast<int>(std::lround(static_cast<double>(view_lo - backdrop_scroll_ms_) /
                                   static_cast<double>(ms_per_px)));
  if (drow == 0) return;
  if (drow >= rows || drow <= -rows) {
    fill_audio_backdrop_rows(v, *waveform, mode, 0, rows);
    backdrop_scroll_ms_ = view_lo;
    return;
  }
  const int bpl = audio_backdrop_.bytesPerLine();
  unsigned char* bits = audio_backdrop_.bits();
  if (drow > 0) {
    std::memmove(bits + static_cast<std::size_t>(drow) * static_cast<std::size_t>(bpl), bits,
                 static_cast<std::size_t>(rows - drow) * static_cast<std::size_t>(bpl));
    fill_audio_backdrop_rows(v, *waveform, mode, 0, drow);
  } else {
    const int up = -drow;
    std::memmove(bits, bits + static_cast<std::size_t>(up) * static_cast<std::size_t>(bpl),
                 static_cast<std::size_t>(rows - up) * static_cast<std::size_t>(bpl));
    fill_audio_backdrop_rows(v, *waveform, mode, rows - up, rows);
  }
  backdrop_scroll_ms_ += static_cast<float>(drow) * ms_per_px;
}

void ChartEditWidget::ensure_note_span_cache(
    const std::vector<wds::chart_editor::NotationNote>& notes) {
  if (!panel_) return;
  const auto& doc = panel_->engine().document();
  const uint64_t gen = doc.content_generation();
  if (note_span_doc_ == &doc && note_span_data_ == notes.data() && note_span_n_ == notes.size() &&
      note_span_gen_ == gen) {
    return;
  }
  note_span_doc_ = &doc;
  note_span_data_ = notes.data();
  note_span_n_ = notes.size();
  note_span_gen_ = gen;
  note_sorted_ = true;
  note_max_span_ticks_ = 0;
  split_max_span_ticks_ = 0;
  for (std::size_t i = 0; i < notes.size(); ++i) {
    const auto& n = notes[i];
    if (i > 0 && notes[i - 1].start_tick > n.start_tick) note_sorted_ = false;
    const int64_t span = static_cast<int64_t>(std::max(n.end_tick, n.start_tick)) - n.start_tick;
    note_max_span_ticks_ = std::max(note_max_span_ticks_, span);
    if (wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) {
      split_max_span_ticks_ = std::max(split_max_span_ticks_, span);
    }
  }
}

void ChartEditWidget::sync_visible_notes(const std::vector<wds::chart_editor::NotationNote>& notes,
                                        int32_t range_lo, int32_t range_hi) {
  // Recomputed on every call (O(log N + k) with the span cache): the window moves with the scroll
  // so a (lo, hi) key would never hit, and the indices must not outlive an in-place edit.
  ensure_note_span_cache(notes);
  visible_note_indices_.clear();
  for_each_note_in_tick_window(notes, note_sorted_, note_max_span_ticks_, range_lo, range_hi,
                               [&](std::size_t i) { visible_note_indices_.push_back(i); });
}

void ChartEditWidget::playfield_cull_ticks(int32_t& lo, int32_t& hi) const {
  lo = 0;
  hi = 0;
  if (!panel_) return;
  const auto& v = panel_->viewport();
  const auto b = v.bounds();
  const auto& timing = panel_->engine().document().timing();
  const float pad_ms =
      (v.note_height_px() + 8.0f) * static_cast<float>(v.visible_ms()) / std::max(b.h, 1.0f);
  const float view_ms_lo_cull = v.scroll_ms();
  const float view_ms_hi_cull = view_ms_lo_cull + static_cast<float>(v.visible_ms());
  lo = wds::chart_editor::milliseconds_to_tick(
      static_cast<int64_t>(std::llround(static_cast<double>(view_ms_lo_cull - pad_ms))), timing);
  hi = wds::chart_editor::milliseconds_to_tick(
      static_cast<int64_t>(std::llround(static_cast<double>(view_ms_hi_cull + pad_ms))), timing);
}

void ChartEditWidget::prepare_after_seek(int64_t timeline_ms) {
  if (!panel_) return;
  panel_->sync_to_timeline_ms(static_cast<double>(timeline_ms));
  const auto& v = panel_->viewport();
  const auto b = v.bounds();
  sync_audio_backdrop(v, b);
  int32_t lo = 0;
  int32_t hi = 0;
  playfield_cull_ticks(lo, hi);
  sync_visible_notes(panel_->engine().document().notes(), lo, hi);
}

void ChartEditWidget::paint_waveform(QPainter& p, const EditViewport& v,
                                    const wds::interaction::Rect& b) {
  if (!panel_) return;
  if (panel_->spectrum_mode() == EditSpectrumMode::None) return;
  sync_audio_backdrop(v, b);
  if (audio_backdrop_.isNull()) return;
  p.drawImage(backdrop_x0_, backdrop_y0_, audio_backdrop_);
}

void ChartEditWidget::paint_notes(QPainter& p,
                                  const std::vector<wds::chart_editor::NotationNote>& notes,
                                  const std::vector<std::size_t>& indices, float opacity,
                                  bool show_selection) {
  if (!panel_ || notes.empty() || indices.empty() || opacity <= 0.001f) return;
  const auto& v = panel_->viewport();
  const auto& timing = panel_->engine().document().timing();
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  if (opacity < 0.999f) p.setOpacity(opacity);
  const auto& playfield = v.bounds();
  const auto draw_skin = [&](const QPixmap& image, const QRectF& target, float dest_world_w) {
    if (image.isNull()) {
      p.fillRect(target, QColor(100, 190, 255));
      return;
    }
    paint_sliced_note(p, image, target, dest_world_w);
  };
  // Use the same millisecond-based ordering contract as the Vulkan editor and
  // preview. Tick order alone is wrong across BPM segments and can put a tap
  // behind a cap that is rendered later at the same wall-clock position.
  // The sort key (start ms) is computed once per note here, not inside the comparator; indices are in
  // document order so equal ticks are adjacent and share one conversion.
  note_draw_key_ms_.resize(indices.size());
  for (std::size_t k = 0; k < indices.size(); ++k) {
    const int32_t tick = notes[indices[k]].start_tick;
    note_draw_key_ms_[k] = (k > 0 && tick == notes[indices[k - 1]].start_tick)
                               ? note_draw_key_ms_[k - 1]
                               : wds::chart_editor::tick_to_milliseconds(tick, timing);
  }
  wds::chart_render::build_draw_order_indices(
      indices.size(), note_draw_order_,
      [&](std::size_t i) { return note_draw_key_ms_[i]; },
      [&](std::size_t i) { return static_cast<int32_t>(notes[indices[i]].note_type); });
  // Paint every hold ribbon before any note sprite. A per-note body/cap pass
  // lets a later hold cover taps that happen to share its time range; the
  // editor must instead keep all selectable note art above every ribbon.
  for (const std::size_t vis : note_draw_order_) {
    const auto& n = notes[indices[vis]];
    if (n.note_type == wds::chart_editor::NoteType::HoldEighth ||
        wds::chart_editor::is_split_lane_gimmick(n.gimmick_type) ||
        n.end_tick <= n.start_tick ||
        !wds::chart_editor::is_bindable_hold_body(n.note_type)) {
      continue;
    }
    const bool scratch = wds::chart_editor::is_scratch_hold_body(n.note_type);
    const float y0 = v.y_at(n.start_tick);
    const float y1 = v.y_at(n.end_tick);
    const float body_inset = v.hold_inset_px(n.width);
    const float body_x = v.x_at(n.lane) + body_inset;
    if (!v.lane_span_visible(n.lane, n.width)) continue;
    const float body_w = std::max(0.0f, v.lane_width(n.width) - body_inset * 2.0f);
    if (body_w < 0.5f) continue;
    const float body_top = std::max(std::min(y0, y1), playfield.y - 2.0f);
    const float body_bot = std::min(std::max(y0, y1), playfield.bottom() + 2.0f);
    if (body_bot > body_top) {
      const QPixmap& ribbon = scratch ? hold_purple_ : hold_blue_;
      p.save();
      p.setOpacity(static_cast<qreal>(opacity) * wds::chart_render::kHoldBodyAlpha);
      p.setRenderHint(QPainter::SmoothPixmapTransform, true);
      paint_sliced_hold(p, ribbon, QRectF(body_x, body_top, body_w, body_bot - body_top),
                        v.hold_visual_world_width(n.width));
      p.restore();
    }
  }
  for (const std::size_t vis : note_draw_order_) {
    const auto& n = notes[indices[vis]];
    if (n.note_type == wds::chart_editor::NoteType::HoldEighth) continue;
    if (wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) continue;
    if (!v.lane_span_visible(n.lane, n.width)) continue;
    const float y0 = v.y_at(n.start_tick), y1 = v.y_at(n.end_tick > n.start_tick ? n.end_tick : n.start_tick);
    const float inset = v.note_inset_px(n.width);
    const float x = v.x_at(n.lane) + inset;
    const float w = std::max(0.0f, v.lane_width(n.width) - inset * 2.0f);
    if (w < 0.5f && n.note_type != wds::chart_editor::NoteType::Sound &&
        n.note_type != wds::chart_editor::NoteType::ScratchSound) {
      continue;
    }
    using NT = wds::chart_editor::NoteType;
    const bool scratch = n.note_type == NT::Flick || n.note_type == NT::ScratchHold ||
                         n.note_type == NT::ScratchCriticalHold || n.note_type == NT::NontailScratchHold ||
                         n.note_type == NT::NontailScratchCriticalHold;
    const bool critical = n.note_type == NT::Critical || n.note_type == NT::CriticalHold ||
                          n.note_type == NT::CriticalHoldStart || n.note_type == NT::NontailCriticalHold ||
                          n.note_type == NT::ScratchCriticalHold || n.note_type == NT::ScratchCriticalHoldStart;
    // Ribbon includes nontail; with_tail is end-cap / JumpScratch span only.
    // Match preview: bindable bodies never invent a start head.
    const bool hold_ribbon =
        n.end_tick > n.start_tick && wds::chart_editor::is_bindable_hold_body(n.note_type);
    const bool with_tail =
        n.end_tick > n.start_tick && wds::chart_editor::is_hold_with_tail(n.note_type);
    int end_lane = n.lane, end_width = n.width;
    if (with_tail && wds::chart_editor::is_hold_chain_body(n.note_type)) {
      const auto span = wds::chart_editor::resolve_end_lane_span(n);
      end_lane = span.first; end_width = span.second;
    }
    const float tail_inset = v.note_inset_px(end_width);
    const float tail_x = v.x_at(end_lane) + tail_inset;
    const float tail_w = std::max(0.0f, v.lane_width(end_width) - tail_inset * 2.0f);
    const QPixmap* skin = &red_;
    if (n.note_type == NT::Sound) skin = &tick_blue_;
    else if (n.note_type == NT::ScratchSound) skin = &tick_purple_;
    else if (critical) skin = &yellow_;
    else if (scratch) skin = &purple_;
    else if (n.note_type == NT::Hold || n.note_type == NT::HoldStart || n.note_type == NT::BlueTap ||
             n.note_type == NT::NontailHold) skin = &blue_;
    QRectF head(x, y0 - v.note_height_px()*.5f, w, v.note_height_px());
    const bool is_tick = n.note_type == NT::Sound || n.note_type == NT::ScratchSound;
    if (is_tick) {
      const auto star = v.mid_star_screen_rect(n);
      head = {star.x, star.y, star.w, star.h};
    }
    // Hold bodies do not own a start cap; their paired HoldStart does. This is
    // essential at ScratchHold chain joints where two caps otherwise overlap.
    // Nontail ribbons also must not invent a start (or end) flat.
    if (!hold_ribbon) {
      if (is_tick) {
        if (skin->isNull()) p.fillRect(head, QColor(100, 190, 255));
        else p.drawPixmap(head, *skin, skin->rect());
      } else {
        draw_skin(*skin, head, v.tap_visual_world_width(n.width));
      }
    }
    if (with_tail) {
      // Tail caps are real note sprites, not just the ribbon endpoint.  Draw
      // them independently so a terminal ScratchHold still exposes its flick.
      // Gold-head bodies stay blue/purple at the tail — only the paired
      // CriticalHoldStart / ScratchCriticalHoldStart is yellow.
      const QPixmap* tail_skin =
          wds::chart_editor::is_scratch_hold_body(n.note_type) ? &purple_ : &blue_;
      QRectF tail(tail_x, y1 - v.note_height_px()*.5f, tail_w, v.note_height_px());
      draw_skin(*tail_skin, tail, v.tap_visual_world_width(end_width));
      if (scratch && !arrow_.isNull()) {
        const float ah = v.note_height_px();
        const float aw = std::min(tail_w, std::clamp(v.lane_width(1) * .55f, 8.0f, ah * 1.2f));
        wds::chart_render::StaticArrowLayoutParams ap;
        ap.span_left = tail_x; ap.span_right = tail_x + tail_w; ap.arrow_w = aw; ap.scratch_length = n.scratch_length;
        for (const auto& a : wds::chart_render::layout_static_scratch_arrows(ap)) {
          const QRectF ar(a.x0, y1-ah*.5f, a.x1-a.x0, ah);
          if (a.flip_x) p.drawPixmap(ar, arrow_mirrored_, arrow_mirrored_.rect());
          else p.drawPixmap(ar, arrow_, arrow_.rect());
        }
      }
    }
    if (show_selection && panel_->selected().count(n.id)) {
      ScopedAA aa(p);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(qcolor(wds::interaction::theme::kEditSelection), 2.5));
      if (hold_ribbon) {
        const float left = std::min(x, tail_x), right = std::max(x + w, tail_x + tail_w);
        // Without its paired head selected, the body box uses the headless-hold
        // style: the visible box starts on the start tick, not half a
        // note-height past it. Whole/Chain select head+body and keep the cap.
        const auto paired_head =
            wds::chart_editor::paired_hold_head_for(panel_->engine().document(), n);
        const bool headless_start =
            !paired_head || panel_->selected().count(paired_head->id) == 0;
        float start_ext = v.note_height_px() * 0.5f;
        float end_ext = v.note_height_px() * 0.5f;
        if (headless_start) {
          if (y0 >= y1) {
            start_ext = 0.0f;
          } else {
            end_ext = 0.0f;
          }
        }
        const float top = std::min(y0, y1) - end_ext;
        const float bottom = std::max(y0, y1) + start_ext;
        p.drawRoundedRect(QRectF(left, top, right-left, bottom-top).adjusted(-3,-3,3,3), 4, 4);
      } else {
        p.drawRoundedRect(head.adjusted(-3,-3,3,3), 4, 4);
      }
    }
  }
  // Flick sprites are a separate top pass in the legacy renderer. This keeps
  // a chain's terminal arrow above the following segment's tap cap.
  if (!arrow_.isNull()) {
    for (const std::size_t index : indices) {
      const auto& n = notes[index];
      if (!wds::chart_editor::is_flick_like(n.note_type)) continue;
      const float y = v.y_at(n.start_tick), inset = v.note_inset_px(n.width);
      const float sx = v.x_at(n.lane) + inset, sw = std::max(0.0f, v.lane_width(n.width) - inset * 2.0f);
      const float ah = v.note_height_px();
      const float aw = std::min(sw, std::clamp(v.lane_width(1) * .55f, 8.0f, ah * 1.2f));
      wds::chart_render::StaticArrowLayoutParams ap{sx, sx + sw, aw, n.scratch_length};
      for (const auto& a : wds::chart_render::layout_static_scratch_arrows(ap)) {
        const QRectF ar(a.x0, y - ah * .5f, a.x1 - a.x0, ah);
        if (a.flip_x) p.drawPixmap(ar, arrow_mirrored_, arrow_mirrored_.rect());
        else p.drawPixmap(ar, arrow_, arrow_.rect());
      }
    }
  }
  p.restore();
}
void ChartEditWidget::present_qt_modals() {
  if (!panel_ || qt_modal_open_ || !panel_->has_modal_popup()) return;
  // Opening on press would click through into the dialog; wait until buttons are up.
  if (QGuiApplication::mouseButtons() != Qt::NoButton) return;
  releaseMouse();

  ChartEditPanel::SplitModalDraft split;
  if (panel_->take_split_modal(split)) {
    qt_modal_open_ = true;
    SplitPickerDialog dialog(split.count, split.color_id, split.type, this, skins_dir_,
                            split_effect_quality_);
    const int result = dialog.exec();
    qt_modal_open_ = false;
    if (result == QDialog::Accepted) {
      if (split.edit_id >= 0) {
        (void)panel_->edit_split_effect(split.edit_id, dialog.count(), dialog.color_id(),
                                        dialog.type());
      } else {
        (void)panel_->add_split_effect(split.tick, dialog.count(), dialog.color_id(),
                                       dialog.type());
      }
    }
    mark_dirty();
    update();
    return;
  }

  ChartEditPanel::TimingModalDraft timing;
  if (panel_->take_timing_modal(timing)) {
    qt_modal_open_ = true;
    TimingEditDialog dialog(timing.bpm_mode, timing.bpm, timing.numerator, timing.denominator, this);
    const int result = dialog.exec();
    qt_modal_open_ = false;
    if (result == QDialog::Accepted) {
      if (timing.bpm_mode) {
        (void)panel_->apply_bpm(timing.tick, dialog.bpm());
      } else {
        (void)panel_->apply_meter(timing.tick, dialog.numerator(), dialog.denominator());
      }
    }
    mark_dirty();
    update();
    return;
  }

  ChartEditPanel::SpeedModalDraft speed;
  if (panel_->take_speed_modal(speed)) {
    qt_modal_open_ = true;
    SpeedEditDialog dialog(speed.multiplier, this);
    const int result = dialog.exec();
    qt_modal_open_ = false;
    if (result == QDialog::Accepted) {
      (void)panel_->apply_scroll_speed(speed.tick, dialog.multiplier());
    }
    mark_dirty();
    update();
  }
}

void ChartEditWidget::mousePressEvent(QMouseEvent* e) {
  if (!panel_) return;
  if (extra_lane_slider_ != nullptr && extra_lane_slider_->isVisible() &&
      extra_lane_slider_->geometry().contains(e->position().toPoint())) {
    return;
  }
  setFocus();
  grabMouse();
  const auto pos = point(e->position());
  const auto btn = button(e->button());
  const auto m = mods(e->modifiers());
  ChartEditJournal scope(wds::common::CrashInputKind::PointerDown, pos.x, pos.y, 0.0f, 0.0f, 0,
                         pack_mods(m), static_cast<std::uint8_t>(btn), 0);
  panel_->sync_global_pointer(pos);
  panel_->on_pointer_down({pos, btn, m});
  if (panel_->has_modal_popup()) releaseMouse();
  present_qt_modals();
  mark_dirty();
  update();
}
void ChartEditWidget::mouseMoveEvent(QMouseEvent* e) {
  if (!panel_) return;
  const auto pos = point(e->position());
  // 0 hover / 1 drag / 2 scrub. Pointer moves are hover or drag; wheel is Scroll.
  const std::uint8_t mode = (e->buttons() != Qt::NoButton) ? std::uint8_t{1} : std::uint8_t{0};
  wds::common::journal_note_move(pos.x, pos.y, mode);
  panel_->sync_global_pointer(pos);
  panel_->on_pointer_move({pos, mods(e->modifiers())});
  switch (panel_->hover_cursor()) {
    case wds::interaction::CursorKind::ResizeHorizontal: setCursor(Qt::SizeHorCursor); break;
    case wds::interaction::CursorKind::ResizeVertical: setCursor(Qt::SizeVerCursor); break;
    default: setCursor(Qt::ArrowCursor); break;
  }
  mark_dirty();
  update();
}
void ChartEditWidget::mouseReleaseEvent(QMouseEvent* e) {
  if (!panel_) return;
  const auto pos = point(e->position());
  const auto btn = button(e->button());
  const auto m = mods(e->modifiers());
  ChartEditJournal scope(wds::common::CrashInputKind::PointerUp, pos.x, pos.y, 0.0f, 0.0f, 0,
                         pack_mods(m), static_cast<std::uint8_t>(btn), 0);
  panel_->sync_global_pointer(pos);
  panel_->on_pointer_up({pos, btn, m});
  if (e->buttons() == Qt::NoButton) releaseMouse();
  present_qt_modals();
  mark_dirty();
  update();
}
void ChartEditWidget::mouseDoubleClickEvent(QMouseEvent* e) {
  if (!panel_) return;
  const auto pos = point(e->position());
  const auto btn = button(e->button());
  const auto m = mods(e->modifiers());
  ChartEditJournal scope(wds::common::CrashInputKind::DoubleClick, pos.x, pos.y, 0.0f, 0.0f, 0,
                         pack_mods(m), static_cast<std::uint8_t>(btn), 0);
  panel_->on_double_click({pos, btn, m});
  mark_dirty();
  update();
}
void ChartEditWidget::wheelEvent(QWheelEvent* e) {
  if (!panel_) return;
  const QPoint angle = e->angleDelta();
  const QPoint pixel = e->pixelDelta();
  // Keep Qt's high-resolution trackpad deltas continuous; legacy wheels use
  // the same 120-unit normalization as the old input adapter.
  float dx = pixel.x() != 0 ? pixel.x() / 10.0f : angle.x() / 40.0f;
  float dy = pixel.y() != 0 ? pixel.y() / 10.0f : angle.y() / 40.0f;
  apply_scroll_invert(dx, dy);
  const auto pos = point(e->position());
  const auto m = mods(e->modifiers());
  ChartEditJournal scope(wds::common::CrashInputKind::Scroll, pos.x, pos.y, dx, dy, 0, pack_mods(m),
                         0, 0);
  // Option/Alt+wheel zooms visible range. Primary (Cmd/Ctrl)+wheel scrubs.
  panel_->on_scroll({pos, dx, dy, m});
  mark_dirty();
  update();
}
void ChartEditWidget::keyPressEvent(QKeyEvent* e) {
  if (!panel_) return;
  const auto ev = wds::interaction::KeyDownEvent{qt_key_code(e->key()), mods(e->modifiers()), e->isAutoRepeat()};
  {
    ChartEditJournal scope(wds::common::CrashInputKind::KeyDown, 0.0f, 0.0f, 0.0f, 0.0f,
                           static_cast<std::int32_t>(ev.key), pack_mods(ev.mods), 0,
                           ev.repeat ? 1 : 0);
    // Space is an application command, never an edit-canvas command. Qt dialogs
    // own their own keyboard; the old painted popups no longer take keys here.
    if (ev.key == wds::interaction::KeyCode::Space && global_key_handler_ &&
        !panel_->captures_keys()) {
      global_key_handler_(ev);
    } else {
      panel_->on_key_down(ev);
    }
  }
  if (!e->isAutoRepeat() && !e->text().isEmpty() && panel_->has_modal_popup()) {
    const std::string text = e->text().toUtf8().toStdString();
    ChartEditJournal scope(wds::common::CrashInputKind::TextInput, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0,
                           0);
    wds::common::journal_set_text(wds::common::journal_allow_sensitive() ? text.c_str() : nullptr,
                                  text.size());
    panel_->on_text_input({text});
  }
  mark_dirty();
  update();
}
void ChartEditWidget::keyReleaseEvent(QKeyEvent* e) {
  if (!panel_) return;
  // queryKeyboardModifiers() is the live OS state after the release; OR-ing
  // e->modifiers() would keep Shift/Cmd stuck on (Qt still reports them).
  const auto ev = wds::interaction::KeyUpEvent{qt_key_code(e->key()), qt_live_modifiers()};
  ChartEditJournal scope(wds::common::CrashInputKind::KeyUp, 0.0f, 0.0f, 0.0f, 0.0f,
                         static_cast<std::int32_t>(ev.key), pack_mods(ev.mods), 0, 0);
  if (ev.key != wds::interaction::KeyCode::Space || panel_->captures_keys()) panel_->on_key_up(ev);
  mark_dirty();
  update();
}
void ChartEditWidget::inputMethodEvent(QInputMethodEvent* e) {
  if (panel_ && panel_->has_modal_popup() && !e->commitString().isEmpty()) {
    const std::string text = e->commitString().toUtf8().toStdString();
    ChartEditJournal scope(wds::common::CrashInputKind::TextInput, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0,
                           0);
    wds::common::journal_set_text(wds::common::journal_allow_sensitive() ? text.c_str() : nullptr,
                                  text.size());
    panel_->on_text_input({text});
    mark_dirty();
    update();
  }
  QWidget::inputMethodEvent(e);
}
}

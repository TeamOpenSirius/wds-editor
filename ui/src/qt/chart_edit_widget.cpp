#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/qt_input_adapter.hpp"
#include "wds/ui/qt/split_effect_snapshot.hpp"
#include "wds/ui/qt/split_picker_dialog.hpp"
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

#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
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

void paint_lane_guides(QPainter& p, const EditViewport& v, const wds::interaction::Rect& b,
                       const std::vector<wds::chart_editor::NotationNote>& notes,
                       const wds::chart_editor::MusicTiming& timing,
                       const wds::chart_editor::PreviewConfig& preview) {
  const float view_ms_lo = v.scroll_ms();
  const float view_ms_hi = view_ms_lo + static_cast<float>(v.visible_ms());
  const auto merged =
      merged_split_hide_ranges_ms(notes, timing, preview, view_ms_lo, view_ms_hi);
  const int lanes = std::max(1, v.grid().lane_count);
  auto draw_span = [&](float y0, float y1) {
    const float top = std::max(std::min(y0, y1), b.y);
    const float bot = std::min(std::max(y0, y1), b.bottom());
    if (bot <= top) return;
    for (int i = 0; i <= lanes; ++i) {
      const bool edge = i == 0 || i == lanes;
      namespace th = wds::interaction::theme;
      p.setPen(QPen(qcolor(edge ? th::kEditLaneEdge : th::kEditLaneInner), edge ? 1.5 : 1.0));
      p.drawLine(QPointF(v.x_at(i), top), QPointF(v.x_at(i), bot));
    }
  };
  float cursor = view_ms_lo;
  for (const auto& seg : merged) {
    if (seg.first > cursor) draw_span(v.y_at_ms(cursor), v.y_at_ms(seg.first));
    cursor = std::max(cursor, seg.second);
  }
  if (view_ms_hi > cursor) draw_span(v.y_at_ms(cursor), v.y_at_ms(view_ms_hi));
}

void paint_split_lines(QPainter& p, const EditViewport& v, const wds::interaction::Rect& b,
                       const std::vector<wds::chart_editor::NotationNote>& notes,
                       const wds::chart_editor::MusicTiming& timing,
                       const wds::chart_editor::PreviewConfig& preview,
                       int32_t highlighted_split_note_id) {
  constexpr float kSplitLineW = 2.0f;
  constexpr float kSplitLineHighlightW = 4.0f;
  const int lanes = std::max(1, v.grid().lane_count);
  const float view_ms_lo = v.scroll_ms();
  const float view_ms_hi = view_ms_lo + static_cast<float>(v.visible_ms());
  auto paint_one = [&](const wds::chart_editor::NotationNote& note, bool highlight) {
    const int64_t start_ms = note.start_ms(timing);
    const int64_t end_ms = std::max(start_ms, note.end_ms(timing));
    const int64_t appear = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::llround(static_cast<double>(preview.split_line_animation_start_sec) * 1000.0)));
    const float fade0 = static_cast<float>(start_ms - appear);
    const float fade1 = static_cast<float>(
        wds::chart_editor::official_split_destroy_ms(start_ms, end_ms));
    if (fade1 < view_ms_lo || fade0 > view_ms_hi) return;
    const int32_t color_id = note.scratch_length;
    const int32_t split_count = wds::chart_editor::get_split_count(note.gimmick_type);
    const float line_w = highlight ? kSplitLineHighlightW : kSplitLineW;
    std::vector<int32_t> mids;
    split_boundaries_12(split_count, mids);
    // `ramp` segments (fade-in / fade-out) carry a vertical alpha gradient sampled from the opacity curve, so
    // opacity varies along the line from the fade start to the fade end instead of one flat value per frame.
    constexpr int kRampStops = 24;
    auto draw_edges = [&](float ms0, float ms1, bool ramp) {
      if (ms1 <= ms0) return;
      const float y0 = v.y_at_ms(ms0);
      const float y1 = v.y_at_ms(ms1);
      const float top = std::max(std::min(y0, y1), b.y);
      const float bot = std::min(std::max(y0, y1), b.bottom());
      if (bot <= top) return;
      auto opacity_at = [&](float ms) {
        return split_line_opacity_at_ms(note, timing, preview,
                                        static_cast<int64_t>(std::llround(ms)));
      };
      // Per-stop opacity along [ms0, ms1] (t = 0 at ms0 / y0, t = 1 at ms1 / y1).
      float stops[kRampStops + 1];
      float max_op = 0.0f;
      const bool use_ramp = ramp && std::abs(y1 - y0) >= 1.0f;
      if (use_ramp) {
        for (int i = 0; i <= kRampStops; ++i) {
          const float t = static_cast<float>(i) / static_cast<float>(kRampStops);
          stops[i] = opacity_at(ms0 + (ms1 - ms0) * t);
          max_op = std::max(max_op, stops[i]);
        }
      } else {
        max_op = opacity_at((ms0 + ms1) * 0.5f);
      }
      if (max_op <= 0.001f) return;
      auto draw_v = [&](int32_t edge, int32_t slot) {
        auto c = split_slot_color(color_id, slot, split_count, nullptr);
        if (c.a < 0.02f) return;
        apply_official_split_rgb_opacity(c);
        if (highlight) {
          c = c.lerp({1.0f, 1.0f, 1.0f, c.a}, 0.28f);
        }
        const float x = std::floor(v.x_at(edge) + 0.5f);
        const QRectF r(x - static_cast<qreal>(line_w) * 0.5, top, line_w, bot - top);
        if (use_ramp) {
          QLinearGradient grad(QPointF(x, y0), QPointF(x, y1));
          for (int i = 0; i <= kRampStops; ++i) {
            auto stop_c = c;
            stop_c.a *= stops[i];
            grad.setColorAt(static_cast<qreal>(i) / kRampStops, qcolor(stop_c));
          }
          p.fillRect(r, QBrush(grad));
        } else {
          c.a *= max_op;
          p.fillRect(r, qcolor(c));
        }
      };
      draw_v(0, 0);
      int32_t slot = 1;
      for (int32_t mid : mids) draw_v(mid + 1, slot++);
      draw_v(lanes, std::max(1, split_count));
    };
    draw_edges(std::max(fade0, view_ms_lo), std::min(static_cast<float>(start_ms), view_ms_hi), true);
    draw_edges(std::max(static_cast<float>(start_ms), view_ms_lo),
               std::min(static_cast<float>(end_ms), view_ms_hi), false);
    draw_edges(std::max(static_cast<float>(end_ms), view_ms_lo), std::min(fade1, view_ms_hi), true);
  };
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.id == highlighted_split_note_id) continue;
    paint_one(note, false);
  }
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.id != highlighted_split_note_id) continue;
    paint_one(note, true);
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

// Fade-in duration exactly as paint_split_lines computes it.
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
                           const std::vector<wds::chart_editor::NotationNote>& notes,
                           const wds::chart_editor::MusicTiming& timing,
                           const wds::chart_editor::PreviewConfig& preview,
                           int32_t highlighted_split_note_id, QObject* notify,
                           const std::function<void()>& on_ready) {
  constexpr float kMinBandPx = 16.0f;
  const float view_ms_lo = v.scroll_ms();
  const float view_ms_hi = view_ms_lo + static_cast<float>(v.visible_ms());
  const float lane_px = split_lane_px(v);
  if (lane_px <= 0.5f) return;
  const float x0 = std::floor(v.x_at(0) + 0.5f);
  auto paint_one = [&](const wds::chart_editor::NotationNote& note) {
    const int64_t start_ms = note.start_ms(timing);
    const int64_t appear = split_appear_ms(preview);
    const float fade0 = static_cast<float>(start_ms - appear);
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
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.id == highlighted_split_note_id) continue;
    paint_one(note);
  }
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.id != highlighted_split_note_id) continue;
    paint_one(note);
  }
}

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
void ChartEditWidget::resizeEvent(QResizeEvent*) {
  if (panel_) panel_->set_bounds({0, 0, static_cast<float>(width()), static_cast<float>(height())});
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
    split_notes_scratch_.clear();
    for (const auto& note : notes) {
      if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
      if (note_in_tick_window(note, split_lo, split_hi)) split_notes_scratch_.push_back(note);
    }
    paint_lane_guides(p, v, b, split_notes_scratch_, timing, preview);
  }
  paint_split_lines(p, v, b, split_notes_scratch_, timing, preview,
                   panel_->active_split_highlight_id());
  // Judgment-area skin sits below notes and spans the authored playfield.
  if (!judgment_.isNull()) {
    const float h = v.judgeline_height_px();
    p.drawPixmap(QRectF(b.x, v.judgeline_y() - h * .5f, b.w, h), judgment_, judgment_.rect());
  }

  paint_notes(p, notes, 1.0f, true, cull_lo, cull_hi);
  {
    // Prefetch first, visible requests second: the builder is LIFO so the visible clusters are built first.
    {
      // Every split shares the same fade-in duration, so the band at the current zoom is one value (px per ms).
      const int64_t appear = split_appear_ms(preview);
      schedule_split_prefetch(notes, split_lane_px(v), split_band_q(v, 0.0f, appear));
    }
    paint_split_particles(p, v, b, split_notes_scratch_, timing, preview, panel_->active_split_highlight_id(),
                          this, [this] { request_split_repaint(); });
  }
  paint_click_marks(p, v, *panel_);
  for (const auto& ghost : panel_->skinned_ghosts()) {
    if (!ghost.visible) continue;
    ghost_scratch_.clear();
    ghost_scratch_.push_back(ghost.note);
    paint_notes(p, ghost_scratch_, ghost.alpha, false, cull_lo, cull_hi);
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
      {
        ScopedAA aa(p);
        paint_lane_guides(p, v, b, split_notes_scratch_, timing, preview);
      }
      paint_split_lines(p, v, b, split_notes_scratch_, timing, preview,
                        panel_->active_split_highlight_id());
      p.restore();
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

void ChartEditWidget::sync_waveform_peaks(const EditViewport& v,
                                         const wds::interaction::Rect& b) {
  const auto* waveform = panel_ ? panel_->waveform() : nullptr;
  if (!waveform || waveform->empty() || b.w <= 1.0f || b.h <= 1.0f) return;

  const float vis = static_cast<float>(std::max(1, v.visible_ms()));
  const float view_lo = v.scroll_ms();
  const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
  const float H = std::max(b.h, 1.0f);
  const int y0 = std::max(0, static_cast<int>(std::floor(b.y)));
  const int y1 = std::min(height(), static_cast<int>(std::ceil(b.bottom())));
  if (y1 < y0) return;
  const int rows = y1 - y0 + 1;
  const float ms_per_px = vis / H;

  auto sample_row = [&](int y) {
    return waveform->peak_in_range(v.ms_at_y(static_cast<float>(y)),
                                   v.ms_at_y(static_cast<float>(y + 1)));
  };

  const bool geom_ok = waveform == wave_cache_key_ &&
                       std::abs(wave_cache_pw_ - b.w) < 0.5f &&
                       std::abs(wave_cache_ph_ - b.h) < 0.5f &&
                       wave_cache_visible_ms_ == v.visible_ms() &&
                       std::abs(wave_cache_dpr_ - dpr) < 0.01 &&
                       wave_peaks_y0_ == y0 &&
                       static_cast<int>(wave_row_peaks_.size()) == rows;

  if (!geom_ok) {
    wave_cache_key_ = waveform;
    wave_cache_pw_ = b.w;
    wave_cache_ph_ = b.h;
    wave_cache_dpr_ = dpr;
    wave_cache_visible_ms_ = v.visible_ms();
    wave_peaks_y0_ = y0;
    wave_row_peaks_.resize(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) wave_row_peaks_[static_cast<std::size_t>(i)] = sample_row(y0 + i);
    wave_peaks_scroll_ms_ = view_lo;
  } else {
    const int drow =
        static_cast<int>(std::lround(static_cast<double>(view_lo - wave_peaks_scroll_ms_) /
                                     static_cast<double>(ms_per_px)));
    if (drow == 0) {
      // Same whole-pixel scroll: reuse cached peaks.
    } else if (drow >= rows || drow <= -rows) {
      for (int i = 0; i < rows; ++i) wave_row_peaks_[static_cast<std::size_t>(i)] = sample_row(y0 + i);
      wave_peaks_scroll_ms_ = view_lo;
    } else if (drow > 0) {
      // Playback: later time at each y. Old row i moves to i+drow.
      std::memmove(wave_row_peaks_.data() + drow, wave_row_peaks_.data(),
                   static_cast<std::size_t>(rows - drow) * sizeof(float));
      for (int i = 0; i < drow; ++i) wave_row_peaks_[static_cast<std::size_t>(i)] = sample_row(y0 + i);
      wave_peaks_scroll_ms_ += static_cast<float>(drow) * ms_per_px;
    } else {
      const int up = -drow;
      std::memmove(wave_row_peaks_.data(), wave_row_peaks_.data() + up,
                   static_cast<std::size_t>(rows - up) * sizeof(float));
      for (int i = rows - up; i < rows; ++i)
        wave_row_peaks_[static_cast<std::size_t>(i)] = sample_row(y0 + i);
      wave_peaks_scroll_ms_ += static_cast<float>(drow) * ms_per_px;
    }
  }
}

void ChartEditWidget::sync_visible_notes(const std::vector<wds::chart_editor::NotationNote>& notes,
                                        int32_t range_lo, int32_t range_hi) {
  if (note_cull_notes_ == notes.data() && note_cull_n_ == notes.size() &&
      note_cull_lo_ == range_lo && note_cull_hi_ == range_hi) {
    return;
  }
  note_cull_notes_ = notes.data();
  note_cull_n_ = notes.size();
  note_cull_lo_ = range_lo;
  note_cull_hi_ = range_hi;
  visible_note_indices_.clear();
  if (visible_note_indices_.capacity() < notes.size()) visible_note_indices_.reserve(notes.size());
  for (std::size_t i = 0; i < notes.size(); ++i) {
    if (note_in_tick_window(notes[i], range_lo, range_hi)) visible_note_indices_.push_back(i);
  }
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
  sync_waveform_peaks(v, b);
  int32_t lo = 0;
  int32_t hi = 0;
  playfield_cull_ticks(lo, hi);
  sync_visible_notes(panel_->engine().document().notes(), lo, hi);
}

void ChartEditWidget::paint_waveform(QPainter& p, const EditViewport& v,
                                    const wds::interaction::Rect& b) {
  const auto* waveform = panel_ ? panel_->waveform() : nullptr;
  if (!waveform || waveform->empty() || b.w <= 1.0f || b.h <= 1.0f) return;
  sync_waveform_peaks(v, b);
  if (wave_row_peaks_.empty()) return;
  const int y0 = wave_peaks_y0_;
  const int rows = static_cast<int>(wave_row_peaks_.size());

  const float center = b.x + b.w * 0.5f;
  const float max_w = b.w * 0.46f;
  const QColor wave_color = qcolor(wds::interaction::theme::kEditWaveform);
  QVarLengthArray<QRectF, 512> wave_rects;
  wave_rects.reserve(rows);
  for (int i = 0; i < rows; ++i) {
    const float amp =
        std::sqrt(std::clamp(wave_row_peaks_[static_cast<std::size_t>(i)], 0.0f, 1.0f)) * max_w;
    if (amp <= 0.5f) continue;
    wave_rects.append(QRectF(center - amp, y0 + i, amp * 2.0f, 1.0f));
  }
  if (!wave_rects.isEmpty()) {
    // Same brush fill as fillRect; AA is off on this painter. Restore pen
    // and brush so later playfield draws keep the previous painter state.
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(wave_color);
    p.drawRects(wave_rects.constData(), wave_rects.size());
    p.restore();
  }
}

void ChartEditWidget::paint_notes(QPainter& p,
                                  const std::vector<wds::chart_editor::NotationNote>& notes,
                                  float opacity, bool show_selection, int32_t range_lo,
                                  int32_t range_hi) {
  if (!panel_ || notes.empty() || opacity <= 0.001f) return;
  const auto& v = panel_->viewport();
  const auto& timing = panel_->engine().document().timing();
  sync_visible_notes(notes, range_lo, range_hi);
  if (visible_note_indices_.empty()) return;
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
  wds::chart_render::build_draw_order_indices(
      visible_note_indices_.size(), note_draw_order_,
      [&](std::size_t i) {
        return wds::chart_editor::tick_to_milliseconds(
            notes[visible_note_indices_[i]].start_tick, timing);
      },
      [&](std::size_t i) {
        return static_cast<int32_t>(notes[visible_note_indices_[i]].note_type);
      });
  // Paint every hold ribbon before any note sprite. A per-note body/cap pass
  // lets a later hold cover taps that happen to share its time range; the
  // editor must instead keep all selectable note art above every ribbon.
  for (const std::size_t vis : note_draw_order_) {
    const auto& n = notes[visible_note_indices_[vis]];
    if (n.note_type == wds::chart_editor::NoteType::HoldEighth ||
        wds::chart_editor::is_split_lane_gimmick(n.gimmick_type) ||
        n.end_tick <= n.start_tick ||
        !wds::chart_editor::is_hold_with_tail(n.note_type)) {
      continue;
    }
    const bool scratch = wds::chart_editor::is_scratch_hold_body(n.note_type);
    const float y0 = v.y_at(n.start_tick);
    const float y1 = v.y_at(n.end_tick);
    const float body_inset = v.hold_inset_px(n.width);
    const float body_x = v.x_at(n.lane) + body_inset;
    const float body_w = std::max(4.0f, v.lane_width(n.width) - body_inset * 2.0f);
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
    const auto& n = notes[visible_note_indices_[vis]];
    if (n.note_type == wds::chart_editor::NoteType::HoldEighth) continue;
    if (wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) continue;
    const float y0 = v.y_at(n.start_tick), y1 = v.y_at(n.end_tick > n.start_tick ? n.end_tick : n.start_tick);
    const float inset = v.note_inset_px(n.width);
    const float x = v.x_at(n.lane) + inset;
    const float w = std::max(4.0f, v.lane_width(n.width) - inset * 2.0f);
    using NT = wds::chart_editor::NoteType;
    const bool scratch = n.note_type == NT::Flick || n.note_type == NT::ScratchHold ||
                         n.note_type == NT::ScratchCriticalHold || n.note_type == NT::NontailScratchHold ||
                         n.note_type == NT::NontailScratchCriticalHold;
    const bool critical = n.note_type == NT::Critical || n.note_type == NT::CriticalHold ||
                          n.note_type == NT::CriticalHoldStart || n.note_type == NT::NontailCriticalHold ||
                          n.note_type == NT::ScratchCriticalHold || n.note_type == NT::ScratchCriticalHoldStart;
    const bool hold_body = n.end_tick > n.start_tick && wds::chart_editor::is_hold_with_tail(n.note_type);
    int end_lane = n.lane, end_width = n.width;
    if (hold_body && wds::chart_editor::is_hold_chain_body(n.note_type)) {
      const auto span = wds::chart_editor::resolve_end_lane_span(n);
      end_lane = span.first; end_width = span.second;
    }
    const float tail_inset = v.note_inset_px(end_width);
    const float tail_x = v.x_at(end_lane) + tail_inset;
    const float tail_w = std::max(4.0f, v.lane_width(end_width) - tail_inset * 2.0f);
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
    if (!hold_body) {
      if (is_tick) {
        if (skin->isNull()) p.fillRect(head, QColor(100, 190, 255));
        else p.drawPixmap(head, *skin, skin->rect());
      } else {
        draw_skin(*skin, head, v.tap_visual_world_width(n.width));
      }
    }
    if (hold_body) {
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
      if (hold_body) {
        const float left = std::min(x, tail_x), right = std::max(x + w, tail_x + tail_w);
        const float top = std::min(y0, y1) - v.note_height_px()*.5f;
        const float bottom = std::max(y0, y1) + v.note_height_px()*.5f;
        p.drawRoundedRect(QRectF(left, top, right-left, bottom-top).adjusted(-3,-3,3,3), 4, 4);
      } else {
        p.drawRoundedRect(head.adjusted(-3,-3,3,3), 4, 4);
      }
    }
  }
  // Flick sprites are a separate top pass in the legacy renderer. This keeps
  // a chain's terminal arrow above the following segment's tap cap.
  if (!arrow_.isNull()) {
    for (const std::size_t index : visible_note_indices_) {
      const auto& n = notes[index];
      if (n.note_type != wds::chart_editor::NoteType::Flick) continue;
      const float y = v.y_at(n.start_tick), inset = v.note_inset_px(n.width);
      const float sx = v.x_at(n.lane) + inset, sw = std::max(4.0f, v.lane_width(n.width) - inset * 2.0f);
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
  }
}

void ChartEditWidget::mousePressEvent(QMouseEvent* e) {
  if (!panel_) return;
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

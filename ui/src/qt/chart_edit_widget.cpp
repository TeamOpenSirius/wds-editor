#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/split_picker_dialog.hpp"
#include "wds/ui/qt/timing_edit_dialog.hpp"
#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/chart_render/note_draw_order.hpp>
#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/ui/regions/edit/edit_gutters.hpp>

#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QDir>
#include <QFont>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

namespace wds::ui {
namespace {

QColor qcolor(const wds::interaction::Color& c) {
  return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f),
                          std::clamp(c.b, 0.0f, 1.0f), std::clamp(c.a, 0.0f, 1.0f));
}

QRectF qrect(const wds::interaction::Rect& r) { return {r.x, r.y, r.w, r.h}; }

void fill_ui_rect(QPainter& p, const wds::interaction::UiPaintRect& r) {
  p.setPen(Qt::NoPen);
  p.setBrush(qcolor(r.color));
  if (r.corner_radius > 0.5f) {
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
      p.setPen(QPen(edge ? QColor(110, 115, 130) : QColor(55, 60, 72), edge ? 1.5 : 1.0));
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
                       const wds::chart_editor::PreviewConfig& preview) {
  const int lanes = std::max(1, v.grid().lane_count);
  const float view_ms_lo = v.scroll_ms();
  const float view_ms_hi = view_ms_lo + static_cast<float>(v.visible_ms());
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    const int64_t start_ms = note.start_ms(timing);
    const int64_t end_ms = std::max(start_ms, note.end_ms(timing));
    const int64_t appear = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::llround(static_cast<double>(preview.split_line_animation_start_sec) * 1000.0)));
    const int64_t disappear = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::llround(static_cast<double>(preview.split_line_animation_end_sec) * 1000.0)));
    const float fade0 = static_cast<float>(start_ms - appear);
    const float fade1 = static_cast<float>(end_ms + disappear);
    if (fade1 < view_ms_lo || fade0 > view_ms_hi) continue;
    const int32_t color_id = note.scratch_length;
    const int32_t split_count = wds::chart_editor::get_split_count(note.gimmick_type);
    std::vector<int32_t> mids;
    split_boundaries_12(split_count, mids);
    auto draw_edges = [&](float ms0, float ms1) {
      if (ms1 <= ms0) return;
      const float y0 = v.y_at_ms(ms0);
      const float y1 = v.y_at_ms(ms1);
      const float top = std::max(std::min(y0, y1), b.y);
      const float bot = std::min(std::max(y0, y1), b.bottom());
      if (bot <= top) return;
      const float op = split_line_opacity_at_ms(
          note, timing, preview, static_cast<int64_t>(std::llround((ms0 + ms1) * 0.5f)));
      if (op <= 0.001f) return;
      auto draw_v = [&](int32_t edge, int32_t slot) {
        auto c = split_slot_color(color_id, slot, split_count, nullptr);
        if (c.a < 0.02f) return;
        apply_official_split_rgb_opacity(c);
        c.a *= op;
        const float x = std::floor(v.x_at(edge) + 0.5f);
        p.fillRect(QRectF(x - 1.0, top, 2.0, bot - top), qcolor(c));
      };
      draw_v(0, 0);
      int32_t slot = 1;
      for (int32_t mid : mids) draw_v(mid + 1, slot++);
      draw_v(lanes, std::max(1, split_count));
    };
    draw_edges(std::max(fade0, view_ms_lo), std::min(static_cast<float>(start_ms), view_ms_hi));
    draw_edges(std::max(static_cast<float>(start_ms), view_ms_lo),
               std::min(static_cast<float>(end_ms), view_ms_hi));
    draw_edges(std::max(static_cast<float>(end_ms), view_ms_lo), std::min(fade1, view_ms_hi));
  }
}

}  // namespace

ChartEditWidget::ChartEditWidget(ChartEditPanel* panel, QWidget* parent)
    : QWidget(parent), panel_(panel), last_tick_(std::chrono::steady_clock::now()) {
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setAutoFillBackground(false);
  setAttribute(Qt::WA_InputMethodEnabled, true);
}

void ChartEditWidget::set_skins_directory(const QString& directory) {
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
  update();
}

wds::interaction::Modifiers ChartEditWidget::mods(Qt::KeyboardModifiers m) const {
  return {m.testFlag(Qt::ShiftModifier), m.testFlag(Qt::ControlModifier),
          m.testFlag(Qt::AltModifier), m.testFlag(Qt::MetaModifier)};
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
  update();
}
void ChartEditWidget::paintEvent(QPaintEvent*) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing, true);
  p.fillRect(rect(), QColor(18, 20, 27));
  if (!panel_) return;
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
  p.fillRect(playfield, QColor(24, 27, 36));

  // Waveform is sampled in the same wall-clock coordinate system as EditViewport.
  if (const auto* waveform = panel_->waveform(); waveform && !waveform->empty()) {
    QPainterPath wave;
    const float center = b.x + b.w * 0.5f;
    const float max_w = b.w * 0.46f;
    wave.moveTo(center, b.y);
    for (int y = std::max(0, int(std::floor(b.y))); y <= std::min(height(), int(std::ceil(b.bottom()))); ++y) {
      const double ms0 = v.ms_at_y(float(y));
      const double ms1 = v.ms_at_y(float(y + 1));
      const float peak = waveform->peak_in_range(ms0, ms1);
      wave.lineTo(center + std::sqrt(std::clamp(peak, 0.0f, 1.0f)) * max_w, y);
    }
    for (int y = std::min(height(), int(std::ceil(b.bottom()))); y >= std::max(0, int(std::floor(b.y))); --y) {
      const float peak = waveform->peak_in_range(v.ms_at_y(float(y)), v.ms_at_y(float(y + 1)));
      wave.lineTo(center - std::sqrt(std::clamp(peak, 0.0f, 1.0f)) * max_w, y);
    }
    wave.closeSubpath(); p.fillPath(wave, QColor(145, 160, 188, 70));
  }

  // Beat/subdivision lines, kept lightweight by stepping only the visible range.
  const auto range = v.visible_tick_range();
  const int step = std::max(1, wds::chart_editor::subdivision_tick_step(v.grid()));
  int tick = (range.first / step) * step;
  if (tick < range.first) tick += step;
  for (; tick <= range.second; tick += step) {
    const bool beat = tick % std::max(1, v.grid().ticks_per_quarter) == 0;
    p.setPen(QPen(beat ? QColor(125, 135, 158, 125) : QColor(75, 82, 99, 65), beat ? 1.2 : 1.0));
    const qreal y = v.y_at(tick); p.drawLine(QPointF(b.x, y), QPointF(b.right(), y));
  }
  paint_lane_guides(p, v, b, notes, timing, preview);
  paint_split_lines(p, v, b, notes, timing, preview);
  // Judgment-area skin sits below notes and spans the authored playfield.
  if (!judgment_.isNull()) {
    const float h = v.judgeline_height_px();
    p.drawPixmap(QRectF(b.x, v.judgeline_y() - h * .5f, b.w, h), judgment_, judgment_.rect());
  }

  const auto draw_skin = [&](const QPixmap& image, const QRectF& target) {
    if (image.isNull()) { p.fillRect(target, QColor(100, 190, 255)); return; }
    p.drawPixmap(target, image, image.rect());
  };
  std::vector<std::size_t> draw_order;
  // Use the same millisecond-based ordering contract as the Vulkan editor and
  // preview. Tick order alone is wrong across BPM segments and can put a tap
  // behind a cap that is rendered later at the same wall-clock position.
  wds::chart_render::build_draw_order_indices(
      notes.size(), draw_order,
      [&](std::size_t i) {
        return wds::chart_editor::tick_to_milliseconds(notes[i].start_tick, timing);
      },
      [&](std::size_t i) { return static_cast<int32_t>(notes[i].note_type); });
  // Paint every hold ribbon before any note sprite. A per-note body/cap pass
  // lets a later hold cover taps that happen to share its time range; the
  // editor must instead keep all selectable note art above every ribbon.
  for (const std::size_t index : draw_order) {
    const auto& n = panel_->engine().document().notes()[index];
    if (n.note_type == wds::chart_editor::NoteType::HoldEighth ||
        wds::chart_editor::is_split_lane_gimmick(n.gimmick_type) ||
        n.end_tick <= n.start_tick ||
        !wds::chart_editor::is_hold_with_tail(n.note_type)) {
      continue;
    }
    using NT = wds::chart_editor::NoteType;
    const bool scratch = n.note_type == NT::ScratchHold ||
                         n.note_type == NT::ScratchCriticalHold ||
                         n.note_type == NT::NontailScratchHold ||
                         n.note_type == NT::NontailScratchCriticalHold;
    const float y0 = v.y_at(n.start_tick);
    const float y1 = v.y_at(n.end_tick);
    const float body_inset = v.hold_inset_px(n.width);
    const float body_x = v.x_at(n.lane) + body_inset;
    const float body_w = std::max(4.0f, v.lane_width(n.width) - body_inset * 2.0f);
    QLinearGradient hold(body_x, 0, body_x + body_w, 0);
    const QColor edge = scratch ? QColor(145, 55, 220, 105) : QColor(45, 150, 235, 105);
    const QColor mid = scratch ? QColor(225, 125, 255, 205) : QColor(110, 235, 255, 205);
    hold.setColorAt(0, edge);
    hold.setColorAt(.5, mid);
    hold.setColorAt(1, edge);
    p.fillRect(QRectF(body_x, std::min(y0, y1), body_w,
                      std::max(2.0f, std::abs(y1 - y0))), hold);
  }
  for (const std::size_t index : draw_order) {
    const auto& n = panel_->engine().document().notes()[index];
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
    if (n.note_type == NT::Sound || n.note_type == NT::ScratchSound) {
      const float side = std::min(w, v.note_height_px()); head = {x + (w-side)*.5f, y0-side*.5f, side, side};
    }
    // Hold bodies do not own a start cap; their paired HoldStart does. This is
    // essential at ScratchHold chain joints where two caps otherwise overlap.
    if (!hold_body) draw_skin(*skin, head);
    if (hold_body) {
      // Tail caps are real note sprites, not just the ribbon endpoint.  Draw
      // them independently so a terminal ScratchHold still exposes its flick.
      QRectF tail(tail_x, y1 - v.note_height_px()*.5f, tail_w, v.note_height_px());
      draw_skin(*skin, tail);
      if (scratch && !arrow_.isNull()) {
        const float ah = v.note_height_px();
        const float aw = std::min(tail_w, std::clamp(v.lane_width(1) * .55f, 8.0f, ah * 1.2f));
        wds::chart_render::StaticArrowLayoutParams ap;
        ap.span_left = tail_x; ap.span_right = tail_x + tail_w; ap.arrow_w = aw; ap.scratch_length = n.scratch_length;
        const QImage mirrored = arrow_.toImage().mirrored(true, false);
        for (const auto& a : wds::chart_render::layout_static_scratch_arrows(ap)) {
          const QRectF ar(a.x0, y1-ah*.5f, a.x1-a.x0, ah);
          if (a.flip_x) p.drawImage(ar, mirrored); else p.drawPixmap(ar, arrow_, arrow_.rect());
        }
      }
    }
    // A ScratchHoldStart uses the purple head skin but has no arrows; arrows
    // belong to Flick notes (and to the terminal cap of a ScratchHold body).
    if (false && !hold_body && n.note_type == NT::Flick && !arrow_.isNull()) {
      const float ah = v.note_height_px();
      const float aw = std::min(w, std::clamp(v.lane_width(1) * .55f, 8.0f, ah * 1.2f));
      wds::chart_render::StaticArrowLayoutParams ap;
      ap.span_left = x; ap.span_right = x + w; ap.arrow_w = aw; ap.scratch_length = n.scratch_length;
      const QImage mirrored = arrow_.toImage().mirrored(true, false);
      for (const auto& a : wds::chart_render::layout_static_scratch_arrows(ap)) {
        const QRectF ar(a.x0, y0-ah*.5f, a.x1-a.x0, ah);
        if (a.flip_x) p.drawImage(ar, mirrored); else p.drawPixmap(ar, arrow_, arrow_.rect());
      }
    }
    if (panel_->selected().count(n.id)) {
      p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(255, 221, 70), 2.5));
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
    const QImage mirrored = arrow_.toImage().mirrored(true, false);
    for (const auto& n : panel_->engine().document().notes()) {
      if (n.note_type != wds::chart_editor::NoteType::Flick) continue;
      const float y = v.y_at(n.start_tick), inset = v.note_inset_px(n.width);
      const float sx = v.x_at(n.lane) + inset, sw = std::max(4.0f, v.lane_width(n.width) - inset * 2.0f);
      const float ah = v.note_height_px();
      const float aw = std::min(sw, std::clamp(v.lane_width(1) * .55f, 8.0f, ah * 1.2f));
      wds::chart_render::StaticArrowLayoutParams ap{sx, sx + sw, aw, n.scratch_length};
      for (const auto& a : wds::chart_render::layout_static_scratch_arrows(ap)) {
        const QRectF ar(a.x0, y - ah * .5f, a.x1 - a.x0, ah);
        if (a.flip_x) p.drawImage(ar, mirrored); else p.drawPixmap(ar, arrow_, arrow_.rect());
      }
    }
  }
  p.restore();
  flush_ui_painter(p, chrome);
}
void ChartEditWidget::present_qt_modals() {
  if (!panel_ || qt_modal_open_ || !panel_->has_modal_popup()) return;
  // Opening on press would click through into the dialog; wait until buttons are up.
  if (QGuiApplication::mouseButtons() != Qt::NoButton) return;
  releaseMouse();

  ChartEditPanel::SplitModalDraft split;
  if (panel_->take_split_modal(split)) {
    qt_modal_open_ = true;
    SplitPickerDialog dialog(split.count, split.color_id, this);
    const int result = dialog.exec();
    qt_modal_open_ = false;
    if (result == QDialog::Accepted) {
      if (split.edit_id >= 0) {
        (void)panel_->edit_split_effect(split.edit_id, dialog.count(), dialog.color_id());
      } else {
        (void)panel_->add_split_effect(split.tick, dialog.count(), dialog.color_id());
      }
    }
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
    update();
  }
}

void ChartEditWidget::mousePressEvent(QMouseEvent* e) {
  if (!panel_) return;
  setFocus();
  grabMouse();
  const auto pos = point(e->position());
  panel_->sync_global_pointer(pos);
  panel_->on_pointer_down({pos, button(e->button()), mods(e->modifiers())});
  if (panel_->has_modal_popup()) releaseMouse();
  present_qt_modals();
  update();
}
void ChartEditWidget::mouseMoveEvent(QMouseEvent* e) {
  if (!panel_) return;
  const auto pos = point(e->position());
  panel_->sync_global_pointer(pos);
  panel_->on_pointer_move({pos, mods(e->modifiers())});
  switch (panel_->hover_cursor()) {
    case wds::interaction::CursorKind::ResizeHorizontal: setCursor(Qt::SizeHorCursor); break;
    case wds::interaction::CursorKind::ResizeVertical: setCursor(Qt::SizeVerCursor); break;
    default: setCursor(Qt::ArrowCursor); break;
  }
  update();
}
void ChartEditWidget::mouseReleaseEvent(QMouseEvent* e) {
  if (!panel_) return;
  const auto pos = point(e->position());
  panel_->sync_global_pointer(pos);
  panel_->on_pointer_up({pos, button(e->button()), mods(e->modifiers())});
  if (e->buttons() == Qt::NoButton) releaseMouse();
  present_qt_modals();
  update();
}
void ChartEditWidget::mouseDoubleClickEvent(QMouseEvent* e) { if (!panel_) return; panel_->on_double_click({point(e->position()), button(e->button()), mods(e->modifiers())}); update(); }
void ChartEditWidget::wheelEvent(QWheelEvent* e) {
  if (!panel_) return;
  const QPoint angle = e->angleDelta();
  const QPoint pixel = e->pixelDelta();
  // Keep Qt's high-resolution trackpad deltas continuous; legacy wheels use
  // the same 120-unit normalization as the old input adapter.
  const float dx = pixel.x() != 0 ? pixel.x() / 10.0f : angle.x() / 40.0f;
  const float dy = pixel.y() != 0 ? pixel.y() / 10.0f : angle.y() / 40.0f;
  auto modifiers = mods(e->modifiers());
  if (modifiers.control && !modifiers.shift && !modifiers.alt) {
    modifiers.control = false; modifiers.alt = true;
  }
  panel_->on_scroll({point(e->position()), dx, dy, modifiers}); update();
}
static wds::interaction::KeyCode key(int k) {
  switch (k) {
    case Qt::Key_Space: return wds::interaction::KeyCode::Space;
    case Qt::Key_Delete: return wds::interaction::KeyCode::Delete;
    case Qt::Key_Backspace: return wds::interaction::KeyCode::Backspace;
    case Qt::Key_Left: return wds::interaction::KeyCode::Left;
    case Qt::Key_Right: return wds::interaction::KeyCode::Right;
    case Qt::Key_Up: return wds::interaction::KeyCode::Up;
    case Qt::Key_Down: return wds::interaction::KeyCode::Down;
    case Qt::Key_Escape: return wds::interaction::KeyCode::Escape;
    case Qt::Key_Return: case Qt::Key_Enter: return wds::interaction::KeyCode::Enter;
    case Qt::Key_Tab: return wds::interaction::KeyCode::Tab;
    default: break;
  }
  if (k >= Qt::Key_0 && k <= Qt::Key_9)
    return static_cast<wds::interaction::KeyCode>(static_cast<int>(wds::interaction::KeyCode::Num0) + k - Qt::Key_0);
  if (k >= Qt::Key_A && k <= Qt::Key_Z)
    return static_cast<wds::interaction::KeyCode>(static_cast<int>(wds::interaction::KeyCode::A) + k - Qt::Key_A);
  return wds::interaction::KeyCode::Unknown;
}
void ChartEditWidget::keyPressEvent(QKeyEvent* e) {
  if (!panel_) return;
  const auto ev = wds::interaction::KeyDownEvent{key(e->key()), mods(e->modifiers()), e->isAutoRepeat()};
  // Space is an application command, never an edit-canvas command. Qt dialogs
  // own their own keyboard; the old painted popups no longer take keys here.
  if (ev.key == wds::interaction::KeyCode::Space && global_key_handler_ &&
      !panel_->captures_keys()) {
    global_key_handler_(ev);
  } else {
    panel_->on_key_down(ev);
  }
  if (!e->isAutoRepeat() && !e->text().isEmpty() && panel_->has_modal_popup()) {
    panel_->on_text_input({e->text().toUtf8().toStdString()});
  }
  update();
}
void ChartEditWidget::keyReleaseEvent(QKeyEvent* e) {
  if (!panel_) return;
  const auto ev = wds::interaction::KeyUpEvent{key(e->key()), mods(e->modifiers())};
  if (ev.key != wds::interaction::KeyCode::Space || panel_->captures_keys()) panel_->on_key_up(ev);
}
void ChartEditWidget::inputMethodEvent(QInputMethodEvent* e) {
  if (panel_ && panel_->has_modal_popup() && !e->commitString().isEmpty()) {
    panel_->on_text_input({e->commitString().toUtf8().toStdString()});
    update();
  }
  QWidget::inputMethodEvent(e);
}
}

#pragma once

#include <QWidget>
#include <QImage>
#include <QPen>
#include <QPixmap>
#include <QString>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

class QEnterEvent;
class QFocusEvent;
class QGraphicsOpacityEffect;
class QInputMethodEvent;
class QLabel;
class QPainter;
class QPropertyAnimation;
class QShowEvent;
class QSlider;
class QTimer;

#include <wds/interaction/types.hpp>
#include "wds/ui/regions/edit/chart_edit_panel.hpp"

namespace wds::ui {

// Per-frame split geometry, built once in paintEvent and drawn by the main pass and the two
// edge-spill strips (the strips only clip, they never recompute). Plain scratch data: vectors keep
// their capacity between frames.
inline constexpr int kChartEditSplitRampStops = 24;  // ramp gradients carry stops [0, 24]

struct ChartEditSplitEdge {
  float x = 0.0f;  // pixel-snapped line centre
  wds::interaction::Color color{};  // final RGB/alpha before the per-segment opacity
};

struct ChartEditSplitSegment {
  bool valid = false;
  bool ramp = false;  // gradient along y0 -> y1 from `stops`; otherwise flat `max_op`
  float y0 = 0.0f, y1 = 0.0f;  // unclamped ramp endpoints
  float top = 0.0f, bot = 0.0f;  // y extent clamped to the playfield
  float max_op = 0.0f;
  float stops[kChartEditSplitRampStops + 1] = {};
};

struct ChartEditSplitItem {
  const wds::chart_editor::NotationNote* note = nullptr;  // points into the document's notes
  bool selected = false;
  bool lines_visible = false;
  int32_t color_id = 0;
  int32_t split_count = 1;
  int64_t start_ms = 0;
  int64_t end_ms = 0;  // >= start_ms
  int64_t appear_ms = 1;
  int64_t fade_start_ms = 0;
  int64_t fade_end_ms = 0;
  float line_w = 2.0f;
  uint32_t edge_begin = 0, edge_end = 0;  // range in ChartEditSplitFrame::edges
  ChartEditSplitSegment seg[3];  // fade-in ramp, steady, fade-out ramp
};

struct ChartEditGuideLine {
  float x = 0.0f;
  QPen pen;
};

struct ChartEditSplitFrame {
  std::vector<std::size_t> note_indices;  // windowed split gimmicks, ascending note index
  std::vector<ChartEditSplitItem> items;  // same order as note_indices
  std::vector<ChartEditSplitEdge> edges;
  std::vector<ChartEditGuideLine> guide_lines;
  std::vector<std::pair<float, float>> guide_spans;  // (top, bot) px not covered by a split
  std::vector<std::pair<float, float>> cover_scratch;
  std::vector<int32_t> mids_scratch;
};

// Native Qt canvas for chart authoring. Preview remains on Vulkan; this widget
// deliberately owns no graphics context and uses QPainter only.
class ChartEditWidget final : public QWidget {
 public:
  explicit ChartEditWidget(ChartEditPanel* panel, QWidget* parent = nullptr);
  void set_skins_directory(const QString& directory);
  void set_split_effect_quality(int quality);
  void set_global_key_handler(std::function<void(const wds::interaction::KeyDownEvent&)> handler) {
    global_key_handler_ = std::move(handler);
  }
  bool captures_keys() const { return panel_ != nullptr && panel_->captures_keys(); }
  QSize minimumSizeHint() const override { return {360, 300}; }
  int64_t last_paint_us() const noexcept { return last_paint_us_; }
  uint64_t paint_count() const noexcept { return paint_count_; }
  void mark_dirty() noexcept { dirty_ = true; }
  bool needs_repaint() const noexcept { return dirty_; }
  // Consumes dirty for this frame. Always true while `playing`. Hidden widgets
  // return false. Also true when visual_revision() changed or 250 ms elapsed.
  bool take_dirty_for_frame(bool playing);
  // Seek handler: sync scroll and rebuild waveform / note cull off the
  // first presented paint. Same pixels as paintEvent at this T.
  void prepare_after_seek(int64_t timeline_ms);

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void showEvent(QShowEvent*) override;
  void enterEvent(QEnterEvent*) override;
  void leaveEvent(QEvent*) override;
  void focusInEvent(QFocusEvent*) override;
  void focusOutEvent(QFocusEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void keyReleaseEvent(QKeyEvent*) override;
  void inputMethodEvent(QInputMethodEvent*) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void sync_extra_lane_slider();
  void fade_lane_slider(bool highlighted);
  void update_extra_lane_slider_label();
  wds::interaction::Modifiers mods(Qt::KeyboardModifiers) const;
  wds::interaction::Vec2 point(const QPointF&) const;
  // Draws notes[indices[k]]; indices must be ascending (document order) and already culled.
  void paint_notes(QPainter& painter,
                   const std::vector<wds::chart_editor::NotationNote>& notes,
                   const std::vector<std::size_t>& indices, float opacity, bool show_selection);
  void sync_audio_backdrop(const EditViewport& v, const wds::interaction::Rect& bounds);
  void fill_audio_backdrop_rows(const EditViewport& v, const wds::audio::WaveformOverview& waveform,
                                EditSpectrumMode mode, int row0, int row1);
  // Refills visible_note_indices_ / visible_back_note_indices_ with the notes
  // touching [range_lo, range_hi] (tick space).
  void sync_visible_notes(const std::vector<wds::chart_editor::NotationNote>& notes,
                          int32_t range_lo, int32_t range_hi);
  // Content-generation-keyed span metadata that lets the tick windows binary-search the
  // (start_tick, id)-sorted note vector instead of scanning every note.
  void ensure_note_span_cache(const std::vector<wds::chart_editor::NotationNote>& notes);
  void playfield_cull_ticks(int32_t& lo, int32_t& hi) const;
  void paint_waveform(QPainter& painter, const EditViewport& v, const wds::interaction::Rect& bounds);
  void present_qt_modals();
  void schedule_split_prefetch(const std::vector<wds::chart_editor::NotationNote>& notes,
                               float lane_px, int band_q);
  // Background cluster builds finished: mark dirty and schedule at most one repaint (see request_split_repaint).
  void request_split_repaint();
  uint64_t prefetch_rev_ = ~uint64_t{0};
  float prefetch_lane_px_ = 0;
  int prefetch_band_q_ = 0;
  bool split_repaint_pending_ = false;  // an update() queued by a finished split cluster build has not painted yet
  ChartEditPanel* panel_ = nullptr;
  QSlider* extra_lane_slider_ = nullptr;
  QLabel* extra_lane_slider_label_ = nullptr;
  QGraphicsOpacityEffect* extra_lane_slider_opacity_ = nullptr;
  QPropertyAnimation* extra_lane_slider_fade_ = nullptr;
  bool qt_modal_open_ = false;
  QString skins_dir_;
  int split_effect_quality_ = 1;
  QPixmap background_, judgment_, red_, yellow_, blue_, purple_, tick_blue_, tick_purple_, arrow_,
      arrow_mirrored_, hold_blue_, hold_purple_;
  std::function<void(const wds::interaction::KeyDownEvent&)> global_key_handler_;
  bool dirty_ = true;
  uint64_t last_visual_revision_ = ~uint64_t{0};
  std::chrono::steady_clock::time_point last_paint_at_{};
  int64_t last_paint_us_ = 0;
  uint64_t paint_count_ = 0;
  std::vector<std::size_t> visible_note_indices_;
  // Editor-only back layer: culled separately so it paints first (behind the
  // interactive notes) at the shared ghost alpha and without selection boxes.
  std::vector<std::size_t> visible_back_note_indices_;
  std::vector<std::size_t> note_draw_order_;
  std::vector<int64_t> note_draw_key_ms_;
  std::vector<wds::chart_editor::NotationNote> ghost_scratch_;
  std::vector<std::size_t> ghost_indices_;
  ChartEditSplitFrame split_frame_;
  QImage audio_backdrop_;
  EditSpectrumMode backdrop_mode_ = EditSpectrumMode::None;
  const void* backdrop_wave_ = nullptr;
  std::size_t backdrop_peaks_ = 0;
  int backdrop_frames_ = 0;
  float backdrop_pw_ = 0;
  float backdrop_ph_ = 0;
  qreal backdrop_dpr_ = 1;
  int32_t backdrop_visible_ms_ = 0;
  float backdrop_scroll_ms_ = 0;
  int backdrop_x0_ = 0;
  int backdrop_y0_ = 0;
  // Key of ensure_note_span_cache(). The spans below only depend on the notes' ticks, so an
  // in-place reorder of equal-start_tick notes (normalize_for_save) cannot invalidate them.
  const void* note_span_doc_ = nullptr;
  const void* note_span_data_ = nullptr;
  std::size_t note_span_n_ = 0;
  uint64_t note_span_gen_ = ~uint64_t{0};
  bool note_sorted_ = false;           // notes are ascending by start_tick
  int64_t note_max_span_ticks_ = 0;    // max(end_tick, start_tick) - start_tick over all notes
  int64_t split_max_span_ticks_ = 0;   // same, split gimmicks only
};
}

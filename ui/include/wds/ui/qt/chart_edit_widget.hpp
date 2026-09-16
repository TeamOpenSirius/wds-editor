#pragma once

#include <QWidget>
#include <QPixmap>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class QEnterEvent;
class QFocusEvent;
class QInputMethodEvent;
class QPainter;
class QShowEvent;

#include "wds/ui/regions/edit/chart_edit_panel.hpp"

namespace wds::ui {

// Native Qt canvas for chart authoring. Preview remains on Vulkan; this widget
// deliberately owns no graphics context and uses QPainter only.
class ChartEditWidget final : public QWidget {
 public:
  explicit ChartEditWidget(ChartEditPanel* panel, QWidget* parent = nullptr);
  void set_skins_directory(const QString& directory);
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

 private:
  wds::interaction::Modifiers mods(Qt::KeyboardModifiers) const;
  wds::interaction::Vec2 point(const QPointF&) const;
  void paint_notes(QPainter& painter,
                   const std::vector<wds::chart_editor::NotationNote>& notes, float opacity,
                   bool show_selection, int32_t range_lo, int32_t range_hi);
  void paint_waveform(QPainter& painter, const EditViewport& v, const wds::interaction::Rect& bounds);
  void present_qt_modals();
  ChartEditPanel* panel_ = nullptr;
  bool qt_modal_open_ = false;
  QPixmap background_, judgment_, red_, yellow_, blue_, purple_, tick_blue_, tick_purple_, arrow_,
      arrow_mirrored_, hold_blue_, hold_purple_;
  std::function<void(const wds::interaction::KeyDownEvent&)> global_key_handler_;
  bool dirty_ = true;
  uint64_t last_visual_revision_ = ~uint64_t{0};
  std::chrono::steady_clock::time_point last_paint_at_{};
  int64_t last_paint_us_ = 0;
  uint64_t paint_count_ = 0;
  std::vector<std::size_t> visible_note_indices_;
  std::vector<std::size_t> note_draw_order_;
  std::vector<wds::chart_editor::NotationNote> ghost_scratch_;
  std::vector<wds::chart_editor::NotationNote> split_notes_scratch_;
  const void* wave_cache_key_ = nullptr;
  float wave_cache_pw_ = 0;
  float wave_cache_ph_ = 0;
  qreal wave_cache_dpr_ = 1;
  int32_t wave_cache_visible_ms_ = 0;
  float wave_peaks_scroll_ms_ = 0;
  int wave_peaks_y0_ = 0;
  std::vector<float> wave_row_peaks_;
};
}

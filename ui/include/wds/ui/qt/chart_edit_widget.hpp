#pragma once

#include <QWidget>
#include <QPixmap>
#include <chrono>
#include <functional>

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
  QSize minimumSizeHint() const override { return {360, 300}; }

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void keyReleaseEvent(QKeyEvent*) override;

 private:
  wds::interaction::Modifiers mods(Qt::KeyboardModifiers) const;
  wds::interaction::Vec2 point(const QPointF&) const;
  ChartEditPanel* panel_ = nullptr;
  QPixmap background_, judgment_, red_, yellow_, blue_, purple_, tick_blue_, tick_purple_, arrow_;
  std::chrono::steady_clock::time_point last_tick_;
  std::function<void(const wds::interaction::KeyDownEvent&)> global_key_handler_;
};
}

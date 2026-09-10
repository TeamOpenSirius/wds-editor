#include "wds/ui/qt/note_icons.hpp"

#include <QDir>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>

namespace wds::ui {
namespace {

constexpr int kIconSide = 40;

QPixmap load_skin(const QString& dir, const char* name) {
  return QPixmap(QDir(dir).filePath(QString::fromUtf8(name)));
}

// Wide note strip centered vertically in a square canvas, like the old
// toolbar's flat note preview.
QPixmap flat_note(const QPixmap& top) {
  QPixmap canvas(kIconSide, kIconSide);
  canvas.fill(Qt::transparent);
  if (top.isNull()) return canvas;
  QPainter p(&canvas);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const int w = kIconSide - 2;
  const int h = qMax(10, w * top.height() / qMax(1, top.width()));
  p.drawPixmap(QRect(1, (kIconSide - h) / 2, w, h), top);
  return canvas;
}

// Flick: note strip with scratch arrows overlaid. The skin arrow points left;
// direction < 0 keeps it, > 0 mirrors it, 0 draws both.
QPixmap flick_note(const QPixmap& top, const QPixmap& arrow, int direction) {
  QPixmap canvas = flat_note(top);
  if (arrow.isNull()) return canvas;
  QPainter p(&canvas);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const int ah = kIconSide * 3 / 5;
  const int aw = qMax(6, ah * arrow.width() / qMax(1, arrow.height()));
  const int ay = (kIconSide - ah) / 2;
  const QPixmap left = arrow;
  const QPixmap right = QPixmap::fromImage(arrow.toImage().mirrored(true, false));
  if (direction < 0) {
    p.drawPixmap(QRect((kIconSide - aw) / 2, ay, aw, ah), left);
  } else if (direction > 0) {
    p.drawPixmap(QRect((kIconSide - aw) / 2, ay, aw, ah), right);
  } else {
    p.drawPixmap(QRect(kIconSide / 2 - aw - 1, ay, aw, ah), left);
    p.drawPixmap(QRect(kIconSide / 2 + 1, ay, aw, ah), right);
  }
  return canvas;
}

// Hold connection ribbon: the runtime bakes these procedurally, so paint an
// equivalent translucent gradient band here.
QPixmap hold_ribbon(const QColor& base) {
  QPixmap canvas(kIconSide, kIconSide);
  canvas.fill(Qt::transparent);
  QPainter p(&canvas);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF band(2.0, kIconSide * 0.32, kIconSide - 4.0, kIconSide * 0.36);
  QLinearGradient grad(band.topLeft(), band.bottomLeft());
  QColor mid = base;
  mid.setAlphaF(0.85f);
  QColor edge = base.lighter(150);
  edge.setAlphaF(0.55f);
  grad.setColorAt(0.0, edge);
  grad.setColorAt(0.5, mid);
  grad.setColorAt(1.0, edge);
  p.setPen(QPen(base.lighter(130), 1.2));
  p.setBrush(grad);
  p.drawRoundedRect(band, 3.0, 3.0);
  return canvas;
}

}  // namespace

std::array<QIcon, 8> build_convert_note_icons(const std::string& skins_dir) {
  const QString dir = QString::fromStdString(skins_dir);
  const QPixmap red = load_skin(dir, "Sirius Note Red Top.png");
  const QPixmap yellow = load_skin(dir, "Sirius Note Yellow Top.png");
  const QPixmap blue = load_skin(dir, "Sirius Note Blue Top.png");
  const QPixmap purple = load_skin(dir, "Sirius Note Purple Top.png");
  const QPixmap arrow = load_skin(dir, "Sirius Scratch Arrow.png");

  return {
      QIcon(flat_note(red)),                       // Tap
      QIcon(flat_note(yellow)),                    // ExTap
      QIcon(flat_note(blue)),                      // Hold Head
      QIcon(hold_ribbon(QColor(90, 170, 255))),    // Hold
      QIcon(flick_note(purple, arrow, -1)),        // Left Flick
      QIcon(flick_note(purple, arrow, 0)),         // Flick
      QIcon(flick_note(purple, arrow, 1)),         // Right Flick
      QIcon(hold_ribbon(QColor(190, 110, 255))),   // Scratch Hold
  };
}

}  // namespace wds::ui

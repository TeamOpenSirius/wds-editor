#include "wds/ui/qt/fluent_icons.hpp"

#include "wds/ui/resource_paths.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace wds::ui {
namespace {
QString g_family;
}

QString load_fluent_font(const QString& font_path) {
  const int id = QFontDatabase::addApplicationFont(font_path);
  if (id < 0) return {};
  const auto families = QFontDatabase::applicationFontFamilies(id);
  if (families.isEmpty()) return {};
  g_family = families.front();
  return g_family;
}

QIcon curve_template_icon(int px) {
  const qreal dpr = qApp->devicePixelRatio();
  QPixmap pm(QSize(px, px) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter painter(&pm);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.scale(px / 48.0, px / 48.0);
  painter.setPen(QPen(qApp->palette().color(QPalette::WindowText), 4,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawRect(QRectF(4, 4, 40, 40));
  QPainterPath curve(QPointF(38, 10));
  curve.cubicTo(32, 10, 27, 14, 24, 24);
  curve.cubicTo(21, 34, 16, 38, 10, 38);
  painter.drawPath(curve);
  painter.end();
  return QIcon(pm);
}

QIcon fluent_icon(char32_t glyph, const QColor& color, int px) {
  if (g_family.isEmpty()) return {};
  const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
  QColor tint = color.isValid()
                    ? color
                    : (qApp != nullptr ? qApp->palette().color(QPalette::WindowText)
                                       : QColor(Qt::white));

  QPixmap pm(QSize(px, px) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::TextAntialiasing);
  QFont font(g_family);
  font.setPixelSize(static_cast<int>(px * 0.82));
  p.setFont(font);
  p.setPen(tint);
  const QString text = QString::fromUcs4(reinterpret_cast<const char32_t*>(&glyph), 1);
  p.drawText(QRect(0, 0, px, px), Qt::AlignCenter, text);
  p.end();

  QIcon icon(pm);
  // Provide a dim variant so disabled toolbar actions read correctly.
  QPixmap disabled = pm;
  {
    QPainter dp(&disabled);
    dp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    dp.fillRect(disabled.rect(), QColor(0, 0, 0, 90));
  }
  icon.addPixmap(disabled, QIcon::Disabled);
  return icon;
}

QIcon themed_svg_icon(const QString& path, const QColor& color, int px) {
  if (path.isEmpty() || px <= 0) return {};
  QSvgRenderer renderer(path);
  if (!renderer.isValid()) return {};
  const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
  const QColor tint = color.isValid()
                          ? color
                          : (qApp != nullptr ? qApp->palette().color(QPalette::WindowText)
                                             : QColor(Qt::white));

  QPixmap pm(QSize(px, px) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  renderer.render(&p, QRectF(0, 0, px, px));
  p.setCompositionMode(QPainter::CompositionMode_SourceIn);
  p.fillRect(QRectF(0, 0, px, px), tint);
  p.end();

  QIcon icon(pm);
  QPixmap disabled = pm;
  {
    QPainter dp(&disabled);
    dp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    dp.fillRect(disabled.rect(), QColor(0, 0, 0, 90));
  }
  icon.addPixmap(disabled, QIcon::Disabled);
  return icon;
}

QIcon themed_named_icon(const char* stem, const QColor& color, int px) {
  if (stem == nullptr || stem[0] == '\0') return {};
  const auto argv0 = QCoreApplication::applicationFilePath();
  const QDir dir(QString::fromStdString(resolve_icons_dir(argv0.toUtf8().constData())));
  return themed_svg_icon(dir.filePath(QString::fromLatin1(stem) + QStringLiteral(".svg")), color,
                         px);
}

}  // namespace wds::ui

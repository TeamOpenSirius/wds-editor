#include "wds/ui/qt/fluent_icons.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

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

}  // namespace wds::ui

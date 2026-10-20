#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace wds::ui {

// The bundled edit_curve.svg shape, drawn with the active text color.
QIcon curve_template_icon(int px = 20);
// Rasterizes a monochrome toolbar SVG and tints it to `color` (palette text
// when invalid) so Light/Dark themes get different icon colors.
QIcon themed_svg_icon(const QString& path, const QColor& color = QColor(), int px = 22);
// `icons/<stem>.svg` via resolve_icons_dir (Fluent UI System Icons, MIT).
QIcon themed_named_icon(const char* stem, const QColor& color = QColor(), int px = 22);

}  // namespace wds::ui

#pragma once

#include <QString>
#include <QVector>

class QApplication;

namespace wds::ui {

struct ThemeInfo {
  QString id;    // e.g. com.obsproject.Yami.Grey
  QString name;  // e.g. Grey
  bool dark = true;
};

// Available OBS themes discovered in `theme_dir` (base Yami + its .ovt
// variants), for a settings picker.
QVector<ThemeInfo> available_themes(const QString& theme_dir);

// Applies an OBS "Yami"-family theme: Fusion style + palette + QSS, resolved
// from the bundled .obt/.ovt (variables/calc/rgb baked to concrete values,
// variant overrides layered on the base, theme: icon urls rewritten, the
// body font-family forced to the bundled Noto Sans SC). `theme_id` empty ->
// the default Yami. Applying again live re-skins the running app.
void apply_wds_theme(QApplication& app, const QString& theme_dir,
                     const QString& theme_id = QString());

}  // namespace wds::ui

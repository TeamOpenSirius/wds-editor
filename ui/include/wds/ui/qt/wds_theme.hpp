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

// Stored appearance/theme values: system | light | dark. Legacy OBS ids
// (com.obsproject.Yami / .Light / other variants) are mapped in.
QString normalize_theme_preference(const QString& stored);

// Available OBS themes discovered in `theme_dir` (base Yami + its .ovt
// variants).
QVector<ThemeInfo> available_themes(const QString& theme_dir);

// Applies an OBS "Yami"-family theme: Fusion style + palette + QSS.
// `theme_id` is a preference (system/light/dark) or a legacy OBS id.
// system follows QStyleHints::colorScheme (Light -> Light, otherwise Yami).
// Applying again live re-skins the running app.
void apply_wds_theme(QApplication& app, const QString& theme_dir,
                     const QString& theme_id = QString());

}  // namespace wds::ui

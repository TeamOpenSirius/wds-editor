#pragma once

#include <QString>

class QApplication;

namespace wds::ui {

// Applies the OBS "Yami" dark theme: Fusion style + palette + QSS, resolved
// once at startup from the bundled .obt (variables/calc/rgb baked to concrete
// values, theme: icon urls rewritten to the bundled asset dir). Not a runtime
// theme engine — a single baked look.
void apply_wds_theme(QApplication& app, const QString& theme_dir);

}  // namespace wds::ui

#pragma once

class QApplication;

namespace wds::ui {

// Stage-styled theme matching the game's in-game backdrop: deep indigo night,
// violet/pink curtain accents, cyan spotlight highlights. Applies the Fusion
// style, a dark palette, and the QSS skin.
void apply_wds_theme(QApplication& app);

}  // namespace wds::ui

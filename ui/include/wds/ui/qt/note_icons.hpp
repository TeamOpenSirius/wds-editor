#pragma once

#include <QIcon>
#include <array>
#include <string>

namespace wds::ui {

// Convert-button icons composed from the skin PNGs, in the same order as the
// old toolbar convert row: Tap, ExTap, Hold Head, Hold, Left Flick, Flick,
// Right Flick, Scratch Hold. Entries fall back to a null QIcon when the skin
// file is missing.
std::array<QIcon, 8> build_convert_note_icons(const std::string& skins_dir);

}  // namespace wds::ui

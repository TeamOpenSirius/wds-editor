#pragma once

#include <string>

namespace wds::ui {

bool looks_like_skins_dir(const std::string& dir);
bool looks_like_effects_dir(const std::string& dir);

std::string resolve_skins_dir(const char* argv0);
std::string resolve_effects_dir(const char* argv0);
std::string resolve_icons_dir(const char* argv0);
// App icon PNG (wds.png) for the desktop shell. Empty if missing.
std::string resolve_app_icon_png(const char* argv0);
std::string resolve_fonts_dir(const char* argv0);
// Bundled full Noto Sans SC Regular. Empty if missing.
std::string resolve_ui_font_path(const char* argv0);
std::string resolve_repo_test_path(const char* relative);
// Demo/helper only — editor starts with no BGM (user imports; persisted in .wdsproject).
std::string resolve_bgm_path(const char* argv0);

}  // namespace wds::ui

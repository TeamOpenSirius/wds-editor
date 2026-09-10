#include "wds/ui/resource_paths.hpp"

#include <wds/common/utf8_path.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace wds::ui {

namespace fs = std::filesystem;

namespace {

using wds::common::executable_dir;
using wds::common::is_directory_utf8;
using wds::common::is_regular_file_utf8;
using wds::common::path_from_utf8;
using wds::common::path_to_utf8;

// Packaged layouts (same names on every platform):
//   zip/folder root:  <root>/<name>
//   macOS .app:       Contents/Resources/<name>  (exe lives in Contents/MacOS)
std::vector<fs::path> resource_candidates(const char* argv0, const char* name) {
  std::vector<fs::path> out;
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) {
    out.push_back(cwd / name);
  }
  const fs::path exe_dir = executable_dir(argv0);
  if (!exe_dir.empty()) {
    out.push_back(exe_dir / name);
    out.push_back(exe_dir / ".." / "Resources" / name);
  }
  return out;
}

}  // namespace

bool looks_like_skins_dir(const std::string& dir) {
  return is_directory_utf8(dir);
}

bool looks_like_effects_dir(const std::string& dir) {
  if (!is_directory_utf8(dir)) {
    return false;
  }
  return is_regular_file_utf8(path_to_utf8(path_from_utf8(dir) / "_PERFECT.ogg"));
}

bool path_is_directory(const fs::path& p) {
  return is_directory_utf8(path_to_utf8(p));
}

bool path_is_regular_file(const fs::path& p) {
  return is_regular_file_utf8(path_to_utf8(p));
}

std::string resolve_skins_dir(const char* argv0) {
  for (const auto& cand : resource_candidates(argv0, "skins")) {
    const std::string utf8 = path_to_utf8(cand.lexically_normal());
    if (looks_like_skins_dir(utf8)) {
      return utf8;
    }
  }

#ifdef WDS_REPO_ROOT
  const std::string repo_skins = path_to_utf8(path_from_utf8(WDS_REPO_ROOT) / "skins");
  if (looks_like_skins_dir(repo_skins)) {
    return repo_skins;
  }
#endif
  return "skins";
}

std::string resolve_icons_dir(const char* argv0) {
  std::error_code ec;
  // Prefer directories that ship SVG (crisp). Legacy PNG-only folders score lower so a
  // stray ui/assets/icons with open.png does not win over repo icons/*.svg.
  const auto score = [&](const fs::path& dir) -> int {
    if (!path_is_directory(dir)) {
      return 0;
    }
    if (path_is_regular_file(dir / "open.svg")) {
      return 2;
    }
    if (path_is_regular_file(dir / "open.png")) {
      return 1;
    }
    return 0;
  };

  fs::path best;
  int best_score = 0;
  const auto consider = [&](const fs::path& cand) {
    const int s = score(cand);
    if (s > best_score) {
      best_score = s;
      best = cand;
    }
  };

  for (const auto& cand : resource_candidates(argv0, "icons")) {
    consider(cand);
  }

#ifdef WDS_REPO_ROOT
  consider(path_from_utf8(WDS_REPO_ROOT) / "icons");
#endif

  // Legacy PNG fallback (only if nothing better was found).
  consider(fs::current_path(ec) / "ui" / "assets" / "icons");
#ifdef WDS_REPO_ROOT
  consider(path_from_utf8(WDS_REPO_ROOT) / "ui" / "assets" / "icons");
#endif

  if (best_score > 0) {
    return path_to_utf8(best.lexically_normal());
  }
  return "icons";
}

std::string resolve_app_icon_png(const char* argv0) {
  const auto try_file = [&](const fs::path& p) -> std::string {
    if (path_is_regular_file(p)) {
      return path_to_utf8(p.lexically_normal());
    }
    return {};
  };

  for (const auto& name : {"wds.png", "app_icon/wds.png"}) {
    for (const auto& cand : resource_candidates(argv0, name)) {
      if (auto hit = try_file(cand); !hit.empty()) {
        return hit;
      }
    }
  }

  if (const fs::path exe_dir = executable_dir(argv0); !exe_dir.empty()) {
    if (auto hit = try_file(exe_dir / "wds.png"); !hit.empty()) {
      return hit;
    }
  }

#ifdef WDS_REPO_ROOT
  if (auto hit = try_file(path_from_utf8(WDS_REPO_ROOT) / "ui" / "assets" / "app_icon" / "wds.png");
      !hit.empty()) {
    return hit;
  }
  if (auto hit = try_file(path_from_utf8(WDS_REPO_ROOT) / "logo.png"); !hit.empty()) {
    return hit;
  }
#endif
  return {};
}

std::string resolve_fonts_dir(const char* argv0) {
  std::error_code ec;
  for (const auto& cand : resource_candidates(argv0, "fonts")) {
    if (path_is_directory(cand)) {
      return path_to_utf8(cand.lexically_normal());
    }
  }

  const fs::path cwd_fonts = fs::current_path(ec) / "ui" / "assets" / "fonts";
  if (!ec && path_is_directory(cwd_fonts)) {
    return path_to_utf8(cwd_fonts);
  }

  if (const fs::path exe_dir = executable_dir(argv0); !exe_dir.empty()) {
    const fs::path exe_fonts = exe_dir / "ui" / "assets" / "fonts";
    if (path_is_directory(exe_fonts)) {
      return path_to_utf8(exe_fonts);
    }
  }

#ifdef WDS_REPO_ROOT
  const fs::path repo_fonts = path_from_utf8(WDS_REPO_ROOT) / "ui" / "assets" / "fonts";
  if (path_is_directory(repo_fonts)) {
    return path_to_utf8(repo_fonts);
  }
#endif
  return "ui/assets/fonts";
}

std::string resolve_ui_font_path(const char* argv0) {
  const fs::path dir = path_from_utf8(resolve_fonts_dir(argv0));
  const fs::path ttf = dir / "NotoSansSC-Regular.ttf";
  if (path_is_regular_file(ttf)) {
    return path_to_utf8(ttf);
  }
  const fs::path otf = dir / "NotoSansSC-Regular.otf";
  if (path_is_regular_file(otf)) {
    return path_to_utf8(otf);
  }
  return {};
}

std::string resolve_fluent_font_path(const char* argv0) {
  const fs::path dir = path_from_utf8(resolve_fonts_dir(argv0));
  const fs::path ttf = dir / "SegoeFluentIcons.ttf";
  if (path_is_regular_file(ttf)) {
    return path_to_utf8(ttf);
  }
  return {};
}

std::string resolve_theme_dir(const char* argv0) {
  std::error_code ec;
  for (const auto& cand : resource_candidates(argv0, "theme")) {
    if (path_is_directory(cand)) {
      return path_to_utf8(cand.lexically_normal());
    }
  }
  const fs::path cwd_theme = fs::current_path(ec) / "ui" / "assets" / "theme";
  if (!ec && path_is_directory(cwd_theme)) {
    return path_to_utf8(cwd_theme);
  }
  if (const fs::path exe_dir = executable_dir(argv0); !exe_dir.empty()) {
    const fs::path exe_theme = exe_dir / "ui" / "assets" / "theme";
    if (path_is_directory(exe_theme)) {
      return path_to_utf8(exe_theme);
    }
  }
#ifdef WDS_REPO_ROOT
  const fs::path repo_theme = path_from_utf8(WDS_REPO_ROOT) / "ui" / "assets" / "theme";
  if (path_is_directory(repo_theme)) {
    return path_to_utf8(repo_theme);
  }
#endif
  return "ui/assets/theme";
}

std::string resolve_effects_dir(const char* argv0) {
  for (const auto& cand : resource_candidates(argv0, "effects")) {
    const std::string utf8 = path_to_utf8(cand.lexically_normal());
    if (looks_like_effects_dir(utf8)) {
      return utf8;
    }
  }

#ifdef WDS_REPO_ROOT
  const std::string repo_effects = path_to_utf8(path_from_utf8(WDS_REPO_ROOT) / "effects");
  if (looks_like_effects_dir(repo_effects)) {
    return repo_effects;
  }
#endif
  return "effects";
}

std::string resolve_repo_test_path(const char* relative) {
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec) / relative;
  if (!ec && wds::common::path_exists_utf8(path_to_utf8(cwd))) {
    return path_to_utf8(cwd);
  }
#ifdef WDS_REPO_ROOT
  const fs::path repo = path_from_utf8(WDS_REPO_ROOT) / relative;
  if (wds::common::path_exists_utf8(path_to_utf8(repo))) {
    return path_to_utf8(repo);
  }
#endif
  return relative;
}

std::string resolve_bgm_path(const char* argv0) {
  const std::string test_bgm = resolve_repo_test_path("test/music_1.ogg");
  if (is_regular_file_utf8(test_bgm)) {
    return test_bgm;
  }

  for (const auto& cand : resource_candidates(argv0, "suzume_no_tojimari.ogg")) {
    if (path_is_regular_file(cand)) {
      return path_to_utf8(cand.lexically_normal());
    }
  }

#ifdef WDS_REPO_ROOT
  const fs::path repo_bgm = path_from_utf8(WDS_REPO_ROOT) / "suzume_no_tojimari.ogg";
  if (path_is_regular_file(repo_bgm)) {
    return path_to_utf8(repo_bgm);
  }
#endif
  return {};
}

}  // namespace wds::ui

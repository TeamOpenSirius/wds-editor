#include "wds/ui/resource_paths.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace wds::ui {

namespace fs = std::filesystem;

namespace {

fs::path exe_dir_from_argv0(const char* argv0) {
  std::error_code ec;
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size > 0) {
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
      buf.resize(std::strlen(buf.c_str()));
      const fs::path resolved = fs::weakly_canonical(fs::path(buf), ec);
      if (!ec && !resolved.empty()) {
        return resolved.parent_path();
      }
      return fs::path(buf).lexically_normal().parent_path();
    }
  }
#endif
  if (argv0 == nullptr || argv0[0] == '\0') {
    return {};
  }
  fs::path exe = fs::path(argv0);
  if (!exe.is_absolute()) {
    exe = fs::current_path(ec) / exe;
  }
  if (ec) {
    return {};
  }
  return exe.parent_path();
}

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
  const fs::path exe_dir = exe_dir_from_argv0(argv0);
  if (!exe_dir.empty()) {
    out.push_back(exe_dir / name);
    out.push_back(exe_dir / ".." / "Resources" / name);
  }
  return out;
}

}  // namespace

bool looks_like_skins_dir(const std::string& dir) {
  std::error_code ec;
  return fs::is_directory(dir, ec) && !ec;
}

bool looks_like_effects_dir(const std::string& dir) {
  std::error_code ec;
  const fs::path p(dir);
  if (!fs::is_directory(p, ec) || ec) {
    return false;
  }
  return fs::is_regular_file(p / "_PERFECT.ogg", ec) && !ec;
}

std::string resolve_skins_dir(const char* argv0) {
  for (const auto& cand : resource_candidates(argv0, "skins")) {
    if (looks_like_skins_dir(cand.string())) {
      return cand.lexically_normal().string();
    }
  }

#ifdef WDS_REPO_ROOT
  const fs::path repo_skins = fs::path(WDS_REPO_ROOT) / "skins";
  if (looks_like_skins_dir(repo_skins.string())) {
    return repo_skins.string();
  }
#endif
  return "skins";
}

std::string resolve_icons_dir(const char* argv0) {
  std::error_code ec;
  // Prefer directories that ship SVG (crisp). Legacy PNG-only folders score lower so a
  // stray ui/assets/icons with open.png does not win over repo icons/*.svg.
  const auto score = [&](const fs::path& dir) -> int {
    if (!fs::is_directory(dir, ec) || ec) {
      return 0;
    }
    if (fs::is_regular_file(dir / "open.svg", ec) && !ec) {
      return 2;
    }
    if (fs::is_regular_file(dir / "open.png", ec) && !ec) {
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
  consider(fs::path(WDS_REPO_ROOT) / "icons");
#endif

  // Legacy PNG fallback (only if nothing better was found).
  consider(fs::current_path(ec) / "ui" / "assets" / "icons");
#ifdef WDS_REPO_ROOT
  consider(fs::path(WDS_REPO_ROOT) / "ui" / "assets" / "icons");
#endif

  if (best_score > 0) {
    return best.lexically_normal().string();
  }
  return "icons";
}

std::string resolve_app_icon_png(const char* argv0) {
  std::error_code ec;
  const auto try_file = [&](const fs::path& p) -> std::string {
    if (fs::is_regular_file(p, ec) && !ec) {
      return p.lexically_normal().string();
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

  if (const fs::path exe_dir = exe_dir_from_argv0(argv0); !exe_dir.empty()) {
    if (auto hit = try_file(exe_dir / "wds.png"); !hit.empty()) {
      return hit;
    }
  }

#ifdef WDS_REPO_ROOT
  if (auto hit = try_file(fs::path(WDS_REPO_ROOT) / "ui" / "assets" / "app_icon" / "wds.png");
      !hit.empty()) {
    return hit;
  }
  if (auto hit = try_file(fs::path(WDS_REPO_ROOT) / "logo.png"); !hit.empty()) {
    return hit;
  }
#endif
  return {};
}

std::string resolve_fonts_dir(const char* argv0) {
  std::error_code ec;
  for (const auto& cand : resource_candidates(argv0, "fonts")) {
    if (fs::is_directory(cand, ec) && !ec) {
      return cand.lexically_normal().string();
    }
  }

  const fs::path cwd_fonts = fs::current_path(ec) / "ui" / "assets" / "fonts";
  if (!ec && fs::is_directory(cwd_fonts, ec) && !ec) {
    return cwd_fonts.string();
  }

  if (const fs::path exe_dir = exe_dir_from_argv0(argv0); !exe_dir.empty()) {
    const fs::path exe_fonts = exe_dir / "ui" / "assets" / "fonts";
    if (fs::is_directory(exe_fonts, ec) && !ec) {
      return exe_fonts.string();
    }
  }

#ifdef WDS_REPO_ROOT
  const fs::path repo_fonts = fs::path(WDS_REPO_ROOT) / "ui" / "assets" / "fonts";
  if (fs::is_directory(repo_fonts, ec) && !ec) {
    return repo_fonts.string();
  }
#endif
  return "ui/assets/fonts";
}

std::string resolve_ui_font_path(const char* argv0) {
  std::error_code ec;
  const fs::path dir = resolve_fonts_dir(argv0);
  const fs::path ttf = dir / "NotoSansSC-Regular.ttf";
  if (fs::is_regular_file(ttf, ec) && !ec) {
    return ttf.string();
  }
  const fs::path otf = dir / "NotoSansSC-Regular.otf";
  if (fs::is_regular_file(otf, ec) && !ec) {
    return otf.string();
  }
  return {};
}

std::string resolve_effects_dir(const char* argv0) {
  for (const auto& cand : resource_candidates(argv0, "effects")) {
    if (looks_like_effects_dir(cand.string())) {
      return cand.lexically_normal().string();
    }
  }

#ifdef WDS_REPO_ROOT
  const fs::path repo_effects = fs::path(WDS_REPO_ROOT) / "effects";
  if (looks_like_effects_dir(repo_effects.string())) {
    return repo_effects.string();
  }
#endif
  return "effects";
}

std::string resolve_repo_test_path(const char* relative) {
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec) / relative;
  if (!ec && fs::exists(cwd, ec) && !ec) {
    return cwd.string();
  }
#ifdef WDS_REPO_ROOT
  const fs::path repo = fs::path(WDS_REPO_ROOT) / relative;
  if (fs::exists(repo, ec) && !ec) {
    return repo.string();
  }
#endif
  return relative;
}

std::string resolve_bgm_path(const char* argv0) {
  std::error_code ec;

  const fs::path test_bgm = resolve_repo_test_path("test/music_1.ogg");
  if (fs::is_regular_file(test_bgm, ec) && !ec) {
    return test_bgm.string();
  }

  for (const auto& cand : resource_candidates(argv0, "suzume_no_tojimari.ogg")) {
    if (fs::is_regular_file(cand, ec) && !ec) {
      return cand.lexically_normal().string();
    }
  }

#ifdef WDS_REPO_ROOT
  const fs::path repo_bgm = fs::path(WDS_REPO_ROOT) / "suzume_no_tojimari.ogg";
  if (fs::is_regular_file(repo_bgm, ec) && !ec) {
    return repo_bgm.string();
  }
#endif
  return {};
}

}  // namespace wds::ui

#include "wds/ui/editor_ui_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace wds::ui {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

bool parse_bool(const std::string& text, bool& out) {
  const auto t = trim(text);
  if (t == "true" || t == "True" || t == "TRUE" || t == "1" || t == "yes") {
    out = true;
    return true;
  }
  if (t == "false" || t == "False" || t == "FALSE" || t == "0" || t == "no") {
    out = false;
    return true;
  }
  return false;
}

bool parse_double(const std::string& text, double& out) {
  try {
    size_t idx = 0;
    const double v = std::stod(trim(text), &idx);
    if (idx == 0) return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_int(const std::string& text, int32_t& out) {
  try {
    size_t idx = 0;
    const long v = std::stol(trim(text), &idx, 10);
    if (idx == 0) return false;
    out = static_cast<int32_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

void apply_key(EditorUiConfig& cfg, const std::string& key, const std::string& value) {
  if (key == "note_speed") {
    double v = cfg.note_speed;
    if (parse_double(value, v)) cfg.note_speed = std::clamp(v, 1.0, 20.0);
  } else if (key == "visible_hectoms") {
    int32_t v = cfg.visible_hectoms;
    if (parse_int(value, v)) cfg.visible_hectoms = std::clamp(v, 1, 1000);
  } else if (key == "music_volume") {
    double v = cfg.music_volume;
    if (parse_double(value, v)) cfg.music_volume = static_cast<float>(std::clamp(v, 0.0, 1.0));
  } else if (key == "music_muted") {
    bool v = cfg.music_muted;
    if (parse_bool(value, v)) cfg.music_muted = v;
  } else if (key == "sfx_volume") {
    double v = cfg.sfx_volume;
    if (parse_double(value, v)) cfg.sfx_volume = static_cast<float>(std::clamp(v, 0.0, 1.0));
  } else if (key == "sfx_muted") {
    bool v = cfg.sfx_muted;
    if (parse_bool(value, v)) cfg.sfx_muted = v;
  } else if (key == "pause_at_current") {
    bool v = cfg.pause_at_current;
    if (parse_bool(value, v)) cfg.pause_at_current = v;
  } else if (key == "split_width_follow") {
    bool v = cfg.split_width_follow;
    if (parse_bool(value, v)) cfg.split_width_follow = v;
  } else if (key == "mute_hold_body_sfx") {
    bool v = cfg.mute_hold_body_sfx;
    if (parse_bool(value, v)) cfg.mute_hold_body_sfx = v;
  } else if (key == "sus_auto_convert") {
    bool v = cfg.sus_auto_convert;
    if (parse_bool(value, v)) cfg.sus_auto_convert = v;
  } else if (key == "invert_scroll_wheel") {
    bool v = cfg.invert_scroll_wheel;
    if (parse_bool(value, v)) cfg.invert_scroll_wheel = v;
  } else if (key == "scroll_wheel_speed") {
    double v = cfg.scroll_wheel_speed;
    if (parse_double(value, v)) {
      cfg.scroll_wheel_speed = static_cast<float>(std::clamp(v, 0.25, 3.0));
    }
  } else if (key.rfind("width_slot_", 0) == 0 && key.size() == 12) {
    const char idx_ch = key[11];
    if (idx_ch >= '0' && idx_ch <= '5') {
      int32_t v = 0;
      if (parse_int(value, v) && v >= 1 && v <= 12) {
        cfg.width_slots[static_cast<std::size_t>(idx_ch - '0')] = static_cast<int>(v);
      }
    }
  }
}

fs::path exe_parent_dir(const char* argv0) {
  std::error_code ec;
  fs::path exe_dir = fs::current_path(ec);
  if (argv0 == nullptr || argv0[0] == '\0') return exe_dir;
  fs::path exe = fs::path(argv0);
  if (!exe.is_absolute()) {
    exe = fs::current_path(ec) / exe;
  }
  exe = fs::weakly_canonical(exe, ec);
  if (!ec) return exe.parent_path();
  return fs::absolute(fs::path(argv0), ec).parent_path();
}

void maybe_migrate_legacy_config(const fs::path& legacy, const fs::path& dest) {
  std::error_code ec;
  if (fs::is_regular_file(dest, ec) && !ec) return;
  if (!fs::is_regular_file(legacy, ec) || ec) return;
  fs::create_directories(dest.parent_path(), ec);
  fs::copy_file(legacy, dest, fs::copy_options::overwrite_existing, ec);
}

#if defined(_WIN32)
fs::path windows_localappdata_config() {
  if (const char* local = std::getenv("LOCALAPPDATA");
      local != nullptr && local[0] != '\0') {
    return fs::path(local) / "WDS" / "config" / "config.yml";
  }
  if (const char* profile = std::getenv("USERPROFILE");
      profile != nullptr && profile[0] != '\0') {
    return fs::path(profile) / "AppData" / "Local" / "WDS" / "config" / "config.yml";
  }
  return {};
}
#elif defined(__APPLE__)
fs::path macos_application_support_config() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') return {};
  return fs::path(home) / "Library" / "Application Support" / "WDS" / "config" / "config.yml";
}
#else
fs::path linux_xdg_config() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME");
      xdg != nullptr && xdg[0] != '\0') {
    return fs::path(xdg) / "WDS" / "config" / "config.yml";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return fs::path(home) / ".config" / "WDS" / "config" / "config.yml";
  }
  return {};
}
#endif

}  // namespace

std::string resolve_editor_config_path(const char* argv0) {
  const fs::path exe_dir = exe_parent_dir(argv0);
  const fs::path legacy = exe_dir / "config" / "config.yml";

#if defined(_WIN32)
  const fs::path support = windows_localappdata_config();
  if (!support.empty()) {
    maybe_migrate_legacy_config(legacy, support);
    return support.string();
  }
#elif defined(__APPLE__)
  const fs::path support = macos_application_support_config();
  if (!support.empty()) {
    maybe_migrate_legacy_config(legacy, support);
    return support.string();
  }
#else
  const fs::path support = linux_xdg_config();
  if (!support.empty()) {
    maybe_migrate_legacy_config(legacy, support);
    return support.string();
  }
#endif

  return legacy.string();
}

bool load_editor_ui_config(const std::string& path, EditorUiConfig& out) {
  std::ifstream in(path);
  if (!in) return false;

  EditorUiConfig cfg = out;
  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trim(line.substr(0, colon));
    const std::string value = trim(line.substr(colon + 1));
    if (key.empty()) continue;
    apply_key(cfg, key, value);
  }
  out = cfg;
  return true;
}

bool save_editor_ui_config(const std::string& path, const EditorUiConfig& cfg) {
  std::error_code ec;
  const fs::path file(path);
  fs::create_directories(file.parent_path(), ec);

  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;

  auto emit_bool = [](bool v) { return v ? "true" : "false"; };
  char speed[64];
  char music[64];
  char sfx[64];
  char scroll_speed[64];
  std::snprintf(speed, sizeof(speed), "%.1f", cfg.note_speed);
  std::snprintf(music, sizeof(music), "%.2f", static_cast<double>(cfg.music_volume));
  std::snprintf(sfx, sizeof(sfx), "%.2f", static_cast<double>(cfg.sfx_volume));
  std::snprintf(scroll_speed, sizeof(scroll_speed), "%.2f",
                static_cast<double>(cfg.scroll_wheel_speed));

  out << "# WDS editor UI preferences\n"
      << "note_speed: " << speed << '\n'
      << "visible_hectoms: " << cfg.visible_hectoms << '\n'
      << "music_volume: " << music << '\n'
      << "music_muted: " << emit_bool(cfg.music_muted) << '\n'
      << "sfx_volume: " << sfx << '\n'
      << "sfx_muted: " << emit_bool(cfg.sfx_muted) << '\n'
      << "pause_at_current: " << emit_bool(cfg.pause_at_current) << '\n'
      << "split_width_follow: " << emit_bool(cfg.split_width_follow) << '\n'
      << "mute_hold_body_sfx: " << emit_bool(cfg.mute_hold_body_sfx) << '\n'
      << "sus_auto_convert: " << emit_bool(cfg.sus_auto_convert) << '\n'
      << "invert_scroll_wheel: " << emit_bool(cfg.invert_scroll_wheel) << '\n'
      << "scroll_wheel_speed: " << scroll_speed << '\n'
      << "width_slot_0: " << cfg.width_slots[0] << '\n'
      << "width_slot_1: " << cfg.width_slots[1] << '\n'
      << "width_slot_2: " << cfg.width_slots[2] << '\n'
      << "width_slot_3: " << cfg.width_slots[3] << '\n'
      << "width_slot_4: " << cfg.width_slots[4] << '\n'
      << "width_slot_5: " << cfg.width_slots[5] << '\n';
  return static_cast<bool>(out);
}

}  // namespace wds::ui

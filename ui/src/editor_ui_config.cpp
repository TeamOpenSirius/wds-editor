#include "wds/ui/editor_ui_config.hpp"

#include <wds/common/utf8_path.hpp>
#include <wds/core/file_io.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/interaction/platform.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
    if (parse_double(value, v)) {
      cfg.note_speed = wds::chart_editor::official_clamp_note_speed(v);
    }
  } else if (key == "note_start_offset") {
    int32_t v = cfg.note_start_offset;
    if (parse_int(value, v)) {
      cfg.note_start_offset = wds::chart_editor::official_clamp_note_start_offset(static_cast<int>(v));
    }
  } else if (key == "note_height_level") {
    int32_t v = cfg.note_height_level;
    if (parse_int(value, v)) {
      cfg.note_height_level = wds::chart_editor::official_clamp_note_height_level(static_cast<int>(v));
    }
  } else if (key == "split_line_opacity") {
    int32_t v = cfg.split_line_opacity;
    if (parse_int(value, v)) {
      cfg.split_line_opacity =
          wds::chart_editor::official_clamp_split_effect_line_opacity(static_cast<int>(v));
    }
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
  } else if (key == "show_judgment_text") {
    bool v = cfg.show_judgment_text;
    if (parse_bool(value, v)) cfg.show_judgment_text = v;
  } else if (key == "sus_auto_convert") {
    bool v = cfg.sus_auto_convert;
    if (parse_bool(value, v)) cfg.sus_auto_convert = v;
  } else if (key == "invert_scroll_wheel") {
    bool v = cfg.invert_scroll_wheel;
    if (parse_bool(value, v)) cfg.invert_scroll_wheel = v;
  } else if (key == "invert_visible_range_scroll") {
    bool v = cfg.invert_visible_range_scroll;
    if (parse_bool(value, v)) cfg.invert_visible_range_scroll = v;
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
  } else if (key.rfind("shortcut_", 0) == 0) {
    const std::string id = key.substr(9);
    for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
      const auto sid = static_cast<wds::interaction::EditorShortcut>(i);
      if (id == wds::interaction::editor_shortcut_id(sid)) {
        if (const auto parsed = wds::interaction::parse_shortcut_chord(value)) {
          cfg.shortcuts[i] = *parsed;
          cfg.shortcuts_initialized = true;
        }
        break;
      }
    }
  }
}

void ensure_shortcut_defaults(EditorUiConfig& cfg) {
  if (cfg.shortcuts_initialized) return;
  for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
    cfg.shortcuts[i] =
        wds::interaction::default_editor_shortcut(static_cast<wds::interaction::EditorShortcut>(i));
  }
  cfg.shortcuts_initialized = true;
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
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    return fs::path(buf) / L"WDS" / L"config" / L"config.yml";
  }
  n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    return fs::path(buf) / L"AppData" / L"Local" / L"WDS" / L"config" / L"config.yml";
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
  const fs::path exe_dir = wds::common::executable_dir(argv0);
  const fs::path legacy = exe_dir / "config" / "config.yml";

#if defined(_WIN32)
  const fs::path support = windows_localappdata_config();
  if (!support.empty()) {
    maybe_migrate_legacy_config(legacy, support);
    return wds::common::path_to_utf8(support);
  }
#elif defined(__APPLE__)
  const fs::path support = macos_application_support_config();
  if (!support.empty()) {
    maybe_migrate_legacy_config(legacy, support);
    return wds::common::path_to_utf8(support);
  }
#else
  const fs::path support = linux_xdg_config();
  if (!support.empty()) {
    maybe_migrate_legacy_config(legacy, support);
    return wds::common::path_to_utf8(support);
  }
#endif

  return wds::common::path_to_utf8(legacy);
}

bool load_editor_ui_config(const std::string& path, EditorUiConfig& out) {
  wds::chart_editor::SerializeResult status;
  const std::string bytes = wds::chart_editor::read_text_file(path, status);
  if (status.error != wds::chart_editor::SerializeError::Ok) return false;

  EditorUiConfig cfg = out;
  ensure_shortcut_defaults(cfg);
  std::istringstream in(bytes);
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

  std::ostringstream out;
  out << "# WDS editor UI preferences\n"
      << "note_speed: " << speed << '\n'
      << "note_start_offset: " << cfg.note_start_offset << '\n'
      << "note_height_level: " << cfg.note_height_level << '\n'
      << "split_line_opacity: " << cfg.split_line_opacity << '\n'
      << "visible_hectoms: " << cfg.visible_hectoms << '\n'
      << "music_volume: " << music << '\n'
      << "music_muted: " << emit_bool(cfg.music_muted) << '\n'
      << "sfx_volume: " << sfx << '\n'
      << "sfx_muted: " << emit_bool(cfg.sfx_muted) << '\n'
      << "pause_at_current: " << emit_bool(cfg.pause_at_current) << '\n'
      << "split_width_follow: " << emit_bool(cfg.split_width_follow) << '\n'
      << "mute_hold_body_sfx: " << emit_bool(cfg.mute_hold_body_sfx) << '\n'
      << "show_judgment_text: " << emit_bool(cfg.show_judgment_text) << '\n'
      << "sus_auto_convert: " << emit_bool(cfg.sus_auto_convert) << '\n'
      << "invert_scroll_wheel: " << emit_bool(cfg.invert_scroll_wheel) << '\n'
      << "invert_visible_range_scroll: " << emit_bool(cfg.invert_visible_range_scroll) << '\n'
      << "scroll_wheel_speed: " << scroll_speed << '\n'
      << "width_slot_0: " << cfg.width_slots[0] << '\n'
      << "width_slot_1: " << cfg.width_slots[1] << '\n'
      << "width_slot_2: " << cfg.width_slots[2] << '\n'
      << "width_slot_3: " << cfg.width_slots[3] << '\n'
      << "width_slot_4: " << cfg.width_slots[4] << '\n'
      << "width_slot_5: " << cfg.width_slots[5] << '\n';
  for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
    const auto id = static_cast<wds::interaction::EditorShortcut>(i);
    const auto chord = cfg.shortcuts_initialized
                           ? cfg.shortcuts[i]
                           : wds::interaction::default_editor_shortcut(id);
    // Persist with Ctrl (platform-neutral primary); load accepts Cmd/Ctrl.
    wds::interaction::ShortcutChord stored = chord;
    stored.mods = wds::interaction::normalize_primary(stored.mods);
    std::string text = wds::interaction::format_shortcut_chord(stored);
    // format_* uses Cmd on Apple — rewrite every token for a stable config file.
    for (std::string::size_type pos = 0; (pos = text.find("Cmd", pos)) != std::string::npos;) {
      text.replace(pos, 3, "Ctrl");
      pos += 4;
    }
    out << "shortcut_" << wds::interaction::editor_shortcut_id(id) << ": " << text << '\n';
  }

  return wds::chart_editor::write_text_atomic(path, out.str()).error ==
         wds::chart_editor::SerializeError::Ok;
}

}  // namespace wds::ui

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
#include <unordered_map>

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

bool parse_u64(const std::string& text, std::uint64_t& out) {
  try {
    const auto t = trim(text);
    if (t.empty() || t[0] == '-') return false;
    size_t idx = 0;
    const unsigned long long v = std::stoull(t, &idx, 10);
    if (idx == 0) return false;
    out = static_cast<std::uint64_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

std::string encode_curve_name_hex(const std::string& name) {
  static const char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.resize(name.size() * 2);
  for (std::size_t i = 0; i < name.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(name[i]);
    hex[i * 2] = kDigits[b >> 4];
    hex[i * 2 + 1] = kDigits[b & 0xF];
  }
  return hex;
}

bool decode_curve_name_hex(const std::string& hex, std::string& out) {
  if (hex.size() % 2 != 0) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string decoded;
  decoded.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    decoded.push_back(static_cast<char>((hi << 4) | lo));
  }
  out = std::move(decoded);
  return true;
}

wds::chart_editor::EasingAlgorithm parse_easing_algorithm(const std::string& text) {
  const auto t = trim(text);
  if (t == "linear") return wds::chart_editor::EasingAlgorithm::Linear;
  if (t == "poly") return wds::chart_editor::EasingAlgorithm::Poly;
  if (t == "exp") return wds::chart_editor::EasingAlgorithm::Exp;
  if (t == "sine") return wds::chart_editor::EasingAlgorithm::Sine;
  return wds::chart_editor::EasingAlgorithm::Linear;
}

const char* easing_algorithm_to_string(wds::chart_editor::EasingAlgorithm algorithm) {
  switch (algorithm) {
    case wds::chart_editor::EasingAlgorithm::Poly:
      return "poly";
    case wds::chart_editor::EasingAlgorithm::Exp:
      return "exp";
    case wds::chart_editor::EasingAlgorithm::Sine:
      return "sine";
    case wds::chart_editor::EasingAlgorithm::Linear:
    default:
      return "linear";
  }
}

wds::chart_editor::EasingDirection parse_easing_direction(const std::string& text) {
  const auto t = trim(text);
  if (t == "in") return wds::chart_editor::EasingDirection::In;
  if (t == "out") return wds::chart_editor::EasingDirection::Out;
  if (t == "inout") return wds::chart_editor::EasingDirection::InOut;
  if (t == "outin") return wds::chart_editor::EasingDirection::OutIn;
  return wds::chart_editor::EasingDirection::In;
}

const char* easing_direction_to_string(wds::chart_editor::EasingDirection direction) {
  switch (direction) {
    case wds::chart_editor::EasingDirection::Out:
      return "out";
    case wds::chart_editor::EasingDirection::InOut:
      return "inout";
    case wds::chart_editor::EasingDirection::OutIn:
      return "outin";
    case wds::chart_editor::EasingDirection::In:
    default:
      return "in";
  }
}

struct CurveTemplateSlot {
  std::uint64_t id = 0;
  bool has_id = false;
  std::string name;
  bool has_name = false;
  wds::chart_editor::EasingAlgorithm algorithm = wds::chart_editor::EasingAlgorithm::Linear;
  bool has_algorithm = false;
  double parameter = 0.0;
  bool has_parameter = false;
};

struct CurveLoadState {
  bool has_count = false;
  int32_t count = 0;
  bool has_selected = false;
  std::uint64_t selected_id = 0;
  bool has_direction = false;
  wds::chart_editor::EasingDirection direction = wds::chart_editor::EasingDirection::In;
  std::unordered_map<int32_t, CurveTemplateSlot> slots;
};

bool parse_curve_template_index_field(const std::string& key, int32_t& index, std::string& field) {
  static const std::string kPrefix = "curve_template_";
  if (key.rfind(kPrefix, 0) != 0) return false;
  const std::string rest = key.substr(kPrefix.size());
  const auto us = rest.find('_');
  if (us == std::string::npos || us == 0) return false;
  const std::string idx_s = rest.substr(0, us);
  if (idx_s.find_first_not_of("0123456789") != std::string::npos) return false;
  try {
    size_t n = 0;
    const long v = std::stol(idx_s, &n, 10);
    if (n != idx_s.size() || v < 0) return false;
    index = static_cast<int32_t>(v);
    field = rest.substr(us + 1);
    return true;
  } catch (...) {
    return false;
  }
}

void apply_curve_key(CurveLoadState& state, const std::string& key, const std::string& value) {
  if (key == "curve_template_count") {
    int32_t v = 0;
    if (parse_int(value, v)) {
      state.has_count = true;
      state.count = std::clamp(v, 0, static_cast<int32_t>(kMaxCurveTemplates));
    }
    return;
  }
  if (key == "curve_selected_template_id") {
    std::uint64_t v = 0;
    if (parse_u64(value, v)) {
      state.has_selected = true;
      state.selected_id = v;
    }
    return;
  }
  if (key == "curve_selected_direction") {
    state.has_direction = true;
    state.direction = parse_easing_direction(value);
    return;
  }
  int32_t index = 0;
  std::string field;
  if (!parse_curve_template_index_field(key, index, field)) return;
  if (index < 0 || static_cast<std::size_t>(index) >= kMaxCurveTemplates) return;
  CurveTemplateSlot& slot = state.slots[index];
  if (field == "id") {
    std::uint64_t v = 0;
    if (parse_u64(value, v)) {
      slot.has_id = true;
      slot.id = v;
    }
  } else if (field == "name_hex") {
    slot.has_name = true;
    std::string decoded;
    if (decode_curve_name_hex(trim(value), decoded)) {
      slot.name = std::move(decoded);
    } else {
      slot.name.clear();
    }
  } else if (field == "algorithm") {
    slot.has_algorithm = true;
    slot.algorithm = parse_easing_algorithm(value);
  } else if (field == "parameter") {
    double v = 0.0;
    if (parse_double(value, v)) {
      slot.has_parameter = true;
      slot.parameter = v;
    }
  }
}

void apply_curve_load(EditorUiConfig& cfg, const CurveLoadState& state) {
  if (state.has_count) {
    const int32_t n = std::min(state.count, static_cast<int32_t>(kMaxCurveTemplates));
    cfg.curve_templates.clear();
    cfg.curve_templates.resize(static_cast<std::size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
      const auto it = state.slots.find(i);
      if (it == state.slots.end()) continue;
      const CurveTemplateSlot& slot = it->second;
      CurveTemplate& tmpl = cfg.curve_templates[static_cast<std::size_t>(i)];
      if (slot.has_id) tmpl.id = slot.id;
      if (slot.has_name) tmpl.name = slot.name;
      if (slot.has_algorithm) tmpl.algorithm = slot.algorithm;
      if (slot.has_parameter) tmpl.parameter = slot.parameter;
    }
  }
  if (state.has_selected) cfg.curve_selected_template_id = state.selected_id;
  if (state.has_direction) cfg.curve_selected_direction = state.direction;
  normalize_curve_config(cfg.curve_templates, cfg.curve_selected_template_id);
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
  CurveLoadState curve_state;
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
    if (key.rfind("curve_", 0) == 0) {
      apply_curve_key(curve_state, key, value);
    } else {
      apply_key(cfg, key, value);
    }
  }
  apply_curve_load(cfg, curve_state);
  out = cfg;
  return true;
}

void capture_curve_template_state(const EditorUiConfig& cfg, CurveTemplateUiState& state) {
  state.templates = cfg.curve_templates;
  state.selected_id = cfg.curve_selected_template_id;
  state.direction = cfg.curve_selected_direction;
}

void apply_curve_template_state(EditorUiConfig& cfg, const CurveTemplateUiState& state) {
  cfg.curve_templates = state.templates;
  cfg.curve_selected_template_id = state.selected_id;
  cfg.curve_selected_direction = state.direction;
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

  std::vector<CurveTemplate> templates = cfg.curve_templates;
  if (templates.size() > kMaxCurveTemplates) {
    templates.resize(kMaxCurveTemplates);
  }
  std::uint64_t selected_id = cfg.curve_selected_template_id;
  for (auto& tmpl : templates) {
    normalize_curve_template(tmpl);
  }
  if (selected_id == 0 || find_curve_template_by_id(templates, selected_id) == nullptr) {
    selected_id = 0;
  }
  out << "curve_template_count: " << templates.size() << '\n';
  for (std::size_t i = 0; i < templates.size(); ++i) {
    const CurveTemplate& tmpl = templates[i];
    char parameter[64];
    std::snprintf(parameter, sizeof(parameter), "%.17g", tmpl.parameter);
    out << "curve_template_" << i << "_id: " << tmpl.id << '\n'
        << "curve_template_" << i << "_name_hex: " << encode_curve_name_hex(tmpl.name) << '\n'
        << "curve_template_" << i << "_algorithm: " << easing_algorithm_to_string(tmpl.algorithm)
        << '\n'
        << "curve_template_" << i << "_parameter: " << parameter << '\n';
  }
  out << "curve_selected_template_id: " << selected_id << '\n'
      << "curve_selected_direction: " << easing_direction_to_string(cfg.curve_selected_direction)
      << '\n';

  return wds::chart_editor::write_text_atomic(path, out.str()).error ==
         wds::chart_editor::SerializeError::Ok;
}

}  // namespace wds::ui

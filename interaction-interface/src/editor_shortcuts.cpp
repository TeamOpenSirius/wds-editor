#include "wds/interaction/editor_shortcuts.hpp"

#include "wds/interaction/platform.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace wds::interaction {
namespace {

std::array<ShortcutChord, kEditorShortcutCount>& storage() noexcept {
  static std::array<ShortcutChord, kEditorShortcutCount> chords = [] {
    std::array<ShortcutChord, kEditorShortcutCount> out{};
    for (std::size_t i = 0; i < kEditorShortcutCount; ++i) {
      out[i] = default_editor_shortcut(static_cast<EditorShortcut>(i));
    }
    return out;
  }();
  return chords;
}

bool is_letter_key(KeyCode key) noexcept {
  const int v = static_cast<int>(key);
  return v >= static_cast<int>(KeyCode::A) && v <= static_cast<int>(KeyCode::Z);
}

bool is_digit_key(KeyCode key) noexcept {
  const int v = static_cast<int>(key);
  return v >= static_cast<int>(KeyCode::Num0) && v <= static_cast<int>(KeyCode::Num9);
}

std::string key_token(KeyCode key) {
  if (is_letter_key(key)) {
    return std::string(1, static_cast<char>(key));
  }
  if (is_digit_key(key)) {
    return std::string(1, static_cast<char>('0' + (static_cast<int>(key) - static_cast<int>(KeyCode::Num0))));
  }
  switch (key) {
    case KeyCode::Space:
      return "Space";
    case KeyCode::Escape:
      return "Esc";
    case KeyCode::Enter:
      return "Enter";
    case KeyCode::Tab:
      return "Tab";
    case KeyCode::Backspace:
      return "Backspace";
    case KeyCode::Delete:
      return "Delete";
    case KeyCode::Left:
      return "Left";
    case KeyCode::Right:
      return "Right";
    case KeyCode::Up:
      return "Up";
    case KeyCode::Down:
      return "Down";
    case KeyCode::F1:
      return "F1";
    case KeyCode::F2:
      return "F2";
    case KeyCode::F3:
      return "F3";
    case KeyCode::F4:
      return "F4";
    case KeyCode::F11:
      return "F11";
    default:
      if (static_cast<int>(key) == 46) return ".";
      return {};
  }
}

std::optional<KeyCode> parse_key_token(std::string token) {
  for (char& c : token) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (token.size() == 1) {
    const char c = token[0];
    if (c >= 'A' && c <= 'Z') return static_cast<KeyCode>(c);
    if (c >= '0' && c <= '9') return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + (c - '0'));
    if (c == '.') return static_cast<KeyCode>(46);
  }
  if (token == "SPACE") return KeyCode::Space;
  if (token == "ESC" || token == "ESCAPE") return KeyCode::Escape;
  if (token == "ENTER" || token == "RETURN") return KeyCode::Enter;
  if (token == "TAB") return KeyCode::Tab;
  if (token == "BACKSPACE") return KeyCode::Backspace;
  if (token == "DELETE" || token == "DEL") return KeyCode::Delete;
  if (token == "LEFT") return KeyCode::Left;
  if (token == "RIGHT") return KeyCode::Right;
  if (token == "UP") return KeyCode::Up;
  if (token == "DOWN") return KeyCode::Down;
  if (token == "F1") return KeyCode::F1;
  if (token == "F2") return KeyCode::F2;
  if (token == "F3") return KeyCode::F3;
  if (token == "F4") return KeyCode::F4;
  if (token == "F11") return KeyCode::F11;
  return std::nullopt;
}

}  // namespace

const char* editor_shortcut_id(EditorShortcut id) noexcept {
  switch (id) {
    case EditorShortcut::TogglePlayback:
      return "toggle_playback";
    case EditorShortcut::PausePlayback:
      return "pause_playback";
    case EditorShortcut::Save:
      return "save";
    case EditorShortcut::Open:
      return "open";
    case EditorShortcut::Undo:
      return "undo";
    case EditorShortcut::Redo:
      return "redo";
    case EditorShortcut::Copy:
      return "copy";
    case EditorShortcut::Paste:
      return "paste";
    case EditorShortcut::Mirror:
      return "mirror";
    case EditorShortcut::MirrorAboutCenter:
      return "mirror_about_center";
    case EditorShortcut::NudgeUp:
      return "nudge_up";
    case EditorShortcut::NudgeDown:
      return "nudge_down";
    case EditorShortcut::NudgeLeft:
      return "nudge_left";
    case EditorShortcut::NudgeRight:
      return "nudge_right";
    case EditorShortcut::DeleteSelection:
      return "delete_selection";
    case EditorShortcut::ToggleFullscreen:
      return "toggle_fullscreen";
    case EditorShortcut::WidthSlot0:
      return "width_slot_0";
    case EditorShortcut::WidthSlot1:
      return "width_slot_1";
    case EditorShortcut::WidthSlot2:
      return "width_slot_2";
    case EditorShortcut::WidthSlot3:
      return "width_slot_3";
    case EditorShortcut::WidthSlot4:
      return "width_slot_4";
    case EditorShortcut::WidthSlot5:
      return "width_slot_5";
    case EditorShortcut::PlaybackRate0:
      return "playback_rate_0";
    case EditorShortcut::PlaybackRate1:
      return "playback_rate_1";
    case EditorShortcut::PlaybackRate2:
      return "playback_rate_2";
    case EditorShortcut::PlaybackRate3:
      return "playback_rate_3";
    case EditorShortcut::ToggleSfxMute:
      return "toggle_sfx_mute";
    case EditorShortcut::PlaceType0:
      return "place_type_0";
    case EditorShortcut::PlaceType1:
      return "place_type_1";
    case EditorShortcut::PlaceType2:
      return "place_type_2";
    case EditorShortcut::PlaceType3:
      return "place_type_3";
    case EditorShortcut::PlaceType4:
      return "place_type_4";
    case EditorShortcut::PlaceType5:
      return "place_type_5";
    case EditorShortcut::PlaceType6:
      return "place_type_6";
    case EditorShortcut::PlaceType7:
      return "place_type_7";
    case EditorShortcut::Count:
      break;
  }
  return "unknown";
}

const char* editor_shortcut_label(EditorShortcut id, bool pause_at_current) noexcept {
  switch (id) {
    case EditorShortcut::TogglePlayback:
      return "播放/暂停";
    case EditorShortcut::PausePlayback:
      // Default: Shift+Space pauses in place; checked option swaps → seek to play start.
      return pause_at_current ? "在开始播放位置暂停" : "在当前位置暂停";
    case EditorShortcut::Save:
      return "保存";
    case EditorShortcut::Open:
      return "打开";
    case EditorShortcut::Undo:
      return "撤销";
    case EditorShortcut::Redo:
      return "重做";
    case EditorShortcut::Copy:
      return "复制";
    case EditorShortcut::Paste:
      return "粘贴";
    case EditorShortcut::Mirror:
      return "镜像";
    case EditorShortcut::MirrorAboutCenter:
      return "中心镜像";
    case EditorShortcut::NudgeUp:
      return "上移";
    case EditorShortcut::NudgeDown:
      return "下移";
    case EditorShortcut::NudgeLeft:
      return "左移";
    case EditorShortcut::NudgeRight:
      return "右移";
    case EditorShortcut::DeleteSelection:
      return "删除选中";
    case EditorShortcut::ToggleFullscreen:
      return "切换全屏";
    case EditorShortcut::WidthSlot0:
      return "宽度一档";
    case EditorShortcut::WidthSlot1:
      return "宽度二档";
    case EditorShortcut::WidthSlot2:
      return "宽度三档";
    case EditorShortcut::WidthSlot3:
      return "宽度四档";
    case EditorShortcut::WidthSlot4:
      return "宽度五档";
    case EditorShortcut::WidthSlot5:
      return "宽度六档";
    case EditorShortcut::PlaybackRate0:
      return "播放速度 0.25x";
    case EditorShortcut::PlaybackRate1:
      return "播放速度 0.5x";
    case EditorShortcut::PlaybackRate2:
      return "播放速度 0.75x";
    case EditorShortcut::PlaybackRate3:
      return "播放速度 1x";
    case EditorShortcut::ToggleSfxMute:
      return "音效静音";
    case EditorShortcut::PlaceType0:
      return "Tap";
    case EditorShortcut::PlaceType1:
      return "ExTap";
    case EditorShortcut::PlaceType2:
      return "Hold Head";
    case EditorShortcut::PlaceType3:
      return "Hold";
    case EditorShortcut::PlaceType4:
      return "Left Flick";
    case EditorShortcut::PlaceType5:
      return "Flick";
    case EditorShortcut::PlaceType6:
      return "Right Flick";
    case EditorShortcut::PlaceType7:
      return "Scratch Hold";
    case EditorShortcut::Count:
      break;
  }
  return "";
}

ShortcutChord default_editor_shortcut(EditorShortcut id) noexcept {
  switch (id) {
    case EditorShortcut::TogglePlayback:
      return {KeyCode::Space, {}};
    case EditorShortcut::PausePlayback: {
      Modifiers mods;
      mods.shift = true;
      return {KeyCode::Space, mods};
    }
    case EditorShortcut::Save:
      return chord_primary(static_cast<KeyCode>('S'));
    case EditorShortcut::Open:
      return chord_primary(static_cast<KeyCode>('O'));
    case EditorShortcut::Undo:
      return chord_primary(static_cast<KeyCode>('Z'));
    case EditorShortcut::Redo:
      return chord_primary(static_cast<KeyCode>('Y'));
    case EditorShortcut::Copy:
      return chord_primary(static_cast<KeyCode>('C'));
    case EditorShortcut::Paste:
      return chord_primary(static_cast<KeyCode>('V'));
    case EditorShortcut::Mirror:
      return chord_primary(static_cast<KeyCode>('M'));
    case EditorShortcut::MirrorAboutCenter:
      return chord_primary(static_cast<KeyCode>('M'), true);
    case EditorShortcut::NudgeUp:
      return {KeyCode::Up, {}};
    case EditorShortcut::NudgeDown:
      return {KeyCode::Down, {}};
    case EditorShortcut::NudgeLeft:
      return {KeyCode::Left, {}};
    case EditorShortcut::NudgeRight:
      return {KeyCode::Right, {}};
    case EditorShortcut::DeleteSelection:
      return {KeyCode::Delete, {}};
    case EditorShortcut::ToggleFullscreen:
      return chord_primary(KeyCode::F11, /*shift=*/true);
    case EditorShortcut::WidthSlot0:
      return {static_cast<KeyCode>('Q'), {}};
    case EditorShortcut::WidthSlot1:
      return {static_cast<KeyCode>('W'), {}};
    case EditorShortcut::WidthSlot2:
      return {static_cast<KeyCode>('E'), {}};
    case EditorShortcut::WidthSlot3:
      return {KeyCode::A, {}};
    case EditorShortcut::WidthSlot4:
      return {static_cast<KeyCode>('S'), {}};
    case EditorShortcut::WidthSlot5:
      return {static_cast<KeyCode>('D'), {}};
    case EditorShortcut::PlaybackRate0:
      return {KeyCode::F1, {}};
    case EditorShortcut::PlaybackRate1:
      return {KeyCode::F2, {}};
    case EditorShortcut::PlaybackRate2:
      return {KeyCode::F3, {}};
    case EditorShortcut::PlaybackRate3:
      return {KeyCode::F4, {}};
    case EditorShortcut::ToggleSfxMute:
      return {static_cast<KeyCode>('X'), {}};
    case EditorShortcut::PlaceType0:
      return {KeyCode::Num1, {}};
    case EditorShortcut::PlaceType1:
      return {KeyCode::Num2, {}};
    case EditorShortcut::PlaceType2:
      return {KeyCode::Num3, {}};
    case EditorShortcut::PlaceType3:
      return {KeyCode::Num4, {}};
    case EditorShortcut::PlaceType4:
      return {KeyCode::Num5, {}};
    case EditorShortcut::PlaceType5:
      return {KeyCode::Num6, {}};
    case EditorShortcut::PlaceType6:
      return {KeyCode::Num7, {}};
    case EditorShortcut::PlaceType7:
      return {KeyCode::Num8, {}};
    case EditorShortcut::Count:
      break;
  }
  return {};
}

const ShortcutChord& editor_shortcut(EditorShortcut id) noexcept {
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= kEditorShortcutCount) {
    static const ShortcutChord kEmpty{};
    return kEmpty;
  }
  return storage()[idx];
}

void set_editor_shortcut(EditorShortcut id, ShortcutChord chord) noexcept {
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= kEditorShortcutCount) return;
  chord.mods = normalize_primary(chord.mods);
  storage()[idx] = chord;
}

void set_editor_shortcuts(const std::array<ShortcutChord, kEditorShortcutCount>& chords) noexcept {
  for (std::size_t i = 0; i < kEditorShortcutCount; ++i) {
    set_editor_shortcut(static_cast<EditorShortcut>(i), chords[i]);
  }
}

void reset_editor_shortcuts() noexcept {
  for (std::size_t i = 0; i < kEditorShortcutCount; ++i) {
    storage()[i] = default_editor_shortcut(static_cast<EditorShortcut>(i));
  }
}

std::array<ShortcutChord, kEditorShortcutCount> editor_shortcuts_snapshot() noexcept {
  return storage();
}

bool is_forbidden_shortcut_key(KeyCode key) noexcept {
  // Period and other typable punctuation that text fields accept. Digits are allowed.
  if (static_cast<int>(key) == 46) return true;  // '.'
  return false;
}

bool is_completing_shortcut_key(KeyCode key) noexcept {
  if (key == KeyCode::Unknown) return false;
  if (is_forbidden_shortcut_key(key)) return false;
  if (is_letter_key(key)) return true;
  if (is_digit_key(key)) return true;
  switch (key) {
    case KeyCode::Space:
    case KeyCode::Delete:
    case KeyCode::Left:
    case KeyCode::Right:
    case KeyCode::Up:
    case KeyCode::Down:
    case KeyCode::F1:
    case KeyCode::F2:
    case KeyCode::F3:
    case KeyCode::F4:
    case KeyCode::F11:
    case KeyCode::Tab:
    case KeyCode::Backspace:
      return true;
    default:
      return false;
  }
}

std::string format_shortcut_modifiers_portable(const Modifiers& mods) {
  std::string out;
  auto append = [&](const char* token) {
    if (!out.empty()) out += '+';
    out += token;
  };
  // Same order as QKeySequence::PortableText: Meta, Ctrl, Alt, Shift.
  if (mods.super) append("Meta");
  if (mods.control) append("Ctrl");
  if (mods.alt) append("Alt");
  if (mods.shift) append("Shift");
  return out;
}

std::string format_shortcut_modifiers(const Modifiers& mods) {
#ifdef __APPLE__
  // Same glyphs and order as QKeySequence::NativeText on macOS.
  std::string out;
  if (mods.super) out += "⌃";
  if (mods.alt) out += "⌥";
  if (mods.shift) out += "⇧";
  if (mods.control) out += "⌘";
  return out;
#else
  return format_shortcut_modifiers_portable(mods);
#endif
}

std::string format_shortcut_chord_portable(const ShortcutChord& chord) {
  std::string out = format_shortcut_modifiers_portable(chord.mods);
  const std::string key = key_token(chord.key);
  if (key.empty()) return out;
  if (!out.empty()) out += '+';
  out += key;
  return out;
}

std::string format_shortcut_chord(const ShortcutChord& chord) {
  const std::string key = key_token(chord.key);
#ifdef __APPLE__
  return format_shortcut_modifiers(chord.mods) + key;
#else
  std::string out = format_shortcut_modifiers(chord.mods);
  if (key.empty()) return out;
  if (!out.empty()) out += '+';
  out += key;
  return out;
#endif
}

std::optional<ShortcutChord> parse_shortcut_chord(const std::string& text) {
  std::string s;
  s.reserve(text.size());
  for (char c : text) {
    if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
  }
  if (s.empty()) return std::nullopt;

  auto replace_utf8 = [&](std::string_view glyph, const char* token) {
    for (std::string::size_type pos = 0; (pos = s.find(glyph, pos)) != std::string::npos;) {
      const std::string repl = std::string("+") + token + "+";
      s.replace(pos, glyph.size(), repl);
      pos += repl.size();
    }
  };
  replace_utf8("⌘", "Cmd");
  replace_utf8("⌥", "Option");
  replace_utf8("⇧", "Shift");
  replace_utf8("⌃", "Meta");
  std::string compact;
  compact.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+' && (compact.empty() || compact.back() == '+')) continue;
    compact.push_back(s[i]);
  }
  if (!compact.empty() && compact.back() == '+') compact.pop_back();
  s = std::move(compact);
  if (s.empty()) return std::nullopt;

  std::vector<std::string> parts;
  std::string cur;
  for (char c : s) {
    if (c == '+') {
      if (!cur.empty()) {
        parts.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  if (parts.empty()) return std::nullopt;

  Modifiers mods;
  std::optional<KeyCode> key;
  for (std::string part : parts) {
    std::string upper = part;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "SHIFT") {
      mods.shift = true;
      continue;
    }
    if (upper == "CTRL" || upper == "CONTROL" || upper == "CMD" || upper == "COMMAND") {
      mods.control = true;
      continue;
    }
    if (upper == "SUPER" || upper == "WIN" || upper == "META") {
      mods.super = true;
      continue;
    }
    if (upper == "ALT" || upper == "OPTION") {
      mods.alt = true;
      continue;
    }
    if (key.has_value()) return std::nullopt;
    key = parse_key_token(part);
    if (!key) return std::nullopt;
  }
  if (!key) return std::nullopt;
  ShortcutChord chord{*key, mods};
  chord.mods = normalize_primary(chord.mods);
  return chord;
}

bool editor_shortcut_conflicts(EditorShortcut self, const ShortcutChord& chord) noexcept {
  ShortcutChord normalized = chord;
  normalized.mods = normalize_primary(normalized.mods);
  for (std::size_t i = 0; i < kEditorShortcutCount; ++i) {
    const auto id = static_cast<EditorShortcut>(i);
    if (id == self) continue;
    ShortcutChord other = storage()[i];
    other.mods = normalize_primary(other.mods);
    if (other == normalized) return true;
  }
  return false;
}

}  // namespace wds::interaction

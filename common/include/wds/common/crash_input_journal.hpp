#pragma once

// Process-wide pre-crash input journal. Runtime stores structured fields only;
// report text is formatted in the crash path (async-signal-safe on POSIX).

#include <cstddef>
#include <cstdint>

namespace wds::common {

inline constexpr std::size_t kCrashInputCap = 100;
inline constexpr std::size_t kCrashEffectCap = 16;
inline constexpr std::size_t kCrashTextCap = 64;

enum class CrashInputKind : std::uint8_t {
  None = 0,
  PointerDown,
  PointerUp,
  Click,
  DoubleClick,
  Scroll,
  KeyDown,
  KeyUp,
  TextInput,
  Menu,
};

enum class CrashRouteVia : std::uint8_t {
  None = 0,
  HitTest,
  Capture,
  Focus,
  PressTarget,
  Shortcut,
  Popup,
  Host,
  Menu,
};

enum class CrashHandlerId : std::uint8_t {
  None = 0,
  PointerDown,
  PointerUp,
  PointerMove,
  Click,
  DoubleClick,
  Scroll,
  KeyDown,
  KeyUp,
  TextInput,
};

enum class CrashEffectId : std::uint8_t {
  None = 0,
  TimelineMs,
  VisibleHectoms,
  EditMode,
  Ghost,
  DocRevision,
  PopupScroll,
  ComboOpen,
  ShortcutListScroll,
};

enum CrashSnapField : std::uint32_t {
  kCrashSnapTimeline = 1u << 0,
  kCrashSnapHectoms = 1u << 1,
  kCrashSnapEditMode = 1u << 2,
  kCrashSnapGhost = 1u << 3,
  kCrashSnapRevision = 1u << 4,
  kCrashSnapPopupScroll = 1u << 5,
  kCrashSnapComboOpen = 1u << 6,
  kCrashSnapShortcutScroll = 1u << 7,
};

struct CrashTraceSnap {
  std::uint32_t mask = 0;
  std::int64_t timeline_ms = 0;
  std::int32_t visible_hectoms = 0;
  std::uint8_t edit_mode = 0;
  std::uint8_t ghost = 0;
  std::uint64_t doc_revision = 0;
  float popup_scroll = 0.0f;
  std::uint8_t combo_open = 0;
  float shortcut_list_scroll = 0.0f;
};

struct CrashEffect {
  CrashEffectId id = CrashEffectId::None;
  std::int64_t i0 = 0;
  std::int64_t i1 = 0;
  float f0 = 0.0f;
  float f1 = 0.0f;
};

struct CrashInputSlot {
  std::uint64_t t_ms = 0;
  CrashInputKind kind = CrashInputKind::None;
  float x = 0.0f;
  float y = 0.0f;
  float dx = 0.0f;
  float dy = 0.0f;
  std::int32_t key = 0;
  std::uint8_t mods = 0;
  std::uint8_t button = 0;
  std::uint8_t repeat = 0;
  std::uint16_t text_len = 0;
  std::uint8_t text_printable = 0;
  char text[kCrashTextCap]{};
  const char* route_target = nullptr;
  CrashRouteVia route_via = CrashRouteVia::None;
  std::int32_t shortcut_id = -1;
  CrashHandlerId handler = CrashHandlerId::None;
  std::uint8_t effect_count = 0;
  CrashEffect effects[kCrashEffectCap]{};
  std::uint16_t move_count = 0;
  float move_x = 0.0f;
  float move_y = 0.0f;
  std::uint8_t drag_mode = 0;
};

void journal_reset_for_test();
void journal_set_allow_sensitive(bool allow) noexcept;
bool journal_allow_sensitive() noexcept;
std::uint64_t journal_uptime_ms() noexcept;

void journal_begin_event(CrashInputKind kind, float x, float y, float dx, float dy,
                         std::int32_t key, std::uint8_t mods, std::uint8_t button,
                         std::uint8_t repeat);
void journal_set_text(const char* text, std::size_t len);
void journal_set_route(const char* target_literal, CrashRouteVia via);
void journal_set_shortcut(std::int32_t id);
void journal_set_handler(CrashHandlerId handler);
void journal_add_effect(CrashEffectId id, std::int64_t i0, std::int64_t i1, float f0, float f1);
void journal_diff_snap(const CrashTraceSnap& before, const CrashTraceSnap& after);
void journal_end_event();

// PointerMove hot path: four scalar stores. No memcpy / snprintf.
void journal_note_move(float x, float y, std::uint8_t mode) noexcept;

std::size_t journal_count() noexcept;
const CrashInputSlot* journal_slot_from_oldest(std::size_t index) noexcept;
const CrashInputSlot* journal_pending_move_slot() noexcept;

void journal_copy_path(char* dst, std::size_t cap, const char* path);

using JournalEmitFn = void (*)(void* ctx, const char* data, std::size_t n);
void journal_write_text(JournalEmitFn emit, void* ctx);

}  // namespace wds::common

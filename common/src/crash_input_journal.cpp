#include "wds/common/crash_input_journal.hpp"

#include <cstring>

#if defined(_WIN32)
#include <chrono>
#else
#include <time.h>
#endif

namespace wds::common {
namespace {

struct PendingMove {
  std::uint16_t count = 0;
  float x = 0.0f;
  float y = 0.0f;
  std::uint8_t mode = 0;
};

CrashInputSlot g_slots[kCrashInputCap];
std::uint32_t g_head = 0;
std::uint32_t g_count = 0;
CrashInputSlot* g_current = nullptr;
PendingMove g_pending{};
bool g_allow_sensitive = false;
#if defined(_WIN32)
const auto g_start = std::chrono::steady_clock::now();
#else
timespec make_start_ts() {
  timespec ts{};
  (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts;
}
const timespec g_start_ts = make_start_ts();
#endif

CrashInputSlot* last_slot() noexcept {
  if (g_count == 0) {
    return nullptr;
  }
  const std::uint32_t idx = (g_head + kCrashInputCap - 1) % kCrashInputCap;
  return &g_slots[idx];
}

void emit_str(JournalEmitFn emit, void* ctx, const char* s) {
  if (emit == nullptr || s == nullptr) {
    return;
  }
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  emit(ctx, s, n);
}

void emit_uint(JournalEmitFn emit, void* ctx, std::uint64_t v) {
  char buf[24];
  int i = 23;
  buf[i] = '\0';
  if (v == 0) {
    buf[--i] = '0';
  } else {
    while (v > 0 && i > 0) {
      buf[--i] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
  }
  emit_str(emit, ctx, buf + i);
}

void emit_int(JournalEmitFn emit, void* ctx, std::int64_t v) {
  if (v < 0) {
    emit_str(emit, ctx, "-");
    emit_uint(emit, ctx, static_cast<std::uint64_t>(-v));
    return;
  }
  emit_uint(emit, ctx, static_cast<std::uint64_t>(v));
}

void emit_float(JournalEmitFn emit, void* ctx, float v) {
  if (v < 0.0f) {
    emit_str(emit, ctx, "-");
    v = -v;
  }
  const auto whole = static_cast<std::uint64_t>(v);
  emit_uint(emit, ctx, whole);
  emit_str(emit, ctx, ".");
  int frac = static_cast<int>((v - static_cast<float>(whole)) * 100.0f + 0.5f);
  if (frac >= 100) {
    frac = 99;
  }
  char d[3] = {static_cast<char>('0' + frac / 10), static_cast<char>('0' + frac % 10), '\0'};
  emit_str(emit, ctx, d);
}

const char* kind_name(CrashInputKind k) {
  switch (k) {
    case CrashInputKind::PointerDown:
      return "PointerDown";
    case CrashInputKind::PointerUp:
      return "PointerUp";
    case CrashInputKind::Click:
      return "Click";
    case CrashInputKind::DoubleClick:
      return "DoubleClick";
    case CrashInputKind::Scroll:
      return "Scroll";
    case CrashInputKind::KeyDown:
      return "KeyDown";
    case CrashInputKind::KeyUp:
      return "KeyUp";
    case CrashInputKind::TextInput:
      return "TextInput";
    case CrashInputKind::Menu:
      return "Menu";
    default:
      return "None";
  }
}

const char* via_name(CrashRouteVia v) {
  switch (v) {
    case CrashRouteVia::HitTest:
      return "hit_test";
    case CrashRouteVia::Capture:
      return "capture";
    case CrashRouteVia::Focus:
      return "focus";
    case CrashRouteVia::PressTarget:
      return "press_target";
    case CrashRouteVia::Shortcut:
      return "shortcut";
    case CrashRouteVia::Popup:
      return "popup";
    case CrashRouteVia::Host:
      return "host";
    case CrashRouteVia::Menu:
      return "menu";
    default:
      return "none";
  }
}

const char* handler_name(CrashHandlerId h) {
  switch (h) {
    case CrashHandlerId::PointerDown:
      return "on_pointer_down";
    case CrashHandlerId::PointerUp:
      return "on_pointer_up";
    case CrashHandlerId::PointerMove:
      return "on_pointer_move";
    case CrashHandlerId::Click:
      return "on_click";
    case CrashHandlerId::DoubleClick:
      return "on_double_click";
    case CrashHandlerId::Scroll:
      return "on_scroll";
    case CrashHandlerId::KeyDown:
      return "on_key_down";
    case CrashHandlerId::KeyUp:
      return "on_key_up";
    case CrashHandlerId::TextInput:
      return "on_text_input";
    default:
      return "none";
  }
}

const char* effect_name(CrashEffectId id) {
  switch (id) {
    case CrashEffectId::TimelineMs:
      return "timeline_ms";
    case CrashEffectId::VisibleHectoms:
      return "visible_hectoms";
    case CrashEffectId::EditMode:
      return "edit_mode";
    case CrashEffectId::Ghost:
      return "ghost";
    case CrashEffectId::DocRevision:
      return "doc_revision";
    case CrashEffectId::PopupScroll:
      return "popup_scroll";
    case CrashEffectId::ComboOpen:
      return "combo_open";
    case CrashEffectId::ShortcutListScroll:
      return "shortcut_list_scroll";
    default:
      return "none";
  }
}

void write_slot(JournalEmitFn emit, void* ctx, const CrashInputSlot& s) {
  emit_str(emit, ctx, "  kind=");
  emit_str(emit, ctx, kind_name(s.kind));
  emit_str(emit, ctx, " t_ms=");
  emit_uint(emit, ctx, s.t_ms);
  emit_str(emit, ctx, " pos=");
  emit_float(emit, ctx, s.x);
  emit_str(emit, ctx, ",");
  emit_float(emit, ctx, s.y);
  if (s.kind == CrashInputKind::Scroll) {
    emit_str(emit, ctx, " d=");
    emit_float(emit, ctx, s.dx);
    emit_str(emit, ctx, ",");
    emit_float(emit, ctx, s.dy);
  }
  if (s.kind == CrashInputKind::KeyDown || s.kind == CrashInputKind::KeyUp) {
    emit_str(emit, ctx, " key=");
    emit_int(emit, ctx, s.key);
    emit_str(emit, ctx, " mods=");
    emit_uint(emit, ctx, s.mods);
    if (s.repeat) {
      emit_str(emit, ctx, " repeat=1");
    }
  }
  if (s.kind == CrashInputKind::PointerDown || s.kind == CrashInputKind::PointerUp ||
      s.kind == CrashInputKind::Click || s.kind == CrashInputKind::DoubleClick) {
    emit_str(emit, ctx, " btn=");
    emit_uint(emit, ctx, s.button);
    emit_str(emit, ctx, " mods=");
    emit_uint(emit, ctx, s.mods);
  }
  if (s.kind == CrashInputKind::TextInput) {
    emit_str(emit, ctx, " text_len=");
    emit_uint(emit, ctx, s.text_len);
    emit_str(emit, ctx, " printable=");
    emit_uint(emit, ctx, s.text_printable);
    if (g_allow_sensitive && s.text[0] != '\0') {
      emit_str(emit, ctx, " text=");
      emit_str(emit, ctx, s.text);
    }
  }
  emit_str(emit, ctx, "\n");
  if (s.route_target != nullptr || s.route_via != CrashRouteVia::None) {
    emit_str(emit, ctx, "    route target=");
    emit_str(emit, ctx, s.route_target != nullptr ? s.route_target : "-");
    emit_str(emit, ctx, " via=");
    emit_str(emit, ctx, via_name(s.route_via));
    if (s.shortcut_id >= 0) {
      emit_str(emit, ctx, " shortcut=");
      emit_int(emit, ctx, s.shortcut_id);
    }
    emit_str(emit, ctx, " handler=");
    emit_str(emit, ctx, handler_name(s.handler));
    emit_str(emit, ctx, "\n");
  }
  for (std::uint8_t i = 0; i < s.effect_count; ++i) {
    const auto& e = s.effects[i];
    emit_str(emit, ctx, "    effect ");
    emit_str(emit, ctx, effect_name(e.id));
    emit_str(emit, ctx, " ");
    emit_int(emit, ctx, e.i0);
    emit_str(emit, ctx, "->");
    emit_int(emit, ctx, e.i1);
    emit_str(emit, ctx, " ");
    emit_float(emit, ctx, e.f0);
    emit_str(emit, ctx, "->");
    emit_float(emit, ctx, e.f1);
    emit_str(emit, ctx, "\n");
  }
  if (s.move_count > 0) {
    emit_str(emit, ctx, "    moves_since n=");
    emit_uint(emit, ctx, s.move_count);
    emit_str(emit, ctx, " last=");
    emit_float(emit, ctx, s.move_x);
    emit_str(emit, ctx, ",");
    emit_float(emit, ctx, s.move_y);
    emit_str(emit, ctx, " mode=");
    emit_uint(emit, ctx, s.drag_mode);
    emit_str(emit, ctx, "\n");
  }
}

}  // namespace

void journal_reset_for_test() {
  for (auto& s : g_slots) {
    s = CrashInputSlot{};
  }
  g_head = 0;
  g_count = 0;
  g_current = nullptr;
  g_pending = PendingMove{};
  g_allow_sensitive = false;
}

void journal_set_allow_sensitive(bool allow) noexcept { g_allow_sensitive = allow; }

bool journal_allow_sensitive() noexcept { return g_allow_sensitive; }

std::uint64_t journal_uptime_ms() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            g_start)
          .count());
#else
  // clock_gettime(CLOCK_MONOTONIC) is async-signal-safe (POSIX.1-2008).
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  const std::uint64_t start_ms = static_cast<std::uint64_t>(g_start_ts.tv_sec) * 1000ull +
                                 static_cast<std::uint64_t>(g_start_ts.tv_nsec) / 1000000ull;
  const std::uint64_t now_ms = static_cast<std::uint64_t>(now.tv_sec) * 1000ull +
                               static_cast<std::uint64_t>(now.tv_nsec) / 1000000ull;
  return now_ms >= start_ms ? now_ms - start_ms : 0;
#endif
}

void journal_begin_event(CrashInputKind kind, float x, float y, float dx, float dy,
                         std::int32_t key, std::uint8_t mods, std::uint8_t button,
                         std::uint8_t repeat) {
  CrashInputSlot& slot = g_slots[g_head];
  slot = CrashInputSlot{};
  slot.t_ms = journal_uptime_ms();
  slot.kind = kind;
  slot.x = x;
  slot.y = y;
  slot.dx = dx;
  slot.dy = dy;
  slot.key = key;
  slot.mods = mods;
  slot.button = button;
  slot.repeat = repeat;
  g_current = &slot;
  g_head = (g_head + 1) % kCrashInputCap;
  if (g_count < kCrashInputCap) {
    ++g_count;
  }
}

void journal_set_text(const char* text, std::size_t len) {
  if (g_current == nullptr) {
    return;
  }
  g_current->text_len = static_cast<std::uint16_t>(len > 0xffff ? 0xffff : len);
  bool printable = true;
  if (text != nullptr) {
    for (std::size_t i = 0; i < len; ++i) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c < 32 && c != 9) {
        printable = false;
        break;
      }
    }
  }
  g_current->text_printable = printable ? 1 : 0;
  if (!g_allow_sensitive || text == nullptr || len == 0) {
    g_current->text[0] = '\0';
    return;
  }
  const std::size_t n = len < (kCrashTextCap - 1) ? len : (kCrashTextCap - 1);
  std::memcpy(g_current->text, text, n);
  g_current->text[n] = '\0';
}

void journal_set_route(const char* target_literal, CrashRouteVia via) {
  if (g_current == nullptr) {
    return;
  }
  g_current->route_target = target_literal;
  g_current->route_via = via;
}

void journal_set_shortcut(std::int32_t id) {
  if (g_current != nullptr) {
    g_current->shortcut_id = id;
  }
}

void journal_set_handler(CrashHandlerId handler) {
  if (g_current != nullptr) {
    g_current->handler = handler;
  }
}

void journal_add_effect(CrashEffectId id, std::int64_t i0, std::int64_t i1, float f0, float f1) {
  if (g_current == nullptr || g_current->effect_count >= kCrashEffectCap) {
    return;
  }
  auto& e = g_current->effects[g_current->effect_count++];
  e.id = id;
  e.i0 = i0;
  e.i1 = i1;
  e.f0 = f0;
  e.f1 = f1;
}

void journal_diff_snap(const CrashTraceSnap& before, const CrashTraceSnap& after) {
  const std::uint32_t mask = before.mask | after.mask;
  if ((mask & kCrashSnapTimeline) != 0 && before.timeline_ms != after.timeline_ms) {
    journal_add_effect(CrashEffectId::TimelineMs, before.timeline_ms, after.timeline_ms, 0, 0);
  }
  if ((mask & kCrashSnapHectoms) != 0 && before.visible_hectoms != after.visible_hectoms) {
    journal_add_effect(CrashEffectId::VisibleHectoms, before.visible_hectoms,
                       after.visible_hectoms, 0, 0);
  }
  if ((mask & kCrashSnapEditMode) != 0 && before.edit_mode != after.edit_mode) {
    journal_add_effect(CrashEffectId::EditMode, before.edit_mode, after.edit_mode, 0, 0);
  }
  if ((mask & kCrashSnapGhost) != 0 && before.ghost != after.ghost) {
    journal_add_effect(CrashEffectId::Ghost, before.ghost, after.ghost, 0, 0);
  }
  if ((mask & kCrashSnapRevision) != 0 && before.doc_revision != after.doc_revision) {
    journal_add_effect(CrashEffectId::DocRevision, static_cast<std::int64_t>(before.doc_revision),
                       static_cast<std::int64_t>(after.doc_revision), 0, 0);
  }
  if ((mask & kCrashSnapPopupScroll) != 0 && before.popup_scroll != after.popup_scroll) {
    journal_add_effect(CrashEffectId::PopupScroll, 0, 0, before.popup_scroll, after.popup_scroll);
  }
  if ((mask & kCrashSnapComboOpen) != 0 && before.combo_open != after.combo_open) {
    journal_add_effect(CrashEffectId::ComboOpen, before.combo_open, after.combo_open, 0, 0);
  }
  if ((mask & kCrashSnapShortcutScroll) != 0 &&
      before.shortcut_list_scroll != after.shortcut_list_scroll) {
    journal_add_effect(CrashEffectId::ShortcutListScroll, 0, 0, before.shortcut_list_scroll,
                       after.shortcut_list_scroll);
  }
}

void journal_end_event() { g_current = nullptr; }

void journal_note_move(float x, float y, std::uint8_t mode) noexcept {
  CrashInputSlot* slot = g_current != nullptr ? g_current : last_slot();
  if (slot == nullptr) {
    ++g_pending.count;
    g_pending.x = x;
    g_pending.y = y;
    g_pending.mode = mode;
    return;
  }
  ++slot->move_count;
  slot->move_x = x;
  slot->move_y = y;
  slot->drag_mode = mode;
}

std::size_t journal_count() noexcept { return g_count; }

const CrashInputSlot* journal_slot_from_oldest(std::size_t index) noexcept {
  if (index >= g_count) {
    return nullptr;
  }
  const std::uint32_t start =
      g_count < kCrashInputCap ? 0 : g_head;
  const std::uint32_t idx = (start + static_cast<std::uint32_t>(index)) % kCrashInputCap;
  return &g_slots[idx];
}

const CrashInputSlot* journal_pending_move_slot() noexcept {
  static CrashInputSlot pending_view{};
  if (g_pending.count == 0) {
    return nullptr;
  }
  pending_view = CrashInputSlot{};
  pending_view.move_count = g_pending.count;
  pending_view.move_x = g_pending.x;
  pending_view.move_y = g_pending.y;
  pending_view.drag_mode = g_pending.mode;
  return &pending_view;
}

void journal_copy_path(char* dst, std::size_t cap, const char* path) {
  if (dst == nullptr || cap == 0) {
    return;
  }
  dst[0] = '\0';
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  const char* src = path;
  if (!g_allow_sensitive) {
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
      if (*p == '/' || *p == '\\') {
        base = p + 1;
      }
    }
    src = (base != nullptr && base[0] != '\0') ? base : "<path>";
  }
  std::size_t n = 0;
  while (src[n] != '\0' && n + 1 < cap) {
    ++n;
  }
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

void journal_write_text(JournalEmitFn emit, void* ctx) {
  if (emit == nullptr) {
    return;
  }
  emit_str(emit, ctx, "--- input journal ---\n");
  emit_str(emit, ctx, "count=");
  emit_uint(emit, ctx, g_count);
  emit_str(emit, ctx, "\n");
  if (g_pending.count > 0 && g_count == 0) {
    emit_str(emit, ctx, "  pending_moves n=");
    emit_uint(emit, ctx, g_pending.count);
    emit_str(emit, ctx, " last=");
    emit_float(emit, ctx, g_pending.x);
    emit_str(emit, ctx, ",");
    emit_float(emit, ctx, g_pending.y);
    emit_str(emit, ctx, " mode=");
    emit_uint(emit, ctx, g_pending.mode);
    emit_str(emit, ctx, "\n");
  }
  for (std::size_t i = 0; i < g_count; ++i) {
    const CrashInputSlot* s = journal_slot_from_oldest(i);
    if (s != nullptr) {
      write_slot(emit, ctx, *s);
    }
  }
}

}  // namespace wds::common

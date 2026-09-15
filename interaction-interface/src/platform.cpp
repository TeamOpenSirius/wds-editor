#include "wds/interaction/platform.hpp"

namespace wds::interaction {

bool primary_modifier_down(const Modifiers& modifiers) noexcept {
  // Qt already maps Command → ControlModifier on macOS, same as Ctrl on Windows.
  return modifiers.control;
}

Modifiers normalize_primary(Modifiers modifiers) noexcept {
  return modifiers;
}

ShortcutChord chord_primary(KeyCode key, bool shift) noexcept {
  Modifiers modifiers;
  modifiers.control = true;
  modifiers.shift = shift;
  return {key, modifiers};
}

}  // namespace wds::interaction

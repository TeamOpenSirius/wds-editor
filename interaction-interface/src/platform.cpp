#include "wds/interaction/platform.hpp"

namespace wds::interaction {

bool primary_modifier_down(const Modifiers& modifiers) noexcept {
#ifdef __APPLE__
  return modifiers.super;
#else
  return modifiers.control;
#endif
}

Modifiers normalize_primary(Modifiers modifiers) noexcept {
#ifdef __APPLE__
  modifiers.control = modifiers.control || modifiers.super;
  modifiers.super = false;
#endif
  return modifiers;
}

ShortcutChord chord_primary(KeyCode key, bool shift) noexcept {
  Modifiers modifiers;
  modifiers.control = true;
  modifiers.shift = shift;
  return {key, modifiers};
}

}  // namespace wds::interaction

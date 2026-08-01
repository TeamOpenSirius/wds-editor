#pragma once

#include "shortcuts.hpp"

namespace wds::interaction {

bool primary_modifier_down(const Modifiers& modifiers) noexcept;
Modifiers normalize_primary(Modifiers modifiers) noexcept;
ShortcutChord chord_primary(KeyCode key, bool shift = false) noexcept;

}  // namespace wds::interaction

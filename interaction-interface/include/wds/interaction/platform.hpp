#pragma once

#include "shortcuts.hpp"

namespace wds::interaction {

// Qt ControlModifier: Command on macOS, Ctrl on Windows. No OS branch.
bool primary_modifier_down(const Modifiers& modifiers) noexcept;
Modifiers normalize_primary(Modifiers modifiers) noexcept;
ShortcutChord chord_primary(KeyCode key, bool shift = false) noexcept;

}  // namespace wds::interaction

#include "wds/interaction/shortcuts.hpp"
#include "wds/interaction/platform.hpp"

namespace wds::interaction {

bool ShortcutNamespace::bind(ShortcutChord chord, ShortcutAction action) {
  chord.mods = normalize_primary(chord.mods);
  if (bindings_.find(chord) != bindings_.end()) {
    return false;
  }
  bindings_.emplace(chord, std::move(action));
  return true;
}

bool ShortcutNamespace::bind_primary(KeyCode key, ShortcutAction action, bool shift) {
  return bind(chord_primary(key, shift), std::move(action));
}

void ShortcutNamespace::unbind(ShortcutChord chord) {
  chord.mods = normalize_primary(chord.mods);
  bindings_.erase(chord);
}

bool ShortcutNamespace::dispatch(const KeyDownEvent& event) const {
  const ShortcutChord chord{event.key, normalize_primary(event.mods)};
  const auto it = bindings_.find(chord);
  if (it == bindings_.end() || !it->second) {
    return false;
  }
  it->second();
  return true;
}

bool ShortcutNamespace::has(const ShortcutChord& chord) const {
  ShortcutChord normalized = chord;
  normalized.mods = normalize_primary(normalized.mods);
  return bindings_.find(normalized) != bindings_.end();
}

ShortcutNamespace& ShortcutManager::namespace_for(const std::string& name) {
  return namespaces_[name];
}

const ShortcutNamespace& ShortcutManager::namespace_for(const std::string& name) const {
  return namespaces_.at(name);
}

void ShortcutManager::set_active_namespace(const std::string& name) { active_ = name; }

bool ShortcutManager::dispatch(const KeyDownEvent& event) const {
  if (active_.empty()) {
    return false;
  }
  const auto it = namespaces_.find(active_);
  if (it == namespaces_.end()) {
    return false;
  }
  return it->second.dispatch(event);
}

}  // namespace wds::interaction

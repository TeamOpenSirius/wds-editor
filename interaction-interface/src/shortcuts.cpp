#include "wds/interaction/shortcuts.hpp"
#include "wds/interaction/platform.hpp"

namespace wds::interaction {

bool ShortcutNamespace::bind(ShortcutChord chord, ShortcutAction action) {
  chord.mods = normalize_primary(chord.mods);
  // Cleared shortcuts use Unknown — skip so they neither fire nor collide in the map.
  if (chord.key == KeyCode::Unknown) {
    return false;
  }
  if (bindings_.find(chord) != bindings_.end()) {
    return false;
  }
  bindings_.emplace(chord, std::move(action));
  return true;
}

bool ShortcutNamespace::bind_primary(KeyCode key, ShortcutAction action, bool shift) {
  return bind(chord_primary(key, shift), std::move(action));
}

void ShortcutNamespace::clear() noexcept { bindings_.clear(); }

bool ShortcutNamespace::contains(const KeyDownEvent& event) const {
  if (event.repeat) {
    return false;
  }
  const ShortcutChord chord{event.key, normalize_primary(event.mods)};
  const auto it = bindings_.find(chord);
  return it != bindings_.end() && static_cast<bool>(it->second);
}

bool ShortcutNamespace::dispatch(const KeyDownEvent& event) const {
  // Ignore OS key-repeat for bound actions (play/pause, save, undo, …).
  if (!contains(event)) {
    return false;
  }
  const ShortcutChord chord{event.key, normalize_primary(event.mods)};
  bindings_.find(chord)->second();
  return true;
}

ShortcutNamespace& ShortcutManager::namespace_for(const std::string& name) {
  return namespaces_[name];
}

const ShortcutNamespace& ShortcutManager::namespace_for(const std::string& name) const {
  return namespaces_.at(name);
}

void ShortcutManager::set_active_namespace(const std::string& name) { active_ = name; }

bool ShortcutManager::contains(const KeyDownEvent& event) const {
  if (active_.empty()) {
    return false;
  }
  const auto it = namespaces_.find(active_);
  if (it == namespaces_.end()) {
    return false;
  }
  return it->second.contains(event);
}

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

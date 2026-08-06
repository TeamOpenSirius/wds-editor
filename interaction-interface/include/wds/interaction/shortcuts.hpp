#pragma once

#include "events.hpp"

#include <functional>
#include <string>
#include <unordered_map>

namespace wds::interaction {

struct ShortcutChord {
  KeyCode key = KeyCode::Unknown;
  Modifiers mods;

  bool operator==(const ShortcutChord& o) const noexcept {
    return key == o.key && mods == o.mods;
  }
};

struct ShortcutChordHash {
  std::size_t operator()(const ShortcutChord& c) const noexcept {
    return (static_cast<std::size_t>(c.key) * 1315423911u) ^
           (static_cast<std::size_t>(c.mods.shift) << 1) ^
           (static_cast<std::size_t>(c.mods.control) << 2) ^
           (static_cast<std::size_t>(c.mods.alt) << 3) ^
           (static_cast<std::size_t>(c.mods.super) << 4);
  }
};

using ShortcutAction = std::function<void()>;

// One chord maps to at most one action within a namespace.
class ShortcutNamespace {
 public:
  // Returns false when the chord is already bound.
  bool bind(ShortcutChord chord, ShortcutAction action);
  bool bind_primary(KeyCode key, ShortcutAction action, bool shift = false);
  void clear() noexcept;
  bool dispatch(const KeyDownEvent& event) const;

  std::size_t size() const noexcept { return bindings_.size(); }

 private:
  std::unordered_map<ShortcutChord, ShortcutAction, ShortcutChordHash> bindings_;
};

class ShortcutManager {
 public:
  ShortcutNamespace& namespace_for(const std::string& name);
  const ShortcutNamespace& namespace_for(const std::string& name) const;

  void set_active_namespace(const std::string& name);
  const std::string& active_namespace() const noexcept { return active_; }

  bool dispatch(const KeyDownEvent& event) const;

 private:
  std::unordered_map<std::string, ShortcutNamespace> namespaces_;
  std::string active_;
};

}  // namespace wds::interaction

#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace wds::interaction {

enum class PointerButton : uint8_t { Left, Right, Middle };

enum class KeyCode : int32_t {
  Unknown = -1,
  Space = 32,
  Num0 = 48,
  Num1 = 49,
  Num2 = 50,
  Num3 = 51,
  Num4 = 52,
  Num5 = 53,
  Num6 = 54,
  Num7 = 55,
  Num8 = 56,
  Num9 = 57,
  Digit0 = Num0,
  Digit1 = Num1,
  Digit2 = Num2,
  Digit3 = Num3,
  Digit4 = Num4,
  Digit5 = Num5,
  Digit6 = Num6,
  Digit7 = Num7,
  Digit8 = Num8,
  Digit9 = Num9,
  Escape = 256,
  Enter = 257,
  Tab = 258,
  Backspace = 259,
  Delete = 261,
  Left = 263,
  Right = 262,
  Up = 265,
  Down = 264,
  F1 = 290,   // matches GLFW_KEY_F1
  F2 = 291,
  F3 = 292,
  F4 = 293,
  F11 = 300,  // matches GLFW_KEY_F11
  A = 65,
  Z = 90,
};

struct Modifiers {
  bool shift = false;
  bool control = false;
  bool alt = false;
  bool super = false;

  bool any() const noexcept { return shift || control || alt || super; }

  bool operator==(const Modifiers& o) const noexcept {
    return shift == o.shift && control == o.control && alt == o.alt && super == o.super;
  }
};

struct PointerDownEvent {
  Vec2 position;
  PointerButton button = PointerButton::Left;
  Modifiers mods;
};

struct PointerUpEvent {
  Vec2 position;
  PointerButton button = PointerButton::Left;
  Modifiers mods;
};

struct PointerMoveEvent {
  Vec2 position;
  Modifiers mods;
};

struct ClickEvent {
  Vec2 position;
  PointerButton button = PointerButton::Left;
  Modifiers mods;
  int click_count = 1;
};

struct DoubleClickEvent {
  Vec2 position;
  PointerButton button = PointerButton::Left;
  Modifiers mods;
};

struct ScrollEvent {
  Vec2 position;
  float delta_x = 0.0f;
  float delta_y = 0.0f;
  Modifiers mods;
};

struct KeyDownEvent {
  KeyCode key = KeyCode::Unknown;
  Modifiers mods;
  bool repeat = false;
};

struct KeyUpEvent {
  KeyCode key = KeyCode::Unknown;
  Modifiers mods;
};

struct TextInputEvent {
  std::string text;
};

using InputEvent = std::variant<PointerDownEvent, PointerUpEvent, PointerMoveEvent, ClickEvent,
                                DoubleClickEvent, ScrollEvent, KeyDownEvent, KeyUpEvent,
                                TextInputEvent>;

class InputQueue {
 public:
  void push(InputEvent event) { events_.push_back(std::move(event)); }
  void clear() noexcept { events_.clear(); }
  bool empty() const noexcept { return events_.empty(); }

  const std::vector<InputEvent>& events() const noexcept { return events_; }
  std::vector<InputEvent>& events() noexcept { return events_; }

 private:
  std::vector<InputEvent> events_;
};

}  // namespace wds::interaction

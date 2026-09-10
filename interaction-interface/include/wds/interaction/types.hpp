#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace wds::interaction {

// Platform-independent mouse cursor shapes mapped by the active UI host.
enum class CursorKind : uint8_t {
  Default,
  ResizeHorizontal,  // note width edges
  ResizeVertical,    // hold start/end time edges
};

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;

  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}

  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;

  Rect() = default;
  Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}

  float right() const noexcept { return x + w; }
  float bottom() const noexcept { return y + h; }

  bool contains(Vec2 p) const noexcept {
    return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
  }

  Rect inset(float dx, float dy) const noexcept {
    return {x + dx, y + dy, std::max(0.0f, w - 2.0f * dx), std::max(0.0f, h - 2.0f * dy)};
  }
};

struct Color {
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;

  Color() = default;
  constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}

  Color lerp(Color to, float t) const noexcept {
    const float u = std::max(0.0f, std::min(1.0f, t));
    return {r + (to.r - r) * u, g + (to.g - g) * u, b + (to.b - b) * u,
            a + (to.a - a) * u};
  }
};

}  // namespace wds::interaction

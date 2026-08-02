#pragma once

// Neutral 2D draw primitives shared by DrawBatch, UiPainter, and chart geometry.
// No Sirius/chart domain types — safe for interaction and other non-Vulkan consumers.

namespace wds::renderer {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;

  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}

  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct Quad {
  Vec2 lb, lt, rt, rb;
};

struct StageBounds {
  float l = -1.0f;
  float r = 1.0f;
  float b = -1.0f;
  float t = 1.0f;
  float w = 2.0f;
  float h = 2.0f;
};

// Orthographic NDC-style bounds used by DrawBatch / UiPainter flush.
struct ScreenBounds {
  float aspect_ratio = 16.0f / 9.0f;
  float l = -16.0f / 9.0f;
  float r = 16.0f / 9.0f;
  float b = -1.0f;
  float t = 1.0f;
  float w = 32.0f / 9.0f;
  float h = 2.0f;
};

}  // namespace wds::renderer

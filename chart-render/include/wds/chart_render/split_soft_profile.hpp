#pragma once

#include <algorithm>
#include <cmath>

namespace wds::renderer {

// Horizontal soft-edge for preview/edit split ribbons (white plate × LineColor).
// Official look: wide gaussian skirts so the judgeline shows through; not the old
// 8-tap hard-core plate which stayed opaque across most of the width.
inline float split_soft_edge_alpha(float u01) noexcept {
  // u01 in [0,1] across ribbon width → peak at center, soft falloff to edges.
  const float x = (std::clamp(u01, 0.0f, 1.0f) - 0.5f) * 2.0f;  // -1..1
  // sigma≈0.42 in normalized half-width units → broad glow, edge α≈0.05.
  float a = std::exp(-(x * x) * 2.6f);
  a = std::pow(std::max(a, 0.0f), 0.78f);
  // Slightly under-1 peak so even full draw-opacity still reads translucent.
  return std::clamp(a * 0.90f, 0.0f, 1.0f);
}

inline constexpr int kSplitSoftPlateW = 48;

}  // namespace wds::renderer

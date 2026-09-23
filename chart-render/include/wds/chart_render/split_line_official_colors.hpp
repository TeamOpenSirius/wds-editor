#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wds::chart_render {

// Official Initialize: RGB *= settings/100; SpriteRenderer.a stays _lineColor.a.
// fade_alpha is the Hide clip (1→0). Do not fold rgb_opacity into a.
inline void apply_split_line_opacity(float& r, float& g, float& b, float& a, float rgb_opacity,
                                     float fade_alpha) noexcept {
  const float k = std::clamp(rgb_opacity, 0.0f, 1.0f);
  r *= k;
  g *= k;
  b *= k;
  a *= std::clamp(fade_alpha, 0.0f, 1.0f);
}

// Whiten LineColor toward (1,1,1), then apply SplitEffectLineOpacity.
// Destination is (k,k,k) at the tip — not leftover full-bright white.
inline void split_line_tinted_rgb(float line_r, float line_g, float line_b, float tip_t,
                                  float rgb_opacity, float& r, float& g, float& b) noexcept {
  const float k = std::clamp(rgb_opacity, 0.0f, 1.0f);
  const float t = std::clamp(tip_t, 0.0f, 1.0f);
  r = (line_r + (1.0f - line_r) * t) * k;
  g = (line_g + (1.0f - line_g) * t) * k;
  b = (line_b + (1.0f - line_b) * t) * k;
}

// Official SplitEffectElement._lineColor for Addressable SplitEffects/{id}.
// official_slot is the prefab Line index (before LineHight z=180 world X flip).
// slot_count==1 broadcasts to every line. Unknown IDs return false.
bool official_split_line_color(int32_t color_id, int32_t official_slot, float& r, float& g,
                               float& b, float& a) noexcept;

std::vector<int32_t> official_split_color_ids();

}  // namespace wds::chart_render

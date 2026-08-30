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

// Official SplitEffectElement._lineColor for Addressable SplitEffects/{id}.
// official_slot is the prefab Line index (before LineHight z=180 world X flip).
// slot_count==1 broadcasts to every line. Unknown IDs return false.
bool official_split_line_color(int32_t color_id, int32_t official_slot, float& r, float& g,
                               float& b, float& a) noexcept;

std::vector<int32_t> official_split_color_ids();

}  // namespace wds::chart_render

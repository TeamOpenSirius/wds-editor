#include <wds/chart_render/note_visual_policy.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_render {

HoldTailLayers hold_tail_layers(const wds::renderer::SkinCatalog& skin,
                                bool scratch_hold) noexcept {
  HoldTailLayers out;
  out.bottom = skin.note_bottom;
  if (scratch_hold) {
    out.top = skin.note_purple_top;
    out.is_scratch_family = true;
  } else {
    out.top = skin.note_blue_top;
    out.is_scratch_family = false;
  }
  return out;
}

std::vector<ArrowInstance> layout_static_scratch_arrows(const StaticArrowLayoutParams& p) {
  std::vector<ArrowInstance> out;
  const float aw = p.arrow_w;
  const float span = p.span_right - p.span_left;
  if (aw <= 1e-5f || span <= 1e-5f) {
    return out;
  }
  const auto sides = scratch_arrow_sides(p.scratch_length);
  const float step = (p.scratch_length == 0) ? aw * 0.9f : aw * 0.55f;
  const int max_n = std::max(1, static_cast<int>(span / std::max(step, 1e-5f)) + 1);
  const float mid = p.span_left + span * 0.5f;

  if (sides.draw_left) {
    int drawn = 0;
    for (int i = 0; i < max_n; ++i) {
      const float ax = p.span_left + static_cast<float>(i) * step;
      if (ax + aw > p.span_right + 0.5f) break;
      if (p.scratch_length == 0 && ax + aw > mid + 0.5f) break;
      out.push_back(ArrowInstance{ax, ax + aw, false, 1.0f});
      ++drawn;
    }
    if (drawn == 0) {
      out.push_back(ArrowInstance{p.span_left, p.span_left + aw, false, 1.0f});
    }
  }
  if (sides.draw_right) {
    int drawn = 0;
    for (int i = 0; i < max_n; ++i) {
      const float ax = p.span_right - aw - static_cast<float>(i) * step;
      if (ax < p.span_left - 0.5f) break;
      if (p.scratch_length == 0 && ax < mid - 0.5f) break;
      out.push_back(ArrowInstance{ax, ax + aw, true, 1.0f});
      ++drawn;
    }
    if (drawn == 0) {
      const float ax = p.span_right - aw;
      out.push_back(ArrowInstance{ax, ax + aw, true, 1.0f});
    }
  }
  return out;
}

std::vector<ArrowInstance> layout_animated_scratch_arrows(const AnimatedArrowLayoutParams& p) {
  std::vector<ArrowInstance> out;
  const float L = p.span_left;
  const float R = p.span_right;
  const float W = p.arrow_w;
  if (W <= 1e-5f || R <= L) {
    return out;
  }
  const auto sides = scratch_arrow_sides(p.scratch_length);
  // Match Sirius utils.cpp: bidirectional uses half density; directional uses full.
  const float num =
      std::max(1.0f, (p.scratch_length == 0) ? p.sonolus_num * 0.5f : p.sonolus_num);
  const float n = num;

  auto arrow_alpha = [&](float i) {
    const float phase =
        std::fmod(i + static_cast<float>(p.anim_time_sec) * p.arrow_speed, n);
    return 1.0f - 0.8f * phase / n;
  };

  if (sides.draw_left) {
    for (float i = 1.0f; i < n; i += 1.0f) {
      const float x0 = L + (i - 1.0f) * W * 0.5f;
      const float x1 = L + (i + 1.0f) * W * 0.5f;
      if (x1 > R) break;
      out.push_back(ArrowInstance{x0, x1, false, arrow_alpha(i)});
    }
  }
  if (sides.draw_right) {
    for (float i = 1.0f; i < n; i += 1.0f) {
      const float rx0 = R - (i - 1.0f) * W * 0.5f;
      const float rx1 = R - (i + 1.0f) * W * 0.5f;
      if (rx1 < L) break;
      out.push_back(ArrowInstance{rx0, rx1, true, arrow_alpha(i)});
    }
  }
  return out;
}

}  // namespace wds::chart_render

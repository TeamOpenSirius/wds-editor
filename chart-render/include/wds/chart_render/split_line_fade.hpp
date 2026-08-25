#pragma once

#include <algorithm>
#include <cmath>

namespace wds::chart_render {

// LineColor→white mix along the visible ribbon. Identity LineHight whites the tip
// (percent 0 / p0); z=180 whites the judge end (percent 1 / p1).
inline float split_line_whiten_t(float percent, float p0, float p1, float tip_span,
                                 bool judge_anchored) noexcept {
  const float span = std::max(tip_span, 1e-4f);
  float t = judge_anchored ? (1.0f - (p1 - percent) / span) : (1.0f - (percent - p0) / span);
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// SplitLine pulse peak: identity travels tip-ward (1→0); z=180 follows local +Y
// toward the judge (0→1). travel_t is the 0→1 envelope sample.
inline float split_line_pulse_peak(float travel_t, bool judge_anchored) noexcept {
  const float t = std::clamp(travel_t, 0.0f, 1.0f);
  return judge_anchored ? t : 1.0f - t;
}

}  // namespace wds::chart_render

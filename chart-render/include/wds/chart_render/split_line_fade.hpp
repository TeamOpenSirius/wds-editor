#pragma once

#include <algorithm>
#include <cmath>

namespace wds::chart_render {

// Unity AnimationCurve cubic Hermite (SplitEffect_all baked keys).
inline float sample_split_hermite(float t, const float* times, const float* values,
                                  const float* slopes, int n) noexcept {
  if (n < 2) {
    return 0.0f;
  }
  if (t <= times[0]) {
    return values[0];
  }
  if (t >= times[n - 1]) {
    return values[n - 1];
  }
  int i = 0;
  while (i + 2 < n && t > times[i + 1]) {
    ++i;
  }
  const float t0 = times[i];
  const float t1 = times[i + 1];
  const float dt = t1 - t0;
  const float u = (t - t0) / dt;
  const float u2 = u * u;
  const float u3 = u2 * u;
  const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
  const float h10 = u3 - 2.0f * u2 + u;
  const float h01 = -2.0f * u3 + 3.0f * u2;
  const float h11 = u3 - u2;
  return h00 * values[i] + h10 * dt * slopes[i] + h01 * values[i + 1] + h11 * dt * slopes[i + 1];
}

// SplitEffect_all SplitLine alpha (m_ExpressionIndex 68): 0 at spawn, peak at
// t=0.05, long tail to 0. Slope after the peak is ~0.07, so most of Height=45
// stays bright — not a smoothstep that dies by mid-span.
inline float official_split_line_vfx_alpha(float t01) noexcept {
  constexpr float kTimes[] = {0.0f, 0.05f, 1.0f};
  constexpr float kValues[] = {0.0f, 1.0f, 0.0f};
  constexpr float kSlopes[] = {25.406168f, 0.06726602f, -2.2623842f};
  return std::clamp(sample_split_hermite(t01, kTimes, kValues, kSlopes, 3), 0.0f, 1.0f);
}

// Map the official decay onto the tip blob: tip = 5% peak, inner end = death.
inline float official_split_line_vfx_tip_alpha(float u01) noexcept {
  const float u = std::clamp(u01, 0.0f, 1.0f);
  return official_split_line_vfx_alpha(0.05f + u * 0.95f);
}

// Identity: far/upward tip (p0). z=180: judge-side end (p1).
inline float split_line_whiten_t(float percent, float p0, float p1, float tip_span,
                                 bool judge_anchored) noexcept {
  const float span = std::max(tip_span, 1e-4f);
  const float u = judge_anchored ? (p1 - percent) / span : (percent - p0) / span;
  return official_split_line_vfx_tip_alpha(u);
}

// SplitLine pulse peak: identity travels tip-ward (1→0); z=180 follows local +Y
// toward the judge (0→1). travel_t is the 0→1 envelope sample.
inline float split_line_pulse_peak(float travel_t, bool judge_anchored) noexcept {
  const float t = std::clamp(travel_t, 0.0f, 1.0f);
  return judge_anchored ? t : 1.0f - t;
}

}  // namespace wds::chart_render

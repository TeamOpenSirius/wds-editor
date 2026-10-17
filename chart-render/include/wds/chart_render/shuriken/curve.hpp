#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wds::chart_render::shuriken {

// Official AnimationCurve cubic Hermite. Slopes are dy/dt.
// Unity 2022.3 Manual Editing Curves: Flat (zero slope) is a Free Smooth case,
// so a zero-slope pair eases (smoothstep), it is not linear (live dump
// orb-11791-v1: 11740_7 startSize matches Hermite). Constant tangents are
// ±Infinity: the segment keeps the left key's value.
struct HermiteKey {
  float t = 0.0f;
  float v = 0.0f;
  float in_s = 0.0f;
  float out_s = 0.0f;
};

inline float eval_hermite(const HermiteKey* keys, uint8_t count, float t01,
                          float fallback) noexcept {
  if (keys == nullptr || count == 0) {
    return fallback;
  }
  const float t = std::clamp(t01, 0.0f, 1.0f);
  if (t <= keys[0].t) {
    return keys[0].v;
  }
  if (t >= keys[count - 1].t) {
    return keys[count - 1].v;
  }
  for (uint8_t i = 1; i < count; ++i) {
    if (t > keys[i].t) {
      continue;
    }
    const HermiteKey& a = keys[i - 1];
    const HermiteKey& b = keys[i];
    const float dt = b.t - a.t;
    if (dt <= 1e-8f) {
      return b.v;
    }
    if (!std::isfinite(a.out_s) || !std::isfinite(b.in_s)) {
      return a.v;
    }
    const float u = (t - a.t) / dt;
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    const float y = h00 * a.v + h10 * (a.out_s * dt) + h01 * b.v + h11 * (b.in_s * dt);
    return std::isfinite(y) ? y : a.v + (b.v - a.v) * u;
  }
  return keys[count - 1].v;
}

// Same Hermite as eval_hermite. `hint` is the last segment index (1..count);
// start the scan there when t only advances. Restart from 1 if t went back.
inline float eval_hermite_hint(const HermiteKey* keys, uint8_t count, float t01, float fallback,
                               uint8_t& hint) noexcept {
  if (keys == nullptr || count == 0) {
    return fallback;
  }
  const float t = std::clamp(t01, 0.0f, 1.0f);
  if (t <= keys[0].t) {
    hint = 1;
    return keys[0].v;
  }
  if (t >= keys[count - 1].t) {
    hint = count;
    return keys[count - 1].v;
  }
  uint8_t i = hint;
  if (i < 1 || i >= count || t <= keys[i - 1].t) {
    i = 1;
  }
  for (; i < count; ++i) {
    if (t > keys[i].t) {
      continue;
    }
    hint = i;
    const HermiteKey& a = keys[i - 1];
    const HermiteKey& b = keys[i];
    const float dt = b.t - a.t;
    if (dt <= 1e-8f) {
      return b.v;
    }
    if (!std::isfinite(a.out_s) || !std::isfinite(b.in_s)) {
      return a.v;
    }
    const float u = (t - a.t) / dt;
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    const float y = h00 * a.v + h10 * (a.out_s * dt) + h01 * b.v + h11 * (b.in_s * dt);
    return std::isfinite(y) ? y : a.v + (b.v - a.v) * u;
  }
  hint = count;
  return keys[count - 1].v;
}

// Closed-form ∫_0^{u01} eval_hermite(s) ds on the unit curve axis. Same hold /
// broken-tangent rules as eval_hermite. u01 is clamped to [0, 1].
inline float integrate_hermite(const HermiteKey* keys, uint8_t count, float u01,
                               float fallback) noexcept {
  const float u = std::clamp(u01, 0.0f, 1.0f);
  if (u <= 0.0f) {
    return 0.0f;
  }
  if (keys == nullptr || count == 0) {
    return fallback * u;
  }
  auto hold = [](float v, float t0, float t1) noexcept { return v * (t1 - t0); };
  auto cub_u = [](const HermiteKey& a, const HermiteKey& b, float dt, float U) noexcept {
    const float U2 = U * U;
    const float U3 = U2 * U;
    const float U4 = U2 * U2;
    const float i00 = 0.5f * U4 - U3 + U;
    const float i10 = 0.25f * U4 - (2.0f / 3.0f) * U3 + 0.5f * U2;
    const float i01 = -0.5f * U4 + U3;
    const float i11 = 0.25f * U4 - (1.0f / 3.0f) * U3;
    return i00 * a.v + i10 * (a.out_s * dt) + i01 * b.v + i11 * (b.in_s * dt);
  };
  if (u <= keys[0].t) {
    return hold(keys[0].v, 0.0f, u);
  }
  float acc = 0.0f;
  if (keys[0].t > 0.0f) {
    acc += hold(keys[0].v, 0.0f, keys[0].t);
  }
  for (uint8_t i = 1; i < count; ++i) {
    const HermiteKey& a = keys[i - 1];
    const HermiteKey& b = keys[i];
    if (u <= a.t) {
      break;
    }
    const float dt = b.t - a.t;
    const float seg_end = std::min(u, b.t);
    if (dt <= 1e-8f) {
      continue;
    }
    if (!std::isfinite(a.out_s) || !std::isfinite(b.in_s)) {
      acc += hold(a.v, a.t, seg_end);
    } else {
      const float U = (seg_end - a.t) / dt;
      acc += dt * cub_u(a, b, dt, U);
    }
    if (u <= b.t) {
      return acc;
    }
  }
  const float last_t = keys[count - 1].t;
  if (u > last_t) {
    acc += hold(keys[count - 1].v, last_t, u);
  }
  return acc;
}

}  // namespace wds::chart_render::shuriken

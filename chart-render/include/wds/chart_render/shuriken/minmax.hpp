#pragma once

#include <wds/chart_render/shuriken/curve.hpp>

#include <cstdint>

namespace wds::chart_render::shuriken {

// Unity ParticleSystemMinMaxCurve.minMaxState
inline constexpr uint8_t kMinMaxConstant = 0;
inline constexpr uint8_t kMinMaxCurve = 1;
inline constexpr uint8_t kMinMaxTwoCurves = 2;
inline constexpr uint8_t kMinMaxTwoConstants = 3;

inline float pick_two_constants(float scalar, float min_scalar, uint8_t state,
                                float u01) noexcept {
  if (state == kMinMaxTwoConstants) {
    return min_scalar + (scalar - min_scalar) * u01;
  }
  return scalar;
}

// Keys already have scalar baked into v / slopes. Constant (0) ignores minScalar.
inline float eval_minmax(const HermiteKey* max_keys, uint8_t max_count, float scalar,
                         const HermiteKey* min_keys, uint8_t min_count, float min_scalar,
                         uint8_t state, float t01, float u01) noexcept {
  if (state == kMinMaxTwoCurves) {
    const float hi = eval_hermite(max_keys, max_count, t01, scalar);
    const float lo = eval_hermite(min_keys, min_count, t01, min_scalar);
    return lo + (hi - lo) * u01;
  }
  if (state == kMinMaxCurve) {
    return eval_hermite(max_keys, max_count, t01, scalar);
  }
  if (state == kMinMaxTwoConstants) {
    return min_scalar + (scalar - min_scalar) * u01;
  }
  return scalar;
}

}  // namespace wds::chart_render::shuriken

#pragma once

namespace wds::chart_editor {

enum class EasingAlgorithm {
  Linear,
  Poly,
  Exp,
  Sine,
};

enum class EasingDirection {
  In,
  Out,
  InOut,
  OutIn,
};

// Ease normalized t to [0, 1]. Finite parameter p is clamped to [0, 20];
// non-finite p becomes 0. Poly(p) is pow(t, 1+p); Exp(p) is expm1(p*t)/expm1(p);
// p==0 is an exact linear path for Poly and Exp. Input/output are clamped to
// [0, 1]. Every variant preserves endpoints and is monotonic.
//
// Sine In is 1 - cos(t * π/2) and ignores p. Directions remap that In function f:
//   Out    1 - f(1 - t)
//   InOut  t < 0.5 ? f(2t)/2 : 1 - f(2 - 2t)/2
//   OutIn  t < 0.5 ? (1 - f(1 - 2t))/2 : 1/2 + f(2t - 1)/2
// Linear is the identity in every direction.
double apply_easing(double t, EasingAlgorithm algorithm, EasingDirection direction,
                    double parameter = 0.0) noexcept;

}  // namespace wds::chart_editor

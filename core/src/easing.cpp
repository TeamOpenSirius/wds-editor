#include <wds/core/easing.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {
namespace {

double sanitize_parameter(double p) noexcept {
  if (!std::isfinite(p)) return 0.0;
  return std::clamp(p, 0.0, 20.0);
}

double sanitize_normalized(double t) noexcept {
  if (!std::isfinite(t)) return 0.0;
  return std::clamp(t, 0.0, 1.0);
}

double ease_in(double t, EasingAlgorithm algorithm, double parameter) noexcept {
  if (t <= 0.0) return 0.0;
  if (t >= 1.0) return 1.0;
  switch (algorithm) {
    case EasingAlgorithm::Linear:
      return t;
    case EasingAlgorithm::Poly:
      if (parameter == 0.0) return t;
      return std::pow(t, 1.0 + parameter);
    case EasingAlgorithm::Exp:
      if (parameter == 0.0) return t;
      return std::expm1(parameter * t) / std::expm1(parameter);
    case EasingAlgorithm::Sine:
      return 1.0 - std::cos(t * std::acos(-1.0) * 0.5);
  }
  return t;
}

double apply_direction(double t, EasingDirection direction,
                       EasingAlgorithm algorithm, double parameter) noexcept {
  const auto in = [&](double u) { return ease_in(u, algorithm, parameter); };
  switch (direction) {
    case EasingDirection::In:
      return in(t);
    case EasingDirection::Out:
      return 1.0 - in(1.0 - t);
    case EasingDirection::InOut:
      if (t < 0.5) return 0.5 * in(2.0 * t);
      return 1.0 - 0.5 * in(2.0 - 2.0 * t);
    case EasingDirection::OutIn:
      if (t < 0.5) return 0.5 * (1.0 - in(1.0 - 2.0 * t));
      return 0.5 + 0.5 * in(2.0 * t - 1.0);
  }
  return t;
}

}  // namespace

double apply_easing(double t, EasingAlgorithm algorithm, EasingDirection direction,
                    double parameter) noexcept {
  t = sanitize_normalized(t);
  parameter = sanitize_parameter(parameter);
  if (t <= 0.0) return 0.0;
  if (t >= 1.0) return 1.0;
  if (algorithm == EasingAlgorithm::Linear) return t;
  return sanitize_normalized(apply_direction(t, direction, algorithm, parameter));
}

}  // namespace wds::chart_editor

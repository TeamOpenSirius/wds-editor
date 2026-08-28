#include "wds/ui/curve_template.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace wds::ui {
namespace {

double sanitize_parameter(double p) noexcept {
  if (!std::isfinite(p)) return 0.0;
  return std::clamp(p, 0.0, 20.0);
}

wds::chart_editor::EasingAlgorithm sanitize_algorithm(
    wds::chart_editor::EasingAlgorithm algorithm) noexcept {
  switch (algorithm) {
    case wds::chart_editor::EasingAlgorithm::Linear:
    case wds::chart_editor::EasingAlgorithm::Poly:
    case wds::chart_editor::EasingAlgorithm::Exp:
    case wds::chart_editor::EasingAlgorithm::Sine:
      return algorithm;
  }
  return wds::chart_editor::EasingAlgorithm::Linear;
}

wds::chart_editor::EasingDirection sanitize_direction(
    wds::chart_editor::EasingDirection direction) noexcept {
  switch (direction) {
    case wds::chart_editor::EasingDirection::In:
    case wds::chart_editor::EasingDirection::Out:
    case wds::chart_editor::EasingDirection::InOut:
    case wds::chart_editor::EasingDirection::OutIn:
      return direction;
  }
  return wds::chart_editor::EasingDirection::In;
}

}  // namespace

std::uint64_t allocate_curve_template_id(const std::vector<CurveTemplate>& templates) {
  std::uint64_t max_id = 0;
  bool any = false;
  for (const auto& tmpl : templates) {
    if (!any || tmpl.id > max_id) {
      max_id = tmpl.id;
      any = true;
    }
  }
  if (!any || max_id == 0) return 1;
  if (max_id < std::numeric_limits<std::uint64_t>::max()) return max_id + 1;
  for (std::uint64_t candidate = 1; candidate != 0; ++candidate) {
    if (find_curve_template_by_id(templates, candidate) == nullptr) return candidate;
  }
  return 1;
}

const CurveTemplate* find_curve_template_by_id(const std::vector<CurveTemplate>& templates,
                                               std::uint64_t id) {
  for (const auto& tmpl : templates) {
    if (tmpl.id == id) return &tmpl;
  }
  return nullptr;
}

CurveTemplate* find_curve_template_by_id(std::vector<CurveTemplate>& templates, std::uint64_t id) {
  return const_cast<CurveTemplate*>(find_curve_template_by_id(
      static_cast<const std::vector<CurveTemplate>&>(templates), id));
}

void normalize_curve_template(CurveTemplate& tmpl) {
  tmpl.algorithm = sanitize_algorithm(tmpl.algorithm);
  tmpl.parameter = sanitize_parameter(tmpl.parameter);
}

void normalize_curve_config(std::vector<CurveTemplate>& templates, std::uint64_t& selected_id) {
  for (auto& tmpl : templates) {
    normalize_curve_template(tmpl);
  }
  std::unordered_set<std::uint64_t> used;
  used.insert(0);
  for (auto& tmpl : templates) {
    if (tmpl.id == 0 || used.count(tmpl.id) != 0) {
      tmpl.id = allocate_curve_template_id(templates);
    }
    used.insert(tmpl.id);
  }
  if (selected_id == 0 || find_curve_template_by_id(templates, selected_id) == nullptr) {
    selected_id = 0;
  }
}

ResolvedCurveEasing resolve_selected_curve_easing(
    const std::vector<CurveTemplate>& templates, std::uint64_t selected_id,
    wds::chart_editor::EasingDirection selected_direction) {
  ResolvedCurveEasing resolved;
  resolved.direction = sanitize_direction(selected_direction);
  if (selected_id == 0) return resolved;
  const CurveTemplate* tmpl = find_curve_template_by_id(templates, selected_id);
  if (tmpl == nullptr) return resolved;
  resolved.algorithm = sanitize_algorithm(tmpl->algorithm);
  resolved.parameter = sanitize_parameter(tmpl->parameter);
  return resolved;
}

}  // namespace wds::ui

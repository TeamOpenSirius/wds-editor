#pragma once

#include <wds/core/easing.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wds::ui {

// Hard cap on persisted / live curve templates. Enough for real preset use;
// larger `curve_template_count` or indexed keys with index >= this value are
// truncated or ignored so load cannot resize/map without bound.
inline constexpr std::size_t kMaxCurveTemplates = 64;

// Stable nonzero ID; 0 is reserved for empty selection. Duplicate names are allowed.
struct CurveTemplate {
  std::uint64_t id = 0;
  std::string name;
  wds::chart_editor::EasingAlgorithm algorithm = wds::chart_editor::EasingAlgorithm::Linear;
  double parameter = 0.0;
};

struct ResolvedCurveEasing {
  wds::chart_editor::EasingAlgorithm algorithm = wds::chart_editor::EasingAlgorithm::Linear;
  wds::chart_editor::EasingDirection direction = wds::chart_editor::EasingDirection::In;
  double parameter = 0.0;
};

// Next stable ID: one past the current max, or 1 when the list is empty.
std::uint64_t allocate_curve_template_id(const std::vector<CurveTemplate>& templates);

const CurveTemplate* find_curve_template_by_id(const std::vector<CurveTemplate>& templates,
                                               std::uint64_t id);
CurveTemplate* find_curve_template_by_id(std::vector<CurveTemplate>& templates, std::uint64_t id);

// Clamp/sanitize a single template parameter (and algorithm fallback).
void normalize_curve_template(CurveTemplate& tmpl);

// Repair zero/duplicate IDs in vector order and drop a missing/invalid selection to 0.
void normalize_curve_config(std::vector<CurveTemplate>& templates, std::uint64_t& selected_id);

// Empty or unknown selection resolves to Linear with parameter 0; direction is preserved.
ResolvedCurveEasing resolve_selected_curve_easing(
    const std::vector<CurveTemplate>& templates, std::uint64_t selected_id,
    wds::chart_editor::EasingDirection selected_direction);

// Live curve-fill state for UiManager / later dialog and toolbar. Ordinary
// settings saves must copy this onto EditorUiConfig so templates are not wiped.
struct CurveTemplateUiState {
  std::vector<CurveTemplate> templates;
  std::uint64_t selected_id = 0;
  wds::chart_editor::EasingDirection direction = wds::chart_editor::EasingDirection::In;
};

}  // namespace wds::ui

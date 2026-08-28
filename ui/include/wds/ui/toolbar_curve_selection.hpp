#pragma once

#include "wds/ui/curve_template.hpp"

#include <wds/interaction/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wds::ui {

enum class EditorToolbarAction {
  Open,
  Save,
  Import,
  Export,
  Music,
  CurveTemplates,
  Check,
  Settings,
};

inline constexpr std::size_t kEditorToolbarActionCount = 8;

inline constexpr std::array<EditorToolbarAction, kEditorToolbarActionCount>
    kEditorToolbarActionOrder{{
        EditorToolbarAction::Open,
        EditorToolbarAction::Save,
        EditorToolbarAction::Import,
        EditorToolbarAction::Export,
        EditorToolbarAction::Music,
        EditorToolbarAction::CurveTemplates,
        EditorToolbarAction::Check,
        EditorToolbarAction::Settings,
    }};

inline constexpr std::array<const char*, kEditorToolbarActionCount> kEditorToolbarIconStems{{
    "open",
    "save",
    "import",
    "export",
    "import-audio",
    "edit_curve",
    "check",
    "settings",
}};

inline constexpr std::array<const char*, kEditorToolbarActionCount> kEditorToolbarTooltips{{
    "打开工程",
    "保存工程",
    "导入谱面（只读）",
    "导出谱面",
    "导入音乐",
    "编辑曲线模板",
    "检查谱面错误",
    "编辑器设置",
}};

inline constexpr const char* kEmptyCurveTemplateLabel = "（空）";

inline constexpr std::array<const char*, 4> kCurveDirectionLabels{{"I", "O", "IO", "OI"}};

inline constexpr std::array<wds::chart_editor::EasingDirection, 4> kCurveDirectionValues{{
    wds::chart_editor::EasingDirection::In,
    wds::chart_editor::EasingDirection::Out,
    wds::chart_editor::EasingDirection::InOut,
    wds::chart_editor::EasingDirection::OutIn,
}};

struct CurveFillSelection {
  std::uint64_t template_id = 0;
  ResolvedCurveEasing easing;
};

CurveFillSelection make_curve_fill_selection(const CurveTemplateUiState& live);

std::vector<std::string> curve_template_dropdown_labels(
    const std::vector<CurveTemplate>& templates);

int dropdown_index_for_curve_id(const std::vector<CurveTemplate>& templates, std::uint64_t id);
std::uint64_t curve_id_for_dropdown_index(const std::vector<CurveTemplate>& templates, int index);

int index_for_curve_direction(wds::chart_editor::EasingDirection direction);
wds::chart_editor::EasingDirection curve_direction_from_index(int index);

inline constexpr float kToolbarMinFieldW = 32.0f;
// Supported narrow left-column toolbar (review evidence width).
inline constexpr float kToolbarSupportedNarrowWidth = 576.0f;
// Typical left-column width (e.g. 1600×0.45 or a mid-size window).
inline constexpr float kToolbarTypicalWidth = 800.0f;

float toolbar_min_usable_width() noexcept;
float toolbar_checkbox_required_width(const std::string& label) noexcept;
// Glyph width at the live Button/Dropdown paint size (`kFontSizeMd`). Falls
// back to 1em-per-codepoint when the UI atlas is not baked (unit tests).
float toolbar_painted_label_width(const std::string& text) noexcept;
float toolbar_dropdown_chrome_width() noexcept;
float toolbar_direction_button_min_width() noexcept;
float toolbar_empty_dropdown_min_width() noexcept;

struct ToolbarControlLayout {
  float pad = 0.0f;
  float gap = 0.0f;
  float col_x[3]{};
  float col_w[3]{};
  float label_w = 0.0f;
  float cluster_w = 0.0f;
  float step_w = 0.0f;
  float field_in_step = 0.0f;
  float field_in_chart = 0.0f;
};

struct ToolbarCheckboxLayout {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float w0 = 0.0f;
  float h0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float w1 = 0.0f;
  float h1 = 0.0f;
  bool stacked = false;
};

struct ToolbarCurveRowLayout {
  wds::interaction::Rect label{};
  wds::interaction::Rect dropdown{};
  std::array<wds::interaction::Rect, 4> dirs{};
};

struct ToolbarControlVerticalLayout {
  float icon = 0.0f;
  float icon_block_h = 0.0f;
  float slot = 0.0f;
  float y_delay = 0.0f;
  float y_range = 0.0f;
  float y_curve = 0.0f;
  float y_checkbox = 0.0f;
  int rows = 4;
};

bool toolbar_checkboxes_need_stack(const ToolbarControlLayout& cols, float measured0,
                                   float measured1) noexcept;
ToolbarControlLayout compute_toolbar_control_layout(float toolbar_w) noexcept;
ToolbarCheckboxLayout compute_toolbar_checkbox_layout(const ToolbarControlLayout& cols,
                                                      float measured0, float measured1, float y,
                                                      float row_h) noexcept;
ToolbarCurveRowLayout compute_toolbar_curve_row_layout(const ToolbarControlLayout& cols, float y,
                                                       float row_h) noexcept;
ToolbarControlVerticalLayout compute_toolbar_control_vertical(float toolbar_w, float toolbar_h,
                                                              bool stacked) noexcept;
wds::interaction::Rect toolbar_action_icon_rect(float toolbar_w, std::size_t index) noexcept;

class CurveToolbarController {
 public:
  using ChangeHandler = std::function<void(const CurveFillSelection&)>;

  void set_on_changed(ChangeHandler handler) { on_changed_ = std::move(handler); }

  // Rebuilds dropdown/direction view from live state. Missing IDs become 0 /
  // Linear. Does not invoke on_changed.
  void refresh_from(CurveTemplateUiState& live);

  const std::vector<std::string>& dropdown_labels() const noexcept { return labels_; }
  int selected_dropdown_index() const noexcept { return dropdown_index_; }
  int selected_direction_index() const noexcept { return direction_index_; }
  CurveFillSelection selection() const noexcept { return selection_; }

  // User gestures. Mutate live and invoke on_changed once when state changes.
  bool select_dropdown_index(CurveTemplateUiState& live, int index);
  bool select_direction_index(CurveTemplateUiState& live, int index);

 private:
  void sync_view(const CurveTemplateUiState& live);
  void publish_if_changed(const CurveTemplateUiState& live, bool emit);

  ChangeHandler on_changed_;
  std::vector<std::string> labels_;
  int dropdown_index_ = 0;
  int direction_index_ = 0;
  CurveFillSelection selection_{};
};

}  // namespace wds::ui

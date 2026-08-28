#include "wds/ui/editor_session.hpp"
#include "wds/ui/layout/editor_layout.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/toolbar/editor_toolbar.hpp"
#include "wds/ui/toolbar_curve_selection.hpp"

#include <wds/interaction/events.hpp>
#include <wds/interaction/font_atlas.hpp>
#include <wds/interaction/theme.hpp>
#include <wds/interaction/types.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/checkbox.hpp>
#include <wds/interaction/widgets/dropdown.hpp>
#include <wds/interaction/widgets/icon_button.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __FILE__, __LINE__)

using wds::chart_editor::EasingAlgorithm;
using wds::chart_editor::EasingDirection;
using wds::ui::CurveFillSelection;
using wds::ui::CurveTemplate;
using wds::ui::CurveTemplateUiState;
using wds::ui::CurveToolbarController;
using wds::ui::EditorToolbar;
using wds::ui::EditorToolbarAction;
using wds::ui::compute_toolbar_checkbox_layout;
using wds::ui::compute_toolbar_control_layout;
using wds::ui::compute_toolbar_control_vertical;
using wds::ui::compute_toolbar_curve_row_layout;
using wds::ui::curve_direction_from_index;
using wds::ui::curve_id_for_dropdown_index;
using wds::ui::curve_template_dropdown_labels;
using wds::ui::dropdown_index_for_curve_id;
using wds::ui::index_for_curve_direction;
using wds::ui::kCurveDirectionLabels;
using wds::ui::kCurveDirectionValues;
using wds::ui::kEditorToolbarActionCount;
using wds::ui::kEditorToolbarActionOrder;
using wds::ui::kEditorToolbarIconStems;
using wds::ui::kEditorToolbarTooltips;
using wds::ui::kEmptyCurveTemplateLabel;
using wds::ui::kToolbarMinFieldW;
using wds::ui::kToolbarSupportedNarrowWidth;
using wds::ui::kToolbarTypicalWidth;
using wds::ui::make_curve_fill_selection;
using wds::ui::toolbar_action_icon_rect;
using wds::ui::toolbar_checkbox_required_width;
using wds::ui::toolbar_checkboxes_need_stack;
using wds::ui::toolbar_min_usable_width;

CurveTemplate make_template(std::uint64_t id, std::string name, EasingAlgorithm algorithm,
                            double parameter) {
  CurveTemplate tmpl;
  tmpl.id = id;
  tmpl.name = std::move(name);
  tmpl.algorithm = algorithm;
  tmpl.parameter = parameter;
  return tmpl;
}

void test_toolbar_action_order_has_no_undo_redo() {
  CHECK_EQ(kEditorToolbarActionOrder.size(), kEditorToolbarActionCount);
  CHECK(kEditorToolbarActionOrder[0] == EditorToolbarAction::Open);
  CHECK(kEditorToolbarActionOrder[1] == EditorToolbarAction::Save);
  CHECK(kEditorToolbarActionOrder[2] == EditorToolbarAction::Import);
  CHECK(kEditorToolbarActionOrder[3] == EditorToolbarAction::Export);
  CHECK(kEditorToolbarActionOrder[4] == EditorToolbarAction::Music);
  CHECK(kEditorToolbarActionOrder[5] == EditorToolbarAction::CurveTemplates);
  CHECK(kEditorToolbarActionOrder[6] == EditorToolbarAction::Check);
  CHECK(kEditorToolbarActionOrder[7] == EditorToolbarAction::Settings);
  CHECK_EQ(std::string(kEditorToolbarIconStems[5]), std::string("edit_curve"));
  CHECK_EQ(std::string(kEditorToolbarIconStems[6]), std::string("check"));
  for (const char* stem : kEditorToolbarIconStems) {
    CHECK(std::string(stem) != "undo");
    CHECK(std::string(stem) != "redo");
  }
}

void click_widget_center(wds::interaction::Widget& host, const wds::interaction::Rect& cell) {
  const wds::interaction::Vec2 p{cell.x + cell.w * 0.5f, cell.y + cell.h * 0.5f};
  wds::interaction::Widget* hit = host.hit_test(p);
  CHECK(hit != nullptr);
  CHECK(hit != &host);
  if (hit != nullptr) {
    hit->on_click(wds::interaction::ClickEvent{p, wds::interaction::PointerButton::Left, {}, 1});
  }
}

void test_curve_and_check_actions_dispatch_from_toolbar_hits() {
  wds::ui::ChartPreviewPanel preview;
  wds::ui::EditorSession session(preview);
  wds::ui::ChartEditPanel edit(session.engine());
  EditorToolbar toolbar(session, edit);

  int curve_count = 0;
  int check_count = 0;
  int other = 0;
  toolbar.set_open_handler([&] { ++other; });
  toolbar.set_import_handler([&] { ++other; });
  toolbar.set_export_handler([&] { ++other; });
  toolbar.set_settings_handler([&] { ++other; });
  toolbar.set_curve_templates_handler([&] { ++curve_count; });
  toolbar.set_check_handler([&] { ++check_count; });

  constexpr float kW = kToolbarSupportedNarrowWidth;
  constexpr float kH = 360.0f;
  toolbar.set_bounds({0.0f, 0.0f, kW, kH});
  toolbar.layout({0.0f, 0.0f, kW, kH});

  CHECK(toolbar.children().size() >= 8);
  for (std::size_t i = 0; i < kEditorToolbarActionCount; ++i) {
    CHECK_EQ(toolbar.children()[i]->tooltip(), std::string(kEditorToolbarTooltips[i]));
    const auto expected = toolbar_action_icon_rect(kW, i);
    const auto actual = toolbar.children()[i]->bounds();
    CHECK(std::fabs(actual.x - expected.x) < 0.5f);
    CHECK(std::fabs(actual.y - expected.y) < 0.5f);
    CHECK(std::fabs(actual.w - expected.w) < 0.5f);
    CHECK(std::fabs(actual.h - expected.h) < 0.5f);
  }

  click_widget_center(toolbar, toolbar_action_icon_rect(kW, 5));
  CHECK_EQ(curve_count, 1);
  CHECK_EQ(check_count, 0);
  CHECK_EQ(other, 0);

  click_widget_center(toolbar, toolbar_action_icon_rect(kW, 6));
  CHECK_EQ(curve_count, 1);
  CHECK_EQ(check_count, 1);
  CHECK_EQ(other, 0);

  click_widget_center(toolbar, toolbar_action_icon_rect(kW, 0));
  CHECK_EQ(curve_count, 1);
  CHECK_EQ(check_count, 1);
  CHECK_EQ(other, 1);
}

void test_dropdown_empty_item_and_stable_id_with_duplicates() {
  const std::vector<CurveTemplate> templates = {
      make_template(4, "dup", EasingAlgorithm::Poly, 1.5),
      make_template(7, "dup", EasingAlgorithm::Exp, 3.0),
      make_template(9, "other", EasingAlgorithm::Sine, 0.0),
  };
  const std::vector<std::string> labels = curve_template_dropdown_labels(templates);
  CHECK_EQ(labels.size(), static_cast<std::size_t>(3));
  if (labels.size() == 3) {
    CHECK_EQ(labels[0], std::string("dup"));
    CHECK_EQ(labels[1], std::string("dup"));
    CHECK_EQ(labels[2], std::string("other"));
  }

  CHECK_EQ(dropdown_index_for_curve_id(templates, 0), -1);
  CHECK_EQ(dropdown_index_for_curve_id(templates, 4), 0);
  CHECK_EQ(dropdown_index_for_curve_id(templates, 7), 1);
  CHECK_EQ(dropdown_index_for_curve_id(templates, 9), 2);
  CHECK_EQ(dropdown_index_for_curve_id(templates, 99), -1);

  CHECK_EQ(curve_id_for_dropdown_index(templates, -1), static_cast<std::uint64_t>(0));
  CHECK_EQ(curve_id_for_dropdown_index(templates, 0), static_cast<std::uint64_t>(4));
  CHECK_EQ(curve_id_for_dropdown_index(templates, 1), static_cast<std::uint64_t>(7));
  CHECK_EQ(curve_id_for_dropdown_index(templates, 2), static_cast<std::uint64_t>(9));
  CHECK_EQ(curve_id_for_dropdown_index(templates, 8), static_cast<std::uint64_t>(0));
}

void test_deleted_id_falls_back_to_empty_linear() {
  CurveTemplateUiState live;
  live.templates = {make_template(2, "keep", EasingAlgorithm::Poly, 4.0)};
  live.selected_id = 99;
  live.direction = EasingDirection::OutIn;

  CurveToolbarController controller;
  int calls = 0;
  controller.set_on_changed([&](const CurveFillSelection&) { ++calls; });
  controller.refresh_from(live);

  CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(0));
  CHECK_EQ(controller.selected_dropdown_index(), -1);
  CHECK_EQ(controller.dropdown_labels().size(), static_cast<std::size_t>(1));
  CHECK_EQ(controller.dropdown_labels()[0], std::string("keep"));
  CHECK(controller.selection().easing.algorithm == EasingAlgorithm::Linear);
  CHECK(controller.selection().easing.direction == EasingDirection::OutIn);
  CHECK_EQ(calls, 0);

  const CurveFillSelection resolved = make_curve_fill_selection(live);
  CHECK_EQ(resolved.template_id, static_cast<std::uint64_t>(0));
  CHECK(resolved.easing.algorithm == EasingAlgorithm::Linear);
}

void test_four_direction_mappings() {
  CHECK_EQ(std::string(kCurveDirectionLabels[0]), std::string("I"));
  CHECK_EQ(std::string(kCurveDirectionLabels[1]), std::string("O"));
  CHECK_EQ(std::string(kCurveDirectionLabels[2]), std::string("IO"));
  CHECK_EQ(std::string(kCurveDirectionLabels[3]), std::string("OI"));
  CHECK(kCurveDirectionValues[0] == EasingDirection::In);
  CHECK(kCurveDirectionValues[1] == EasingDirection::Out);
  CHECK(kCurveDirectionValues[2] == EasingDirection::InOut);
  CHECK(kCurveDirectionValues[3] == EasingDirection::OutIn);
  CHECK_EQ(index_for_curve_direction(EasingDirection::In), 0);
  CHECK_EQ(index_for_curve_direction(EasingDirection::Out), 1);
  CHECK_EQ(index_for_curve_direction(EasingDirection::InOut), 2);
  CHECK_EQ(index_for_curve_direction(EasingDirection::OutIn), 3);
  CHECK(curve_direction_from_index(0) == EasingDirection::In);
  CHECK(curve_direction_from_index(1) == EasingDirection::Out);
  CHECK(curve_direction_from_index(2) == EasingDirection::InOut);
  CHECK(curve_direction_from_index(3) == EasingDirection::OutIn);
  CHECK(curve_direction_from_index(99) == EasingDirection::In);
}

void test_programmatic_refresh_emits_no_callback() {
  CurveTemplateUiState live;
  live.templates = {
      make_template(1, "a", EasingAlgorithm::Poly, 2.0),
      make_template(2, "b", EasingAlgorithm::Exp, 3.0),
  };
  live.selected_id = 2;
  live.direction = EasingDirection::Out;

  CurveToolbarController controller;
  int calls = 0;
  controller.set_on_changed([&](const CurveFillSelection&) { ++calls; });
  controller.refresh_from(live);
  CHECK_EQ(calls, 0);
  CHECK_EQ(controller.selected_dropdown_index(), 1);
  CHECK(controller.selection().easing.algorithm == EasingAlgorithm::Exp);

  live.templates[1].parameter = 5.0;
  controller.refresh_from(live);
  CHECK_EQ(calls, 0);
  CHECK_EQ(controller.selection().easing.parameter, 5.0);
}

void test_one_user_change_emits_one_callback() {
  CurveTemplateUiState live;
  live.templates = {
      make_template(1, "dup", EasingAlgorithm::Poly, 2.0),
      make_template(2, "dup", EasingAlgorithm::Exp, 3.0),
  };
  live.selected_id = 0;
  live.direction = EasingDirection::In;

  CurveToolbarController controller;
  int calls = 0;
  CurveFillSelection last;
  controller.set_on_changed([&](const CurveFillSelection& sel) {
    ++calls;
    last = sel;
  });
  controller.refresh_from(live);
  CHECK_EQ(calls, 0);

  CHECK(controller.select_dropdown_index(live, 1));
  CHECK_EQ(calls, 1);
  CHECK_EQ(live.selected_id, static_cast<std::uint64_t>(2));
  CHECK_EQ(last.template_id, static_cast<std::uint64_t>(2));
  CHECK(last.easing.algorithm == EasingAlgorithm::Exp);
  CHECK(last.easing.direction == EasingDirection::In);

  CHECK(!controller.select_dropdown_index(live, 1));
  CHECK_EQ(calls, 1);

  CHECK(controller.select_direction_index(live, 2));
  CHECK_EQ(calls, 2);
  CHECK(live.direction == EasingDirection::InOut);
  CHECK(last.easing.direction == EasingDirection::InOut);
  CHECK_EQ(last.template_id, static_cast<std::uint64_t>(2));

  CHECK(!controller.select_direction_index(live, 2));
  CHECK_EQ(calls, 2);

  controller.refresh_from(live);
  CHECK_EQ(calls, 2);
}

std::size_t utf8_count(const std::string& text) noexcept {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if ((lead & 0x80u) == 0) {
      i += 1;
    } else if ((lead & 0xE0u) == 0xC0u) {
      i += 2;
    } else if ((lead & 0xF0u) == 0xE0u) {
      i += 3;
    } else if ((lead & 0xF8u) == 0xF0u) {
      i += 4;
    } else {
      i += 1;
    }
    ++count;
  }
  return count;
}

float painted_label_width(const std::string& text) {
  namespace th = wds::interaction::theme;
  const float fallback = static_cast<float>(utf8_count(text)) * th::kFontSizeMd;
  if (wds::interaction::FontAtlas::instance().atlas_width() > 0) {
    wds::interaction::UiPainter painter;
    const float measured = painter.measure_text(text, th::kFontSizeMd).x;
    if (measured > 0.0f) return measured;
  }
  return fallback;
}

float dropdown_chrome_width() {
  namespace th = wds::interaction::theme;
  return 4.0f + std::max(18.0f, th::px(11.0f));
}

wds::interaction::Dropdown* toolbar_curve_dropdown(EditorToolbar& toolbar) {
  wds::interaction::Dropdown* second = nullptr;
  int seen = 0;
  for (const auto& child : toolbar.children()) {
    auto* dropdown = dynamic_cast<wds::interaction::Dropdown*>(child.get());
    if (dropdown == nullptr) continue;
    ++seen;
    if (seen == 2) second = dropdown;
  }
  return second;
}

std::array<wds::interaction::Button*, 4> toolbar_direction_buttons(EditorToolbar& toolbar) {
  std::array<wds::interaction::Button*, 4> out{};
  for (const auto& child : toolbar.children()) {
    auto* button = dynamic_cast<wds::interaction::Button*>(child.get());
    if (button == nullptr) continue;
    if (button->label() == "I") out[0] = button;
    if (button->label() == "O") out[1] = button;
    if (button->label() == "IO") out[2] = button;
    if (button->label() == "OI") out[3] = button;
  }
  return out;
}

bool rects_overlap(const wds::interaction::Rect& a, const wds::interaction::Rect& b) {
  constexpr float kEps = 0.01f;
  return a.x + a.w > b.x + kEps && b.x + b.w > a.x + kEps && a.y + a.h > b.y + kEps &&
         b.y + b.h > a.y + kEps;
}

bool rect_in_span(const wds::interaction::Rect& r, float left, float right) {
  return r.x + 0.01f >= left && r.x + r.w <= right + 0.01f;
}

struct ToolbarGroupMetrics {
  float col0 = 0.0f;
  float col1 = 0.0f;
  float group_w = 0.0f;
  float label_w = 0.0f;
  float cluster_w = 0.0f;
};

ToolbarGroupMetrics toolbar_group_metrics(const wds::ui::ToolbarControlLayout& cols) {
  ToolbarGroupMetrics g;
  g.label_w = cols.label_w;
  g.cluster_w = cols.cluster_w;
  g.group_w = cols.label_w + cols.cluster_w;
  g.col0 = cols.col_x[0] + std::max(0.0f, (cols.col_w[0] - g.group_w) * 0.5f);
  g.col1 = cols.col_x[1] + std::max(0.0f, (cols.col_w[1] - g.group_w) * 0.5f);
  return g;
}

bool checkbox_centered_placement_needs_shift(const wds::ui::ToolbarControlLayout& cols, float m0,
                                             float m1) {
  const auto g = toolbar_group_metrics(cols);
  const float c0 = g.col0 + g.group_w * 0.5f;
  const float c1 = g.col1 + g.group_w * 0.5f;
  const float l0 = c0 - m0 * 0.5f;
  const float r0 = c0 + m0 * 0.5f;
  const float l1 = c1 - m1 * 0.5f;
  const float r1 = c1 + m1 * 0.5f;
  if (r0 + cols.gap > l1 + 0.01f) return true;
  if (l0 < cols.pad - 0.01f) return true;
  if (r1 > cols.col_x[2] + 0.01f) return true;
  return false;
}

void assert_curve_row_layout_matches_groups(const wds::ui::ToolbarControlLayout& cols, float y) {
  namespace th = wds::interaction::theme;
  const auto g = toolbar_group_metrics(cols);
  const auto row = compute_toolbar_curve_row_layout(cols, y, th::kControlHeight);
  CHECK(std::fabs(row.label.x - g.col0) < 0.5f);
  CHECK(std::fabs(row.label.w - g.label_w) < 0.5f);
  CHECK(std::fabs(row.label.y - y) < 0.5f);
  CHECK(std::fabs(row.label.h - th::kControlHeight) < 0.5f);
  CHECK(std::fabs(row.dropdown.x - (g.col0 + g.label_w)) < 0.5f);
  CHECK(std::fabs(row.dropdown.w - g.cluster_w) < 0.5f);
  CHECK(std::fabs(row.dropdown.y - y) < 0.5f);
  CHECK(std::fabs(row.dropdown.h - th::kControlHeight) < 0.5f);
  CHECK(std::fabs(row.dirs[0].x - g.col1) < 0.5f);
  CHECK(std::fabs(row.dirs[3].x + row.dirs[3].w - (g.col1 + g.group_w)) < 0.5f);
  const float dir_w = row.dirs[0].w;
  for (const auto& dir : row.dirs) {
    CHECK(std::fabs(dir.w - dir_w) < 0.5f);
    CHECK(std::fabs(dir.y - y) < 0.5f);
    CHECK(std::fabs(dir.h - th::kControlHeight) < 0.5f);
  }
}

void assert_checkbox_full_required_width(const wds::ui::ToolbarControlLayout& cols,
                                         const wds::ui::ToolbarCheckboxLayout& boxes,
                                         float measured0, float measured1) {
  namespace th = wds::interaction::theme;
  CHECK(std::fabs(boxes.w0 - measured0) < 0.5f);
  CHECK(std::fabs(boxes.w1 - measured1) < 0.5f);
  CHECK(std::fabs(boxes.y0 - boxes.y1) < 0.5f);
  CHECK(!rects_overlap({boxes.x0, boxes.y0, boxes.w0, boxes.h0},
                       {boxes.x1, boxes.y1, boxes.w1, boxes.h1}));
  CHECK(boxes.x0 + boxes.w0 + th::kUiGap <= boxes.x1 + 0.01f);
  const auto g = toolbar_group_metrics(cols);
  const float c0 = g.col0 + g.group_w * 0.5f;
  const float c1 = g.col1 + g.group_w * 0.5f;
  if (!checkbox_centered_placement_needs_shift(cols, measured0, measured1)) {
    CHECK(std::fabs((boxes.x0 + boxes.w0 * 0.5f) - c0) < 4.0f);
    CHECK(std::fabs((boxes.x1 + boxes.w1 * 0.5f) - c1) < 4.0f);
  } else {
    CHECK((boxes.x0 + boxes.w0 * 0.5f) < (boxes.x1 + boxes.w1 * 0.5f));
  }
  const float avail = cols.col_x[2] - cols.pad;
  if (measured0 + measured1 + cols.gap <= avail + 0.01f) {
    CHECK(rect_in_span({boxes.x0, boxes.y0, boxes.w0, boxes.h0}, cols.pad, cols.col_x[2]));
    CHECK(rect_in_span({boxes.x1, boxes.y1, boxes.w1, boxes.h1}, cols.pad, cols.col_x[2]));
  }
}

void assert_checkbox_helper(float toolbar_w, float measured0, float measured1, bool expect_stack) {
  namespace th = wds::interaction::theme;
  const auto cols = compute_toolbar_control_layout(toolbar_w);
  CHECK(toolbar_checkboxes_need_stack(cols, measured0, measured1) == expect_stack);
  const auto boxes =
      compute_toolbar_checkbox_layout(cols, measured0, measured1, 100.0f, th::kControlHeight);
  CHECK(boxes.stacked == expect_stack);
  CHECK(boxes.h0 + 0.01f >= th::kControlHeight);
  CHECK(boxes.h1 + 0.01f >= th::kControlHeight);
  CHECK(!rects_overlap({boxes.x0, boxes.y0, boxes.w0, boxes.h0},
                       {boxes.x1, boxes.y1, boxes.w1, boxes.h1}));
  if (!expect_stack) {
    assert_checkbox_full_required_width(cols, boxes, measured0, measured1);
  }
}

void assert_live_toolbar_no_overlap(float toolbar_w) {
  const auto cols = compute_toolbar_control_layout(toolbar_w);
  wds::ui::ChartPreviewPanel preview;
  wds::ui::EditorSession session(preview);
  wds::ui::ChartEditPanel edit(session.engine());
  EditorToolbar toolbar(session, edit);
  CurveTemplateUiState live;
  toolbar.bind_curve_state(&live);
  toolbar.refresh_curve_controls();
  toolbar.set_bounds({0.0f, 0.0f, toolbar_w, 360.0f});
  toolbar.layout({0.0f, 0.0f, toolbar_w, 360.0f});
  CHECK(toolbar.children().size() >= 24);

  for (std::size_t i = 0; i < toolbar.children().size(); ++i) {
    const auto* child = toolbar.children()[i].get();
    CHECK(child->visible());
    CHECK(child->bounds().w > 1.0f);
    CHECK(child->bounds().h > 1.0f);
    for (std::size_t j = i + 1; j < toolbar.children().size(); ++j) {
      CHECK(!rects_overlap(child->bounds(), toolbar.children()[j]->bounds()));
    }
  }

  CHECK(toolbar.children()[12]->bounds().w + 0.01f >= kToolbarMinFieldW);
  CHECK(toolbar.children()[15]->bounds().w + 0.01f >= kToolbarMinFieldW);

  auto* cb0 = static_cast<wds::interaction::Checkbox*>(toolbar.children()[17].get());
  auto* cb1 = static_cast<wds::interaction::Checkbox*>(toolbar.children()[18].get());
  const float need0 = toolbar_checkbox_required_width(cb0->label());
  const float need1 = toolbar_checkbox_required_width(cb1->label());
  CHECK(std::fabs(cb0->bounds().w - need0) < 0.5f);
  CHECK(std::fabs(cb1->bounds().w - need1) < 0.5f);
  CHECK(cb0->bounds().x + cb0->bounds().w + wds::interaction::theme::kUiGap <=
        cb1->bounds().x + 0.01f);
  const float avail = cols.col_x[2] - cols.pad;
  if (need0 + need1 + cols.gap <= avail + 0.01f) {
    CHECK(rect_in_span(cb0->bounds(), cols.pad, cols.col_x[2]));
    CHECK(rect_in_span(cb1->bounds(), cols.pad, cols.col_x[2]));
  }

  CHECK(rect_in_span(toolbar.children()[8]->bounds(), cols.col_x[0], cols.col_x[0] + cols.col_w[0]));
  CHECK(rect_in_span(toolbar.children()[11]->bounds(), cols.col_x[0], cols.col_x[0] + cols.col_w[0]));
  CHECK(rect_in_span(toolbar.children()[12]->bounds(), cols.col_x[0], cols.col_x[0] + cols.col_w[0]));
  CHECK(rect_in_span(toolbar.children()[13]->bounds(), cols.col_x[0], cols.col_x[0] + cols.col_w[0]));
  CHECK(rect_in_span(toolbar.children()[9]->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));
  CHECK(rect_in_span(toolbar.children()[10]->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));
  CHECK(rect_in_span(toolbar.children()[14]->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));
  CHECK(rect_in_span(toolbar.children()[15]->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));
  CHECK(rect_in_span(toolbar.children()[16]->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));

  const auto groups = toolbar_group_metrics(cols);
  assert_curve_row_layout_matches_groups(cols, 40.0f);
  CHECK(!toolbar_checkboxes_need_stack(cols, need0, need1));
  const auto vert = compute_toolbar_control_vertical(toolbar_w, 360.0f, false);
  CHECK_EQ(vert.rows, 4);

  auto* curve_dropdown = toolbar_curve_dropdown(toolbar);
  const auto dirs = toolbar_direction_buttons(toolbar);
  CHECK(curve_dropdown != nullptr);
  CHECK(dirs[0] != nullptr && dirs[1] != nullptr && dirs[2] != nullptr && dirs[3] != nullptr);
  const auto delay = toolbar.children()[8]->bounds();
  const auto chart = toolbar.children()[9]->bounds();
  const float field_bottom =
      std::max(delay.bottom(), toolbar.children()[13]->bounds().bottom());
  CHECK(curve_dropdown->bounds().y + 0.01f >= field_bottom);
  CHECK(std::fabs(curve_dropdown->bounds().y - dirs[0]->bounds().y) < 0.5f);
  CHECK(rect_in_span(curve_dropdown->bounds(), cols.col_x[0], cols.col_x[0] + cols.col_w[0]));
  CHECK(std::fabs(curve_dropdown->bounds().x - delay.x) < 0.5f);
  CHECK(std::fabs(curve_dropdown->bounds().w - delay.w) < 0.5f);
  CHECK(std::fabs(curve_dropdown->bounds().h - delay.h) < 0.5f);
  CHECK(std::fabs(delay.x - (groups.col0 + groups.label_w)) < 0.5f);
  CHECK(std::fabs(delay.w - groups.cluster_w) < 0.5f);
  const float chart_group_left = chart.x - cols.label_w;
  const float chart_group_right = groups.col1 + groups.group_w;
  CHECK(std::fabs(chart_group_left - groups.col1) < 0.5f);
  CHECK(std::fabs(dirs[0]->bounds().x - groups.col1) < 0.5f);
  CHECK(std::fabs(dirs[3]->bounds().x + dirs[3]->bounds().w - chart_group_right) < 0.5f);
  const float dir_w = dirs[0]->bounds().w;
  for (auto* button : dirs) {
    CHECK(std::fabs(button->bounds().w - dir_w) < 0.5f);
    CHECK(rect_in_span(button->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));
    CHECK(std::fabs(button->bounds().y - curve_dropdown->bounds().y) < 0.5f);
  }
  CHECK(curve_dropdown->bounds().x + curve_dropdown->bounds().w <= dirs[0]->bounds().x + 0.01f);
  const float upper_gutter = groups.col1 - (groups.col0 + groups.group_w);
  const float curve_gutter = dirs[0]->bounds().x - curve_dropdown->bounds().right();
  CHECK(std::fabs(curve_gutter - upper_gutter) < 1.0f);
  CHECK(std::fabs(cb0->bounds().y - cb1->bounds().y) < 0.5f);
  CHECK(cb0->bounds().y + 0.01f >= curve_dropdown->bounds().bottom());
  CHECK(cb1->bounds().y + 0.01f >= curve_dropdown->bounds().bottom());
  if (!checkbox_centered_placement_needs_shift(cols, need0, need1)) {
    const float c0 = groups.col0 + groups.group_w * 0.5f;
    const float c1 = groups.col1 + groups.group_w * 0.5f;
    CHECK(std::fabs((cb0->bounds().x + cb0->bounds().w * 0.5f) - c0) < 4.0f);
    CHECK(std::fabs((cb1->bounds().x + cb1->bounds().w * 0.5f) - c1) < 4.0f);
  } else {
    CHECK((cb0->bounds().x + cb0->bounds().w * 0.5f) < (cb1->bounds().x + cb1->bounds().w * 0.5f));
  }
}

void assert_curve_row_readable(float toolbar_w) {
  wds::ui::ChartPreviewPanel preview;
  wds::ui::EditorSession session(preview);
  wds::ui::ChartEditPanel edit(session.engine());
  EditorToolbar toolbar(session, edit);
  CurveTemplateUiState live;
  toolbar.bind_curve_state(&live);
  toolbar.refresh_curve_controls();
  toolbar.set_bounds({0.0f, 0.0f, toolbar_w, 360.0f});
  toolbar.layout({0.0f, 0.0f, toolbar_w, 360.0f});

  auto* curve_dropdown = toolbar_curve_dropdown(toolbar);
  const auto dirs = toolbar_direction_buttons(toolbar);
  CHECK(curve_dropdown != nullptr);
  CHECK(dirs[2] != nullptr);
  const auto cols = compute_toolbar_control_layout(toolbar_w);
  const float io_w = painted_label_width("IO");
  const float empty_w = painted_label_width(kEmptyCurveTemplateLabel);
  CHECK(dirs[2]->bounds().w + 0.01f >= io_w);
  for (auto* button : dirs) {
    CHECK(button != nullptr);
    CHECK(button->bounds().w + 0.01f >= io_w);
    CHECK(rect_in_span(button->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));
  }
  CHECK(curve_dropdown->bounds().w + 0.01f >= empty_w + dropdown_chrome_width());
  CHECK(rect_in_span(curve_dropdown->bounds(), cols.col_x[0], cols.col_x[0] + cols.col_w[0]));
  const auto groups = toolbar_group_metrics(cols);
  CHECK(std::fabs(curve_dropdown->bounds().x - (groups.col0 + groups.label_w)) < 0.5f);
  CHECK(std::fabs(curve_dropdown->bounds().w - groups.cluster_w) < 0.5f);
  CHECK(std::fabs(dirs[0]->bounds().x - groups.col1) < 0.5f);
  CHECK(std::fabs(dirs[3]->bounds().x + dirs[3]->bounds().w - (groups.col1 + groups.group_w)) <
        0.5f);
  assert_curve_row_layout_matches_groups(cols, curve_dropdown->bounds().y);
}

void test_default_window_toolbar_uses_standard_gaps_and_curve_row_columns() {
  namespace th = wds::interaction::theme;
  wds::ui::EditorLayouter layouter;
  const auto regions = layouter.compute(1280, 800).regions;
  wds::ui::ChartPreviewPanel preview;
  wds::ui::EditorSession session(preview);
  wds::ui::ChartEditPanel edit(session.engine());
  EditorToolbar toolbar(session, edit);
  CurveTemplateUiState live;
  toolbar.bind_curve_state(&live);
  toolbar.refresh_curve_controls();
  toolbar.set_bounds(regions.toolbar);
  toolbar.layout(regions.toolbar);

  const auto cols = compute_toolbar_control_layout(regions.toolbar.w);
  const auto groups = toolbar_group_metrics(cols);
  auto* curve_dropdown = toolbar_curve_dropdown(toolbar);
  const auto dirs = toolbar_direction_buttons(toolbar);
  CHECK(curve_dropdown != nullptr);
  CHECK(rect_in_span(curve_dropdown->bounds(), cols.col_x[0], cols.col_x[0] + cols.col_w[0]));
  const auto delay = toolbar.children()[8]->bounds();
  CHECK(std::fabs(curve_dropdown->bounds().x - delay.x) < 0.5f);
  CHECK(std::fabs(curve_dropdown->bounds().w - delay.w) < 0.5f);
  CHECK(std::fabs(curve_dropdown->bounds().h - delay.h) < 0.5f);
  CHECK(std::fabs(dirs[0]->bounds().x - groups.col1) < 0.5f);
  CHECK(std::fabs(dirs[3]->bounds().x + dirs[3]->bounds().w - (groups.col1 + groups.group_w)) < 0.5f);
  const float dir_w = dirs[0]->bounds().w;
  for (auto* b : dirs) {
    CHECK(std::fabs(b->bounds().w - dir_w) < 0.5f);
    CHECK(rect_in_span(b->bounds(), cols.col_x[1], cols.col_x[1] + cols.col_w[1]));
  }
  CHECK(curve_dropdown->bounds().x + curve_dropdown->bounds().w <= dirs[0]->bounds().x + 0.01f);
  const float upper_gutter = groups.col1 - (groups.col0 + groups.group_w);
  CHECK(std::fabs((dirs[0]->bounds().x - curve_dropdown->bounds().right()) - upper_gutter) < 1.0f);
  assert_curve_row_layout_matches_groups(cols, curve_dropdown->bounds().y);
  auto* cb0 = static_cast<wds::interaction::Checkbox*>(toolbar.children()[17].get());
  auto* cb1 = static_cast<wds::interaction::Checkbox*>(toolbar.children()[18].get());
  const float need0 = toolbar_checkbox_required_width(cb0->label());
  const float need1 = toolbar_checkbox_required_width(cb1->label());
  CHECK(std::fabs(cb0->bounds().y - cb1->bounds().y) < 0.5f);
  CHECK(std::fabs(cb0->bounds().w - need0) < 0.5f);
  CHECK(std::fabs(cb1->bounds().w - need1) < 0.5f);
  CHECK(!rects_overlap(cb0->bounds(), cb1->bounds()));
  CHECK(cb0->bounds().x + cb0->bounds().w + th::kUiGap <= cb1->bounds().x + 0.01f);
  CHECK(!toolbar_checkboxes_need_stack(cols, need0, need1));
  if (!checkbox_centered_placement_needs_shift(cols, need0, need1)) {
    const float c0 = groups.col0 + groups.group_w * 0.5f;
    const float c1 = groups.col1 + groups.group_w * 0.5f;
    CHECK(std::fabs((cb0->bounds().x + cb0->bounds().w * 0.5f) - c0) < 4.0f);
    CHECK(std::fabs((cb1->bounds().x + cb1->bounds().w * 0.5f) - c1) < 4.0f);
  } else {
    CHECK((cb0->bounds().x + cb0->bounds().w * 0.5f) < (cb1->bounds().x + cb1->bounds().w * 0.5f));
  }
  const auto vert =
      compute_toolbar_control_vertical(regions.toolbar.w, regions.toolbar.h, false);
  CHECK_EQ(vert.rows, 4);

  const float gap_ab = toolbar.children()[13]->bounds().y - toolbar.children()[8]->bounds().bottom();
  CHECK(gap_ab + 0.01f >= th::kUiGap);
  const float gap_bc = curve_dropdown->bounds().y - toolbar.children()[13]->bounds().bottom();
  CHECK(gap_bc + 0.01f >= th::kUiGap);
}

void test_crushed_toolbar_controls_do_not_overlap() {
  wds::ui::EditorLayouter layouter;
  const auto regions = layouter.compute(1280, 480).regions;
  wds::ui::ChartPreviewPanel preview;
  wds::ui::EditorSession session(preview);
  wds::ui::ChartEditPanel edit(session.engine());
  EditorToolbar toolbar(session, edit);
  CurveTemplateUiState live;
  toolbar.bind_curve_state(&live);
  toolbar.refresh_curve_controls();
  toolbar.set_bounds(regions.toolbar);
  toolbar.layout(regions.toolbar);
  CHECK(toolbar.children().size() >= 24);
  for (std::size_t i = 0; i < toolbar.children().size(); ++i) {
    CHECK(toolbar.children()[i]->bounds().h + 0.01f >= 1.0f);
    for (std::size_t j = i + 1; j < toolbar.children().size(); ++j) {
      CHECK(!rects_overlap(toolbar.children()[i]->bounds(), toolbar.children()[j]->bounds()));
    }
  }
}

void test_two_column_settings_and_aligned_curve_row() {
  CHECK(kToolbarSupportedNarrowWidth + 0.01f >= toolbar_min_usable_width());
  const auto tight = compute_toolbar_control_layout(toolbar_min_usable_width());
  CHECK(tight.field_in_step + 0.01f >= kToolbarMinFieldW);
  CHECK(tight.field_in_chart + 0.01f >= kToolbarMinFieldW);
  CHECK(tight.col_x[0] + tight.col_w[0] <= tight.col_x[1] + 0.01f);
  CHECK(tight.col_x[1] + tight.col_w[1] <= tight.col_x[2] + 0.01f);
  CHECK(tight.col_w[2] < 1.0f);

  for (float width : {kToolbarSupportedNarrowWidth, kToolbarTypicalWidth}) {
    const auto layout = compute_toolbar_control_layout(width);
    CHECK(layout.col_w[0] > 1.0f);
    CHECK(layout.col_w[1] > 1.0f);
    CHECK(layout.col_w[2] < 1.0f);
    CHECK(layout.col_x[0] + layout.col_w[0] <= layout.col_x[1] + 0.01f);
    CHECK(layout.col_x[1] + layout.col_w[1] <= layout.col_x[2] + 0.01f);
    CHECK(layout.col_x[2] + 0.01f >= width - layout.pad);
    CHECK(layout.label_w + layout.cluster_w <= layout.col_w[0] + 0.01f);
    CHECK(layout.label_w + layout.cluster_w <= layout.col_w[1] + 0.01f);
    CHECK(layout.field_in_step + 0.01f >= kToolbarMinFieldW);
    CHECK(layout.field_in_chart + 0.01f >= kToolbarMinFieldW);
    CHECK(layout.col_w[0] + layout.col_w[1] + layout.gap + 0.01f >=
          width - layout.pad * 2.0f);
  }

  constexpr float kNarrowMeasured = 280.0f;
  constexpr float kTypicalMeasured = 200.0f;
  assert_checkbox_helper(kToolbarSupportedNarrowWidth, kNarrowMeasured, kNarrowMeasured, false);
  assert_checkbox_helper(kToolbarTypicalWidth, kTypicalMeasured, kTypicalMeasured, false);
  assert_live_toolbar_no_overlap(kToolbarSupportedNarrowWidth);
  assert_live_toolbar_no_overlap(kToolbarTypicalWidth);
  assert_curve_row_readable(kToolbarSupportedNarrowWidth);
  assert_curve_row_readable(kToolbarTypicalWidth);
}

}  // namespace

int main() {
  test_toolbar_action_order_has_no_undo_redo();
  test_curve_and_check_actions_dispatch_from_toolbar_hits();
  test_dropdown_empty_item_and_stable_id_with_duplicates();
  test_deleted_id_falls_back_to_empty_linear();
  test_four_direction_mappings();
  test_programmatic_refresh_emits_no_callback();
  test_one_user_change_emits_one_callback();
  test_two_column_settings_and_aligned_curve_row();
  test_default_window_toolbar_uses_standard_gaps_and_curve_row_columns();
  test_crushed_toolbar_controls_do_not_overlap();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}

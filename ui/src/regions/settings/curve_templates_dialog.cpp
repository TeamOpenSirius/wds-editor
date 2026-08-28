#include "wds/ui/regions/settings/curve_templates_dialog.hpp"

#include <wds/core/easing.hpp>
#include <wds/interaction/events.hpp>
#include <wds/interaction/theme.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/interaction/widget_root.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/text_field.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>

namespace wds::ui {

void CurveTemplateDialogSession::begin(const CurveTemplateUiState& live) {
  templates_ = live.templates;
  if (templates_.size() > kMaxCurveTemplates) templates_.resize(kMaxCurveTemplates);
  selected_id_ = live.selected_id;
  if (selected_id_ != 0 && find_curve_template_by_id(templates_, selected_id_) == nullptr) {
    selected_id_ = 0;
  }
}

void CurveTemplateDialogSession::discard() {
  templates_.clear();
  selected_id_ = 0;
}

void CurveTemplateDialogSession::commit(CurveTemplateUiState& live) const {
  std::vector<CurveTemplate> next = templates_;
  if (next.size() > kMaxCurveTemplates) next.resize(kMaxCurveTemplates);
  std::uint64_t fill_id = live.selected_id;
  normalize_curve_config(next, fill_id);
  live.templates = std::move(next);
  live.selected_id = fill_id;
}

const CurveTemplate* CurveTemplateDialogSession::selected() const {
  return find_curve_template_by_id(templates_, selected_id_);
}

CurveTemplate* CurveTemplateDialogSession::selected() {
  return find_curve_template_by_id(templates_, selected_id_);
}

bool CurveTemplateDialogSession::can_add() const noexcept {
  return templates_.size() < kMaxCurveTemplates;
}

void CurveTemplateDialogSession::add() {
  if (!can_add()) return;
  CurveTemplate tmpl;
  tmpl.id = allocate_curve_template_id(templates_);
  tmpl.name = next_default_name(templates_);
  tmpl.algorithm = wds::chart_editor::EasingAlgorithm::Linear;
  tmpl.parameter = 0.0;
  templates_.push_back(std::move(tmpl));
  selected_id_ = templates_.back().id;
}

void CurveTemplateDialogSession::remove_at(std::size_t index) {
  if (index >= templates_.size()) return;
  const std::uint64_t removed_id = templates_[index].id;
  const bool removing_selected = removed_id == selected_id_;
  templates_.erase(templates_.begin() + static_cast<std::ptrdiff_t>(index));
  if (!removing_selected) return;
  if (templates_.empty()) {
    selected_id_ = 0;
    return;
  }
  const std::size_t next = index < templates_.size() ? index : templates_.size() - 1;
  selected_id_ = templates_[next].id;
}

void CurveTemplateDialogSession::select_id(std::uint64_t id) {
  if (id == 0 || find_curve_template_by_id(templates_, id) == nullptr) {
    selected_id_ = 0;
    return;
  }
  selected_id_ = id;
}

void CurveTemplateDialogSession::select_index(std::size_t index) {
  if (index >= templates_.size()) {
    selected_id_ = 0;
    return;
  }
  selected_id_ = templates_[index].id;
}

void CurveTemplateDialogSession::set_selected_name(std::string name) {
  CurveTemplate* tmpl = selected();
  if (tmpl == nullptr) return;
  tmpl->name = std::move(name);
}

void CurveTemplateDialogSession::set_selected_algorithm(
    wds::chart_editor::EasingAlgorithm algorithm) {
  CurveTemplate* tmpl = selected();
  if (tmpl == nullptr) return;
  tmpl->algorithm = algorithm;
  normalize_curve_template(*tmpl);
}

void CurveTemplateDialogSession::set_selected_parameter(double parameter) {
  CurveTemplate* tmpl = selected();
  if (tmpl == nullptr) return;
  tmpl->parameter = parameter;
  normalize_curve_template(*tmpl);
}

std::string CurveTemplateDialogSession::next_default_name(
    const std::vector<CurveTemplate>& templates) {
  for (int n = 1;; ++n) {
    const std::string candidate = "曲线 " + std::to_string(n);
    bool used = false;
    for (const auto& tmpl : templates) {
      if (tmpl.name == candidate) {
        used = true;
        break;
      }
    }
    if (!used) return candidate;
  }
}

namespace {

namespace th = wds::interaction::theme;

constexpr wds::chart_editor::EasingAlgorithm kAlgorithms[] = {
    wds::chart_editor::EasingAlgorithm::Linear,
    wds::chart_editor::EasingAlgorithm::Poly,
    wds::chart_editor::EasingAlgorithm::Exp,
    wds::chart_editor::EasingAlgorithm::Sine,
};

constexpr const char* kAlgorithmLabels[] = {"Linear", "Poly", "Exp", "Sine"};

constexpr wds::chart_editor::EasingDirection kPreviewDirections[] = {
    wds::chart_editor::EasingDirection::In,
    wds::chart_editor::EasingDirection::Out,
    wds::chart_editor::EasingDirection::InOut,
    wds::chart_editor::EasingDirection::OutIn,
};

constexpr const char* kPreviewLabels[] = {"In", "Out", "InOut", "OutIn"};

bool algorithm_uses_parameter(wds::chart_editor::EasingAlgorithm algorithm) noexcept {
  return algorithm == wds::chart_editor::EasingAlgorithm::Poly ||
         algorithm == wds::chart_editor::EasingAlgorithm::Exp;
}

std::optional<double> parse_finite_decimal(const std::string& text) {
  try {
    std::size_t parsed = 0;
    const double value = std::stod(text, &parsed);
    if (parsed != text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::string format_parameter(double value) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", value);
  return buf;
}

}  // namespace

void paint_easing_preview_polyline(wds::interaction::UiPainter& painter,
                                   wds::interaction::Rect inner,
                                   wds::chart_editor::EasingAlgorithm algorithm,
                                   wds::chart_editor::EasingDirection direction, double parameter,
                                   float z) {
  namespace th = wds::interaction::theme;
  const int cols = static_cast<int>(std::floor(inner.w));
  if (cols <= 0 || inner.h <= 0.0f) return;
  constexpr float kStroke = 1.5f;
  for (int x = 0; x < cols; ++x) {
    const double t0 = static_cast<double>(x) / static_cast<double>(cols);
    const double t1 = static_cast<double>(x + 1) / static_cast<double>(cols);
    const float y0 =
        inner.bottom() -
        static_cast<float>(wds::chart_editor::apply_easing(t0, algorithm, direction, parameter)) *
            inner.h;
    const float y1 =
        inner.bottom() -
        static_cast<float>(wds::chart_editor::apply_easing(t1, algorithm, direction, parameter)) *
            inner.h;
    const float top = std::min(y0, y1) - kStroke * 0.5f;
    const float h = std::max(kStroke, std::fabs(y1 - y0) + kStroke);
    painter.fill_rect({inner.x + static_cast<float>(x), top, 1.0f, h}, th::kPrimary, 0.0f, z);
  }
}

CurveTemplatesDialog::CurveTemplatesDialog() {
  set_visible(false);

  for (std::size_t i = 0; i < kMaxCurveTemplates; ++i) {
    auto clear = std::make_unique<wds::interaction::Button>("×");
    clear->set_tone(wds::interaction::ButtonTone::Cancel);
    clear->on_click([this, i] {
      session_.remove_at(i);
      sync_fields_from_session();
      if (open_) layout_content(bounds_);
    });
    delete_buttons_[i] = clear.get();
    add_child(std::move(clear));
  }

  auto add = std::make_unique<wds::interaction::Button>("添加");
  add->on_click([this] {
    session_.add();
    sync_fields_from_session();
    if (open_) layout_content(bounds_);
  });
  add_button_ = add.get();
  add_child(std::move(add));

  auto name = std::make_unique<wds::interaction::TextField>("名称");
  name->on_change([this](const std::string& text) { session_.set_selected_name(text); });
  name_field_ = name.get();
  add_child(std::move(name));

  auto parameter = std::make_unique<wds::interaction::TextField>("参数");
  parameter->set_validator([](const std::string& text) {
    return parse_finite_decimal(text).has_value();
  });
  parameter->on_change([this](const std::string& text) {
    if (const auto parsed = parse_finite_decimal(text)) {
      session_.set_selected_parameter(*parsed);
    }
  });
  parameter->on_commit([this](const std::string& text) {
    if (const auto parsed = parse_finite_decimal(text)) {
      session_.set_selected_parameter(*parsed);
      if (const CurveTemplate* tmpl = session_.selected()) {
        static_cast<wds::interaction::TextField*>(parameter_field_)
            ->set_text(format_parameter(tmpl->parameter));
      }
    }
  });
  parameter_field_ = parameter.get();
  add_child(std::move(parameter));

  auto confirm = std::make_unique<wds::interaction::Button>("确认");
  confirm->set_tone(wds::interaction::ButtonTone::Confirm);
  confirm->on_click([this] { try_confirm(); });
  confirm_button_ = confirm.get();
  add_child(std::move(confirm));

  auto cancel = std::make_unique<wds::interaction::Button>("取消");
  cancel->set_tone(wds::interaction::ButtonTone::Cancel);
  cancel->on_click([this] { close(); });
  cancel_button_ = cancel.get();
  add_child(std::move(cancel));
}

void CurveTemplatesDialog::open(const CurveTemplateUiState& live) {
  fill_id_at_open_ = live.selected_id;
  direction_at_open_ = live.direction;
  session_.begin(live);
  open_ = true;
  set_visible(true);
  list_scroll_ = 0.0f;
  if (auto* root = find_root()) {
    root->close_exclusive_popup_outside(this);
    root->set_focus(this);
  }
  sync_fields_from_session();
}

void CurveTemplatesDialog::close() {
  session_.discard();
  open_ = false;
  set_visible(false);
  list_scroll_ = 0.0f;
  if (auto* root = find_root()) {
    for (wds::interaction::Widget* p = root->focused_widget(); p != nullptr; p = p->parent()) {
      if (p == this) {
        root->clear_focus();
        break;
      }
    }
  }
}

void CurveTemplatesDialog::layout(const wds::interaction::Rect& parent_bounds) {
  bounds_ = {0.0f, 0.0f, parent_bounds.w, parent_bounds.h};
  if (open_) {
    layout_content(bounds_);
  }
  Widget::layout(parent_bounds);
}

void CurveTemplatesDialog::update(float delta_seconds) {
  Widget::update(delta_seconds);
  if (!open_) return;
  if (auto* root = find_root()) {
    if (root->focused_widget() == nullptr) {
      root->set_focus(this);
    }
  }
}

void CurveTemplatesDialog::paint(wds::interaction::UiPainter& /*painter*/) const {}

void CurveTemplatesDialog::clamp_list_scroll() {
  const float row_h = th::kControlHeight;
  const float content_h = row_h * static_cast<float>(session_.templates().size());
  const float max_scroll = std::max(0.0f, content_h - list_bounds_.h);
  list_scroll_ = std::clamp(list_scroll_, 0.0f, max_scroll);
}

void CurveTemplatesDialog::update_control_enabled() {
  const CurveTemplate* tmpl = session_.selected();
  const bool has_item = tmpl != nullptr;
  if (add_button_ != nullptr) add_button_->set_enabled(session_.can_add());
  if (name_field_ != nullptr) name_field_->set_enabled(open_ && has_item);
  if (parameter_field_ != nullptr) {
    parameter_field_->set_enabled(open_ && has_item && algorithm_uses_parameter(tmpl->algorithm));
  }
}

void CurveTemplatesDialog::sync_fields_from_session() {
  const CurveTemplate* tmpl = session_.selected();
  if (name_field_ != nullptr) {
    static_cast<wds::interaction::TextField*>(name_field_)
        ->set_text(tmpl != nullptr ? tmpl->name : std::string{});
  }
  if (parameter_field_ != nullptr) {
    static_cast<wds::interaction::TextField*>(parameter_field_)
        ->set_text(tmpl != nullptr ? format_parameter(tmpl->parameter) : std::string{});
  }
  update_control_enabled();
}

void CurveTemplatesDialog::apply_fields_to_session() {
  if (session_.selected() == nullptr) return;
  if (name_field_ != nullptr) {
    session_.set_selected_name(
        static_cast<wds::interaction::TextField*>(name_field_)->text());
  }
  const CurveTemplate* tmpl = session_.selected();
  if (tmpl != nullptr && algorithm_uses_parameter(tmpl->algorithm) && parameter_field_ != nullptr) {
    if (const auto parsed = parse_finite_decimal(
            static_cast<wds::interaction::TextField*>(parameter_field_)->text())) {
      session_.set_selected_parameter(*parsed);
    }
  }
}

void CurveTemplatesDialog::try_confirm() {
  apply_fields_to_session();
  CurveTemplateUiState next;
  next.selected_id = fill_id_at_open_;
  next.direction = direction_at_open_;
  session_.commit(next);
  if (on_confirmed_) on_confirmed_(next);
  session_.discard();
  open_ = false;
  set_visible(false);
  list_scroll_ = 0.0f;
  if (auto* root = find_root()) {
    root->clear_focus_if(this);
  }
}

void CurveTemplatesDialog::layout_content(const wds::interaction::Rect& host) {
  const float pad = th::kUiPad * 1.5f;
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  const float title_h = ctrl_h + th::px(4.0f);
  const float btn_h = ctrl_h;
  const float panel_w = std::min(th::px(720.0f), std::max(th::px(520.0f), host.w * 0.62f));
  const float min_panel_h = pad * 2.0f + title_h + gap + ctrl_h * 8.0f + gap * 7.0f + btn_h;
  const float panel_h = std::clamp(host.h * 0.80f, min_panel_h, host.h * 0.92f);
  content_bounds_ = {(host.w - panel_w) * 0.5f, (host.h - panel_h) * 0.5f, panel_w, panel_h};

  const float btn_y = content_bounds_.bottom() - pad - btn_h;
  const float inner_w = panel_w - pad * 2.0f;
  const float btn_w = std::min(th::px(120.0f), (inner_w - gap) * 0.5f);
  cancel_button_->set_bounds({content_bounds_.x + pad, btn_y, btn_w, btn_h});
  confirm_button_->set_bounds({content_bounds_.right() - pad - btn_w, btn_y, btn_w, btn_h});

  const float body_top = content_bounds_.y + pad + title_h + gap;
  const float body_bottom = btn_y - gap;
  const float left_w = th::px(220.0f);
  const float left_x = content_bounds_.x + pad;
  add_row_bounds_ = {left_x, body_bottom - ctrl_h, left_w, ctrl_h};
  add_button_->set_bounds(add_row_bounds_);
  list_bounds_ = {left_x, body_top, left_w,
                  std::max(0.0f, add_row_bounds_.y - gap - body_top)};
  clamp_list_scroll();

  const float clear_w = th::kStepButtonW + th::px(4.0f);
  const float row_h = ctrl_h;
  for (std::size_t i = 0; i < kMaxCurveTemplates; ++i) {
    auto* clear = delete_buttons_[i];
    if (clear == nullptr) continue;
    const bool in_range = open_ && i < session_.templates().size();
    const float row_y = list_bounds_.y + static_cast<float>(i) * row_h - list_scroll_;
    const bool fully_visible = in_range && row_y >= list_bounds_.y - 0.5f &&
                               row_y + ctrl_h <= list_bounds_.bottom() + 0.5f;
    clear->set_visible(fully_visible);
    if (fully_visible) {
      clear->set_bounds(
          {list_bounds_.right() - clear_w, row_y, clear_w, ctrl_h});
    }
  }

  const float right_x = list_bounds_.right() + gap * 1.5f;
  const float right_w = content_bounds_.right() - pad - right_x;
  const float algo_gap = gap * 0.5f;
  const float algo_w = (right_w - algo_gap * 3.0f) / 4.0f;
  for (int i = 0; i < 4; ++i) {
    algo_bounds_[static_cast<std::size_t>(i)] = {
        right_x + static_cast<float>(i) * (algo_w + algo_gap), body_top, algo_w, ctrl_h};
  }

  const float label_w = th::px(48.0f);
  const float field_y = body_top + ctrl_h + gap;
  const float col_w = std::max(1.0f, (right_w - gap * 3.0f) * 0.5f);
  const float name_col_x = right_x + gap;
  const float param_col_x = name_col_x + col_w + gap;
  const float name_field_x = name_col_x + label_w + gap;
  const float param_field_x = param_col_x + label_w + gap;
  const float field_w = std::max(th::px(40.0f), col_w - label_w - gap);
  if (name_field_ != nullptr) {
    name_field_->set_bounds({name_field_x, field_y, field_w, ctrl_h});
  }
  if (parameter_field_ != nullptr) {
    parameter_field_->set_bounds({param_field_x, field_y, field_w, ctrl_h});
  }

  const float preview_top = field_y + ctrl_h + gap;
  const float preview_h = std::max(0.0f, body_bottom - preview_top);
  const float cell_w = (right_w - gap) * 0.5f;
  const float label_h = th::px(22.0f);
  const float cell_h = (preview_h - gap) * 0.5f;
  const float pane_h = std::max(0.0f, cell_h - label_h);
  for (int i = 0; i < 4; ++i) {
    const int row = i / 2;
    const int col = i % 2;
    const float x = right_x + static_cast<float>(col) * (cell_w + gap);
    const float y = preview_top + static_cast<float>(row) * (cell_h + gap) + label_h;
    preview_bounds_[static_cast<std::size_t>(i)] = {x, y, cell_w, pane_h};
  }

  if (name_field_ != nullptr) name_field_->set_visible(open_);
  if (parameter_field_ != nullptr) parameter_field_->set_visible(open_);
  if (add_button_ != nullptr) add_button_->set_visible(open_);
  if (confirm_button_ != nullptr) confirm_button_->set_visible(open_);
  if (cancel_button_ != nullptr) cancel_button_->set_visible(open_);
  update_control_enabled();
}

void CurveTemplatesDialog::paint_preview(wds::interaction::UiPainter& painter,
                                         const wds::interaction::Rect& pane,
                                         wds::chart_editor::EasingDirection direction,
                                         float z) const {
  const CurveTemplate* tmpl = session_.selected();
  if (tmpl == nullptr) return;
  painter.fill_rect(pane, th::kSurfaceVariant, th::kCornerRadiusSm, z);
  const wds::interaction::Rect inner = pane.inset(th::px(6.0f), th::px(6.0f));
  if (inner.w < 4.0f || inner.h < 4.0f) return;
  paint_easing_preview_polyline(painter, inner, tmpl->algorithm, direction, tmpl->parameter,
                                z + 0.001f);
}

void CurveTemplatesDialog::paint_modal(wds::interaction::UiPainter& painter) const {
  if (!open_ || !visible_) return;
  const wds::interaction::Rect abs = absolute_bounds();
  painter.fill_rect(abs, {0.0f, 0.0f, 0.0f, 0.55f}, 0.0f, 0.985f);

  wds::interaction::Rect content = content_bounds_;
  content.x += abs.x;
  content.y += abs.y;
  painter.fill_rect(content, th::kSurface, th::kCornerRadiusMd, 0.986f);

  const float pad = th::kUiPad * 1.5f;
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  const float title_h = ctrl_h + th::px(4.0f);
  const float title_px = th::kFontSizeMd + 4.0f;
  painter.label({content.x + pad, content.y + pad, content.w - pad * 2.0f, title_h}, "曲线模板",
                th::kOnSurface, 0.987f, false, title_px, false);

  const wds::interaction::Rect abs_list{list_bounds_.x + abs.x, list_bounds_.y + abs.y,
                                        list_bounds_.w, list_bounds_.h};
  painter.fill_rect(abs_list, th::kSurface, th::kCornerRadiusSm, 0.987f);

  const float clear_w = th::kStepButtonW + th::px(4.0f);
  const float clear_gap = th::px(4.0f);
  const float row_h = ctrl_h;
  const float name_w = std::max(0.0f, list_bounds_.w - clear_w - clear_gap - th::px(6.0f));
  const std::size_t count = session_.templates().size();
  const float content_h = row_h * static_cast<float>(count);
  if (list_scroll_ > 1.0f) {
    painter.fill_rect({abs_list.x, abs_list.y, abs_list.w, 3.0f}, th::kOutline, 0.0f, 0.989f);
  }
  if (list_scroll_ + list_bounds_.h < content_h - 1.0f) {
    painter.fill_rect({abs_list.x, abs_list.bottom() - 3.0f, abs_list.w, 3.0f}, th::kOutline, 0.0f,
                      0.989f);
  }
  for (std::size_t i = 0; i < count; ++i) {
    const float row_y = abs_list.y + static_cast<float>(i) * row_h - list_scroll_;
    if (row_y < abs_list.y - 0.5f || row_y + ctrl_h > abs_list.bottom() + 0.5f) continue;
    const bool selected = session_.templates()[i].id == session_.selected_id();
    const wds::interaction::Color fill =
        selected ? th::kSurfaceVariant.lerp(th::kPrimary, 0.22f) : th::kSurfaceVariant;
    const wds::interaction::Color outline =
        selected ? th::kPrimary.lerp(th::kOutline, 0.35f) : th::kOutline;
    painter.fill_rect_outline({abs_list.x, row_y, abs_list.w, ctrl_h}, fill, outline, 0.0f,
                              0.9875f);
    painter.label({abs_list.x + th::px(6.0f), row_y, name_w, ctrl_h}, session_.templates()[i].name,
                  selected ? th::kOnSurface : th::kOnSurfaceMuted, 0.988f, false, 0.0f, true);
  }

  constexpr float kFieldZ = 0.988f;
  for (std::size_t i = 0; i < kMaxCurveTemplates; ++i) {
    auto* clear = delete_buttons_[i];
    if (clear != nullptr && clear->visible()) {
      static_cast<const wds::interaction::Button*>(clear)->paint_at(painter, kFieldZ);
    }
  }

  const CurveTemplate* tmpl = session_.selected();
  const bool has_item = tmpl != nullptr;
  for (int i = 0; i < 4; ++i) {
    wds::interaction::Rect r = algo_bounds_[static_cast<std::size_t>(i)];
    r.x += abs.x;
    r.y += abs.y;
    const bool selected = has_item && tmpl->algorithm == kAlgorithms[i];
    painter.fill_rect(r, selected ? th::kPrimary : th::kSurfaceVariant, th::kCornerRadiusSm,
                      0.987f);
    painter.label(r, kAlgorithmLabels[i],
                  selected ? th::kOnPrimary : (has_item ? th::kOnSurface : th::kOnSurfaceMuted),
                  0.988f);
  }

  const float right_x = list_bounds_.right() + gap * 1.5f + abs.x;
  const float label_w = th::px(48.0f);
  const float param_y = parameter_field_ != nullptr ? parameter_field_->bounds().y + abs.y
                                                     : algo_bounds_[0].y + ctrl_h + gap + abs.y;
  const float name_y = name_field_ != nullptr ? name_field_->bounds().y + abs.y : param_y;
  const float param_label_x =
      parameter_field_ != nullptr ? parameter_field_->bounds().x + abs.x - gap - label_w : right_x;
  const float name_label_x =
      name_field_ != nullptr ? name_field_->bounds().x + abs.x - gap - label_w : right_x;
  painter.label({param_label_x, param_y, label_w, ctrl_h}, "参数", th::kOnSurfaceMuted, 0.987f,
                false, 0.0f, true);
  painter.label({name_label_x, name_y, label_w, ctrl_h}, "名称", th::kOnSurfaceMuted, 0.987f, false,
                0.0f, true);

  if (parameter_field_ != nullptr && parameter_field_->visible()) {
    static_cast<const wds::interaction::TextField*>(parameter_field_)->paint_at(painter, kFieldZ);
  }
  if (name_field_ != nullptr && name_field_->visible()) {
    static_cast<const wds::interaction::TextField*>(name_field_)->paint_at(painter, kFieldZ);
  }

  if (!has_item) {
    wds::interaction::Rect empty = preview_bounds_[0];
    empty.x += abs.x;
    empty.y += abs.y;
    empty.w = preview_bounds_[1].right() - preview_bounds_[0].x;
    empty.h = preview_bounds_[2].bottom() - preview_bounds_[0].y;
    if (empty.h > 0.0f && empty.w > 0.0f) {
      painter.label(empty, "未选择模板", th::kOnSurfaceMuted, 0.988f, false, 0.0f, false);
    }
  } else {
    for (int i = 0; i < 4; ++i) {
      wds::interaction::Rect pane = preview_bounds_[static_cast<std::size_t>(i)];
      pane.x += abs.x;
      pane.y += abs.y;
      painter.label({pane.x, pane.y - th::px(22.0f), pane.w, th::px(22.0f)}, kPreviewLabels[i],
                    th::kOnSurfaceMuted, 0.987f, false, 0.0f, true);
      paint_preview(painter, pane, kPreviewDirections[i], kFieldZ);
    }
  }

  if (add_button_ != nullptr) {
    static_cast<const wds::interaction::Button*>(add_button_)->paint_at(painter, kFieldZ);
  }
  if (cancel_button_ != nullptr) {
    static_cast<const wds::interaction::Button*>(cancel_button_)->paint_at(painter, kFieldZ);
  }
  if (confirm_button_ != nullptr) {
    static_cast<const wds::interaction::Button*>(confirm_button_)->paint_at(painter, kFieldZ);
  }
}

wds::interaction::Widget* CurveTemplatesDialog::hit_test(wds::interaction::Vec2 point) {
  if (!open_ || !visible_ || !enabled_) return nullptr;
  if (!absolute_bounds().contains(point)) return nullptr;

  const wds::interaction::Rect abs = absolute_bounds();
  wds::interaction::Rect list = list_bounds_;
  list.x += abs.x;
  list.y += abs.y;
  if (list.contains(point)) {
    for (auto it = delete_buttons_.rbegin(); it != delete_buttons_.rend(); ++it) {
      if (*it == nullptr || !(*it)->visible()) continue;
      if (Widget* hit = (*it)->hit_test(point)) return hit;
    }
    return this;
  }

  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (!(*it)->visible()) continue;
    bool is_delete = false;
    for (auto* b : delete_buttons_) {
      if (b == it->get()) {
        is_delete = true;
        break;
      }
    }
    if (is_delete) continue;
    if (Widget* hit = (*it)->hit_test(point)) return hit;
  }
  return this;
}

void CurveTemplatesDialog::on_scroll(const wds::interaction::ScrollEvent& event) {
  if (!open_) return;
  const wds::interaction::Rect abs = absolute_bounds();
  wds::interaction::Rect list = list_bounds_;
  list.x += abs.x;
  list.y += abs.y;
  if (!list.contains(event.position)) return;
  list_scroll_ -= event.delta_y * 24.0f;
  clamp_list_scroll();
  layout_content(bounds_);
}

void CurveTemplatesDialog::on_click(const wds::interaction::ClickEvent& event) {
  if (!open_ || event.button != wds::interaction::PointerButton::Left) return;
  const wds::interaction::Rect abs = absolute_bounds();

  auto hit_local = [&](const wds::interaction::Rect& local) {
    wds::interaction::Rect r = local;
    r.x += abs.x;
    r.y += abs.y;
    return r.contains(event.position);
  };

  if (session_.selected() != nullptr) {
    for (int i = 0; i < 4; ++i) {
      if (hit_local(algo_bounds_[static_cast<std::size_t>(i)])) {
        session_.set_selected_algorithm(kAlgorithms[i]);
        sync_fields_from_session();
        return;
      }
    }
  }

  wds::interaction::Rect list = list_bounds_;
  list.x += abs.x;
  list.y += abs.y;
  if (list.contains(event.position)) {
    const float row_h = th::kControlHeight;
    const float local_y = event.position.y - list.y + list_scroll_;
    if (local_y >= 0.0f) {
      const auto index = static_cast<std::size_t>(local_y / row_h);
      if (index < session_.templates().size()) {
        session_.select_index(index);
        sync_fields_from_session();
      }
    }
    return;
  }

  wds::interaction::Rect content = content_bounds_;
  content.x += abs.x;
  content.y += abs.y;
  if (!content.contains(event.position)) {
    close();
  }
}

bool CurveTemplatesDialog::intercept_modal_key_down(
    const wds::interaction::KeyDownEvent& event) {
  if (!open_ || event.repeat) return false;
  if (event.key != wds::interaction::KeyCode::Escape) return false;
  close();
  return true;
}

void CurveTemplatesDialog::on_key_down(const wds::interaction::KeyDownEvent& event) {
  intercept_modal_key_down(event);
}

}  // namespace wds::ui

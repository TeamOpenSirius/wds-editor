#include "wds/ui/regions/settings/chart_add_dialog.hpp"

#include <wds/interaction/theme.hpp>
#include <wds/interaction/widget_root.hpp>
#include <wds/interaction/widgets/button.hpp>

#include <algorithm>

namespace wds::ui {
namespace {

namespace th = wds::interaction::theme;

}  // namespace

ChartAddDialog::ChartAddDialog() {
  set_visible(false);

  auto create = std::make_unique<wds::interaction::Button>("创建新谱面");
  create->set_tone(wds::interaction::ButtonTone::Confirm);
  create->on_click([this] {
    close();
    if (on_create_new_) on_create_new_();
  });
  create_button_ = create.get();
  add_child(std::move(create));

  auto add = std::make_unique<wds::interaction::Button>("添加已有谱面");
  add->set_tone(wds::interaction::ButtonTone::Default);
  add->on_click([this] {
    close();
    if (on_add_existing_) on_add_existing_();
  });
  add_button_ = add.get();
  add_child(std::move(add));

  auto cancel = std::make_unique<wds::interaction::Button>("取消");
  cancel->set_tone(wds::interaction::ButtonTone::Cancel);
  cancel->on_click([this] { close(); });
  cancel_button_ = cancel.get();
  add_child(std::move(cancel));
}

void ChartAddDialog::open() {
  open_ = true;
  set_visible(true);
  if (auto* root = find_root()) {
    root->close_exclusive_popup_outside(this);
  }
}

void ChartAddDialog::close() {
  open_ = false;
  set_visible(false);
}

void ChartAddDialog::layout_content(const wds::interaction::Rect& host) {
  const float pad = th::kUiPad * 1.5f;
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  const float title_h = ctrl_h;
  const float panel_w = std::min(th::px(280.0f), std::max(th::px(200.0f), host.w * 0.36f));
  const float panel_h = pad * 2.0f + title_h + gap + ctrl_h * 3.0f + gap * 3.0f;
  content_bounds_ = {(host.w - panel_w) * 0.5f, (host.h - panel_h) * 0.5f, panel_w, panel_h};

  const float inner_w = panel_w - pad * 2.0f;
  float y = content_bounds_.y + pad + title_h + gap;
  create_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
  y += ctrl_h + gap;
  add_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
  y += ctrl_h + gap;
  cancel_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
}

void ChartAddDialog::layout(const wds::interaction::Rect& parent_bounds) {
  bounds_ = {0.0f, 0.0f, parent_bounds.w, parent_bounds.h};
  if (open_) {
    layout_content(bounds_);
  }
  Widget::layout(parent_bounds);
}

void ChartAddDialog::paint(wds::interaction::UiPainter& /*painter*/) const {}

void ChartAddDialog::paint_modal(wds::interaction::UiPainter& painter) const {
  if (!open_ || !visible_) return;
  const wds::interaction::Rect abs = absolute_bounds();
  painter.fill_rect(abs, {0.0f, 0.0f, 0.0f, 0.55f}, 0.0f, 0.985f);

  wds::interaction::Rect content = content_bounds_;
  content.x += abs.x;
  content.y += abs.y;
  painter.fill_rect(content, th::kSurface, th::kCornerRadiusMd, 0.986f);

  const float pad = th::kUiPad * 1.5f;
  const float ctrl_h = th::kControlHeight;
  painter.label({content.x + pad, content.y + pad, content.w - pad * 2.0f, ctrl_h}, "添加谱面",
                th::kOnSurface, 0.987f);

  constexpr float kFieldZ = 0.988f;
  static_cast<const wds::interaction::Button*>(create_button_)->paint_at(painter, kFieldZ);
  static_cast<const wds::interaction::Button*>(add_button_)->paint_at(painter, kFieldZ);
  static_cast<const wds::interaction::Button*>(cancel_button_)->paint_at(painter, kFieldZ);
}

wds::interaction::Widget* ChartAddDialog::hit_test(wds::interaction::Vec2 point) {
  if (!open_ || !visible_ || !enabled_) return nullptr;
  if (!absolute_bounds().contains(point)) return nullptr;
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test(point)) return hit;
  }
  return this;
}

void ChartAddDialog::on_click(const wds::interaction::ClickEvent& event) {
  if (!open_ || event.button != wds::interaction::PointerButton::Left) return;
  wds::interaction::Rect content = content_bounds_;
  const wds::interaction::Rect abs = absolute_bounds();
  content.x += abs.x;
  content.y += abs.y;
  if (!content.contains(event.position)) {
    close();
  }
}

}  // namespace wds::ui

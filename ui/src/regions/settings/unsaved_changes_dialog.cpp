#include "wds/ui/regions/settings/unsaved_changes_dialog.hpp"

#include <wds/interaction/theme.hpp>
#include <wds/interaction/widgets/button.hpp>

#include <algorithm>

namespace wds::ui {
namespace {

namespace th = wds::interaction::theme;

}  // namespace

UnsavedChangesDialog::UnsavedChangesDialog() {
  set_visible(false);

  auto save = std::make_unique<wds::interaction::Button>("保存");
  save->set_tone(wds::interaction::ButtonTone::Confirm);
  save->on_click([this] {
    close();
    if (on_save_) on_save_();
  });
  save_button_ = save.get();
  add_child(std::move(save));

  auto discard = std::make_unique<wds::interaction::Button>("不保存");
  discard->set_tone(wds::interaction::ButtonTone::Default);
  discard->on_click([this] {
    close();
    if (on_discard_) on_discard_();
  });
  discard_button_ = discard.get();
  add_child(std::move(discard));

  auto cancel = std::make_unique<wds::interaction::Button>("取消");
  cancel->set_tone(wds::interaction::ButtonTone::Cancel);
  cancel->on_click([this] { choose_cancel(); });
  cancel_button_ = cancel.get();
  add_child(std::move(cancel));
}

void UnsavedChangesDialog::open() {
  open_ = true;
  set_visible(true);
}

void UnsavedChangesDialog::close() {
  open_ = false;
  set_visible(false);
}

void UnsavedChangesDialog::choose_cancel() {
  close();
  if (on_cancel_) on_cancel_();
}

void UnsavedChangesDialog::layout_content(const wds::interaction::Rect& host) {
  const float pad = th::kUiPad * 1.5f;
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  const float title_h = ctrl_h;
  const float msg_h = th::kFontSizeMd * 2.4f + gap;
  const float panel_w = std::min(th::px(380.0f), std::max(th::px(280.0f), host.w * 0.44f));
  const float panel_h = pad * 2.0f + title_h + gap + msg_h + gap + ctrl_h * 3.0f + gap * 3.0f;
  content_bounds_ = {(host.w - panel_w) * 0.5f, (host.h - panel_h) * 0.5f, panel_w, panel_h};

  const float inner_w = panel_w - pad * 2.0f;
  float y = content_bounds_.y + pad + title_h + gap + msg_h + gap;
  save_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
  y += ctrl_h + gap;
  discard_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
  y += ctrl_h + gap;
  cancel_button_->set_bounds({content_bounds_.x + pad, y, inner_w, ctrl_h});
}

void UnsavedChangesDialog::layout(const wds::interaction::Rect& parent_bounds) {
  bounds_ = {0.0f, 0.0f, parent_bounds.w, parent_bounds.h};
  if (open_) {
    layout_content(bounds_);
  }
  Widget::layout(parent_bounds);
}

void UnsavedChangesDialog::paint(wds::interaction::UiPainter& /*painter*/) const {}

void UnsavedChangesDialog::paint_modal(wds::interaction::UiPainter& painter) const {
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
  painter.label({content.x + pad, content.y + pad, content.w - pad * 2.0f, ctrl_h}, "未保存的更改",
                th::kOnSurface, 0.987f);
  const float msg_h = th::kFontSizeMd * 2.4f + gap;
  painter.label({content.x + pad, content.y + pad + ctrl_h + gap, content.w - pad * 2.0f, msg_h},
                "当前项目有未保存的更改，是否保存？", th::kOnSurfaceMuted, 0.987f, true,
                th::kFontSizeMd);

  constexpr float kFieldZ = 0.988f;
  static_cast<const wds::interaction::Button*>(save_button_)->paint_at(painter, kFieldZ);
  static_cast<const wds::interaction::Button*>(discard_button_)->paint_at(painter, kFieldZ);
  static_cast<const wds::interaction::Button*>(cancel_button_)->paint_at(painter, kFieldZ);
}

wds::interaction::Widget* UnsavedChangesDialog::hit_test(wds::interaction::Vec2 point) {
  if (!open_ || !visible_ || !enabled_) return nullptr;
  if (!absolute_bounds().contains(point)) return nullptr;
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* hit = (*it)->hit_test(point)) return hit;
  }
  return this;
}

void UnsavedChangesDialog::on_click(const wds::interaction::ClickEvent& event) {
  if (!open_ || event.button != wds::interaction::PointerButton::Left) return;
  wds::interaction::Rect content = content_bounds_;
  const wds::interaction::Rect abs = absolute_bounds();
  content.x += abs.x;
  content.y += abs.y;
  if (!content.contains(event.position)) {
    choose_cancel();
  }
}

}  // namespace wds::ui

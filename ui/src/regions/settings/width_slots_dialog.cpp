#include "wds/ui/regions/settings/width_slots_dialog.hpp"

#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/theme.hpp>
#include <wds/interaction/validators.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/checkbox.hpp>
#include <wds/interaction/widgets/combo_box.hpp>
#include <wds/interaction/widgets/text_field.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace wds::ui {
namespace {

namespace th = wds::interaction::theme;

constexpr const char* kKeyLabels[6] = {"Q", "W", "E", "A", "S", "D"};

const std::vector<std::string>& scroll_speed_items() {
  static const std::vector<std::string> kItems{"0.25x", "0.5x",  "0.75x", "1x",   "1.25x",
                                               "1.5x",  "1.75x", "2x",    "2.5x", "3x"};
  return kItems;
}

float scroll_speed_from_label(const std::string& label) {
  if (label == "0.25x") return 0.25f;
  if (label == "0.5x") return 0.5f;
  if (label == "0.75x") return 0.75f;
  if (label == "1.25x") return 1.25f;
  if (label == "1.5x") return 1.5f;
  if (label == "1.75x") return 1.75f;
  if (label == "2x") return 2.0f;
  if (label == "2.5x") return 2.5f;
  if (label == "3x") return 3.0f;
  return 1.0f;
}

std::string format_scroll_speed(float speed) {
  static constexpr float kOptions[] = {0.25f, 0.5f, 0.75f, 1.0f,  1.25f,
                                       1.5f,  1.75f, 2.0f, 2.5f, 3.0f};
  float best = 1.0f;
  float best_err = std::fabs(speed - best);
  for (float opt : kOptions) {
    const float err = std::fabs(speed - opt);
    if (err < best_err) {
      best = opt;
      best_err = err;
    }
  }
  if (std::fabs(best - 0.25f) < 0.001f) return "0.25x";
  if (std::fabs(best - 0.5f) < 0.001f) return "0.5x";
  if (std::fabs(best - 0.75f) < 0.001f) return "0.75x";
  if (std::fabs(best - 1.25f) < 0.001f) return "1.25x";
  if (std::fabs(best - 1.5f) < 0.001f) return "1.5x";
  if (std::fabs(best - 1.75f) < 0.001f) return "1.75x";
  if (std::fabs(best - 2.0f) < 0.001f) return "2x";
  if (std::fabs(best - 2.5f) < 0.001f) return "2.5x";
  if (std::fabs(best - 3.0f) < 0.001f) return "3x";
  return "1x";
}

void sanitize_width_text(wds::interaction::TextField& field) {
  std::string cleaned;
  cleaned.reserve(field.text().size());
  for (char c : field.text()) {
    if (c >= '0' && c <= '9') cleaned.push_back(c);
  }
  if (cleaned.size() > 2) cleaned.resize(2);
  if (cleaned != field.text()) field.set_text(std::move(cleaned));
}

}  // namespace

WidthSlotsDialog::WidthSlotsDialog() {
  set_visible(false);
  for (int i = 0; i < 6; ++i) {
    auto field = std::make_unique<wds::interaction::TextField>();
    auto* raw = field.get();
    raw->set_validator([](const std::string& text) {
      const auto v = wds::interaction::parse_positive_int(text);
      return v.has_value() && *v >= 1 && *v <= 12;
    });
    raw->on_change([raw](const std::string&) { sanitize_width_text(*raw); });
    fields_[static_cast<std::size_t>(i)] = raw;
    add_child(std::move(field));
  }

  auto sus = std::make_unique<wds::interaction::Checkbox>("导入 sus 谱面时自动转换（实验性）");
  sus_auto_convert_ = sus.get();
  add_child(std::move(sus));

  auto mute = std::make_unique<wds::interaction::Checkbox>("关闭 Hold 体音效播放");
  mute_hold_body_sfx_ = mute.get();
  add_child(std::move(mute));

  auto invert = std::make_unique<wds::interaction::Checkbox>("反转滚轮方向");
  invert_scroll_wheel_ = invert.get();
  add_child(std::move(invert));

  auto speed = std::make_unique<wds::interaction::ComboBox>();
  speed->set_items(scroll_speed_items());
  speed->set_dropdown_only(true);
  speed->set_opens_upward(false);
  speed->set_text("1x");
  scroll_wheel_speed_ = speed.get();
  add_child(std::move(speed));

  auto confirm = std::make_unique<wds::interaction::Button>("确认");
  confirm->set_tone(wds::interaction::ButtonTone::Confirm);
  confirm->on_click([this] {
    apply_fields();
    close();
  });
  confirm_button_ = confirm.get();
  add_child(std::move(confirm));

  auto cancel = std::make_unique<wds::interaction::Button>("取消");
  cancel->set_tone(wds::interaction::ButtonTone::Cancel);
  cancel->on_click([this] { close(); });
  cancel_button_ = cancel.get();
  add_child(std::move(cancel));

  update_tab_visibility();
}

void WidthSlotsDialog::open() {
  open_ = true;
  set_visible(true);
  sync_fields_from_state();
  // Always land on the first sidebar tab (文件), not the last-used one.
  set_tab(Tab::File);
}

void WidthSlotsDialog::close() {
  open_ = false;
  set_visible(false);
  if (auto* combo = static_cast<wds::interaction::ComboBox*>(scroll_wheel_speed_)) {
    combo->dismiss_popups({-1.0f, -1.0f});
  }
}

void WidthSlotsDialog::set_config(const EditorUiConfig& cfg) {
  static_cast<wds::interaction::Checkbox*>(sus_auto_convert_)->set_checked(cfg.sus_auto_convert);
  static_cast<wds::interaction::Checkbox*>(mute_hold_body_sfx_)->set_checked(cfg.mute_hold_body_sfx);
  static_cast<wds::interaction::Checkbox*>(invert_scroll_wheel_)->set_checked(cfg.invert_scroll_wheel);
  static_cast<wds::interaction::ComboBox*>(scroll_wheel_speed_)
      ->set_text(format_scroll_speed(cfg.scroll_wheel_speed));
  for (int i = 0; i < 6; ++i) {
    static_cast<wds::interaction::TextField*>(fields_[static_cast<std::size_t>(i)])
        ->set_text(std::to_string(cfg.width_slots[static_cast<std::size_t>(i)]));
  }
}

void WidthSlotsDialog::capture_config(EditorUiConfig& cfg) const {
  cfg.sus_auto_convert =
      static_cast<const wds::interaction::Checkbox*>(sus_auto_convert_)->checked();
  cfg.mute_hold_body_sfx =
      static_cast<const wds::interaction::Checkbox*>(mute_hold_body_sfx_)->checked();
  cfg.invert_scroll_wheel =
      static_cast<const wds::interaction::Checkbox*>(invert_scroll_wheel_)->checked();
  cfg.scroll_wheel_speed = scroll_speed_from_label(
      static_cast<const wds::interaction::ComboBox*>(scroll_wheel_speed_)->text());
  for (int i = 0; i < 6; ++i) {
    const auto& text =
        static_cast<const wds::interaction::TextField*>(fields_[static_cast<std::size_t>(i)])
            ->text();
    if (const auto parsed = wds::interaction::parse_positive_int(text)) {
      if (*parsed >= 1 && *parsed <= 12) {
        cfg.width_slots[static_cast<std::size_t>(i)] = *parsed;
      }
    }
  }
}

void WidthSlotsDialog::sync_fields_from_state() {
  const auto& slots = wds::interaction::width_slot_values_const();
  for (int i = 0; i < 6; ++i) {
    static_cast<wds::interaction::TextField*>(fields_[static_cast<std::size_t>(i)])
        ->set_text(std::to_string(slots[static_cast<std::size_t>(i)]));
  }
}

void WidthSlotsDialog::apply_fields() {
  std::array<int, 6> next = wds::interaction::width_slot_values_const();
  for (int i = 0; i < 6; ++i) {
    const auto& text =
        static_cast<wds::interaction::TextField*>(fields_[static_cast<std::size_t>(i)])->text();
    if (const auto parsed = wds::interaction::parse_positive_int(text)) {
      if (*parsed >= 1 && *parsed <= 12) {
        next[static_cast<std::size_t>(i)] = *parsed;
      }
    }
  }
  wds::interaction::set_width_slot_values(next);
  wds::interaction::set_invert_scroll_wheel(
      static_cast<wds::interaction::Checkbox*>(invert_scroll_wheel_)->checked());
  wds::interaction::set_scroll_wheel_speed(scroll_speed_from_label(
      static_cast<wds::interaction::ComboBox*>(scroll_wheel_speed_)->text()));
  if (on_applied_) on_applied_();
}

void WidthSlotsDialog::set_tab(Tab tab) {
  if (tab_ == Tab::Input && tab != Tab::Input) {
    if (auto* combo = static_cast<wds::interaction::ComboBox*>(scroll_wheel_speed_)) {
      combo->dismiss_popups({-1.0f, -1.0f});
    }
  }
  tab_ = tab;
  update_tab_visibility();
  if (open_) layout_content(bounds_);
}

void WidthSlotsDialog::update_tab_visibility() {
  const bool width = tab_ == Tab::Width;
  const bool file = tab_ == Tab::File;
  const bool audio = tab_ == Tab::Audio;
  const bool input = tab_ == Tab::Input;
  for (auto* f : fields_) {
    if (f) f->set_visible(open_ && width);
  }
  if (sus_auto_convert_) sus_auto_convert_->set_visible(open_ && file);
  if (mute_hold_body_sfx_) mute_hold_body_sfx_->set_visible(open_ && audio);
  if (invert_scroll_wheel_) invert_scroll_wheel_->set_visible(open_ && input);
  if (scroll_wheel_speed_) scroll_wheel_speed_->set_visible(open_ && input);
}

void WidthSlotsDialog::layout_content(const wds::interaction::Rect& host) {
  const float pad = th::kUiPad * 1.5f;
  const float gap = th::kUiGap;
  const float ctrl_h = th::kControlHeight;
  const float title_h = ctrl_h;
  const float btn_h = ctrl_h;
  const float tab_w = th::px(110.0f);
  const float panel_w = std::min(th::px(560.0f), std::max(th::px(380.0f), host.w * 0.48f));
  const float panel_h =
      pad * 2.0f + title_h + gap + ctrl_h * 5.0f + gap * 4.0f + btn_h + gap;
  content_bounds_ = {(host.w - panel_w) * 0.5f, (host.h - panel_h) * 0.5f, panel_w, panel_h};

  const float tab_x = content_bounds_.x + pad;
  float tab_y = content_bounds_.y + pad + title_h + gap;
  tab_file_bounds_ = {tab_x, tab_y, tab_w, ctrl_h};
  tab_y += ctrl_h + gap * 0.5f;
  tab_audio_bounds_ = {tab_x, tab_y, tab_w, ctrl_h};
  tab_y += ctrl_h + gap * 0.5f;
  tab_input_bounds_ = {tab_x, tab_y, tab_w, ctrl_h};
  tab_y += ctrl_h + gap * 0.5f;
  tab_width_bounds_ = {tab_x, tab_y, tab_w, ctrl_h};

  const float body_x = content_bounds_.x + pad + tab_w + gap * 1.5f;
  const float body_w = content_bounds_.right() - pad - body_x;
  float y = content_bounds_.y + pad + title_h + gap;

  // File / Audio checkboxes (single row each).
  sus_auto_convert_->set_bounds({body_x, y, body_w, ctrl_h});
  mute_hold_body_sfx_->set_bounds({body_x, y, body_w, ctrl_h});

  // Input: invert checkbox, then labeled scroll-speed combo.
  invert_scroll_wheel_->set_bounds({body_x, y, body_w, ctrl_h});
  const float speed_y = y + ctrl_h + gap;
  // Tighter than kSettingsLabelW so the combo sits close to "滚轮速度".
  const float speed_label_w = th::px(88.0f);
  const float speed_label_gap = th::px(4.0f);
  const float combo_x = body_x + speed_label_w + speed_label_gap;
  // Compact field: speed labels are short (e.g. "1.25x"); full body width looked oversized.
  const float speed_combo_w =
      std::max(th::px(72.0f), (body_w - speed_label_w - speed_label_gap) / 3.0f);
  scroll_wheel_speed_->set_bounds({combo_x, speed_y, speed_combo_w, ctrl_h});

  // Width: 2 columns × 3 rows.
  const float label_w = th::px(28.0f);
  const float col_gap = gap;
  const float col_w = (body_w - col_gap) * 0.5f;
  const float field_w = std::max(th::px(48.0f), col_w - label_w);
  for (int i = 0; i < 6; ++i) {
    const int row = i / 2;
    const int col = i % 2;
    const float fx = body_x + static_cast<float>(col) * (col_w + col_gap) + label_w;
    const float fy = y + static_cast<float>(row) * (ctrl_h + gap);
    fields_[static_cast<std::size_t>(i)]->set_bounds({fx, fy, field_w, ctrl_h});
  }

  const float inner_w = panel_w - pad * 2.0f;
  const float btn_w = std::min(th::px(120.0f), (inner_w - gap) * 0.5f);
  const float btn_y = content_bounds_.bottom() - pad - btn_h;
  cancel_button_->set_bounds({content_bounds_.x + pad, btn_y, btn_w, btn_h});
  confirm_button_->set_bounds({content_bounds_.right() - pad - btn_w, btn_y, btn_w, btn_h});
  update_tab_visibility();
}

void WidthSlotsDialog::layout(const wds::interaction::Rect& parent_bounds) {
  bounds_ = {0.0f, 0.0f, parent_bounds.w, parent_bounds.h};
  if (open_) {
    layout_content(bounds_);
  }
  Widget::layout(parent_bounds);
}

void WidthSlotsDialog::paint(wds::interaction::UiPainter& /*painter*/) const {}

void WidthSlotsDialog::paint_popup_layers(wds::interaction::UiPainter& /*painter*/) const {
  // Speed combo popup is painted in paint_dropdown() via the chrome batch.
}

void WidthSlotsDialog::paint_modal(wds::interaction::UiPainter& painter) const {
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
  painter.label({content.x + pad, content.y + pad, content.w - pad * 2.0f, ctrl_h}, "设置",
                th::kOnSurface, 0.987f);

  auto paint_tab = [&](const wds::interaction::Rect& local, const char* title, bool selected) {
    wds::interaction::Rect r = local;
    r.x += abs.x;
    r.y += abs.y;
    painter.fill_rect(r, selected ? th::kSurfaceVariant : th::kSurface, th::kCornerRadiusSm, 0.987f);
    painter.label(r, title, selected ? th::kOnSurface : th::kOnSurfaceMuted, 0.988f, false, 0.0f,
                  true);
  };
  paint_tab(tab_file_bounds_, "文件", tab_ == Tab::File);
  paint_tab(tab_audio_bounds_, "音频", tab_ == Tab::Audio);
  paint_tab(tab_input_bounds_, "输入", tab_ == Tab::Input);
  paint_tab(tab_width_bounds_, "快捷键宽", tab_ == Tab::Width);

  if (tab_ == Tab::Width) {
    const float tab_w = th::px(110.0f);
    const float body_x = content.x + pad + tab_w + gap * 1.5f;
    const float body_w = content.right() - pad - body_x;
    const float label_w = th::px(28.0f);
    const float col_w = (body_w - gap) * 0.5f;
    const float y0 = content.y + pad + ctrl_h + gap;
    for (int i = 0; i < 6; ++i) {
      const int row = i / 2;
      const int col = i % 2;
      const float x = body_x + static_cast<float>(col) * (col_w + gap);
      const float y = y0 + static_cast<float>(row) * (ctrl_h + gap);
      painter.label({x, y, label_w, ctrl_h}, kKeyLabels[i], th::kOnSurfaceMuted, 0.987f);
    }
  }

  constexpr float kFieldZ = 0.988f;
  if (tab_ == Tab::Width) {
    for (auto* field : fields_) {
      static_cast<const wds::interaction::TextField*>(field)->paint_at(painter, kFieldZ);
    }
  } else if (tab_ == Tab::File) {
    static_cast<const wds::interaction::Checkbox*>(sus_auto_convert_)->paint_at(painter, kFieldZ);
  } else if (tab_ == Tab::Audio) {
    static_cast<const wds::interaction::Checkbox*>(mute_hold_body_sfx_)->paint_at(painter, kFieldZ);
  } else if (tab_ == Tab::Input) {
    static_cast<const wds::interaction::Checkbox*>(invert_scroll_wheel_)->paint_at(painter, kFieldZ);
    const float tab_w = th::px(110.0f);
    const float body_x = content.x + pad + tab_w + gap * 1.5f;
    const float speed_y = content.y + pad + ctrl_h + gap + ctrl_h + gap;
    painter.label({body_x, speed_y, th::px(88.0f), ctrl_h}, "滚轮速度", th::kOnSurfaceMuted,
                  0.987f, false, 0.0f, true);
    static_cast<const wds::interaction::ComboBox*>(scroll_wheel_speed_)->paint_at(painter, kFieldZ);
  }
  cancel_button_->paint(painter);
  confirm_button_->paint(painter);
}

void WidthSlotsDialog::paint_dropdown(wds::interaction::UiPainter& painter) const {
  if (!open_ || !visible_ || scroll_wheel_speed_ == nullptr) return;
  if (tab_ != Tab::Input) return;
  scroll_wheel_speed_->paint_popup_layer(painter);
}

wds::interaction::Widget* WidthSlotsDialog::hit_test(wds::interaction::Vec2 point) {
  if (!open_ || !visible_ || !enabled_) return nullptr;
  if (!absolute_bounds().contains(point)) return nullptr;
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (!(*it)->visible()) continue;
    if (Widget* hit = (*it)->hit_test(point)) return hit;
  }
  return this;
}

void WidthSlotsDialog::on_click(const wds::interaction::ClickEvent& event) {
  if (!open_ || event.button != wds::interaction::PointerButton::Left) return;
  const wds::interaction::Rect abs = absolute_bounds();
  auto hit_tab = [&](const wds::interaction::Rect& local) {
    wds::interaction::Rect r = local;
    r.x += abs.x;
    r.y += abs.y;
    return r.contains(event.position);
  };
  if (hit_tab(tab_file_bounds_)) {
    set_tab(Tab::File);
    return;
  }
  if (hit_tab(tab_audio_bounds_)) {
    set_tab(Tab::Audio);
    return;
  }
  if (hit_tab(tab_input_bounds_)) {
    set_tab(Tab::Input);
    return;
  }
  if (hit_tab(tab_width_bounds_)) {
    set_tab(Tab::Width);
    return;
  }

  wds::interaction::Rect content = content_bounds_;
  content.x += abs.x;
  content.y += abs.y;
  if (!content.contains(event.position)) {
    close();
  }
}

}  // namespace wds::ui

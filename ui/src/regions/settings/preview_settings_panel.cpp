#include "wds/ui/regions/settings/preview_settings_panel.hpp"

#include "wds/ui/regions/preview/chart_preview_panel.hpp"

#include <wds/interaction/theme.hpp>
#include <wds/interaction/validators.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/checkbox.hpp>
#include <wds/interaction/widgets/combo_box.hpp>
#include <wds/interaction/widgets/slider.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wds::ui {
namespace {

namespace th = wds::interaction::theme;

std::string format_speed(double speed) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", speed);
  return buf;
}

std::string format_volume_pct(int pct) { return std::to_string(pct) + "%"; }

int volume_to_pct(float gain) { return static_cast<int>(std::lround(gain * 100.0f)); }

float pct_to_volume(int pct) {
  return std::clamp(static_cast<float>(pct), 0.0f, 100.0f) / 100.0f;
}

const std::vector<std::string>& volume_items() {
  static const std::vector<std::string> kItems{"0%", "25%", "50%", "75%", "100%"};
  return kItems;
}

const std::vector<std::string>& rate_items() {
  static const std::vector<std::string> kItems{"0.25x", "0.5x", "0.75x", "1x", "1.5x", "2x"};
  return kItems;
}

float rate_from_label(const std::string& label) {
  if (label == "0.25x") return 0.25f;
  if (label == "0.5x") return 0.5f;
  if (label == "0.75x") return 0.75f;
  if (label == "1.5x") return 1.5f;
  if (label == "2x") return 2.0f;
  return 1.0f;
}

std::string format_rate(float rate) {
  if (std::fabs(rate - 0.25f) < 0.001f) return "0.25x";
  if (std::fabs(rate - 0.5f) < 0.001f) return "0.5x";
  if (std::fabs(rate - 0.75f) < 0.001f) return "0.75x";
  if (std::fabs(rate - 1.5f) < 0.001f) return "1.5x";
  if (std::fabs(rate - 2.0f) < 0.001f) return "2x";
  return "1x";
}

int parse_volume_pct_label(const std::string& text) {
  auto cleaned = text;
  if (!cleaned.empty() && cleaned.back() == '%') {
    cleaned.pop_back();
  }
  if (auto pct = wds::interaction::parse_non_negative_int(cleaned)) {
    return std::min(*pct, 100);
  }
  return 100;
}

struct SettingsMetrics {
  float pad = th::kUiPad;
  float gap = th::kUiGap;
  float row_h = th::kControlHeight;
  float seek_h = th::kControlHeight * 0.75f;
  float y0 = 0.0f;
  float y1 = 0.0f;
};

// Two rows: 流速+seek | 音乐/音效/播放速度.
// Free height is split evenly across top pad, inter-row gap, and bottom pad.
SettingsMetrics compute_metrics(const wds::interaction::Rect& b) {
  SettingsMetrics m;
  const float rows = m.row_h * 2.0f;
  const float free = std::max(0.0f, b.h - rows);
  const float slot = free / 3.0f;  // top = mid = bottom
  m.y0 = slot;
  m.y1 = m.y0 + m.row_h + slot;
  return m;
}

}  // namespace

PreviewSettingsPanel::PreviewSettingsPanel(ChartPreviewPanel& preview) : preview_(preview) {
  auto speed_minus = std::make_unique<wds::interaction::Button>("-");
  speed_minus->on_click([this] {
    speed_ = std::max(1.0, std::round((speed_ - 0.5) * 10.0) / 10.0);
    preview_.set_note_speed(speed_);
    sync_from_state();
    notify_persist();
  });
  speed_minus_ = speed_minus.get();
  add_child(std::move(speed_minus));

  auto speed = std::make_unique<wds::interaction::ComboBox>();
  speed->set_items({"3", "5", "7", "9", "11"});
  speed->set_text(format_speed(speed_));
  speed->set_validator([](const std::string& text) {
    const auto value = wds::interaction::parse_speed(text, 1.0);
    return value.has_value() && *value <= 20.0;
  });
  speed->on_commit([this](const std::string& text) {
    if (auto value = wds::interaction::parse_speed(text, 1.0)) {
      speed_ = std::min(20.0, std::round(*value * 10.0) / 10.0);
      preview_.set_note_speed(speed_);
    }
    sync_from_state();
    notify_persist();
  });
  speed_combo_ = speed.get();
  add_child(std::move(speed));

  auto speed_plus = std::make_unique<wds::interaction::Button>("+");
  speed_plus->on_click([this] {
    speed_ = std::min(20.0, std::round((speed_ + 0.5) * 10.0) / 10.0);
    preview_.set_note_speed(speed_);
    sync_from_state();
    notify_persist();
  });
  speed_plus_ = speed_plus.get();
  add_child(std::move(speed_plus));

  preview_.set_note_speed(speed_);

  auto seek = std::make_unique<wds::interaction::Slider>();
  seek->set_range(0.0f, 1.0f);
  seek->on_change([this](float fraction) {
    int64_t duration = preview_.transport().audio().duration_ms();
    duration = std::max<int64_t>(duration, 1);
    preview_.transport().request_seek_ms(static_cast<int64_t>(fraction * duration));
  });
  seek_slider_ = seek.get();
  add_child(std::move(seek));

  auto music = std::make_unique<wds::interaction::ComboBox>();
  music->set_items(volume_items());
  music->set_dropdown_only(true);
  music->set_text(format_volume_pct(100));
  music->on_commit([this](const std::string& text) {
    music_gain_ = pct_to_volume(parse_volume_pct_label(text));
    music_muted_ = false;
    apply_music_gain();
    sync_from_state();
    notify_persist();
  });
  music_combo_ = music.get();
  add_child(std::move(music));

  auto music_mute = std::make_unique<wds::interaction::Checkbox>("静音");
  music_mute->on_change([this](bool checked) {
    music_muted_ = checked;
    apply_music_gain();
    sync_from_state();
    notify_persist();
  });
  music_mute_ = music_mute.get();
  add_child(std::move(music_mute));

  auto sfx = std::make_unique<wds::interaction::ComboBox>();
  sfx->set_items(volume_items());
  sfx->set_dropdown_only(true);
  sfx->set_text(format_volume_pct(100));
  sfx->on_commit([this](const std::string& text) {
    sfx_gain_ = pct_to_volume(parse_volume_pct_label(text));
    sfx_muted_ = false;
    apply_sfx_gain();
    sync_from_state();
    notify_persist();
  });
  sfx_combo_ = sfx.get();
  add_child(std::move(sfx));

  auto sfx_mute = std::make_unique<wds::interaction::Checkbox>("静音");
  sfx_mute->on_change([this](bool checked) {
    sfx_muted_ = checked;
    apply_sfx_gain();
    sync_from_state();
    notify_persist();
  });
  sfx_mute_ = sfx_mute.get();
  add_child(std::move(sfx_mute));

  auto rate = std::make_unique<wds::interaction::ComboBox>();
  rate->set_items(rate_items());
  rate->set_dropdown_only(true);
  rate->set_text(format_rate(playback_rate_));
  rate->on_commit([this](const std::string& text) {
    playback_rate_ = rate_from_label(text);
    apply_playback_rate();
    sync_from_state();
  });
  rate_combo_ = rate.get();
  add_child(std::move(rate));

  apply_playback_rate();
}

void PreviewSettingsPanel::apply_music_gain() {
  preview_.transport().audio().set_music_gain(music_muted_ ? 0.0f : music_gain_);
}

void PreviewSettingsPanel::apply_sfx_gain() {
  preview_.transport().audio().set_sfx_gain(sfx_muted_ ? 0.0f : sfx_gain_);
}

void PreviewSettingsPanel::apply_playback_rate() {
  preview_.transport().set_playback_rate(playback_rate_);
}

void PreviewSettingsPanel::set_playback_rate(float rate) {
  playback_rate_ = rate_from_label(format_rate(rate));
  apply_playback_rate();
  sync_from_state();
}

void PreviewSettingsPanel::notify_persist() const {
  if (on_persist_) on_persist_();
}

void PreviewSettingsPanel::apply_config(const EditorUiConfig& cfg) {
  speed_ = std::clamp(cfg.note_speed, 1.0, 20.0);
  music_gain_ = std::clamp(cfg.music_volume, 0.0f, 1.0f);
  music_muted_ = cfg.music_muted;
  sfx_gain_ = std::clamp(cfg.sfx_volume, 0.0f, 1.0f);
  sfx_muted_ = cfg.sfx_muted;
  preview_.set_note_speed(speed_);
  apply_music_gain();
  apply_sfx_gain();
  sync_from_state();
}

void PreviewSettingsPanel::capture_config(EditorUiConfig& cfg) const {
  cfg.note_speed = speed_;
  cfg.music_volume = music_gain_;
  cfg.music_muted = music_muted_;
  cfg.sfx_volume = sfx_gain_;
  cfg.sfx_muted = sfx_muted_;
}

void PreviewSettingsPanel::sync_from_state() const {
  auto* speed = static_cast<wds::interaction::ComboBox*>(speed_combo_);
  if (speed->visual_state() != wds::interaction::WidgetState::Focused) {
    speed->set_text(format_speed(speed_));
  }
  auto* music = static_cast<wds::interaction::ComboBox*>(music_combo_);
  music->set_text(format_volume_pct(music_muted_ ? 0 : volume_to_pct(music_gain_)));
  auto* sfx = static_cast<wds::interaction::ComboBox*>(sfx_combo_);
  sfx->set_text(format_volume_pct(sfx_muted_ ? 0 : volume_to_pct(sfx_gain_)));
  auto* rate = static_cast<wds::interaction::ComboBox*>(rate_combo_);
  rate->set_text(format_rate(playback_rate_));
  static_cast<wds::interaction::Checkbox*>(music_mute_)->set_checked(music_muted_);
  static_cast<wds::interaction::Checkbox*>(sfx_mute_)->set_checked(sfx_muted_);
}

void PreviewSettingsPanel::layout(const wds::interaction::Rect& parent_bounds) {
  const auto b = bounds_;
  const auto m = compute_metrics(b);
  const float pad = m.pad;
  const float gap = m.gap;
  const float row_h = m.row_h;

  const float label2 = th::kLabelW2;
  const float label4 = th::kLabelW4;
  const float step_w = th::kStepButtonW;
  const float field_w = th::kFieldW;
  const float mute_w = th::kMuteLabelW;

  // Row 0: 流速 stepper, then seek in the remaining width with equal L/R insets.
  float x = pad + label2;
  speed_minus_->set_bounds({x, m.y0, step_w, row_h});
  x += step_w + gap;
  speed_combo_->set_bounds({x, m.y0, field_w, row_h});
  x += field_w + gap;
  speed_plus_->set_bounds({x, m.y0, step_w, row_h});
  x += step_w;

  const float seek_region_end = b.w - pad;
  const float seek_available = std::max(0.0f, seek_region_end - x);
  const float seek_inset = gap;
  const float seek_w = std::max(th::px(24.0f), seek_available - seek_inset * 2.0f);
  const float seek_y = m.y0 + (row_h - m.seek_h) * 0.5f;
  seek_slider_->set_bounds({x + seek_inset, seek_y, seek_w, m.seek_h});
  const float seek_end = x + seek_inset + seek_w;

  // Row 1: 音乐 starts with 流速; 播放速度 ends with the seek bar;
  // 音效 sits between them with equal gaps, nudged slightly right.
  const float music_group = label2 + field_w + gap + mute_w;
  const float sfx_group = music_group;
  const float rate_group = label4 + field_w;
  const float music_x = pad;
  const float rate_x = seek_end - rate_group;
  const float between = std::max(0.0f, rate_x - (music_x + music_group));
  const float kSfxNudgeX = th::px(5.0f);
  float sfx_x = music_x + music_group + std::max(0.0f, (between - sfx_group) * 0.5f) + kSfxNudgeX;
  sfx_x = std::min(sfx_x, rate_x - sfx_group);

  music_combo_->set_bounds({music_x + label2, m.y1, field_w, row_h});
  music_mute_->set_bounds({music_x + label2 + field_w + gap, m.y1, mute_w, row_h});
  sfx_combo_->set_bounds({sfx_x + label2, m.y1, field_w, row_h});
  sfx_mute_->set_bounds({sfx_x + label2 + field_w + gap, m.y1, mute_w, row_h});
  rate_combo_->set_bounds({rate_x + label4, m.y1, field_w, row_h});

  Widget::layout(parent_bounds);
}

void PreviewSettingsPanel::paint(wds::interaction::UiPainter& painter) const {
  const auto b = absolute_bounds();
  painter.fill_rect(b, th::kSurface);

  const auto m = compute_metrics(bounds_);
  const float pad = m.pad;
  const float gap = m.gap;
  const float row_h = m.row_h;
  const float y0 = b.y + m.y0;
  const float y1 = b.y + m.y1;

  const float label2 = th::kLabelW2;
  const float label4 = th::kLabelW4;
  const float step_w = th::kStepButtonW;
  const float field_w = th::kFieldW;
  const float mute_w = th::kMuteLabelW;

  painter.label({b.x + pad, y0, label2, row_h}, "流速", th::kOnSurfaceMuted);

  // Mirror layout(): 音乐↔流速, 播放速度↔seek end, 音效 centered (+ slight right nudge).
  float sx = pad + label2 + step_w + gap + field_w + gap + step_w;
  const float seek_inset = gap;
  const float seek_available = std::max(0.0f, (b.w - pad) - sx);
  const float seek_w = std::max(th::px(24.0f), seek_available - seek_inset * 2.0f);
  const float seek_end = b.x + sx + seek_inset + seek_w;

  const float music_group = label2 + field_w + gap + mute_w;
  const float sfx_group = music_group;
  const float rate_group = label4 + field_w;
  const float music_x = b.x + pad;
  const float rate_x = seek_end - rate_group;
  const float between = std::max(0.0f, rate_x - (music_x + music_group));
  const float kSfxNudgeX = th::px(5.0f);
  float sfx_x = music_x + music_group + std::max(0.0f, (between - sfx_group) * 0.5f) + kSfxNudgeX;
  sfx_x = std::min(sfx_x, rate_x - sfx_group);

  painter.label({music_x, y1, label2, row_h}, "音乐", th::kOnSurfaceMuted);
  painter.label({sfx_x, y1, label2, row_h}, "音效", th::kOnSurfaceMuted);
  painter.label({rate_x, y1, label4, row_h}, "播放速度", th::kOnSurfaceMuted);

  painter.fill_rect({b.x + pad, b.bottom() - 1.0f, std::max(1.0f, b.w - pad * 2.0f), 1.0f},
                    th::kOutline);

  int64_t duration = preview_.transport().audio().duration_ms();
  if (duration <= 0) duration = 1;
  const float frac = std::clamp(
      static_cast<float>(preview_.transport().committed_ms()) / static_cast<float>(duration),
      0.0f, 1.0f);
  static_cast<wds::interaction::Slider*>(seek_slider_)->set_value(frac);
  sync_from_state();

  speed_minus_->paint(painter);
  speed_plus_->paint(painter);
  seek_slider_->paint(painter);
  music_mute_->paint(painter);
  sfx_mute_->paint(painter);
  speed_combo_->paint(painter);
  music_combo_->paint(painter);
  sfx_combo_->paint(painter);
  rate_combo_->paint(painter);
}

void PreviewSettingsPanel::update(float delta_seconds) { Widget::update(delta_seconds); }

}  // namespace wds::ui

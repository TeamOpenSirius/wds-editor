#include "wds/ui/qt/playback_dock.hpp"

#include "wds/ui/qt/flow_layout.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/settings/preview_settings_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/audio/transport.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace wds::ui {
namespace {

constexpr int kSeekSteps = 1000;

float rate_from_combo(const QComboBox* combo) {
  QString text = combo->currentText();
  if (text.endsWith(QLatin1Char('x'))) text.chop(1);
  bool ok = false;
  const float rate = text.toFloat(&ok);
  return ok ? rate : 1.0f;
}

// Scroll + flow scaffold shared by the dock panels: controls wrap into
// rows/columns so the dock stays usable at any size or orientation.
FlowLayout* make_flow_panel(QWidget* panel, QVBoxLayout* root) {
  auto* scroll = new QScrollArea(panel);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* content = new QWidget(scroll);
  auto* flow = new FlowLayout(content, 2, 12, 6);
  scroll->setWidget(content);
  root->addWidget(scroll, 1);
  return flow;
}

QWidget* make_group(FlowLayout* flow, std::initializer_list<QWidget*> widgets) {
  auto* group = new QWidget(flow->parentWidget());
  auto* row = new QHBoxLayout(group);
  row->setContentsMargins(0, 0, 0, 0);
  for (QWidget* widget : widgets) row->addWidget(widget);
  flow->addWidget(group);
  return group;
}

}  // namespace

// --- 播放 -------------------------------------------------------------------

PlaybackAudioPanel::PlaybackAudioPanel(UiManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  build_ui();
  sync_timer_ = new QTimer(this);
  sync_timer_->setInterval(250);
  connect(sync_timer_, &QTimer::timeout, this, &PlaybackAudioPanel::sync_from_runtime);
  sync_timer_->start();
  sync_from_runtime();
}

void PlaybackAudioPanel::seek_to_slider(int value) {
  auto* settings = manager_->settings_panel();
  if (settings == nullptr) return;
  int64_t start = 0;
  int64_t end = 1;
  settings->seek_window_ms(start, end);
  const int64_t span = std::max<int64_t>(end - start, 1);
  manager_->chart_preview().transport().request_seek_ms(start + span * value / kSeekSteps);
}

void PlaybackAudioPanel::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(4, 4, 4, 4);

  auto* seekRow = new QHBoxLayout;
  seek_ = new QSlider(Qt::Horizontal, this);
  seek_->setRange(0, kSeekSteps);
  seek_->setMinimumWidth(120);
  play_ = new QPushButton(tr("播放"), this);
  stop_ = new QPushButton(tr("回到开头"), this);
  seekRow->addWidget(seek_, 1);
  seekRow->addWidget(play_);
  seekRow->addWidget(stop_);
  root->addLayout(seekRow);

  auto* flow = make_flow_panel(this, root);
  auto* content = flow->parentWidget();

  rate_ = new QComboBox(content);
  rate_->addItems({"0.25x", "0.5x", "0.75x", "1x", "1.5x", "2x"});
  rate_->setCurrentText(QStringLiteral("1x"));
  make_group(flow, {new QLabel(tr("播放速度"), content), rate_});

  delay_ms_ = new QSpinBox(content);
  delay_ms_->setRange(-60000, 60000);
  delay_ms_->setSuffix(QStringLiteral(" ms"));
  delay_ms_->setKeyboardTracking(false);
  make_group(flow, {new QLabel(tr("谱面延迟"), content), delay_ms_});

  visible_range_ = new QComboBox(content);
  visible_range_->setEditable(true);
  visible_range_->addItems({"10", "15", "20", "25", "30", "35", "40", "80"});
  make_group(flow, {new QLabel(tr("可见范围"), content), visible_range_});

  subdivisions_ = new QComboBox(content);
  subdivisions_->setEditable(true);
  subdivisions_->addItems({"2", "3", "4", "6", "8", "12", "16"});
  make_group(flow, {new QLabel(tr("拍内分格"), content), subdivisions_});

  chart_select_ = new QComboBox(content);
  chart_select_->setMinimumWidth(110);
  chart_add_ = new QPushButton(QStringLiteral("+"), content);
  chart_add_->setFixedWidth(28);
  make_group(flow, {new QLabel(tr("谱面"), content), chart_select_, chart_add_});

  pause_at_current_ = new QCheckBox(tr("停止播放后停在当前时间"), content);
  make_group(flow, {pause_at_current_});
  split_width_follow_ = new QCheckBox(tr("音符默认对齐分割线轨道"), content);
  make_group(flow, {split_width_follow_});

  // Seek only when the drag is released (or on groove clicks) — live seeking
  // while dragging restarts audio every few ms and crackles.
  connect(seek_, &QSlider::sliderReleased, this,
          [this] { seek_to_slider(seek_->value()); });
  connect(seek_, &QSlider::actionTriggered, this, [this](int action) {
    if (action == QAbstractSlider::SliderMove || seek_->isSliderDown()) return;
    seek_to_slider(seek_->sliderPosition());
  });
  connect(play_, &QPushButton::clicked, this, [this] {
    auto& transport = manager_->chart_preview().transport();
    transport.playing() ? transport.request_pause() : transport.request_play();
  });
  connect(stop_, &QPushButton::clicked, this,
          [this] { manager_->chart_preview().reset_playback(); });
  connect(rate_, &QComboBox::textActivated, this, [this](const QString&) {
    if (syncing_) return;
    if (auto* settings = manager_->settings_panel()) {
      settings->set_playback_rate_from_qt(rate_from_combo(rate_));
      manager_->request_save_ui_config(true);
    }
  });
  connect(delay_ms_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
    if (!syncing_) apply_delay();
  });
  connect(visible_range_, &QComboBox::textActivated, this, [this](const QString&) { apply_grid(); });
  connect(visible_range_->lineEdit(), &QLineEdit::editingFinished, this,
          [this] { apply_grid(); });
  connect(subdivisions_, &QComboBox::textActivated, this, [this](const QString&) { apply_grid(); });
  connect(subdivisions_->lineEdit(), &QLineEdit::editingFinished, this,
          [this] { apply_grid(); });
  connect(chart_select_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
    auto& session = manager_->session();
    if (index >= 0 && static_cast<std::size_t>(index) < session.chart_count())
      session.switch_chart(static_cast<std::size_t>(index));
  });
  connect(chart_add_, &QPushButton::clicked, this, [this] {
    if (on_add_chart_) on_add_chart_();
  });
  connect(pause_at_current_, &QCheckBox::toggled, this, [this](bool checked) {
    if (syncing_) return;
    if (auto* edit = manager_->edit_panel()) {
      edit->set_pause_at_current(checked);
      manager_->request_save_ui_config(true);
    }
  });
  connect(split_width_follow_, &QCheckBox::toggled, this, [this](bool checked) {
    if (syncing_) return;
    if (auto* edit = manager_->edit_panel()) {
      edit->set_split_width_follow(checked);
      manager_->request_save_ui_config(true);
    }
  });
}

void PlaybackAudioPanel::apply_delay() {
  auto& session = manager_->session();
  const int64_t value = std::clamp<int64_t>(delay_ms_->value(), -60000, 60000);
  if (value == session.offset_ms()) return;
  if (!session.set_offset_ms(value)) {
    manager_->set_status("谱面延迟修改被拒绝：会把音符移到时间轴之前", StatusLevel::Error);
    if (auto* edit = manager_->edit_panel())
      edit->flash_offset_violations(session.last_offset_violation_ids());
  }
}

void PlaybackAudioPanel::apply_grid() {
  auto* edit = manager_->edit_panel();
  if (edit == nullptr) return;
  auto grid = edit->viewport().grid();
  bool ok = false;
  const int hectoms = visible_range_->currentText().toInt(&ok);
  if (ok) grid.visible_hectoms = std::clamp(hectoms, 1, 1000);
  ok = false;
  const int divisions = subdivisions_->currentText().toInt(&ok);
  if (ok) grid.subdivisions_per_beat = std::clamp(divisions, 1, 64);
  edit->set_grid(grid);
  manager_->request_save_ui_config(true);
}

void PlaybackAudioPanel::sync_from_runtime() {
  syncing_ = true;
  auto& session = manager_->session();
  auto& transport = manager_->chart_preview().transport();

  play_->setText(transport.playing() ? tr("暂停") : tr("播放"));
  if (!seek_->isSliderDown()) {
    if (auto* settings = manager_->settings_panel()) {
      int64_t start = 0;
      int64_t end = 1;
      settings->seek_window_ms(start, end);
      const int64_t span = std::max<int64_t>(end - start, 1);
      const int64_t pos = std::clamp<int64_t>(transport.committed_ms() - start, 0, span);
      const QSignalBlocker blocker(seek_);
      seek_->setValue(static_cast<int>(pos * kSeekSteps / span));
    }
  }

  if (!delay_ms_->hasFocus()) {
    const QSignalBlocker blocker(delay_ms_);
    delay_ms_->setValue(static_cast<int>(
        std::clamp<int64_t>(session.offset_ms(), int64_t{-60000}, int64_t{60000})));
  }
  if (auto* edit = manager_->edit_panel()) {
    const auto& grid = edit->viewport().grid();
    if (!visible_range_->lineEdit()->hasFocus()) {
      const QSignalBlocker blocker(visible_range_);
      visible_range_->setCurrentText(QString::number(grid.visible_hectoms));
    }
    if (!subdivisions_->lineEdit()->hasFocus()) {
      const QSignalBlocker blocker(subdivisions_);
      subdivisions_->setCurrentText(QString::number(grid.subdivisions_per_beat));
    }
    {
      const QSignalBlocker p(pause_at_current_);
      pause_at_current_->setChecked(edit->pause_at_current());
      const QSignalBlocker w(split_width_follow_);
      split_width_follow_->setChecked(edit->split_width_follow());
    }
  }

  const int count = std::max<int>(1, static_cast<int>(session.chart_count()));
  if (chart_select_->count() != count) {
    const QSignalBlocker blocker(chart_select_);
    chart_select_->clear();
    for (int i = 0; i < count; ++i) chart_select_->addItem(tr("谱面 %1").arg(i + 1));
  }
  if (chart_select_->currentIndex() != static_cast<int>(session.active_chart_index())) {
    const QSignalBlocker blocker(chart_select_);
    chart_select_->setCurrentIndex(static_cast<int>(session.active_chart_index()));
  }
  const bool editable = !session.read_only();
  delay_ms_->setEnabled(session.delay_editable());
  chart_select_->setEnabled(editable || session.chart_count() > 1);
  chart_add_->setEnabled(editable);
  syncing_ = false;
}

// --- 音频 -------------------------------------------------------------------

AudioMixPanel::AudioMixPanel(UiManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(4, 4, 4, 4);
  auto* flow = make_flow_panel(this, root);
  auto* content = flow->parentWidget();

  auto make_volume_slider = [content](QSlider*& slider, QLabel*& value) {
    slider = new QSlider(Qt::Horizontal, content);
    slider->setRange(0, 100);
    slider->setValue(100);
    slider->setMinimumWidth(120);
    value = new QLabel(QStringLiteral("100%"), content);
    value->setMinimumWidth(38);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  };
  make_volume_slider(music_volume_, music_value_);
  music_mute_ = new QCheckBox(tr("静音"), content);
  make_group(flow, {new QLabel(tr("音乐"), content), music_volume_, music_value_, music_mute_});
  make_volume_slider(sfx_volume_, sfx_value_);
  sfx_mute_ = new QCheckBox(tr("静音"), content);
  make_group(flow, {new QLabel(tr("音效"), content), sfx_volume_, sfx_value_, sfx_mute_});

  const auto apply_music = [this] {
    if (syncing_) return;
    music_value_->setText(QString::number(music_volume_->value()) + "%");
    if (auto* settings = manager_->settings_panel()) {
      settings->set_music_state_from_qt(music_volume_->value() / 100.0f, music_mute_->isChecked());
      manager_->request_save_ui_config(false);
    }
  };
  const auto apply_sfx = [this] {
    if (syncing_) return;
    sfx_value_->setText(QString::number(sfx_volume_->value()) + "%");
    if (auto* settings = manager_->settings_panel()) {
      settings->set_sfx_state_from_qt(sfx_volume_->value() / 100.0f, sfx_mute_->isChecked());
      manager_->request_save_ui_config(false);
    }
  };
  connect(music_volume_, &QSlider::valueChanged, this, [apply_music](int) { apply_music(); });
  connect(music_mute_, &QCheckBox::toggled, this, [apply_music](bool) { apply_music(); });
  connect(sfx_volume_, &QSlider::valueChanged, this, [apply_sfx](int) { apply_sfx(); });
  connect(sfx_mute_, &QCheckBox::toggled, this, [apply_sfx](bool) { apply_sfx(); });

  sync_timer_ = new QTimer(this);
  sync_timer_->setInterval(500);
  connect(sync_timer_, &QTimer::timeout, this, &AudioMixPanel::sync_from_runtime);
  sync_timer_->start();
  sync_from_runtime();
}

void AudioMixPanel::sync_from_runtime() {
  syncing_ = true;
  if (auto* settings = manager_->settings_panel()) {
    if (!music_volume_->isSliderDown()) {
      const QSignalBlocker blocker(music_volume_);
      const int pct = static_cast<int>(std::lround(settings->music_gain() * 100.0f));
      music_volume_->setValue(pct);
      music_value_->setText(QString::number(pct) + "%");
    }
    if (!sfx_volume_->isSliderDown()) {
      const QSignalBlocker blocker(sfx_volume_);
      const int pct = static_cast<int>(std::lround(settings->sfx_gain() * 100.0f));
      sfx_volume_->setValue(pct);
      sfx_value_->setText(QString::number(pct) + "%");
    }
    const QSignalBlocker m(music_mute_);
    music_mute_->setChecked(settings->music_muted());
    const QSignalBlocker s(sfx_mute_);
    sfx_mute_->setChecked(settings->sfx_muted());
  }
  syncing_ = false;
}

// --- 曲线填充 ---------------------------------------------------------------

CurveFillWidget::CurveFillWidget(UiManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->addWidget(new QLabel(tr("曲线填充"), this));
  curve_template_ = new QComboBox(this);
  curve_template_->setMinimumWidth(140);
  row->addWidget(curve_template_);
  for (int i = 0; i < 4; ++i) {
    auto* button = new QToolButton(this);
    button->setText(QString::fromUtf8(kCurveDirectionLabels[static_cast<std::size_t>(i)]));
    button->setCheckable(true);
    button->setAutoRaise(false);
    curve_directions_[static_cast<std::size_t>(i)] = button;
    row->addWidget(button);
    connect(button, &QToolButton::clicked, this, [this, i] {
      curve_controller_.select_direction_index(manager_->curve_template_state(), i);
      refresh_curve_controls();
    });
  }
  connect(curve_template_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
    curve_controller_.select_dropdown_index(manager_->curve_template_state(), index);
    refresh_curve_controls();
  });
  curve_controller_.set_on_changed([this](const CurveFillSelection&) {
    manager_->push_curve_fill_selection_from_qt();
    manager_->request_save_ui_config(true);
  });
  refresh_curve_controls();
}

void CurveFillWidget::refresh_curve_controls() {
  curve_controller_.refresh_from(manager_->curve_template_state());
  {
    const QSignalBlocker blocker(curve_template_);
    curve_template_->clear();
    for (const auto& label : curve_controller_.dropdown_labels())
      curve_template_->addItem(QString::fromStdString(label));
    curve_template_->setCurrentIndex(curve_controller_.selected_dropdown_index());
  }
  const int selected = curve_controller_.selected_direction_index();
  for (int i = 0; i < 4; ++i) {
    auto* button = curve_directions_[static_cast<std::size_t>(i)];
    const QSignalBlocker blocker(button);
    button->setChecked(i == selected);
  }
}

}  // namespace wds::ui

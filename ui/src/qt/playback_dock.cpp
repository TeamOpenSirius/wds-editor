#include "wds/ui/qt/playback_dock.hpp"

#include "wds/ui/qt/spread_layout.hpp"
#include "wds/ui/qt/caption_check.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/qt/note_icons.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/settings/preview_settings_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/audio/transport.hpp>
#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/editor_shortcuts.hpp>

#include <QAbstractSlider>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <cmath>
#include <limits>
#include <optional>

namespace wds::ui {
namespace {

constexpr std::array<wds::interaction::PlaceIntent, 8> kPlaceIntents = {{
    wds::interaction::PlaceIntent::Tap,
    wds::interaction::PlaceIntent::ExTap,
    wds::interaction::PlaceIntent::HoldStart,
    wds::interaction::PlaceIntent::HoldBody,
    wds::interaction::PlaceIntent::FlickLeft,
    wds::interaction::PlaceIntent::Flick,
    wds::interaction::PlaceIntent::FlickRight,
    wds::interaction::PlaceIntent::ScratchHoldBody,
}};

struct ConvertSpec {
  const char* name;
  wds::chart_editor::NoteType type;
  int direction;
};

constexpr std::array<ConvertSpec, 8> kConvertSpecs = {{
    {"Tap", wds::chart_editor::NoteType::Normal, 0},
    {"ExTap", wds::chart_editor::NoteType::Critical, 0},
    {"Hold Head", wds::chart_editor::NoteType::HoldStart, 0},
    {"Hold", wds::chart_editor::NoteType::Hold, 0},
    {"Left Flick", wds::chart_editor::NoteType::Flick, -1},
    {"Flick", wds::chart_editor::NoteType::Flick, 0},
    {"Right Flick", wds::chart_editor::NoteType::Flick, 1},
    {"Scratch Hold", wds::chart_editor::NoteType::ScratchHold, 0},
}};

constexpr std::array<const char*, 8> kConvertJournalIds = {{
    "convert.to_tap",
    "convert.to_extap",
    "convert.to_hold_start",
    "convert.to_hold",
    "convert.to_flick_left",
    "convert.to_flick",
    "convert.to_flick_right",
    "convert.to_scratch_hold",
}};

constexpr std::array<const char*, 4> kCurveEaseJournalIds = {{
    "curve.ease_in",
    "curve.ease_out",
    "curve.ease_in_out",
    "curve.ease_out_in",
}};

float rate_from_combo(const QComboBox* combo) {
  QString text = combo->currentText();
  if (text.endsWith(QLatin1Char('x'))) text.chop(1);
  bool ok = false;
  const float rate = text.toFloat(&ok);
  return ok ? rate : 1.0f;
}

int volume_pct_from_combo(const QComboBox* combo) {
  QString text = combo->currentText();
  if (text.endsWith(QLatin1Char('%'))) text.chop(1);
  bool ok = false;
  const int pct = text.toInt(&ok);
  return ok ? std::clamp(pct, 0, 100) : 100;
}

int nearest_volume_index(int pct) {
  static constexpr int kStops[] = {0, 25, 50, 75, 100};
  int best = 4;
  int best_d = 999;
  for (int i = 0; i < 5; ++i) {
    const int d = std::abs(kStops[i] - pct);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

QString format_rate(float rate) {
  if (std::fabs(rate - 0.25f) < 0.001f) return QStringLiteral("0.25x");
  if (std::fabs(rate - 0.5f) < 0.001f) return QStringLiteral("0.5x");
  if (std::fabs(rate - 0.75f) < 0.001f) return QStringLiteral("0.75x");
  if (std::fabs(rate - 1.5f) < 0.001f) return QStringLiteral("1.5x");
  if (std::fabs(rate - 2.0f) < 0.001f) return QStringLiteral("2x");
  return QStringLiteral("1x");
}

constexpr int kToolbarFieldW = 120;
constexpr int kChartInnerGap = 6;
constexpr int kEaseButtonGap = 4;
constexpr int kVolumeFieldW = 76;
constexpr int kLabelFieldGap = 16;

void apply_compact_field(QWidget* field, int width) {
  field->setMaximumWidth(width);
  field->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void apply_toolbar_field(QWidget* field, int width = kToolbarFieldW) {
  field->setFixedWidth(width);
  field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

int toolbar_field_height(const QWidget* field) {
  return std::max(1, field->sizeHint().height());
}

void apply_square_icon_button(QPushButton* button, int side, int icon_px) {
  button->setFixedSize(side, side);
  button->setIconSize(QSize(icon_px, icon_px));
  // Yami's QPushButton rule uses wide padding + input_height, which keeps
  // icon-only transport buttons as 36x32 pills. Pin a square box here only.
  button->setStyleSheet(QStringLiteral(
      "QPushButton { min-width:%1px; max-width:%1px; width:%1px;"
      " min-height:%1px; max-height:%1px; height:%1px;"
      " padding:0px; margin:0px; }")
                            .arg(side));
}

QLabel* make_form_label(QWidget* host, const QString& title) {
  auto* label = new QLabel(title, host);
  label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  return label;
}

QWidget* make_hbox_group(QWidget* parent, std::initializer_list<QWidget*> widgets) {
  auto* group = new QWidget(parent);
  auto* row = new QHBoxLayout(group);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(kLabelFieldGap);
  for (QWidget* widget : widgets) {
    row->addWidget(widget, 0, Qt::AlignVCenter);
  }
  return group;
}

QWidget* make_labeled_field(QWidget* parent, const QString& title, QWidget* field) {
  auto* cell = new QWidget(parent);
  cell->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
  auto* grid = new QGridLayout(cell);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(kLabelFieldGap);
  grid->setVerticalSpacing(0);
  auto* label = make_form_label(cell, title);
  const int field_h = field->sizeHint().height();
  if (field_h > 0) label->setMinimumHeight(field_h);
  grid->addWidget(label, 0, 0, Qt::AlignRight | Qt::AlignVCenter);
  grid->addWidget(field, 0, 1, Qt::AlignLeft | Qt::AlignVCenter);
  grid->setColumnMinimumWidth(1, kToolbarFieldW);
  return cell;
}

QWidget* center_in_cell(QWidget* parent, QWidget* inner) {
  auto* cell = new QWidget(parent);
  cell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto* row = new QHBoxLayout(cell);
  row->setContentsMargins(0, 0, 0, 0);
  row->addStretch(1);
  row->addWidget(inner, 0, Qt::AlignVCenter);
  row->addStretch(1);
  return cell;
}

int sync_min_width(std::initializer_list<QWidget*> widgets) {
  int width = 0;
  for (QWidget* widget : widgets) width = std::max(width, widget->sizeHint().width());
  for (QWidget* widget : widgets) widget->setMinimumWidth(width);
  return width;
}

QWidget* make_pair_row(QWidget* parent, QWidget* left, QWidget* right, int col_w) {
  auto* row = new QWidget(parent);
  row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto* pair = new TwoColumnThreeGapLayout(row);
  auto add_col = [&](QWidget* inner) {
    auto* cell = center_in_cell(row, inner);
    cell->setMinimumWidth(col_w);
    pair->addWidget(cell);
  };
  add_col(left);
  add_col(right);
  return row;
}

QComboBox* make_volume_combo(QWidget* parent) {
  auto* combo = new QComboBox(parent);
  combo->addItems({QStringLiteral("0%"), QStringLiteral("25%"), QStringLiteral("50%"),
                   QStringLiteral("75%"), QStringLiteral("100%")});
  combo->setCurrentIndex(4);
  return combo;
}

}  // namespace

// --- 播放 -------------------------------------------------------------------

PlaybackBar::PlaybackBar(UiManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  build_ui();
  refresh_settings_widgets();
}

void PlaybackBar::seek_to_slider(int value) {
  auto* settings = manager_->settings_panel();
  if (settings == nullptr) return;
  int64_t start = 0;
  int64_t end = 1;
  settings->seek_window_ms(start, end);
  (void)end;
  const int64_t target = start + static_cast<int64_t>(value);
  if (seek_->isSliderDown() && last_scrub_ms_ && *last_scrub_ms_ == target) return;
  last_scrub_ms_ = target;
  // Queue only — poll() commits on the next preview tick, same as the
  // legacy Slider::on_change path. Do not latch audio.position() or
  // rewrite play_anchor_ms_ here; that was the pause-to-start creep.
  manager_->chart_preview().transport().request_seek_ms(target);
}

void PlaybackBar::build_ui() {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  auto* content = new QWidget;
  auto* block = new QWidget(content);
  block->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
  auto* box = new QVBoxLayout(block);
  box->setContentsMargins(0, 0, 0, 0);
  box->setSpacing(0);

  seek_ = new QSlider(Qt::Horizontal, block);
  seek_->setRange(0, 1);
  seek_->setMinimumWidth(120);
  play_ = new QPushButton(tr("播放"), block);
  stop_ = new QPushButton(tr("回到开头"), block);
  play_icon_ = fluent_icon(fluent::Play);
  pause_icon_ = fluent_icon(fluent::Pause);
  if (play_icon_.isNull()) play_icon_ = style()->standardIcon(QStyle::SP_MediaPlay);
  if (pause_icon_.isNull()) pause_icon_ = style()->standardIcon(QStyle::SP_MediaPause);
  auto previous = fluent_icon(fluent::Previous);
  if (previous.isNull()) previous = style()->standardIcon(QStyle::SP_MediaSkipBackward);
  play_->setIcon(play_icon_);
  stop_->setIcon(previous);
  for (auto* button : {play_, stop_}) {
    button->setToolTip(button->text());
    button->setAccessibleName(button->text());
    button->setText({});
    apply_square_icon_button(button, 32, 20);
  }
  auto* transport = new QWidget(block);
  auto* transport_row = new QHBoxLayout(transport);
  transport_row->setContentsMargins(0, 0, 0, 0);
  transport_row->setSpacing(6);
  transport_row->addWidget(play_);
  transport_row->addWidget(stop_);
  auto* seek_row = new QWidget(block);
  auto* seek_layout = new QHBoxLayout(seek_row);
  seek_layout->setContentsMargins(0, 0, 0, 0);
  seek_layout->setSpacing(kLabelFieldGap);
  seek_layout->addWidget(seek_, 1);
  seek_layout->addWidget(transport, 0);
  seek_row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  auto* mix = new QWidget(block);
  mix->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  auto* mix_row = new QHBoxLayout(mix);
  mix_row->setContentsMargins(0, 0, 0, 0);
  mix_row->setSpacing(kLabelFieldGap);
  music_volume_ = make_volume_combo(mix);
  apply_compact_field(music_volume_, kVolumeFieldW);
  auto* music_mute_row = new CaptionCheckRow(tr("静音"), mix, false);
  music_mute_ = music_mute_row->box();
  sfx_volume_ = make_volume_combo(mix);
  apply_compact_field(sfx_volume_, kVolumeFieldW);
  auto* sfx_mute_row = new CaptionCheckRow(tr("静音"), mix, false);
  sfx_mute_ = sfx_mute_row->box();
  rate_ = new QComboBox(mix);
  apply_compact_field(rate_, kVolumeFieldW);
  rate_->addItems({QStringLiteral("0.25x"), QStringLiteral("0.5x"), QStringLiteral("0.75x"),
                   QStringLiteral("1x"), QStringLiteral("1.5x"), QStringLiteral("2x")});
  rate_->setCurrentText(QStringLiteral("1x"));
  mix_row->addWidget(make_hbox_group(mix, {new QLabel(tr("音乐"), mix), music_volume_, music_mute_row}));
  mix_row->addWidget(make_hbox_group(mix, {new QLabel(tr("音效"), mix), sfx_volume_, sfx_mute_row}));
  mix_row->addWidget(make_hbox_group(mix, {new QLabel(tr("播放速度"), mix), rate_}));
  box->addStretch(1);
  box->addWidget(seek_row);
  box->addStretch(1);
  box->addWidget(mix);
  box->addStretch(1);

  auto* host = new QVBoxLayout(content);
  host->setContentsMargins(kSpreadMinMargin, 0, kSpreadMinMargin, 0);
  auto* mid = new QHBoxLayout();
  mid->setContentsMargins(0, 0, 0, 0);
  mid->addStretch(1);
  mid->addWidget(block, 0);
  mid->addStretch(1);
  host->addLayout(mid, 1);

  auto* scroll = new FillScrollArea(this);
  scroll->setWidget(content);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(scroll);

  // Live scrub like the legacy interaction Slider (on_change while dragging).
  // valueChanged covers drag, groove click, and key/page steps. sync_position()
  // must keep QSignalBlocker around setValue — an unblocked sync seek was the
  // pause-to-start forward-creep (each pause latched a slightly later ms).
  connect(seek_, &QSlider::sliderPressed, this, [this] {
    journal_menu_action("playback.seek");
    last_scrub_ms_.reset();
  });
  connect(seek_, &QSlider::valueChanged, this, [this](int value) { seek_to_slider(value); });
  connect(seek_, &QSlider::actionTriggered, this, [this](int action) {
    if (action == QAbstractSlider::SliderMove || seek_->isSliderDown()) return;
    journal_menu_action("playback.seek");
  });
  connect(play_, &QPushButton::clicked, this, [this] {
    journal_menu_action("playback.play");
    manager_->toggle_playback(false);
    apply_play_icon(manager_->playback_intends_playing());
  });
  connect(stop_, &QPushButton::clicked, this, [this] {
    journal_menu_action("playback.stop");
    manager_->chart_preview().reset_playback();
    apply_play_icon(manager_->playback_intends_playing());
  });
  const auto apply_music = [this] {
    if (syncing_) return;
    if (auto* settings = manager_->settings_panel()) {
      settings->set_music_state_from_qt(volume_pct_from_combo(music_volume_) / 100.0f,
                                        music_mute_->isChecked());
      manager_->request_save_ui_config(false);
    }
  };
  const auto apply_sfx = [this] {
    if (syncing_) return;
    if (auto* settings = manager_->settings_panel()) {
      settings->set_sfx_state_from_qt(volume_pct_from_combo(sfx_volume_) / 100.0f,
                                      sfx_mute_->isChecked());
      manager_->request_save_ui_config(false);
    }
  };
  connect(music_volume_, &QComboBox::textActivated, this, [this, apply_music](const QString&) {
    journal_menu_action("playback.music_volume");
    if (syncing_) return;
    music_mute_->setChecked(false);
    apply_music();
  });
  connect(music_mute_, &QCheckBox::toggled, this, [this, apply_music](bool) {
    if (!syncing_) journal_menu_action("playback.music_mute");
    apply_music();
  });
  connect(sfx_volume_, &QComboBox::textActivated, this, [this, apply_sfx](const QString&) {
    journal_menu_action("playback.sfx_volume");
    if (syncing_) return;
    sfx_mute_->setChecked(false);
    apply_sfx();
  });
  connect(sfx_mute_, &QCheckBox::toggled, this, [this, apply_sfx](bool) {
    if (!syncing_) journal_menu_action("playback.sfx_mute");
    apply_sfx();
  });
  connect(rate_, &QComboBox::textActivated, this, [this](const QString&) {
    if (syncing_) return;
    journal_menu_action("playback.rate");
    if (auto* settings = manager_->settings_panel()) {
      settings->set_playback_rate_from_qt(rate_from_combo(rate_));
      manager_->request_save_ui_config(true);
    }
  });
}

void PlaybackBar::apply_play_icon(bool playing) {
  const QString action = playing ? tr("暂停") : tr("播放");
  if (play_->toolTip() != action) {
    play_->setIcon(playing ? pause_icon_ : play_icon_);
    play_->setToolTip(action);
    play_->setAccessibleName(action);
  }
}

void PlaybackBar::sync_position() {
  auto& transport = manager_->chart_preview().transport();
  apply_play_icon(transport.intends_playing());
  if (!seek_->isSliderDown()) {
    last_scrub_ms_.reset();
    if (auto* settings = manager_->settings_panel()) {
      int64_t start = 0;
      int64_t end = 1;
      settings->seek_window_ms(start, end);
      const int64_t span64 = std::max<int64_t>(end - start, 1);
      const int span = static_cast<int>(
          std::min<int64_t>(span64, static_cast<int64_t>(std::numeric_limits<int>::max())));
      const int64_t pos = std::clamp<int64_t>(transport.committed_ms() - start, 0, span64);
      const QSignalBlocker blocker(seek_);
      if (seek_->maximum() != span) {
        seek_->setRange(0, span);
        seek_->setSingleStep(std::max(1, span / 1000));
        seek_->setPageStep(std::max(1, span / 100));
      }
      seek_->setValue(static_cast<int>(std::min<int64_t>(pos, static_cast<int64_t>(span))));
    }
  }
}

void PlaybackBar::refresh_settings_widgets() {
  if (manager_ == nullptr) return;
  syncing_ = true;
  if (auto* settings = manager_->settings_panel()) {
    if (!music_volume_->hasFocus()) {
      const QSignalBlocker blocker(music_volume_);
      music_volume_->setCurrentIndex(
          nearest_volume_index(static_cast<int>(std::lround(settings->music_gain() * 100.0f))));
    }
    if (!sfx_volume_->hasFocus()) {
      const QSignalBlocker blocker(sfx_volume_);
      sfx_volume_->setCurrentIndex(
          nearest_volume_index(static_cast<int>(std::lround(settings->sfx_gain() * 100.0f))));
    }
    {
      const QSignalBlocker m(music_mute_);
      music_mute_->setChecked(settings->music_muted());
      const QSignalBlocker s(sfx_mute_);
      sfx_mute_->setChecked(settings->sfx_muted());
    }
    if (!rate_->hasFocus()) {
      const QSignalBlocker blocker(rate_);
      rate_->setCurrentText(format_rate(settings->playback_rate()));
    }
  }
  syncing_ = false;
}

// --- 转换 -------------------------------------------------------------------

ConvertBar::ConvertBar(UiManager* manager, const std::string& skins_dir, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  build_ui(skins_dir);
  sync_place_checks();
}

void ConvertBar::build_ui(const std::string& skins_dir) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(6, 4, 6, 4);
  row->setSpacing(0);
  const auto icons = build_convert_note_icons(skins_dir);
  for (std::size_t i = 0; i < kConvertSpecs.size(); ++i) {
    if (i > 0) {
      auto* gap = new QWidget(this);
      gap->setFixedWidth(4);
      gap->setAttribute(Qt::WA_TransparentForMouseEvents);
      row->addWidget(gap, 0);
    }
    auto* button = new QToolButton(this);
    button->setIcon(icons[i]);
    button->setIconSize(QSize(36, 36));
    button->setMinimumSize(40, 40);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setCheckable(true);
    button->setAutoRaise(true);
    convert_buttons_[i] = button;
    row->addWidget(button, 1);
    connect(button, &QToolButton::clicked, this, [this, i] {
      journal_menu_action(kConvertJournalIds[i]);
      if (manager_ != nullptr) manager_->activate_convert_bar_slot(static_cast<int>(i));
      sync_place_checks();
    });
  }
}

void ConvertBar::set_ribbon_mode() {
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  if (auto* row = qobject_cast<QHBoxLayout*>(layout())) {
    row->setContentsMargins(2, 0, 2, 0);
    row->setSpacing(0);
    for (int i = 0; i < row->count(); ++i) row->setStretch(i, 0);
  }
  for (auto* button : convert_buttons_) {
    if (button == nullptr) continue;
    button->setIconSize(QSize(24, 24));
    button->setMinimumSize(28, 28);
    button->setFixedSize(32, 32);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }
}

void ConvertBar::sync_place_checks() {
  auto* edit = manager_ != nullptr ? manager_->edit_panel() : nullptr;
  const bool enabled = manager_ != nullptr && manager_->new_note_place_logic();
  auto current =
      edit != nullptr ? edit->place_intent_override() : wds::interaction::PlaceIntent::None;
  if (enabled && current == wds::interaction::PlaceIntent::None) {
    current = wds::interaction::PlaceIntent::Tap;
  }
  const bool editable = manager_ != nullptr && !manager_->session().read_only();
  for (std::size_t i = 0; i < convert_buttons_.size(); ++i) {
    if (convert_buttons_[i] == nullptr) continue;
    convert_buttons_[i]->setEnabled(editable);
    const QString name = QString::fromUtf8(kConvertSpecs[i].name);
    const auto id = static_cast<wds::interaction::EditorShortcut>(
        static_cast<int>(wds::interaction::EditorShortcut::PlaceType0) +
        static_cast<int>(i));
    const auto chord = wds::interaction::editor_shortcut(id);
    const std::string chord_text = wds::interaction::format_shortcut_chord(chord);
    const QString labeled =
        enabled ? name : tr("转换为%1").arg(name);
    convert_buttons_[i]->setToolTip(
        chord_text.empty() ? labeled
                           : tr("%1（%2）").arg(labeled, QString::fromStdString(chord_text)));
    const QSignalBlocker blocker(convert_buttons_[i]);
    convert_buttons_[i]->setChecked(enabled && kPlaceIntents[i] == current);
  }
}

// --- 工具栏 -----------------------------------------------------------------

EditorToolbarWidget::EditorToolbarWidget(UiManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  build_ui();
  refresh_offset_field();
  refresh_grid_fields();
  refresh_flags();
  refresh_chart_selector();
  refresh_enabled_states();
}

void EditorToolbarWidget::build_ui() {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  auto* content = new QWidget;
  auto* stack = new EvenGapStackLayout(content);
  stack->setSizeConstraint(QLayout::SetMinimumSize);

  delay_ms_ = new QSpinBox(content);
  delay_ms_->setRange(-60000, 60000);
  delay_ms_->setSuffix(QStringLiteral(" ms"));
  delay_ms_->setKeyboardTracking(false);
  apply_toolbar_field(delay_ms_);
  const int field_h = toolbar_field_height(delay_ms_);

  chart_select_ = new QComboBox(content);
  apply_toolbar_field(chart_select_, kToolbarFieldW - kChartInnerGap - field_h);
  chart_add_ = new QPushButton(content);
  chart_add_->setFixedSize(field_h, field_h);
  apply_add_chart_icon();
  chart_add_->setToolTip(tr("增加谱面"));
  chart_add_->setAccessibleName(tr("增加谱面"));
  auto* chart_field = new QWidget(content);
  chart_field->setFixedWidth(kToolbarFieldW);
  chart_field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto* chart_row = new QHBoxLayout(chart_field);
  chart_row->setContentsMargins(0, 0, 0, 0);
  chart_row->setSpacing(kChartInnerGap);
  chart_row->addWidget(chart_select_);
  chart_row->addWidget(chart_add_);

  visible_range_ = new QSpinBox(content);
  visible_range_->setRange(1, 1000);
  visible_range_->setSingleStep(5);
  visible_range_->setKeyboardTracking(false);
  apply_toolbar_field(visible_range_);

  subdivisions_ = new QSpinBox(content);
  subdivisions_->setRange(1, 64);
  subdivisions_->setSingleStep(1);
  subdivisions_->setKeyboardTracking(false);
  apply_toolbar_field(subdivisions_);
  delay_ms_->setFixedHeight(field_h);
  visible_range_->setFixedHeight(field_h);
  chart_select_->setFixedHeight(field_h);
  subdivisions_->setFixedHeight(field_h);

  curve_fill_widget_ = new CurveFillWidget(manager_, content);
  curve_fill_widget_->set_control_height(field_h);
  curve_fill_widget_->buttons()->setFixedWidth(kToolbarFieldW);

  auto* delay_cell = make_labeled_field(content, tr("谱面延迟"), delay_ms_);
  auto* chart_cell = make_labeled_field(content, tr("谱面选择"), chart_field);
  auto* visible_cell = make_labeled_field(content, tr("可见范围"), visible_range_);
  auto* subdiv_cell = make_labeled_field(content, tr("拍内分割"), subdivisions_);
  auto* curve_cell = make_labeled_field(content, tr("曲线选择"), curve_fill_widget_->combo());
  auto* ease_cell = make_labeled_field(content, tr("缓动选择"), curve_fill_widget_->buttons());
  auto* pause_row = new CaptionCheckRow(tr("停止播放后停在当前时间"), content);
  pause_at_current_ = pause_row->box();
  auto* split_row = new CaptionCheckRow(tr("音符默认对齐分割线轨道"), content);
  split_width_follow_ = split_row->box();

  const int left_form_w = sync_min_width({delay_cell, visible_cell, curve_cell});
  const int right_form_w = sync_min_width({chart_cell, subdiv_cell, ease_cell});
  const int col_w = std::max({left_form_w, right_form_w, pause_row->sizeHint().width(),
                             split_row->sizeHint().width()});

  stack->addWidget(make_pair_row(content, delay_cell, chart_cell, col_w));
  stack->addWidget(make_pair_row(content, visible_cell, subdiv_cell, col_w));
  stack->addWidget(make_pair_row(content, curve_cell, ease_cell, col_w));
  stack->addWidget(make_pair_row(content, pause_row, split_row, col_w));

  auto* scroll = new FillScrollArea(this);
  scroll->setWidget(content);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(scroll);

  connect(delay_ms_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
    if (syncing_) return;
    journal_menu_action("toolbar.delay");
    apply_delay();
  });
  connect(visible_range_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
    if (syncing_) return;
    journal_menu_action("toolbar.visible_range");
    apply_grid();
  });
  connect(subdivisions_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
    if (syncing_) return;
    journal_menu_action("toolbar.subdivisions");
    apply_grid();
  });
  connect(chart_select_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
    journal_menu_action("toolbar.chart_select");
    auto& session = manager_->session();
    if (index >= 0 && static_cast<std::size_t>(index) < session.chart_count())
      session.switch_chart(static_cast<std::size_t>(index));
  });
  connect(chart_add_, &QPushButton::clicked, this, [this] {
    journal_menu_action("toolbar.chart_add");
    if (on_add_chart_) on_add_chart_();
  });
  connect(pause_at_current_, &QCheckBox::toggled, this, [this](bool checked) {
    if (syncing_) return;
    journal_menu_action("toolbar.pause_at_current");
    if (auto* edit = manager_->edit_panel()) {
      edit->set_pause_at_current(checked);
      manager_->request_save_ui_config(true);
    }
  });
  connect(split_width_follow_, &QCheckBox::toggled, this, [this](bool checked) {
    if (syncing_) return;
    journal_menu_action("toolbar.split_width_follow");
    if (auto* edit = manager_->edit_panel()) {
      edit->set_split_width_follow(checked);
      manager_->request_save_ui_config(true);
    }
  });
}

void EditorToolbarWidget::apply_add_chart_icon() {
  if (chart_add_ == nullptr) return;
  const int side = std::max(1, chart_add_->width());
  const int icon_px = std::max(12, side - 8);
  chart_add_->setIcon(themed_named_icon("add", {}, icon_px));
  chart_add_->setIconSize(QSize(icon_px, icon_px));
}

void EditorToolbarWidget::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event != nullptr && (event->type() == QEvent::PaletteChange ||
                           event->type() == QEvent::ApplicationPaletteChange)) {
    apply_add_chart_icon();
  }
}

void EditorToolbarWidget::apply_delay() {
  auto& session = manager_->session();
  const int64_t value = std::clamp<int64_t>(delay_ms_->value(), -60000, 60000);
  if (value == session.offset_ms()) return;
  if (!session.set_offset_ms(value)) {
    manager_->set_status("谱面延迟修改被拒绝：会把音符移到时间轴之前", StatusLevel::Error);
    if (auto* edit = manager_->edit_panel())
      edit->flash_offset_violations(session.last_offset_violation_ids());
  }
}

void EditorToolbarWidget::apply_grid() {
  auto* edit = manager_->edit_panel();
  if (edit == nullptr) return;
  auto grid = edit->viewport().grid();
  grid.visible_hectoms = std::clamp(visible_range_->value(), 1, 1000);
  grid.subdivisions_per_beat = std::clamp(subdivisions_->value(), 1, 64);
  edit->set_grid(grid);
  manager_->request_save_ui_config(true);
}

void EditorToolbarWidget::refresh_offset_field() {
  if (manager_ == nullptr || delay_ms_ == nullptr) return;
  syncing_ = true;
  auto& session = manager_->session();
  if (!delay_ms_->hasFocus()) {
    const QSignalBlocker blocker(delay_ms_);
    delay_ms_->setValue(static_cast<int>(
        std::clamp<int64_t>(session.offset_ms(), int64_t{-60000}, int64_t{60000})));
  }
  syncing_ = false;
}

void EditorToolbarWidget::refresh_grid_fields() {
  if (manager_ == nullptr) return;
  syncing_ = true;
  if (auto* edit = manager_->edit_panel()) {
    const auto& grid = edit->viewport().grid();
    if (visible_range_ != nullptr && !visible_range_->hasFocus()) {
      const QSignalBlocker blocker(visible_range_);
      visible_range_->setValue(grid.visible_hectoms);
    }
    if (subdivisions_ != nullptr && !subdivisions_->hasFocus()) {
      const QSignalBlocker blocker(subdivisions_);
      subdivisions_->setValue(grid.subdivisions_per_beat);
    }
  }
  syncing_ = false;
}

void EditorToolbarWidget::refresh_flags() {
  if (manager_ == nullptr || pause_at_current_ == nullptr || split_width_follow_ == nullptr) {
    return;
  }
  syncing_ = true;
  if (auto* edit = manager_->edit_panel()) {
    const QSignalBlocker p(pause_at_current_);
    pause_at_current_->setChecked(edit->pause_at_current());
    const QSignalBlocker w(split_width_follow_);
    split_width_follow_->setChecked(edit->split_width_follow());
  }
  syncing_ = false;
}

void EditorToolbarWidget::refresh_chart_selector() {
  if (manager_ == nullptr || chart_select_ == nullptr) return;
  auto& session = manager_->session();
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
}

void EditorToolbarWidget::refresh_enabled_states() {
  if (manager_ == nullptr) return;
  auto& session = manager_->session();
  const bool editable = !session.read_only();
  if (delay_ms_ != nullptr) delay_ms_->setEnabled(session.delay_editable());
  if (chart_select_ != nullptr) {
    chart_select_->setEnabled(editable || session.chart_count() > 1);
  }
  if (chart_add_ != nullptr) chart_add_->setEnabled(editable);
}

void EditorToolbarWidget::refresh_curve_controls() {
  if (curve_fill_widget_ != nullptr) curve_fill_widget_->refresh_curve_controls();
}

// --- 曲线填充 ---------------------------------------------------------------

CurveFillWidget::CurveFillWidget(UiManager* manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
  hide();
  setMaximumSize(0, 0);
  curve_template_ = new QComboBox(parent);
  apply_toolbar_field(curve_template_);
  buttons_ = new QWidget(parent);
  const int side = (kToolbarFieldW - kEaseButtonGap * 3) / 4;
  buttons_->setFixedSize(kToolbarFieldW, side);
  buttons_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto* buttons_row = new QHBoxLayout(buttons_);
  buttons_row->setContentsMargins(0, 0, 0, 0);
  buttons_row->setSpacing(kEaseButtonGap);
  for (int i = 0; i < 4; ++i) {
    auto* button = new QToolButton(buttons_);
    button->setText(QString::fromUtf8(kCurveDirectionLabels[static_cast<std::size_t>(i)]));
    button->setCheckable(true);
    button->setAutoRaise(false);
    button->setFixedSize(side, side);
    QFont font = button->font();
    font.setPointSize(std::max(13, font.pointSize() + 3));
    button->setFont(font);
    button->setStyleSheet(QStringLiteral("QToolButton { padding: 0px; }"));
    curve_directions_[static_cast<std::size_t>(i)] = button;
    buttons_row->addWidget(button, 0);
    connect(button, &QToolButton::clicked, this, [this, i] {
      journal_menu_action(kCurveEaseJournalIds[static_cast<std::size_t>(i)]);
      curve_controller_.select_direction_index(manager_->curve_template_state(), i);
      refresh_curve_controls();
    });
  }
  connect(curve_template_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
    journal_menu_action("curve.select");
    curve_controller_.select_dropdown_index(manager_->curve_template_state(), index);
    refresh_curve_controls();
  });
  curve_controller_.set_on_changed([this](const CurveFillSelection&) {
    manager_->push_curve_fill_selection_from_qt();
    manager_->request_save_ui_config(true);
  });
  refresh_curve_controls();
}

void CurveFillWidget::set_control_height(int height) {
  if (curve_template_ != nullptr) curve_template_->setFixedHeight(std::max(1, height));
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

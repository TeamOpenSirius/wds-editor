#include "wds/ui/qt/settings_dialog.hpp"

#include "wds/ui/ui_manager.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/qt/wds_theme.hpp"
#include "wds/common/crash_handler.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>

#include <wds/interaction/platform.hpp>
#include <wds/interaction/shortcuts.hpp>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEnterEvent>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPainter>
#include <array>
#include <utility>
#include <QPushButton>
#include <QFont>
#include <QFontMetrics>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace wds::ui {
namespace {

using wds::interaction::KeyCode;
using wds::interaction::ShortcutChord;

int chord_to_qt_key(const ShortcutChord& chord) {
  const int v = static_cast<int>(chord.key);
  if (v >= '0' && v <= '9') return Qt::Key_0 + (v - '0');
  if (v >= 'A' && v <= 'Z') return Qt::Key_A + (v - 'A');
  switch (chord.key) {
    case KeyCode::Space: return Qt::Key_Space;
    case KeyCode::Escape: return Qt::Key_Escape;
    case KeyCode::Enter: return Qt::Key_Return;
    case KeyCode::Tab: return Qt::Key_Tab;
    case KeyCode::Backspace: return Qt::Key_Backspace;
    case KeyCode::Delete: return Qt::Key_Delete;
    case KeyCode::Left: return Qt::Key_Left;
    case KeyCode::Right: return Qt::Key_Right;
    case KeyCode::Up: return Qt::Key_Up;
    case KeyCode::Down: return Qt::Key_Down;
    case KeyCode::F1: return Qt::Key_F1;
    case KeyCode::F2: return Qt::Key_F2;
    case KeyCode::F3: return Qt::Key_F3;
    case KeyCode::F4: return Qt::Key_F4;
    case KeyCode::F11: return Qt::Key_F11;
    default: break;
  }
  if (v == '.') return Qt::Key_Period;
  return 0;
}

KeyCode qt_key_to_chord_key(int key) {
  if (key >= Qt::Key_0 && key <= Qt::Key_9)
    return static_cast<KeyCode>('0' + key - Qt::Key_0);
  if (key >= Qt::Key_A && key <= Qt::Key_Z)
    return static_cast<KeyCode>('A' + key - Qt::Key_A);
  switch (key) {
    case Qt::Key_Space: return KeyCode::Space;
    case Qt::Key_Escape: return KeyCode::Escape;
    case Qt::Key_Return:
    case Qt::Key_Enter: return KeyCode::Enter;
    case Qt::Key_Tab: return KeyCode::Tab;
    case Qt::Key_Backspace: return KeyCode::Backspace;
    case Qt::Key_Delete: return KeyCode::Delete;
    case Qt::Key_Left: return KeyCode::Left;
    case Qt::Key_Right: return KeyCode::Right;
    case Qt::Key_Up: return KeyCode::Up;
    case Qt::Key_Down: return KeyCode::Down;
    case Qt::Key_F1: return KeyCode::F1;
    case Qt::Key_F2: return KeyCode::F2;
    case Qt::Key_F3: return KeyCode::F3;
    case Qt::Key_F4: return KeyCode::F4;
    case Qt::Key_F11: return KeyCode::F11;
    case Qt::Key_Period: return static_cast<KeyCode>('.');
    default: return KeyCode::Unknown;
  }
}

QKeySequence chord_to_sequence(const ShortcutChord& chord) {
  const int key = chord_to_qt_key(chord);
  if (key == 0) return {};
  Qt::KeyboardModifiers mods;
  if (chord.mods.shift) mods |= Qt::ShiftModifier;
  if (chord.mods.control) mods |= Qt::ControlModifier;
  if (chord.mods.alt) mods |= Qt::AltModifier;
  if (chord.mods.super) mods |= Qt::MetaModifier;
  return QKeySequence(QKeyCombination(mods, static_cast<Qt::Key>(key)));
}

ShortcutChord sequence_to_chord(const QKeySequence& sequence) {
  ShortcutChord chord;
  if (sequence.isEmpty()) return chord;
  const QKeyCombination combo = sequence[0];
  chord.key = qt_key_to_chord_key(combo.key());
  if (chord.key == KeyCode::Unknown ||
      wds::interaction::is_forbidden_shortcut_key(chord.key)) {
    return {};
  }
  const auto mods = combo.keyboardModifiers();
  chord.mods.shift = mods.testFlag(Qt::ShiftModifier);
  chord.mods.control = mods.testFlag(Qt::ControlModifier);
  chord.mods.alt = mods.testFlag(Qt::AltModifier);
  chord.mods.super = mods.testFlag(Qt::MetaModifier);
  return chord;
}

QComboBox* make_combo(const QStringList& items, QWidget* parent) {
  auto* combo = new QComboBox(parent);
  combo->addItems(items);
  combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  return combo;
}

class LabelToggleFilter final : public QObject {
 public:
  LabelToggleFilter(QCheckBox* box, QObject* parent) : QObject(parent), box_(box) {}

 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::MouseButtonRelease && box_ != nullptr) {
      box_->toggle();
      return true;
    }
    return false;
  }

 private:
  QCheckBox* box_ = nullptr;
};

QCheckBox* add_wrapping_check(QLayout* layout, const QString& text, QWidget* parent) {
  auto* box = new QCheckBox(parent);
  box->setText(QString());
  box->setAccessibleName(text);
  const int side = std::max({box->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, box),
                             box->style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, box), 16});
  box->setFixedSize(side, side);
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
  label->setCursor(Qt::PointingHandCursor);
  label->installEventFilter(new LabelToggleFilter(box, label));
  auto* row = new QWidget(parent);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(6);
  h->addWidget(box, 0, Qt::AlignTop);
  h->addWidget(label, 1);
  layout->addWidget(row);
  return box;
}

constexpr int kNavCollapsedW = 16;
constexpr int kNavExpandedW = 104;
constexpr int kNavItemH = 26;
constexpr int kSettingsSectionCount = 8;

std::array<QString, kSettingsSectionCount> settings_section_titles() {
  return {
      QCoreApplication::translate("wds::ui", "外观"),
      QCoreApplication::translate("wds::ui", "文件"),
      QCoreApplication::translate("wds::ui", "音频"),
      QCoreApplication::translate("wds::ui", "输入"),
      QCoreApplication::translate("wds::ui", "显示"),
      QCoreApplication::translate("wds::ui", "宽度"),
      QCoreApplication::translate("wds::ui", "快捷键"),
      QCoreApplication::translate("wds::ui", "隐私"),
  };
}

constexpr std::array<const char*, kSettingsSectionCount> kSettingsTabJournalIds = {{
    "settings.tab_appearance",
    "settings.tab_file",
    "settings.tab_audio",
    "settings.tab_input",
    "settings.tab_display",
    "settings.tab_width",
    "settings.tab_shortcuts",
    "settings.tab_privacy",
}};

QColor nav_rail_fill(const QPalette& palette) {
  const QColor window = palette.color(QPalette::Window);
  const QColor alt = palette.color(QPalette::AlternateBase);
  if (std::abs(alt.lightness() - window.lightness()) >= 14) return alt;
  return window.lightness() > 128 ? window.darker(112) : window.lighter(128);
}

class NavDotButton final : public QAbstractButton {
 public:
  explicit NavDotButton(const QString& label, QWidget* parent) : QAbstractButton(parent) {
    setText(label);
    setCheckable(true);
    setAutoExclusive(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(label);
    setAccessibleName(label);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(kNavItemH);
  }

  void set_expanded(bool expanded) {
    expanded_ = expanded;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect r = rect();
    const bool on = isChecked();
    if (on) {
      const int cx = expanded_ ? kNavCollapsedW / 2 : r.center().x();
      painter.setPen(Qt::NoPen);
      painter.setBrush(palette().highlight().color());
      painter.drawEllipse(QPoint(cx, r.center().y()), 3, 3);
    }
    if (expanded_) {
      QColor ink = palette().text().color();
      if (on || underMouse()) ink = palette().highlight().color();
      painter.setPen(ink);
      painter.drawText(QRect(kNavCollapsedW + 4, 0, r.width() - kNavCollapsedW - 10, r.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, text());
    }
  }

 private:
  bool expanded_ = false;
};

class SettingsNavRail final : public QFrame {
 public:
  explicit SettingsNavRail(QWidget* parent = nullptr) : QFrame(parent) {
    setObjectName(QStringLiteral("settingsNavRail"));
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(false);
    setMouseTracking(true);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    const auto labels = settings_section_titles();
    for (int i = 0; i < kSettingsSectionCount; ++i) {
      auto* button = new NavDotButton(labels[static_cast<std::size_t>(i)], this);
      group_->addButton(button, i);
      root->addWidget(button);
      connect(button, &QAbstractButton::clicked, this, [this, i] {
        journal_menu_action(kSettingsTabJournalIds[static_cast<std::size_t>(i)]);
        if (on_row_changed_) on_row_changed_(i);
      });
    }
    root->addStretch(1);
    set_expanded(false);
    if (auto* first = group_->button(0)) first->setChecked(true);
  }

  void set_on_row_changed(std::function<void(int)> handler) {
    on_row_changed_ = std::move(handler);
  }

  void set_on_geometry_changed(std::function<void()> handler) {
    on_geometry_changed_ = std::move(handler);
  }

  void set_current_row(int row) {
    if (auto* button = group_->button(row)) button->setChecked(true);
  }

  int width_for_state() const { return expanded_ ? kNavExpandedW : kNavCollapsedW; }

 protected:
  void enterEvent(QEnterEvent* event) override {
    QFrame::enterEvent(event);
    set_expanded(true);
  }

  void leaveEvent(QEvent* event) override {
    QFrame::leaveEvent(event);
    set_expanded(false);
  }

  void paintEvent(QPaintEvent* event) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor fill = nav_rail_fill(palette());
    if (expanded_) fill.setAlpha(242);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRect(rect());
    painter.setPen(QPen(palette().mid(), 1));
    painter.drawLine(width() - 1, 0, width() - 1, height());
    QFrame::paintEvent(event);
  }

 private:
  void set_expanded(bool expanded) {
    if (expanded_ == expanded) return;
    expanded_ = expanded;
    for (auto* button : group_->buttons()) {
      static_cast<NavDotButton*>(button)->set_expanded(expanded);
    }
    setFixedWidth(width_for_state());
    if (on_geometry_changed_) on_geometry_changed_();
    update();
  }

  QButtonGroup* group_ = nullptr;
  std::function<void(int)> on_row_changed_;
  std::function<void()> on_geometry_changed_;
  bool expanded_ = true;
};

void wrap_form(QFormLayout* form) {
  form->setContentsMargins(0, 0, 0, 0);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  // Word-wrapped labels report height-for-width; QFormLayout then top-aligns
  // them against taller combos/spin boxes. Stretch the label to the field
  // height and keep both roles vertically centered.
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  for (int i = 0; i < form->rowCount(); ++i) {
    auto* label_item = form->itemAt(i, QFormLayout::LabelRole);
    auto* field_item = form->itemAt(i, QFormLayout::FieldRole);
    auto* label = label_item != nullptr ? qobject_cast<QLabel*>(label_item->widget()) : nullptr;
    if (label == nullptr) continue;
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    if (QWidget* field = field_item != nullptr ? field_item->widget() : nullptr) {
      field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
      const int h = field->sizeHint().height();
      if (h > 0) label->setMinimumHeight(h);
    }
  }
}

}  // namespace

SettingsPanel::SettingsPanel(UiManager* manager, QString theme_dir, QWidget* parent)
    : QWidget(parent), manager_(manager), theme_dir_(std::move(theme_dir)) {
  manager_->snapshot_ui_config_for_qt(cfg_);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 6, 0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  scroll_ = new QScrollArea(this);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* body = new QHBoxLayout;
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);
  auto* rail_pad = new QWidget(this);
  rail_pad->setFixedWidth(kNavCollapsedW + 6);
  rail_pad->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  body->addWidget(rail_pad);
  body->addWidget(scroll_, 1);
  root->addLayout(body, 1);

  auto* rail = new SettingsNavRail(this);
  sidebar_ = rail;
  rail->set_on_row_changed([this](int row) { jump_to_section(row); });
  rail->set_on_geometry_changed([this] { layout_nav_rail(); });

  build_pages();
  load_from_config();
  rail->set_current_row(0);
  layout_nav_rail();
  connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
    sync_nav_from_scroll();
  });
  connect(scroll_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
    update_section_scroll_pad();
  });
}

void SettingsPanel::layout_nav_rail() {
  auto* rail = static_cast<SettingsNavRail*>(sidebar_);
  if (rail == nullptr) return;
  rail->setGeometry(0, 0, rail->width_for_state(), std::max(1, height()));
  rail->raise();
}

void SettingsPanel::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  layout_nav_rail();
  update_section_scroll_pad();
  sync_nav_from_scroll();
}

void SettingsPanel::jump_to_section(int row) {
  if (scroll_ == nullptr || row < 0 || row >= kSettingsSectionCount) return;
  auto* section = sections_[static_cast<std::size_t>(row)];
  auto* content = scroll_->widget();
  if (section == nullptr || content == nullptr) return;
  jumping_section_ = true;
  const int y = std::max(0, section->mapTo(content, QPoint(0, 0)).y());
  scroll_->verticalScrollBar()->setValue(y);
  static_cast<SettingsNavRail*>(sidebar_)->set_current_row(row);
  jumping_section_ = false;
}

void SettingsPanel::sync_nav_from_scroll() {
  if (jumping_section_ || sidebar_ == nullptr || scroll_ == nullptr) return;
  auto* content = scroll_->widget();
  if (content == nullptr) return;
  const int y = scroll_->verticalScrollBar()->value();
  int row = 0;
  for (int i = 0; i < kSettingsSectionCount; ++i) {
    auto* section = sections_[static_cast<std::size_t>(i)];
    if (section == nullptr) continue;
    if (section->mapTo(content, QPoint(0, 0)).y() <= y + 8) row = i;
  }
  static_cast<SettingsNavRail*>(sidebar_)->set_current_row(row);
}

void SettingsPanel::update_section_scroll_pad() {
  if (scroll_ == nullptr || scroll_pad_ == nullptr || jumping_section_) return;
  auto* last = sections_[static_cast<std::size_t>(kSettingsSectionCount - 1)];
  auto* content = scroll_->widget();
  if (last == nullptr || content == nullptr) return;
  const int view_h = scroll_->viewport()->height();
  const int without_pad = std::max(0, content->height() - scroll_pad_->height());
  const int pad = without_pad > view_h ? std::max(0, view_h - last->height()) : 0;
  if (scroll_pad_->height() != pad) scroll_pad_->setFixedHeight(pad);
}

void SettingsPanel::build_pages() {
  auto* content = new QWidget;
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(4, 4, 16, 8);
  layout->setSpacing(0);

  const auto titles = settings_section_titles();
  auto section_at = [&](int index) -> QVBoxLayout* {
    auto* section = new QWidget(content);
    auto* v = new QVBoxLayout(section);
    v->setContentsMargins(0, index == 0 ? 10 : 14, 4, 8);
    v->setSpacing(8);
    if (index > 0) {
      auto* line = new QFrame(section);
      line->setFrameShape(QFrame::HLine);
      line->setFrameShadow(QFrame::Plain);
      line->setFixedHeight(1);
      v->addWidget(line);
    }
    auto* heading = new QLabel(titles[static_cast<std::size_t>(index)], section);
    QFont font = heading->font();
    font.setBold(true);
    font.setWeight(QFont::Bold);
    font.setPointSizeF(std::max(16.0, static_cast<double>(font.pointSizeF()) + 5.0));
    heading->setFont(font);
    v->addWidget(heading);
    sections_[static_cast<std::size_t>(index)] = section;
    layout->addWidget(section);
    return v;
  };

  auto* appearance = section_at(0);
  auto* appearance_host = appearance->parentWidget();
  theme_combo_ = new QComboBox(appearance_host);
  theme_combo_->addItem(tr("浅色"), QStringLiteral("light"));
  theme_combo_->addItem(tr("深色"), QStringLiteral("dark"));
  theme_combo_->addItem(tr("跟随系统"), QStringLiteral("system"));
  const QString current = normalize_theme_preference(
      QSettings("WDS", "WDS Editor").value("appearance/theme").toString());
  int theme_index = theme_combo_->findData(current);
  if (theme_index < 0) theme_index = theme_combo_->findData(QStringLiteral("system"));
  theme_combo_->setCurrentIndex(std::max(0, theme_index));
  auto* appearanceForm = new QFormLayout;
  appearanceForm->addRow(tr("主题"), theme_combo_);
  wrap_form(appearanceForm);
  appearance->addLayout(appearanceForm);

  auto* file = section_at(1);
  sus_auto_convert_ =
      add_wrapping_check(file, tr("导入 sus 谱面时自动转换"), file->parentWidget());

  auto* audio = section_at(2);
  mute_hold_body_sfx_ =
      add_wrapping_check(audio, tr("关闭 Hold 体音效播放"), audio->parentWidget());

  auto* input = section_at(3);
  auto* input_host = input->parentWidget();
  invert_scroll_wheel_ = add_wrapping_check(input, tr("反转时间轴滚轮方向"), input_host);
  invert_visible_range_scroll_ =
      add_wrapping_check(input, tr("反转滚轮调节可见范围大小方向"), input_host);
  new_note_place_logic_ = add_wrapping_check(input, tr("启用类Ched放置逻辑"), input_host);
  auto* speedForm = new QFormLayout;
  scroll_wheel_speed_ = make_combo({"0.25x", "0.5x", "0.75x", "1x", "1.25x", "1.5x", "1.75x",
                                    "2x", "2.5x", "3x"},
                                   input_host);
  speedForm->addRow(tr("时间轴滚轮速度"), scroll_wheel_speed_);
  wrap_form(speedForm);
  input->addLayout(speedForm);

  auto* display = section_at(4);
  auto* display_host = display->parentWidget();
  show_judgment_text_ = add_wrapping_check(display, tr("开启判定文字显示"), display_host);
  auto* form = new QFormLayout;
  note_speed_ = new QDoubleSpinBox(display_host);
  note_speed_->setRange(1.0, 15.0);
  note_speed_->setDecimals(1);
  note_speed_->setSingleStep(0.5);
  form->addRow(tr("流速"), note_speed_);
  note_start_offset_ = new QSpinBox(display_host);
  note_start_offset_->setRange(0, 100);
  note_start_offset_->setSingleStep(5);
  form->addRow(tr("挡板高度"), note_start_offset_);
  note_height_level_ = new QSpinBox(display_host);
  note_height_level_->setRange(1, 10);
  form->addRow(tr("note厚度"), note_height_level_);
  split_line_opacity_ = new QSpinBox(display_host);
  split_line_opacity_->setRange(10, 100);
  split_line_opacity_->setSingleStep(10);
  form->addRow(tr("分割线透明度"), split_line_opacity_);
  spectrum_display_ = make_combo({tr("无"), tr("包络图"), tr("频率频谱图")}, display_host);
  form->addRow(tr("频谱显示"), spectrum_display_);
  msaa_samples_ = make_combo({tr("低"), tr("中"), tr("高")}, display_host);
  form->addRow(tr("抗锯齿"), msaa_samples_);
  wrap_form(form);
  display->addLayout(form);

  auto* width = section_at(5);
  auto* width_host = width->parentWidget();
  auto* widthLayout = new QFormLayout;
  static constexpr const char* kSlotLabels[6] = {"一档", "二档", "三档",
                                                 "四档", "五档", "六档"};
  for (int i = 0; i < 6; ++i) {
    auto* box = new QSpinBox(width_host);
    box->setRange(1, 12);
    box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    width_slots_[static_cast<std::size_t>(i)] = box;
    widthLayout->addRow(QString::fromUtf8(kSlotLabels[i]), box);
  }
  wrap_form(widthLayout);
  width->addLayout(widthLayout);

  auto* shortcuts_box = section_at(6);
  auto* shortcuts_host = shortcuts_box->parentWidget();
  auto* shortcutsLayout = new QGridLayout;
  shortcutsLayout->setContentsMargins(0, 0, 0, 0);
  shortcutsLayout->setColumnStretch(0, 1);
  shortcutsLayout->setColumnStretch(1, 0);
  shortcutsLayout->setColumnStretch(2, 0);
  const QFontMetrics shortcut_fm(font());
  const int hanzi = std::max(1, shortcut_fm.horizontalAdvance(QStringLiteral("字")));
  // Original box tracked this sample string. Subtract one hanzi so the label
  // gains that space; do not add slack (that made the field wider than start).
  const int compact_shortcut_w = std::max(
      hanzi * 4, shortcut_fm.horizontalAdvance(QStringLiteral("Ctrl+Shift+F11")) - hanzi);
  for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
    const auto id = static_cast<wds::interaction::EditorShortcut>(i);
    const int row = static_cast<int>(i);
    auto* shortcut_label = new QLabel(
        QString::fromUtf8(wds::interaction::editor_shortcut_label(id, cfg_.pause_at_current)),
        shortcuts_host);
    shortcut_label->setWordWrap(true);
    shortcut_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    shortcut_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    shortcutsLayout->addWidget(shortcut_label, row, 0, Qt::AlignVCenter);
    auto* edit = new QKeySequenceEdit(shortcuts_host);
    // QKeySequenceEdit already paints NativeText (⌘ on macOS, Ctrl on Windows).
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    edit->setMaximumSequenceLength(1);
#endif
    // Ignore QKeySequenceEdit's wide sizeHint so leftover width goes to the label.
    edit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    edit->setStyleSheet(QStringLiteral(
        "QKeySequenceEdit, QKeySequenceEdit QLineEdit { min-width: 0; }"));
    edit->setFixedWidth(compact_shortcut_w);
    shortcut_edits_[i] = edit;
    shortcutsLayout->addWidget(edit, row, 1, Qt::AlignVCenter);
    auto* clear = new QPushButton(shortcuts_host);
    auto clear_icon = fluent_icon(fluent::Clear);
    if (clear_icon.isNull()) clear_icon = style()->standardIcon(QStyle::SP_DialogCloseButton);
    clear->setIcon(clear_icon);
    clear->setIconSize(QSize(18, 18));
    clear->setFixedSize(32, 32);
    const QString clear_label = tr("清除快捷键：%1").arg(QString::fromUtf8(
        wds::interaction::editor_shortcut_label(id, cfg_.pause_at_current)));
    clear->setToolTip(clear_label);
    clear->setAccessibleName(clear_label);
    connect(clear, &QPushButton::clicked, this, [edit] {
      journal_menu_action("settings.shortcut_clear");
      edit->clear();
    });
    shortcutsLayout->addWidget(clear, row, 2, Qt::AlignVCenter);
  }
  shortcuts_box->addLayout(shortcutsLayout);

  auto* privacy = section_at(7);
  auto* privacy_host = privacy->parentWidget();
  allow_crash_log_sensitive_ = add_wrapping_check(
      privacy, tr("允许崩溃日志记录真实文本与文件路径"), privacy_host);
  auto* open_logs = new QPushButton(tr("打开崩溃日志文件夹"), privacy_host);
  connect(open_logs, &QPushButton::clicked, this, [] {
    journal_menu_action("settings.open_logs");
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromUtf8(wds::common::crash_log_directory())));
  });
  privacy->addWidget(open_logs);

  scroll_pad_ = new QWidget(content);
  scroll_pad_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  scroll_pad_->setFixedHeight(0);
  layout->addWidget(scroll_pad_);

  scroll_->setWidget(content);

  const auto live = [this] { apply_live(); };
  connect(theme_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
    apply_live();
    if (theme_combo_ == nullptr || theme_combo_->currentIndex() < 0) return;
    const QString id = theme_combo_->currentData().toString();
    QSettings prefs("WDS", "WDS Editor");
    prefs.setValue("appearance/theme", id);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
      apply_wds_theme(*app, theme_dir_, id);
    }
  });
  connect(sus_auto_convert_, &QCheckBox::toggled, this, [live](bool) { live(); });
  connect(mute_hold_body_sfx_, &QCheckBox::toggled, this, [live](bool) { live(); });
  connect(invert_scroll_wheel_, &QCheckBox::toggled, this, [live](bool) { live(); });
  connect(invert_visible_range_scroll_, &QCheckBox::toggled, this, [live](bool) { live(); });
  connect(new_note_place_logic_, &QCheckBox::toggled, this, [live](bool) { live(); });
  connect(scroll_wheel_speed_, &QComboBox::currentIndexChanged, this, [live](int) { live(); });
  connect(show_judgment_text_, &QCheckBox::toggled, this, [live](bool) { live(); });
  connect(note_speed_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [live](double) { live(); });
  connect(note_start_offset_, qOverload<int>(&QSpinBox::valueChanged), this,
          [live](int) { live(); });
  connect(note_height_level_, qOverload<int>(&QSpinBox::valueChanged), this,
          [live](int) { live(); });
  connect(split_line_opacity_, qOverload<int>(&QSpinBox::valueChanged), this,
          [live](int) { live(); });
  connect(spectrum_display_, &QComboBox::currentIndexChanged, this, [live](int) { live(); });
  connect(msaa_samples_, &QComboBox::currentIndexChanged, this, [live](int) { live(); });
  for (auto* box : width_slots_) {
    connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [live](int) { live(); });
  }
  for (auto* edit : shortcut_edits_) {
    connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [live](const QKeySequence&) {
      live();
    });
    connect(edit, &QKeySequenceEdit::editingFinished, this, [] {
      journal_menu_action("settings.shortcut_edit");
    });
  }
  connect(allow_crash_log_sensitive_, &QCheckBox::toggled, this, [this, live](bool) {
    if (!applying_) journal_menu_action("settings.privacy_toggle");
    live();
  });
}

void SettingsPanel::load_from_config() {
  applying_ = true;
  sus_auto_convert_->setChecked(cfg_.sus_auto_convert);
  mute_hold_body_sfx_->setChecked(cfg_.mute_hold_body_sfx);
  invert_scroll_wheel_->setChecked(cfg_.invert_scroll_wheel);
  invert_visible_range_scroll_->setChecked(cfg_.invert_visible_range_scroll);
  new_note_place_logic_->setChecked(cfg_.new_note_place_logic);
  {
    const QString label = QString::number(static_cast<double>(cfg_.scroll_wheel_speed)) + "x";
    int best = scroll_wheel_speed_->findText(label);
    if (best < 0) best = scroll_wheel_speed_->findText(QStringLiteral("1x"));
    scroll_wheel_speed_->setCurrentIndex(std::max(0, best));
  }
  show_judgment_text_->setChecked(cfg_.show_judgment_text);
  note_speed_->setValue(cfg_.note_speed);
  note_start_offset_->setValue(cfg_.note_start_offset);
  note_height_level_->setValue(cfg_.note_height_level);
  split_line_opacity_->setValue(cfg_.split_line_opacity);
  spectrum_display_->setCurrentIndex(static_cast<int>(cfg_.spectrum_display));
  msaa_samples_->setCurrentIndex(cfg_.msaa_samples >= 4 ? 2 : cfg_.msaa_samples >= 2 ? 1 : 0);
  for (int i = 0; i < 6; ++i) {
    width_slots_[static_cast<std::size_t>(i)]->setValue(
        cfg_.width_slots[static_cast<std::size_t>(i)]);
  }
  for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
    const auto id = static_cast<wds::interaction::EditorShortcut>(i);
    const ShortcutChord chord =
        cfg_.shortcuts_initialized ? cfg_.shortcuts[i] : wds::interaction::editor_shortcut(id);
    shortcut_edits_[i]->setKeySequence(chord_to_sequence(chord));
  }
  allow_crash_log_sensitive_->setChecked(cfg_.allow_crash_log_sensitive);
  applying_ = false;
}

int SettingsPanel::refresh_shortcut_conflicts() {
  std::unordered_map<ShortcutChord, int, wds::interaction::ShortcutChordHash> counts;
  std::array<ShortcutChord, wds::interaction::kEditorShortcutCount> chords{};
  for (std::size_t i = 0; i < chords.size(); ++i) {
    chords[i] = sequence_to_chord(shortcut_edits_[i]->keySequence());
    chords[i].mods = wds::interaction::normalize_primary(chords[i].mods);
    if (chords[i].key != KeyCode::Unknown) ++counts[chords[i]];
  }
  int first = -1;
  for (std::size_t i = 0; i < chords.size(); ++i) {
    const bool conflict = chords[i].key != KeyCode::Unknown && counts[chords[i]] > 1;
    shortcut_edits_[i]->setStyleSheet(
        conflict ? QStringLiteral(
                       "QKeySequenceEdit, QKeySequenceEdit QLineEdit { min-width: 0; }"
                       "QKeySequenceEdit { border: 1px solid #d64545; }")
                 : QStringLiteral(
                       "QKeySequenceEdit, QKeySequenceEdit QLineEdit { min-width: 0; }"));
    if (conflict && first < 0) first = static_cast<int>(i);
  }
  return first;
}

bool SettingsPanel::capture_into_config() {
  cfg_.sus_auto_convert = sus_auto_convert_->isChecked();
  cfg_.mute_hold_body_sfx = mute_hold_body_sfx_->isChecked();
  cfg_.invert_scroll_wheel = invert_scroll_wheel_->isChecked();
  cfg_.invert_visible_range_scroll = invert_visible_range_scroll_->isChecked();
  cfg_.new_note_place_logic = new_note_place_logic_->isChecked();
  {
    QString label = scroll_wheel_speed_->currentText();
    label.chop(1);
    bool ok = false;
    const float speed = label.toFloat(&ok);
    if (ok) cfg_.scroll_wheel_speed = speed;
  }
  cfg_.show_judgment_text = show_judgment_text_->isChecked();
  cfg_.note_speed = note_speed_->value();
  cfg_.note_start_offset = note_start_offset_->value();
  cfg_.note_height_level = note_height_level_->value();
  cfg_.split_line_opacity = split_line_opacity_->value();
  cfg_.spectrum_display = clamp_spectrum_display(spectrum_display_->currentIndex());
  cfg_.msaa_samples = msaa_samples_->currentIndex() == 0   ? 1
                      : msaa_samples_->currentIndex() == 2 ? 4
                                                           : 2;
  for (int i = 0; i < 6; ++i) {
    cfg_.width_slots[static_cast<std::size_t>(i)] =
        width_slots_[static_cast<std::size_t>(i)]->value();
  }
  const int conflict = refresh_shortcut_conflicts();
  if (conflict < 0) {
    for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
      cfg_.shortcuts[i] = sequence_to_chord(shortcut_edits_[i]->keySequence());
    }
    cfg_.shortcuts_initialized = true;
  }
  cfg_.allow_crash_log_sensitive = allow_crash_log_sensitive_->isChecked();
  return true;
}

void SettingsPanel::apply_live() {
  if (applying_ || manager_ == nullptr) return;
  if (!capture_into_config()) return;
  manager_->apply_ui_config_from_qt(cfg_);
  if (on_applied_) on_applied_();
}

SettingsDialog::SettingsDialog(UiManager* manager, QString theme_dir, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(tr("设置"));
  resize(760, 560);
  auto* root = new QVBoxLayout(this);
  root->addWidget(new SettingsPanel(manager, std::move(theme_dir), this), 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, [this] {
    journal_menu_action("settings.ok");
    reject();
  });
}

}  // namespace wds::ui

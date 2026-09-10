#include "wds/ui/qt/settings_dialog.hpp"

#include "wds/ui/ui_manager.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/qt/wds_theme.hpp"

#include <QApplication>
#include <QSettings>

#include <wds/interaction/platform.hpp>
#include <wds/interaction/shortcuts.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <array>
#include <utility>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

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
  return combo;
}

}  // namespace

SettingsDialog::SettingsDialog(UiManager* manager, QString theme_dir, QWidget* parent)
    : QDialog(parent), manager_(manager), theme_dir_(std::move(theme_dir)) {
  setWindowTitle(tr("设置"));
  resize(760, 560);
  manager_->snapshot_ui_config_for_qt(cfg_);

  auto* root = new QVBoxLayout(this);
  auto* body = new QHBoxLayout;
  sidebar_ = new QListWidget(this);
  const std::array<std::pair<QString, char32_t>, 8> tabs = {{
      {tr("外观"), fluent::Display},
      {tr("文件"), fluent::Files},
      {tr("音频"), fluent::Audio},
      {tr("输入"), fluent::Keyboard},
      {tr("显示"), fluent::Display},
      {tr("宽度"), fluent::Ruler},
      {tr("快捷键"), fluent::Keyboard},
      {tr("隐私"), fluent::Shield},
  }};
  for (const auto& [label, glyph] : tabs) {
    auto* item = new QListWidgetItem(fluent_icon(glyph), label, sidebar_);
    item->setSizeHint(QSize(0, 34));
  }
  sidebar_->setIconSize(QSize(18, 18));
  sidebar_->setFixedWidth(132);
  pages_ = new QStackedWidget(this);
  body->addWidget(sidebar_);
  body->addWidget(pages_, 1);
  root->addLayout(body, 1);

  auto* buttons = new QDialogButtonBox(this);
  auto* confirm = buttons->addButton(tr("确认"), QDialogButtonBox::AcceptRole);
  confirm->setIcon(fluent_icon(fluent::Accept));
  auto* cancel = buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
  cancel->setIcon(fluent_icon(fluent::Clear));
  root->addWidget(buttons);
  connect(confirm, &QPushButton::clicked, this, &SettingsDialog::try_confirm);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(sidebar_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);

  build_pages();
  load_from_config();
  sidebar_->setCurrentRow(0);
}

void SettingsDialog::build_pages() {
  // 外观 (persist on confirm, apply at next startup)
  auto* appearance = new QWidget(pages_);
  auto* appearanceForm = new QFormLayout(appearance);
  theme_combo_ = new QComboBox(appearance);
  const QString current =
      QSettings("WDS", "WDS Editor").value("appearance/theme").toString();
  for (const auto& info : available_themes(theme_dir_)) {
    theme_combo_->addItem(info.name, info.id);
    if (info.id == current) theme_combo_->setCurrentIndex(theme_combo_->count() - 1);
  }
  appearanceForm->addRow(tr("主题"), theme_combo_);
  appearanceForm->addRow(new QLabel(tr("保存后重启编辑器生效。"), appearance));
  pages_->addWidget(appearance);

  // 文件
  auto* file = new QWidget(pages_);
  auto* fileLayout = new QVBoxLayout(file);
  sus_auto_convert_ = new QCheckBox(tr("导入 sus 谱面时自动转换（实验性）"), file);
  fileLayout->addWidget(sus_auto_convert_);
  fileLayout->addStretch(1);
  pages_->addWidget(file);

  // 音频
  auto* audio = new QWidget(pages_);
  auto* audioLayout = new QVBoxLayout(audio);
  mute_hold_body_sfx_ = new QCheckBox(tr("关闭 Hold 体音效播放"), audio);
  audioLayout->addWidget(mute_hold_body_sfx_);
  audioLayout->addStretch(1);
  pages_->addWidget(audio);

  // 输入
  auto* input = new QWidget(pages_);
  auto* inputLayout = new QVBoxLayout(input);
  invert_scroll_wheel_ = new QCheckBox(tr("反转时间轴滚轮方向"), input);
  invert_visible_range_scroll_ = new QCheckBox(tr("反转滚轮调节可见范围大小方向"), input);
  inputLayout->addWidget(invert_scroll_wheel_);
  inputLayout->addWidget(invert_visible_range_scroll_);
  new_note_place_logic_ =
      new QCheckBox(tr("启用新版音符放置逻辑（工具箱按钮切换左键放置类型）"), input);
  inputLayout->addWidget(new_note_place_logic_);
  auto* speedRow = new QHBoxLayout;
  speedRow->addWidget(new QLabel(tr("时间轴滚轮速度"), input));
  scroll_wheel_speed_ = make_combo({"0.25x", "0.5x", "0.75x", "1x", "1.25x", "1.5x", "1.75x",
                                    "2x", "2.5x", "3x"},
                                   input);
  speedRow->addWidget(scroll_wheel_speed_);
  speedRow->addStretch(1);
  inputLayout->addLayout(speedRow);
  inputLayout->addStretch(1);
  pages_->addWidget(input);

  // 显示
  auto* display = new QWidget(pages_);
  auto* displayLayout = new QVBoxLayout(display);
  show_judgment_text_ = new QCheckBox(tr("开启判定文字显示"), display);
  displayLayout->addWidget(show_judgment_text_);
  auto* form = new QFormLayout;
  note_speed_ = new QDoubleSpinBox(display);
  note_speed_->setRange(1.0, 15.0);
  note_speed_->setDecimals(1);
  note_speed_->setSingleStep(0.5);
  form->addRow(tr("流速"), note_speed_);
  note_start_offset_ = new QSpinBox(display);
  note_start_offset_->setRange(0, 100);
  note_start_offset_->setSingleStep(5);
  form->addRow(tr("挡板高度"), note_start_offset_);
  note_height_level_ = new QSpinBox(display);
  note_height_level_->setRange(1, 10);
  form->addRow(tr("note厚度"), note_height_level_);
  split_line_opacity_ = new QSpinBox(display);
  split_line_opacity_->setRange(10, 100);
  split_line_opacity_->setSingleStep(10);
  form->addRow(tr("分割线特效透明度"), split_line_opacity_);
  spectrum_display_ = make_combo({tr("无"), tr("包络图"), tr("频率频谱图")}, display);
  form->addRow(tr("频谱显示"), spectrum_display_);
  msaa_samples_ = make_combo({tr("低"), tr("中"), tr("高")}, display);
  form->addRow(tr("抗锯齿"), msaa_samples_);
  displayLayout->addLayout(form);
  displayLayout->addStretch(1);
  pages_->addWidget(display);

  // 宽度 (Q/W/E/A/S/D place-width slots)
  auto* width = new QWidget(pages_);
  auto* widthLayout = new QGridLayout(width);
  static constexpr const char* kSlotLabels[6] = {"一档", "二档", "三档",
                                                 "四档", "五档", "六档"};
  for (int i = 0; i < 6; ++i) {
    widthLayout->addWidget(new QLabel(QString::fromUtf8(kSlotLabels[i]), width), i / 2,
                           (i % 2) * 2);
    auto* box = new QSpinBox(width);
    box->setRange(1, 12);
    width_slots_[static_cast<std::size_t>(i)] = box;
    widthLayout->addWidget(box, i / 2, (i % 2) * 2 + 1);
  }
  widthLayout->setRowStretch(3, 1);
  pages_->addWidget(width);

  // 快捷键
  auto* scroll = new QScrollArea(pages_);
  scroll->setWidgetResizable(true);
  auto* shortcuts = new QWidget(scroll);
  auto* shortcutsLayout = new QGridLayout(shortcuts);
  for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
    const auto id = static_cast<wds::interaction::EditorShortcut>(i);
    const int row = static_cast<int>(i);
    shortcutsLayout->addWidget(
        new QLabel(QString::fromUtf8(
                       wds::interaction::editor_shortcut_label(id, cfg_.pause_at_current)),
                   shortcuts),
        row, 0);
    auto* edit = new QKeySequenceEdit(shortcuts);
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    edit->setMaximumSequenceLength(1);
#endif
    shortcut_edits_[i] = edit;
    shortcutsLayout->addWidget(edit, row, 1);
    auto* clear = new QPushButton(shortcuts);
    auto clear_icon = fluent_icon(fluent::Clear);
    if (clear_icon.isNull()) clear_icon = style()->standardIcon(QStyle::SP_DialogCloseButton);
    clear->setIcon(clear_icon);
    clear->setIconSize(QSize(18, 18));
    clear->setFixedSize(32, 32);
    const QString clear_label = tr("清除快捷键：%1").arg(QString::fromUtf8(
        wds::interaction::editor_shortcut_label(id, cfg_.pause_at_current)));
    clear->setToolTip(clear_label);
    clear->setAccessibleName(clear_label);
    connect(clear, &QPushButton::clicked, edit, &QKeySequenceEdit::clear);
    connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
            [this] { refresh_shortcut_conflicts(); });
    shortcutsLayout->addWidget(clear, row, 2);
  }
  shortcutsLayout->setRowStretch(static_cast<int>(wds::interaction::kEditorShortcutCount), 1);
  scroll->setWidget(shortcuts);
  pages_->addWidget(scroll);

  // 隐私
  auto* privacy = new QWidget(pages_);
  auto* privacyLayout = new QVBoxLayout(privacy);
  allow_crash_log_sensitive_ =
      new QCheckBox(tr("允许崩溃日志记录真实文本与文件路径"), privacy);
  privacyLayout->addWidget(allow_crash_log_sensitive_);
  privacyLayout->addStretch(1);
  pages_->addWidget(privacy);
}

void SettingsDialog::load_from_config() {
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
}

int SettingsDialog::refresh_shortcut_conflicts() {
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
        conflict ? QStringLiteral("QKeySequenceEdit { border: 1px solid #d64545; }") : QString());
    if (conflict && first < 0) first = static_cast<int>(i);
  }
  return first;
}

bool SettingsDialog::capture_into_config() {
  cfg_.sus_auto_convert = sus_auto_convert_->isChecked();
  cfg_.mute_hold_body_sfx = mute_hold_body_sfx_->isChecked();
  cfg_.invert_scroll_wheel = invert_scroll_wheel_->isChecked();
  cfg_.invert_visible_range_scroll = invert_visible_range_scroll_->isChecked();
  cfg_.new_note_place_logic = new_note_place_logic_->isChecked();
  {
    QString label = scroll_wheel_speed_->currentText();
    label.chop(1);  // trailing "x"
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
  for (std::size_t i = 0; i < wds::interaction::kEditorShortcutCount; ++i) {
    cfg_.shortcuts[i] = sequence_to_chord(shortcut_edits_[i]->keySequence());
  }
  cfg_.shortcuts_initialized = true;
  cfg_.allow_crash_log_sensitive = allow_crash_log_sensitive_->isChecked();
  return true;
}

void SettingsDialog::try_confirm() {
  const int conflict = refresh_shortcut_conflicts();
  if (conflict >= 0) {
    sidebar_->setCurrentRow(6);  // 快捷键 tab (外观 inserted at 0)
    shortcut_edits_[static_cast<std::size_t>(conflict)]->setFocus();
    return;
  }
  if (!capture_into_config()) return;
  manager_->apply_ui_config_from_qt(cfg_);
  // Apply at startup so existing rasterized icons use the new palette too.
  if (theme_combo_ != nullptr && theme_combo_->currentIndex() >= 0) {
    const QString id = theme_combo_->currentData().toString();
    QSettings prefs("WDS", "WDS Editor");
    if (prefs.value("appearance/theme").toString() != id) {
      prefs.setValue("appearance/theme", id);
    }
  }
  accept();
}

}  // namespace wds::ui

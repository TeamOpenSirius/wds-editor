#include "wds/ui/qt/shortcut_settings_dialog.hpp"

#include "wds/ui/editor_ui_config.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/interaction/platform.hpp>
#include <wds/interaction/shortcuts.hpp>

#include <QDialogButtonBox>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <unordered_map>

namespace wds::ui {
namespace {

using wds::interaction::EditorShortcut;
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
  if (chord.key == KeyCode::Unknown || wds::interaction::is_forbidden_shortcut_key(chord.key)) {
    return {};
  }
  const auto mods = combo.keyboardModifiers();
  chord.mods.shift = mods.testFlag(Qt::ShiftModifier);
  chord.mods.control = mods.testFlag(Qt::ControlModifier);
  chord.mods.alt = mods.testFlag(Qt::AltModifier);
  chord.mods.super = mods.testFlag(Qt::MetaModifier);
  return chord;
}

struct ShortcutCategory {
  const char* title;
  const EditorShortcut* ids;
  std::size_t count;
};

constexpr EditorShortcut kFileShortcuts[] = {
    EditorShortcut::Open,
    EditorShortcut::Save,
};

constexpr EditorShortcut kEditShortcuts[] = {
    EditorShortcut::Undo,       EditorShortcut::Redo,
    EditorShortcut::Copy,       EditorShortcut::Paste,
    EditorShortcut::DeleteSelection,
    EditorShortcut::Mirror,     EditorShortcut::MirrorAboutCenter,
    EditorShortcut::NudgeUp,    EditorShortcut::NudgeDown,
    EditorShortcut::NudgeLeft,  EditorShortcut::NudgeRight,
};

constexpr EditorShortcut kPlaybackShortcuts[] = {
    EditorShortcut::TogglePlayback, EditorShortcut::PausePlayback,
    EditorShortcut::PlaybackRate0,  EditorShortcut::PlaybackRate1,
    EditorShortcut::PlaybackRate2,  EditorShortcut::PlaybackRate3,
    EditorShortcut::ToggleSfxMute,
};

constexpr EditorShortcut kNoteShortcuts[] = {
    EditorShortcut::PlaceType0, EditorShortcut::PlaceType1, EditorShortcut::PlaceType2,
    EditorShortcut::PlaceType3, EditorShortcut::PlaceType4, EditorShortcut::PlaceType5,
    EditorShortcut::PlaceType6, EditorShortcut::PlaceType7,
};

constexpr EditorShortcut kWidthShortcuts[] = {
    EditorShortcut::WidthSlot0, EditorShortcut::WidthSlot1, EditorShortcut::WidthSlot2,
    EditorShortcut::WidthSlot3, EditorShortcut::WidthSlot4, EditorShortcut::WidthSlot5,
};

constexpr EditorShortcut kViewShortcuts[] = {
    EditorShortcut::ToggleFullscreen,
};

constexpr ShortcutCategory kCategories[] = {
    {"文件", kFileShortcuts, std::size(kFileShortcuts)},
    {"编辑", kEditShortcuts, std::size(kEditShortcuts)},
    {"播放", kPlaybackShortcuts, std::size(kPlaybackShortcuts)},
    {"音符", kNoteShortcuts, std::size(kNoteShortcuts)},
    {"宽度", kWidthShortcuts, std::size(kWidthShortcuts)},
    {"视图", kViewShortcuts, std::size(kViewShortcuts)},
};

constexpr std::size_t categorized_shortcut_count() {
  std::size_t n = 0;
  for (const ShortcutCategory& category : kCategories) n += category.count;
  return n;
}

static_assert(categorized_shortcut_count() == wds::interaction::kEditorShortcutCount,
              "every editor shortcut must belong to one category");

const char* kFieldStyle = "QKeySequenceEdit, QKeySequenceEdit QLineEdit { min-width: 0; }";
const char* kConflictStyle =
    "QKeySequenceEdit, QKeySequenceEdit QLineEdit { min-width: 0; }"
    "QKeySequenceEdit { border: 1px solid #d64545; }";

}  // namespace

ShortcutSettingsDialog::ShortcutSettingsDialog(UiManager* manager, QWidget* parent)
    : QDialog(parent), manager_(manager) {
  setWindowTitle(tr("快捷键设置"));
  resize(960, 680);
  setMinimumSize(760, 420);

  bool pause_at_current = false;
  if (manager_ != nullptr) {
    EditorUiConfig cfg;
    manager_->snapshot_ui_config_for_qt(cfg);
    pause_at_current = cfg.pause_at_current;
  }
  build_ui(pause_at_current);
  load_from_manager();
}

void ShortcutSettingsDialog::build_ui(bool pause_at_current) {
  std::array<int, wds::interaction::kEditorShortcutCount> seen{};

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 12, 12, 12);
  root->setSpacing(8);

  auto* scroll = new QScrollArea(this);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* content = new QWidget(scroll);
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(4, 0, 8, 8);
  layout->setSpacing(0);

  const QFontMetrics shortcut_fm(font());
  const int hanzi = std::max(1, shortcut_fm.horizontalAdvance(QStringLiteral("字")));
  const int field_w = std::max(
      hanzi * 4, shortcut_fm.horizontalAdvance(QStringLiteral("Ctrl+Shift+F11")) - hanzi);

  bool first = true;
  for (const ShortcutCategory& category : kCategories) {
    if (!first) {
      auto* line = new QFrame(content);
      line->setFrameShape(QFrame::HLine);
      line->setFrameShadow(QFrame::Plain);
      line->setFixedHeight(1);
      layout->addSpacing(8);
      layout->addWidget(line);
    }
    first = false;

    auto* heading = new QLabel(QString::fromUtf8(category.title), content);
    QFont heading_font = heading->font();
    heading_font.setBold(true);
    heading_font.setWeight(QFont::Bold);
    heading_font.setPointSizeF(std::max(16.0, heading_font.pointSizeF() + 5.0));
    heading->setFont(heading_font);
    layout->addSpacing(10);
    layout->addWidget(heading);

    auto* columns = new QHBoxLayout;
    columns->setContentsMargins(8, 4, 0, 0);
    columns->setSpacing(24);
    auto* left_grid = new QGridLayout;
    auto* right_grid = new QGridLayout;
    for (QGridLayout* grid : {left_grid, right_grid}) {
      grid->setContentsMargins(0, 0, 0, 0);
      grid->setHorizontalSpacing(8);
      grid->setVerticalSpacing(6);
      grid->setColumnStretch(0, 1);
    }
    // Ignored + equal stretch keeps the empty right half the same width, so a
    // lone shortcut (切换全屏) stays on the left column instead of sliding right.
    auto* left_host = new QWidget(content);
    auto* right_host = new QWidget(content);
    left_host->setLayout(left_grid);
    right_host->setLayout(right_grid);
    left_host->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    right_host->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    columns->addWidget(left_host, 1);
    columns->addWidget(right_host, 1);

    for (std::size_t i = 0; i < category.count; ++i) {
      const EditorShortcut id = category.ids[i];
      const auto index = static_cast<std::size_t>(id);
      assert(index < seen.size());
      assert(seen[index] == 0);
      seen[index] += 1;

      QGridLayout* grid = (i % 2 == 0) ? left_grid : right_grid;
      const int row = static_cast<int>(i / 2);
      const QString label_text =
          QString::fromUtf8(wds::interaction::editor_shortcut_label(id, pause_at_current));
      auto* shortcut_label = new QLabel(label_text, content);
      shortcut_label->setWordWrap(true);
      shortcut_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      shortcut_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
      grid->addWidget(shortcut_label, row, 0, Qt::AlignVCenter);

      auto* edit = new QKeySequenceEdit(content);
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
      edit->setMaximumSequenceLength(1);
#endif
      edit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      edit->setStyleSheet(QString::fromLatin1(kFieldStyle));
      edit->setFixedWidth(field_w);
      edits_[index] = edit;
      grid->addWidget(edit, row, 1, Qt::AlignVCenter);

      auto* clear = new QPushButton(content);
      clear->setObjectName(QStringLiteral("shortcutClear"));
      clear->setIcon(themed_named_icon("clear", {}, 18));
      clear->setIconSize(QSize(18, 18));
      clear->setFixedSize(32, 32);
      const QString clear_label = tr("清除快捷键：%1").arg(label_text);
      clear->setToolTip(clear_label);
      clear->setAccessibleName(clear_label);
      connect(clear, &QPushButton::clicked, this, [edit] {
        journal_menu_action("settings.shortcut_clear");
        edit->clear();
      });
      grid->addWidget(clear, row, 2, Qt::AlignVCenter);
    }
    layout->addLayout(columns);
  }
  for (int hit : seen) assert(hit == 1);
  layout->addStretch(1);

  scroll->setWidget(content);
  root->addWidget(scroll, 1);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  status_->setStyleSheet(QStringLiteral("color: #d64545;"));
  status_->hide();
  root->addWidget(status_);

  auto* buttons = new QDialogButtonBox(this);
  auto add_action = [&](const QString& text) {
    auto* button = buttons->addButton(text, QDialogButtonBox::ActionRole);
    button->setAutoDefault(false);
    button->setDefault(false);
    return button;
  };
  exit_button_ = add_action(tr("退出"));
  auto* apply_button = add_action(tr("应用"));
  auto* ok_button = add_action(tr("确定"));
  root->addWidget(buttons);
  connect(exit_button_, &QPushButton::clicked, this, [this] {
    journal_menu_action("settings.shortcuts_exit");
    reject();
  });
  connect(apply_button, &QPushButton::clicked, this, [this] { apply_changes(); });
  connect(ok_button, &QPushButton::clicked, this, [this] {
    if (apply_changes()) {
      journal_menu_action("settings.shortcuts_ok");
      accept();
    }
  });

  for (auto* edit : edits_) {
    connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence&) {
      if (!loading_) refresh_shortcut_conflicts();
    });
    connect(edit, &QKeySequenceEdit::editingFinished, this, [] {
      journal_menu_action("settings.shortcut_edit");
    });
  }
}

void ShortcutSettingsDialog::load_from_manager() {
  loading_ = true;
  EditorUiConfig cfg;
  if (manager_ != nullptr) manager_->snapshot_ui_config_for_qt(cfg);
  for (std::size_t i = 0; i < edits_.size(); ++i) {
    const auto id = static_cast<EditorShortcut>(i);
    const ShortcutChord chord = cfg.shortcuts_initialized ? cfg.shortcuts[i]
                                                          : wds::interaction::editor_shortcut(id);
    const QSignalBlocker blocker(edits_[i]);
    edits_[i]->setKeySequence(chord_to_sequence(chord));
  }
  loading_ = false;
  refresh_shortcut_conflicts();
}

int ShortcutSettingsDialog::refresh_shortcut_conflicts() {
  std::unordered_map<ShortcutChord, int, wds::interaction::ShortcutChordHash> counts;
  std::array<ShortcutChord, wds::interaction::kEditorShortcutCount> chords{};
  for (std::size_t i = 0; i < chords.size(); ++i) {
    chords[i] = sequence_to_chord(edits_[i]->keySequence());
    chords[i].mods = wds::interaction::normalize_primary(chords[i].mods);
    if (chords[i].key != KeyCode::Unknown) ++counts[chords[i]];
  }
  int first = -1;
  for (std::size_t i = 0; i < chords.size(); ++i) {
    const bool conflict = chords[i].key != KeyCode::Unknown && counts[chords[i]] > 1;
    edits_[i]->setStyleSheet(QString::fromLatin1(conflict ? kConflictStyle : kFieldStyle));
    if (conflict && first < 0) first = static_cast<int>(i);
  }
  if (status_ != nullptr) {
    status_->setVisible(first >= 0);
    if (first >= 0) status_->setText(tr("有快捷键互相冲突，标红项无法应用"));
  }
  return first;
}

bool ShortcutSettingsDialog::apply_changes() {
  if (manager_ == nullptr) return false;
  if (refresh_shortcut_conflicts() >= 0) return false;
  journal_menu_action("settings.shortcut_apply");
  EditorUiConfig cfg;
  manager_->snapshot_ui_config_for_qt(cfg);
  for (std::size_t i = 0; i < edits_.size(); ++i) {
    cfg.shortcuts[i] = sequence_to_chord(edits_[i]->keySequence());
  }
  cfg.shortcuts_initialized = true;
  manager_->apply_ui_config_from_qt(cfg);
  return true;
}

void ShortcutSettingsDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  if (!focused_exit_ && exit_button_ != nullptr) {
    focused_exit_ = true;
    QTimer::singleShot(0, exit_button_, [this] {
      if (exit_button_ != nullptr) exit_button_->setFocus();
    });
  }
}

void ShortcutSettingsDialog::changeEvent(QEvent* event) {
  QDialog::changeEvent(event);
  if (event != nullptr && (event->type() == QEvent::PaletteChange ||
                           event->type() == QEvent::ApplicationPaletteChange)) {
    const QIcon icon = themed_named_icon("clear", {}, 18);
    for (auto* button : findChildren<QPushButton*>(QStringLiteral("shortcutClear"))) {
      button->setIcon(icon);
    }
  }
}

}  // namespace wds::ui

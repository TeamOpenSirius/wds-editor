#include "wds/ui/qt/settings_dialog.hpp"

#include "wds/ui/ui_manager.hpp"
#include "wds/ui/qt/shortcut_settings_dialog.hpp"
#include "wds/ui/qt/wds_theme.hpp"
#include "wds/ui/qt/caption_check.hpp"
#include "wds/ui/qt/update_checker.hpp"
#include "wds/common/crash_handler.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>

#include <wds/interaction/editor_shortcuts.hpp>

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
#include <QLabel>
#include <QLayout>
#include <QMargins>
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
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace wds::ui {
namespace {

QComboBox* make_combo(const QStringList& items, QWidget* parent) {
  auto* combo = new QComboBox(parent);
  combo->addItems(items);
  combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  return combo;
}

// QLabel::heightForWidth on Windows often keeps the previous two-line
// layout after the widget gets wider. Measure the string against the new
// width instead of asking the label for a cached HFW.
int wrap_text_height(const QFont& font, const QString& text, int width, const QMargins& margins) {
  const QFontMetrics metrics(font);
  const int inner = std::max(1, width - margins.left() - margins.right());
  const QRect bounds = metrics.boundingRect(QRect(0, 0, inner, 100000),
                                            Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, text);
  return std::max(metrics.height(), bounds.height()) + margins.top() + margins.bottom();
}

void pin_widget_height(QWidget* widget, int height) {
  if (widget == nullptr || height <= 0) return;
  if (widget->minimumHeight() != height || widget->maximumHeight() != height) {
    widget->setFixedHeight(height);
  }
}

void sync_wrap_label_height(QLabel* label) {
  if (label == nullptr || !label->wordWrap() || label->width() <= 0) return;
  const int floor = label->property("wdsMinH").toInt();
  const int h = std::max(floor, wrap_text_height(label->font(), label->text(), label->width(),
                                                 label->contentsMargins()));
  pin_widget_height(label, h);
}

class HfwWidget final : public QWidget {
 public:
  using QWidget::QWidget;
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override {
    if (QLayout* lay = layout()) return lay->totalHeightForWidth(w);
    return QWidget::heightForWidth(w);
  }
  QSize sizeHint() const override {
    const int w = width() > 0 ? width() : QWidget::sizeHint().width();
    return QSize(QWidget::sizeHint().width(), heightForWidth(w));
  }
};

class WrappingLabel final : public QLabel {
 public:
  using QLabel::QLabel;

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QLabel::resizeEvent(event);
    sync_wrap_label_height(this);
  }
};

class RelayoutOnResizeFilter final : public QObject {
 public:
  using QObject::QObject;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Resize) {
      if (auto* label = qobject_cast<QLabel*>(watched)) sync_wrap_label_height(label);
    }
    return false;
  }
};

// A section that is only a heading and a button has no height-for-width
// child, so the scroll layout gives it the same short height as an empty
// heading and squashes the button. Pin the panel to the layout's own hint.
void fit_panel_to_layout(QWidget* panel) {
  if (panel == nullptr || panel->layout() == nullptr) return;
  const int need = panel->layout()->totalSizeHint().height();
  if (need > 0 && panel->minimumHeight() != need) panel->setMinimumHeight(need);
}

QCheckBox* add_wrapping_check(QLayout* layout, const QString& text, QWidget* parent) {
  auto* row = new CaptionCheckRow(text, parent);
  layout->addWidget(row);
  return row->box();
}

constexpr int kNavCollapsedW = 16;
constexpr int kNavExpandedW = 104;
constexpr int kNavItemH = 26;
constexpr int kSettingsSectionCount = 9;

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
      QCoreApplication::translate("wds::ui", "更新"),
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
    "settings.tab_updates",
}};

QColor nav_rail_fill(const QPalette& palette) {
  const QColor window = palette.color(QPalette::Window);
  const QColor base = palette.color(QPalette::Base);
  if (std::abs(base.lightness() - window.lightness()) >= 14) return base;
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
  // Word-wrapped labels report height-for-width. Keep the label at least as
  // tall as the field so a one-line caption centers on the spin/combo; do
  // not use Expanding or leftover height never returns when the text unwraps.
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  for (int i = 0; i < form->rowCount(); ++i) {
    auto* label_item = form->itemAt(i, QFormLayout::LabelRole);
    auto* field_item = form->itemAt(i, QFormLayout::FieldRole);
    auto* label = label_item != nullptr ? qobject_cast<QLabel*>(label_item->widget()) : nullptr;
    if (label == nullptr) continue;
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    label->installEventFilter(new RelayoutOnResizeFilter(label));
    if (QWidget* field = field_item != nullptr ? field_item->widget() : nullptr) {
      field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
      const int h = field->sizeHint().height();
      if (h > 0) label->setProperty("wdsMinH", h);
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
  QTimer::singleShot(0, this, [this] {
    QWidget* content = scroll_ != nullptr ? scroll_->widget() : nullptr;
    if (content == nullptr) return;
    const auto widgets = content->findChildren<QWidget*>();
    for (QWidget* widget : widgets) {
      if (widget->property("wdsWrapRow").toBool()) sync_caption_check_row(widget);
    }
    const auto labels = content->findChildren<QLabel*>();
    for (QLabel* label : labels) sync_wrap_label_height(label);
    if (QLayout* lay = content->layout()) {
      lay->invalidate();
      lay->activate();
    }
    content->updateGeometry();
    fit_panel_to_layout(sections_[6]);
    update_section_scroll_pad();
    sync_nav_from_scroll();
  });
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
  const int pad_was = scroll_pad_->height();
  if (pad_was != 0) scroll_pad_->setFixedHeight(0);
  const int without_pad = content->hasHeightForWidth()
                              ? content->heightForWidth(std::max(1, content->width()))
                              : content->sizeHint().height();
  const int pad = without_pad > view_h ? std::max(0, view_h - last->height()) : 0;
  if (pad != 0) scroll_pad_->setFixedHeight(pad);
}

void SettingsPanel::build_pages() {
  auto* content = new HfwWidget;
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(4, 4, 16, 8);
  layout->setSpacing(0);

  const auto titles = settings_section_titles();
  auto section_at = [&](int index) -> QVBoxLayout* {
    auto* section = new HfwWidget(content);
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

  auto* shortcuts = section_at(6);
  auto* open_shortcuts = new QPushButton(tr("设置快捷键"), shortcuts->parentWidget());
  open_shortcuts->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  connect(open_shortcuts, &QPushButton::clicked, this, [this] {
    journal_menu_action("settings.open_shortcuts");
    QWidget* host = window();
    ShortcutSettingsDialog dialog(manager_, host != nullptr ? host : this);
    dialog.exec();
  });
  shortcuts->addWidget(open_shortcuts, 0, Qt::AlignLeft);
  fit_panel_to_layout(shortcuts->parentWidget());

  auto* privacy = section_at(7);
  auto* privacy_host = privacy->parentWidget();
  allow_crash_log_sensitive_ = add_wrapping_check(
      privacy, tr("允许崩溃日志记录真实文本与文件路径"), privacy_host);
  auto* open_logs = new QPushButton(tr("打开崩溃日志文件夹"), privacy_host);
  open_logs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  connect(open_logs, &QPushButton::clicked, this, [] {
    journal_menu_action("settings.open_logs");
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromUtf8(wds::common::crash_log_directory())));
  });
  privacy->addWidget(open_logs, 0, Qt::AlignLeft);

  auto* updates = section_at(8);
  auto_check_updates_ =
      add_wrapping_check(updates, tr("启动时自动检查更新"), updates->parentWidget());

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
  connect(allow_crash_log_sensitive_, &QCheckBox::toggled, this, [this, live](bool) {
    if (!applying_) journal_menu_action("settings.privacy_toggle");
    live();
  });
  connect(auto_check_updates_, &QCheckBox::toggled, this, [this](bool on) {
    if (applying_) return;
    journal_menu_action("settings.auto_check_updates");
    set_auto_check_updates_enabled(on);
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
  allow_crash_log_sensitive_->setChecked(cfg_.allow_crash_log_sensitive);
  auto_check_updates_->setChecked(auto_check_updates_enabled());
  applying_ = false;
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
  cfg_.allow_crash_log_sensitive = allow_crash_log_sensitive_->isChecked();
  // Shortcuts are edited in ShortcutSettingsDialog. Re-read the live chords so
  // a later settings apply does not write the snapshot taken when this panel opened.
  cfg_.shortcuts = wds::interaction::editor_shortcuts_snapshot();
  cfg_.shortcuts_initialized = true;
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

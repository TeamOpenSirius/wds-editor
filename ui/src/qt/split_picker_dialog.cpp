#include "wds/ui/qt/split_picker_dialog.hpp"
#include "wds/ui/qt/split_effect_snapshot.hpp"

#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/regions/edit/edit_gutters.hpp"
#include "wds/ui/regions/edit/split_effect_tags.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QIcon>
#include <QPaintEvent>
#include <QFontMetrics>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QLineEdit>
#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScreen>
#include <QPushButton>
#include <QRect>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wds::ui {
namespace {

constexpr int kPreviewCols = 3;
constexpr int kPreviewGap = 6;
constexpr int kPreviewMinBox = 192;
constexpr int kPreviewPlateH = 48;
constexpr int kPreviewIdGap = 8;
// Outer height of every control in the top area. The theme gives each control a 1px border, so the QSS (content-box)
// height is kControlH - 2. QSS min/max/height are what polish() turns into the widget's min/max size, so the height is
// defined only through the box model here and polish can never undo it.
constexpr int kControlH = 24;
constexpr int kControlContentH = kControlH - 2;
// Horizontal spacing of the top panel grid (label column <-> controls).
constexpr int kGridSpacing = 8;
// Vertical spacing between the grid rows; also the spacing between the wrapped 主题 chip lines.
constexpr int kGridRowSpacing = 8;
constexpr int kPanelToListGap = 10;  // extra gap under the filter panel, on top of the root layout spacing

// Common QSS body for the top-area controls: fixed content height, no vertical padding, no margin.
QString control_box_css() {
  return QStringLiteral(
             "min-height:%1px; max-height:%1px; height:%1px;"
             " padding-top:0px; padding-bottom:0px; margin:0px;")
      .arg(kControlContentH);
}

// 图层 / 主题 / 轨道数 / 类型 / 编号 labels: same height as the controls, text centred vertically.
void style_control_label(QLabel* label, Qt::Alignment align) {
  label->setFixedHeight(kControlH);
  label->setAlignment(align);
  label->setStyleSheet(QStringLiteral("QLabel { padding:0px; margin:0px; }"));
}

QColor qcolor(const wds::interaction::Color& c) {
  return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f),
                          std::clamp(c.b, 0.0f, 1.0f), std::clamp(c.a, 0.0f, 1.0f));
}

// Horizontal inset of the 12-lane layout. Must match indicator_h_inset in
// split_effect_snapshot.cpp.
float indicator_h_inset(float line_w, float plate_w) {
  return std::max(line_w * 0.5f + 2.0f, 0.06f * plate_w);
}

struct PreviewLayout {
  float line_w = 1.5f;
  float pad = 2.0f;
  float inner_w = 1.0f;
};

// Line layout shared by painting and cluster keys.
PreviewLayout preview_layout(int box_w) {
  constexpr float kLanes = 12.0f;
  PreviewLayout l;
  l.line_w = std::clamp(static_cast<float>(box_w) / kLanes * 0.14f, 1.5f, 2.5f);
  // Lane 0 / 12 sit on the field edges. Inset so the black ground is a few
  // pixels wider than those centers and the full stroke stays visible.
  l.pad = indicator_h_inset(l.line_w, static_cast<float>(box_w));
  l.inner_w = std::max(1.0f, static_cast<float>(box_w) - l.pad * 2.0f);
  return l;
}

SplitIndicatorClusterKey preview_cluster_key(int32_t count, int32_t color_id,
                                             wds::chart_editor::SplitLaneType type, int box_w) {
  SplitIndicatorClusterKey k;
  k.color_id = color_id;
  k.split_count = count;
  k.type = type;
  k.lane_px = preview_layout(box_w).inner_w / 12.0f;
  k.band_h = static_cast<int>(kSplitIndicatorRefBandPx);
  k.size_scale = kSplitIndicatorSizeScale;
  k.min_sprite_px = kSplitIndicatorRefMinSpritePx;
  return k;
}

}  // namespace

// State shared by the dialog and its thumbs: current count/type.
struct SplitPickerPreviewState {
  int32_t count = 2;
  wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full;
};

namespace {

// Paints itself (plate + lines + cached particle cluster + id text) so only visible thumbs ever build a cluster.
class ColorThumb final : public QWidget {
 public:
  ColorThumb(int32_t id, std::shared_ptr<SplitPickerPreviewState> state, QWidget* parent)
      : QWidget(parent), id_(id), state_(std::move(state)) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
  }

  int32_t color_id() const { return id_; }
  void set_id(int32_t id) {
    id_ = id;
    update();
  }
  void set_box_w(int w) {
    if (box_w_ == w) return;
    box_w_ = w;
    update();
  }
  void set_selected(bool on) {
    if (selected_ == on) return;
    selected_ = on;
    update();
  }

  std::function<void()> on_click;
  std::function<void()> on_dblclick;

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int box_w = std::max(1, std::min(box_w_, width()));
    const int xoff = std::max(0, (width() - box_w) / 2);
    p.translate(xoff, 0);
    const QRect plate(0, 0, box_w, kPreviewPlateH);
    p.fillRect(plate, QColor(5, 8, 13));

    const int32_t count = state_->count;
    const auto type = state_->type;
    constexpr int32_t kLanes = 12;
    std::vector<int32_t> mids;
    split_boundaries_12(count, mids);
    const PreviewLayout lay = preview_layout(box_w);
    auto draw_edge = [&](int32_t edge_lane, int32_t slot) {
      const float x = std::floor(lay.pad + static_cast<float>(edge_lane) * lay.inner_w /
                                               static_cast<float>(kLanes) +
                                 0.5f);
      auto c = split_slot_color(id_, slot, count, nullptr);
      if (c.a < 0.02f) return;
      apply_official_split_rgb_opacity(c);
      p.fillRect(QRectF(x - lay.line_w * 0.5f, 2.0, static_cast<qreal>(lay.line_w),
                        kPreviewPlateH - 4.0),
                 qcolor(c));
    };
    draw_edge(0, 0);
    int32_t slot = 1;
    for (int32_t mid : mids) draw_edge(mid + 1, slot++);
    draw_edge(kLanes, std::max(1, count));

    const SplitIndicatorClusterKey key = preview_cluster_key(count, id_, type, box_w);
    // Never builds on the GUI thread: nullptr = queued on the worker, plate + lines only until update() fires.
    // `c` is used right here and not held across event processing (the LRU may evict after a delivery).
    const SplitIndicatorCluster* c =
        split_effect_indicator_cluster_request(key, this, [this] { update(); });
    if (c != nullptr && !c->empty) {
      p.save();
      p.setClipRect(plate);
      draw_split_effect_indicator_cluster(p, *c, QPointF(lay.pad, 2.0));
      p.restore();
    }

    const int text_h = text_height();
    // "id ↑/↓": the arrow is the fadeIn direction (LineHight z rotation: 0 = grows up, 180 = grows down).
    const QString label = QString::number(id_) + QLatin1Char(' ') +
                          QString::fromUtf8(split_fade_arrow_utf8(id_));
    p.setPen(QColor(220, 222, 228));
    p.drawText(QRect(0, kPreviewPlateH + kPreviewIdGap, box_w, text_h),
               Qt::AlignHCenter | Qt::AlignTop, label);
    p.resetTransform();
    if (selected_) {
      p.setPen(QColor(0x6e, 0xa8, 0xfe));
      p.drawRect(QRect(0, 0, width() - 1, height() - 1));
    }
  }
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && on_click) on_click();
    QWidget::mousePressEvent(event);
  }
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && on_dblclick) on_dblclick();
    QWidget::mouseDoubleClickEvent(event);
  }

 private:
  int text_height() const { return std::max(QFontMetrics(font()).height(), 12); }

  int32_t id_ = 0;
  int box_w_ = kPreviewMinBox;
  bool selected_ = false;
  std::shared_ptr<SplitPickerPreviewState> state_;
};

// Layout for the filter chips. Three modes:
//
//  * normal: left-aligned wrapping flow, chips keep their size hint and wrap when the width runs out, so they never
//    widen the (fixed-width) dialog.
//  * balanced_justified (the 主题 row): all chips live in uniform SLOTS (slot_w = max(widest chip, min_slot_w_)).
//    The chips are split by COUNT as evenly as possible over the minimal number of lines n whose per-line count
//    (ceil(count / n)) fits the width. A column grid is derived ONCE from a full line (the spare width is spread over
//    the gaps) and every line, including a shorter last line, uses the same column x positions, so the columns line
//    up vertically. Each chip is centred in its slot. The size hint is a single slot, so the row never drives the
//    panel width.
//  * follower (the 图层 row): ignores its own wrapping, takes the column grid of a master layout (the 主题 row) for
//    the same rect width and places its chips in columns 0, 1, 2, ... on a single line. One optional trailing item
//    (the clear button) is right-aligned to the right edge of the rect, i.e. the right edge of the master's last
//    column. There is NO wrapping: if the follower has more chips than the master has columns minus one, the extra
//    chips simply overflow to the right.
class ChipFlowLayout final : public QLayout {
 public:
  ChipFlowLayout(QWidget* parent, int h_gap, int v_gap, bool balanced_justified = false)
      : QLayout(parent), h_gap_(h_gap), v_gap_(v_gap), balanced_(balanced_justified) {
    setContentsMargins(0, 0, 0, 0);
  }
  ~ChipFlowLayout() override {
    while (QLayoutItem* item = takeAt(0)) delete item;
  }

  // Lower bound of the slot width; the dialog sets the same value (widest chip overall) on both rows.
  void set_min_slot_width(int w) {
    if (min_slot_w_ == w) return;
    min_slot_w_ = w;
    invalidate();
  }
  // Follower mode: take the column grid from `master` (which must outlive this layout's use).
  void set_column_master(const ChipFlowLayout* master) {
    master_ = master;
    invalidate();
  }
  // Item whose widget is right-aligned instead of occupying a column (follower mode).
  void set_trailing_item(QWidget* w) {
    trailing_ = w;
    invalidate();
  }

  // Column grid for `width`: per = slots per line, slot_w = slot width, gap = distance between slots, rem = number of
  // leading gaps that get one extra pixel. Column c starts at c * (slot_w + gap) + min(c, rem).
  void column_grid(int width, int& per, int& slot_w, int& gap, int& rem) const {
    slot_w = slot_width();
    const int n = non_trailing_count();
    per = 1;
    for (int lines = 1; lines <= n; ++lines) {
      per = (n + lines - 1) / lines;
      if (per * slot_w + (per - 1) * h_gap_ <= width) break;
    }
    const int extra = std::max(0, width - per * slot_w - (per - 1) * h_gap_);
    gap = h_gap_;
    rem = 0;
    if (per > 1) {
      gap += extra / (per - 1);
      rem = extra % (per - 1);
    }
  }

  void addItem(QLayoutItem* item) override { items_.append(item); }
  int count() const override { return static_cast<int>(items_.size()); }
  QLayoutItem* itemAt(int index) const override { return items_.value(index); }
  QLayoutItem* takeAt(int index) override {
    if (index >= 0 && index < items_.size()) return items_.takeAt(index);
    return nullptr;
  }
  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return place(QRect(0, 0, width, 0), true); }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    place(rect, false);
  }
  QSize sizeHint() const override {
    if (master_ != nullptr) {
      // (non-trailing chips + 1 slot for the trailing item) at the minimum gap.
      const int k = non_trailing_count();
      const int cols = k + (has_trailing() ? 1 : 0);
      const int w = cols > 0 ? cols * slot_width() + (cols - 1) * h_gap_ : 0;
      return {w, max_item_height()};
    }
    if (balanced_) return minimumSize();  // one slot x chip height; the line count follows the width
    int w = 0;
    int h = 0;
    for (int i = 0; i < items_.size(); ++i) {
      const QSize s = items_[i]->sizeHint();
      if (i > 0) w += h_gap_;
      w += s.width();
      h = std::max(h, s.height());
    }
    return {w, h};
  }
  QSize minimumSize() const override {
    if (master_ != nullptr) return sizeHint();
    if (balanced_) return {slot_width(), max_item_height()};
    QSize size;
    for (const QLayoutItem* item : items_) size = size.expandedTo(item->sizeHint());
    return size;
  }

 private:
  bool is_trailing(const QLayoutItem* item) const {
    return trailing_ != nullptr && item->widget() == trailing_;
  }
  bool has_trailing() const {
    for (const QLayoutItem* item : items_) {
      if (is_trailing(item)) return true;
    }
    return false;
  }
  int non_trailing_count() const {
    int n = 0;
    for (const QLayoutItem* item : items_) {
      if (!is_trailing(item)) ++n;
    }
    return n;
  }
  int max_item_height() const {
    int h = 0;
    for (const QLayoutItem* item : items_) h = std::max(h, item->sizeHint().height());
    return h;
  }
  // Uniform slot width: widest own (non-trailing) item, at least min_slot_w_.
  int slot_width() const {
    int w = min_slot_w_;
    for (const QLayoutItem* item : items_) {
      if (!is_trailing(item)) w = std::max(w, item->sizeHint().width());
    }
    return w;
  }

  int place_balanced(const QRect& rect, bool test_only) const {
    const int n = static_cast<int>(items_.size());
    if (n == 0) return 0;
    int per = 1;
    int slot_w = 0;
    int gap = 0;
    int rem = 0;
    column_grid(rect.width(), per, slot_w, gap, rem);

    int y = rect.y();
    for (int start = 0; start < n; start += per) {
      const int end = std::min(n, start + per);
      int line_h = 0;
      for (int i = start; i < end; ++i) line_h = std::max(line_h, items_[i]->sizeHint().height());
      if (!test_only) {
        for (int i = start; i < end; ++i) {
          const int col = i - start;
          const QSize s = items_[i]->sizeHint();
          const int x = rect.x() + col * (slot_w + gap) + std::min(col, rem) + (slot_w - s.width()) / 2;
          items_[i]->setGeometry(QRect(QPoint(x, y), s));
        }
      }
      y += line_h + v_gap_;
    }
    return y - v_gap_ - rect.y();
  }

  int place_follower(const QRect& rect, bool test_only) const {
    const int line_h = max_item_height();
    if (test_only) return line_h;
    int per = 1;
    int slot_w = 0;
    int gap = 0;
    int rem = 0;
    master_->column_grid(rect.width(), per, slot_w, gap, rem);
    (void)per;  // columns past the master's last one are not wrapped (see class comment)
    int col = 0;
    for (QLayoutItem* item : items_) {
      const QSize s = item->sizeHint();
      int x = 0;
      if (is_trailing(item)) {
        x = rect.x() + rect.width() - s.width();
      } else {
        // Columns beyond the master's last one (see class comment) just continue the same arithmetic.
        x = rect.x() + col * (slot_w + gap) + std::min(col, rem) + (slot_w - s.width()) / 2;
        ++col;
      }
      item->setGeometry(QRect(QPoint(x, rect.y() + (line_h - s.height()) / 2), s));
    }
    return line_h;
  }

  int place(const QRect& rect, bool test_only) const {
    if (master_ != nullptr) return place_follower(rect, test_only);
    if (balanced_) return place_balanced(rect, test_only);
    int x = rect.x();
    int y = rect.y();
    int line_h = 0;
    for (QLayoutItem* item : items_) {
      const QSize s = item->sizeHint();
      if (x > rect.x() && x + s.width() > rect.x() + rect.width()) {
        x = rect.x();
        y += line_h + v_gap_;
        line_h = 0;
      }
      if (!test_only) item->setGeometry(QRect(QPoint(x, y), s));
      x += s.width() + h_gap_;
      line_h = std::max(line_h, s.height());
    }
    return y + line_h - rect.y();
  }

  QList<QLayoutItem*> items_;
  int h_gap_;
  int v_gap_;
  bool balanced_ = false;
  int min_slot_w_ = 0;
  const ChipFlowLayout* master_ = nullptr;
  QWidget* trailing_ = nullptr;
};

ChipFlowLayout* flow_layout(QWidget* w) { return w != nullptr ? static_cast<ChipFlowLayout*>(w->layout()) : nullptr; }

// Tri-state filter chip: off / include (checked) / exclude (red outline).
// Each left click cycles Off -> Include -> Exclude -> Off.
class TagChip final : public QPushButton {
 public:
  enum class State { Off, Include, Exclude };

  TagChip(const SplitEffectTagInfo& info, QWidget* parent)
      : QPushButton(parent),
        bit_(info.bit),
        label_(QString::fromUtf8(info.label_utf8)) {
    setCheckable(true);
    setAutoDefault(false);
    setDefault(false);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(kControlH);
    setToolTip(QString::fromUtf8(info.tooltip_utf8));
    apply_state();
    // clicked fires after QAbstractButton flipped the check state; set_state() re-syncs it from state_.
    connect(this, &QPushButton::clicked, this, [this] {
      switch (state_) {
        case State::Off:
          set_state(State::Include);
          break;
        case State::Include:
          set_state(State::Exclude);
          break;
        case State::Exclude:
          set_state(State::Off);
          break;
      }
      if (on_changed) on_changed();
    });
  }

  uint32_t bit() const { return bit_; }
  State state() const { return state_; }
  void set_state(State s) {
    state_ = s;
    apply_state();
  }

  std::function<void()> on_changed;

 private:
  void apply_state() {
    // Height is defined by the QSS box model (content kControlContentH + 1px border top/bottom = kControlH).
    QString css = QStringLiteral("QPushButton { ") + control_box_css() +
                  QStringLiteral(" padding-left:10px; padding-right:10px; icon-size:0px;");
    if (state_ == State::Exclude) {
      // Border only; still 1px, same radius as the theme's --border_radius (4px).
      css += QStringLiteral(" border:1px solid #e06c75; border-radius:4px;");
    }
    css += QStringLiteral(" }");
    setStyleSheet(css);
    setFixedHeight(kControlH);  // consistent with the QSS; belt and braces
    setChecked(state_ == State::Include);
    setText(label_);
  }

  uint32_t bit_;
  QString label_;
  State state_ = State::Off;
};

// Filter chips survive across dialog openings within the process, on accept and cancel alike
// (not persisted to disk). The 编号 search text is intentionally not remembered.
struct SplitPickerMemory {
  uint32_t include = 0;
  uint32_t exclude = 0;
};

SplitPickerMemory& picker_memory() {
  static SplitPickerMemory m;
  return m;
}

SplitTagFilter build_tag_filter(const std::vector<QPushButton*>& chips) {
  SplitTagFilter f;
  for (QPushButton* b : chips) {
    const auto* chip = static_cast<const TagChip*>(b);
    if (chip->state() == TagChip::State::Include) {
      if ((chip->bit() & kSplitTagLayerMask) != 0) {
        f.layer_any |= chip->bit();
      } else {
        f.theme_any |= chip->bit();
      }
    } else if (chip->state() == TagChip::State::Exclude) {
      f.exclude |= chip->bit();
    }
  }
  return f;
}

// "11621\n图层：闪光、丝带\n主题：蝶翼" (empty lines omitted).
QString thumb_tooltip(int32_t id) {
  QString s = QString::number(id);
  const uint32_t tags = split_effect_tags(id);
  auto join = [tags](const std::vector<SplitEffectTagInfo>& list) {
    QStringList parts;
    for (const auto& t : list) {
      if ((tags & t.bit) != 0) parts << QString::fromUtf8(t.label_utf8);
    }
    return parts.join(QStringLiteral("、"));
  };
  const QString layers = join(split_effect_layer_tags());
  const QString themes = join(split_effect_theme_tags());
  if (!layers.isEmpty()) s += QStringLiteral("\n图层：") + layers;
  if (!themes.isEmpty()) s += QStringLiteral("\n主题：") + themes;
  return s;
}

}  // namespace

SplitPickerDialog::SplitPickerDialog(int32_t count, int32_t color_id,
                                     wds::chart_editor::SplitLaneType type, QWidget* parent,
                                     QString skins_dir, int quality)
    : QDialog(parent),
      count_(std::clamp(count, 1, 6)),
      color_id_(color_id),
      type_(type),
      skins_dir_(std::move(skins_dir)) {
  (void)quality;  // previews are pinned to Normal quality; kept for API compatibility
  ensure_split_effect_uv_atlas(skins_dir_);
  state_ = std::make_shared<SplitPickerPreviewState>();
  state_->count = count_;
  state_->type = type_;
  setWindowTitle(tr("分割线"));
  setMinimumHeight(640);
  resize(400, 720);

  auto* root = new QVBoxLayout(this);

  // The 3-row control block lives in its own fixed-width panel (width computed in sync_aligned_width()) that is
  // centred in the dialog, so the right edges of the three rows can be made to coincide.
  top_panel_ = new QWidget(this);

  controls_ = new QWidget(top_panel_);
  controls_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* count_type_row = new QHBoxLayout(controls_);
  count_type_row->setContentsMargins(0, 0, 0, 0);
  count_type_row->setSpacing(6);
  counts_ = new QButtonGroup(this);
  counts_->setExclusive(true);
  for (int i = 1; i <= 6; ++i) {
    auto* btn = new QPushButton(QString::number(i), controls_);
    btn->setCheckable(true);
    btn->setChecked(i == count_);
    btn->setAutoDefault(false);
    btn->setDefault(false);
    // Exact kControlH x kControlH square, defined through the QSS box model (content = kControlContentH, no padding,
    // 1px border) so polish() keeps it.
    btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btn->setStyleSheet(QStringLiteral("QPushButton { ") + control_box_css() +
                       QStringLiteral(" min-width:%1px; max-width:%1px; width:%1px; padding:0px;"
                                      " icon-size:0px; }")
                           .arg(kControlContentH));
    btn->setFixedSize(kControlH, kControlH);
    counts_->addButton(btn, i);
    count_type_row->addWidget(btn);
  }
  // The slack of the row is split evenly into the gap before 类型 and the gap before 编号, so the combo sits
  // visually centred between its neighbours (10px is the minimum for both).
  count_type_row->addSpacing(10);
  count_type_row->addStretch(1);
  auto* type_label = new QLabel(tr("类型"), controls_);
  style_control_label(type_label, Qt::AlignVCenter);
  count_type_row->addWidget(type_label);
  type_combo_ = new QComboBox(controls_);
  type_combo_->addItem(QString::fromLatin1(wds::chart_editor::split_lane_type_name(
                           wds::chart_editor::SplitLaneType::BothEnds)),
                       static_cast<int>(wds::chart_editor::SplitLaneType::BothEnds));
  type_combo_->addItem(QString::fromLatin1(wds::chart_editor::split_lane_type_name(
                           wds::chart_editor::SplitLaneType::Full)),
                       static_cast<int>(wds::chart_editor::SplitLaneType::Full));
  type_combo_->addItem(QString::fromLatin1(wds::chart_editor::split_lane_type_name(
                           wds::chart_editor::SplitLaneType::Light)),
                       static_cast<int>(wds::chart_editor::SplitLaneType::Light));
  type_combo_->addItem(QString::fromLatin1(wds::chart_editor::split_lane_type_name(
                           wds::chart_editor::SplitLaneType::Ignore)),
                       static_cast<int>(wds::chart_editor::SplitLaneType::Ignore));
  // Natural width (fits the longest entry), no stretching.
  type_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  type_combo_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  // Keep the theme's horizontal padding; only zero the vertical padding/margin and pin the content height.
  type_combo_->setStyleSheet(QStringLiteral("QComboBox { ") + control_box_css() + QStringLiteral(" }"));
  type_combo_->setFixedHeight(kControlH);
  int type_index = type_combo_->findData(static_cast<int>(type_));
  if (type_index < 0) {
    type_index = type_combo_->findData(static_cast<int>(wds::chart_editor::SplitLaneType::Full));
  }
  type_combo_->setCurrentIndex(type_index);
  type_ = static_cast<wds::chart_editor::SplitLaneType>(type_combo_->currentData().toInt());
  count_type_row->addWidget(type_combo_);

  // 编号 search sits inline in the same row, flush right: the stretch goes before the label so the search box's
  // right edge is the right edge of the row. The fixed 10px spacing is the minimum gap after the type combo.
  count_type_row->addSpacing(10);
  count_type_row->addStretch(1);
  id_label_ = new QLabel(tr("编号"), controls_);
  style_control_label(id_label_, Qt::AlignVCenter);
  count_type_row->addWidget(id_label_);
  search_ = new QLineEdit(controls_);
  search_->setPlaceholderText(tr("筛选"));
  search_->setMaxLength(8);
  // Compact: just wide enough for 8 digits plus frame/padding.
  search_->setFixedWidth(QFontMetrics(search_->font()).horizontalAdvance(QStringLiteral("00000000")) +
                         search_->textMargins().left() + search_->textMargins().right() + 24);
  search_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  // Same as the combo: theme horizontal padding kept, vertical padding/margin zeroed, content height pinned.
  search_->setStyleSheet(QStringLiteral("QLineEdit { ") + control_box_css() + QStringLiteral(" }"));
  search_->setFixedHeight(kControlH);
  search_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("^[0-9]{0,8}$")),
                                                        search_));
  count_type_row->addWidget(search_);

  // Tag chip flow (图层 / 主题). The theme list is data-driven.
  // `balanced` (theme row): even-by-count lines, each full line justified to both edges, line spacing equal to the
  // grid's row spacing so the lines read as regular rows.
  auto make_chip_flow = [this](QWidget* parent, const std::vector<SplitEffectTagInfo>& tags, bool balanced) {
    auto* row = new QWidget(parent);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* flow = new ChipFlowLayout(row, 6, balanced ? kGridRowSpacing : 6, balanced);
    for (const auto& info : tags) {
      auto* chip = new TagChip(info, row);
      chip->on_changed = [this] {
        journal_menu_action("split_picker.filter");
        rebuild_colors();
      };
      flow->addWidget(chip);
      chips_.push_back(chip);
    }
    return row;
  };
  layer_flow_ = make_chip_flow(top_panel_, split_effect_layer_tags(), false);
  theme_flow_ = make_chip_flow(top_panel_, split_effect_theme_tags(), true);
  // 清除筛选 (icon only, same X as the shortcut dialog's clear buttons) is the trailing item of the 图层 flow, which
  // follows the column grid of the 主题 flow: the button sits right-aligned in the last column.
  clear_btn_ = new QPushButton(layer_flow_);
  clear_btn_->setObjectName(QStringLiteral("splitPickerClear"));
  clear_btn_->setIcon(themed_named_icon("clear", {}, 16));
  clear_btn_->setIconSize(QSize(16, 16));
  clear_btn_->setFlat(true);
  clear_btn_->setAutoDefault(false);
  clear_btn_->setDefault(false);
  clear_btn_->setFocusPolicy(Qt::NoFocus);
  clear_btn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  clear_btn_->setStyleSheet(QStringLiteral("QPushButton { ") + control_box_css() +
                            QStringLiteral(" min-width:%1px; max-width:%1px; width:%1px; padding:0px; }")
                                .arg(kControlContentH));
  clear_btn_->setFixedSize(kControlH, kControlH);
  clear_btn_->setToolTip(tr("清空筛选"));
  clear_btn_->setAccessibleName(tr("清空筛选"));
  clear_btn_->setEnabled(false);
  {
    ChipFlowLayout* layer_lay = flow_layout(layer_flow_);
    layer_lay->addWidget(clear_btn_);
    layer_lay->set_trailing_item(clear_btn_);
    layer_lay->set_column_master(flow_layout(theme_flow_));
  }
  update_chip_slot_width();
  QWidget* theme_row = theme_flow_;
  layer_label_ = new QLabel(tr("图层"), top_panel_);
  theme_label_ = new QLabel(tr("主题"), top_panel_);
  style_control_label(layer_label_, Qt::AlignLeft | Qt::AlignVCenter);
  style_control_label(theme_label_, Qt::AlignLeft | Qt::AlignVCenter);

  // Restore the previous opening's filter before the first rebuild_colors().
  {
    const SplitPickerMemory& mem = picker_memory();
    for (QPushButton* b : chips_) {
      auto* chip = static_cast<TagChip*>(b);
      if ((mem.include & chip->bit()) != 0) {
        chip->set_state(TagChip::State::Include);
      } else if ((mem.exclude & chip->bit()) != 0) {
        chip->set_state(TagChip::State::Exclude);
      }
    }
  }

  empty_label_ = new QLabel(tr("没有符合条件的分割线"));
  empty_label_->setAlignment(Qt::AlignCenter);
  empty_label_->setMinimumHeight(160);
  empty_label_->setStyleSheet(QStringLiteral("color: rgb(150,154,162);"));

  colors_ = new QScrollArea(this);
  colors_->setObjectName(QStringLiteral("splitPickerPreview"));
  colors_->setWidgetResizable(true);
  colors_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  colors_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  colors_->setMinimumWidth(0);
  colors_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  colors_->setStyleSheet(QStringLiteral(
      "QScrollArea#splitPickerPreview { padding:0px; margin:0px; }"));
  colors_host_ = new QWidget;
  colors_grid_ = new QGridLayout(colors_host_);
  colors_grid_->setContentsMargins(0, 0, 0, 0);
  colors_grid_->setHorizontalSpacing(6);
  colors_grid_->setVerticalSpacing(6);
  colors_grid_->setColumnStretch(0, 1);
  colors_grid_->setColumnStretch(1, 1);
  colors_grid_->setColumnStretch(2, 1);
  colors_->setWidget(colors_host_);
  empty_label_->setParent(colors_host_);
  empty_label_->hide();

  auto* grid = new QGridLayout(top_panel_);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(kGridSpacing);
  grid->setColumnStretch(0, 0);
  grid->setColumnStretch(1, 1);
  grid->setVerticalSpacing(kGridRowSpacing);
  count_label_ = new QLabel(tr("轨道数"), top_panel_);
  style_control_label(count_label_, Qt::AlignLeft | Qt::AlignVCenter);
  // Row 0 is centred; the wrapping rows are top-aligned so the label lines up with the FIRST chip line.
  grid->addWidget(count_label_, 0, 0, Qt::AlignVCenter);
  grid->addWidget(controls_, 0, 1, Qt::AlignVCenter);
  grid->addWidget(layer_label_, 1, 0, Qt::AlignTop);
  grid->addWidget(layer_flow_, 1, 1, Qt::AlignTop);
  grid->addWidget(theme_label_, 2, 0, Qt::AlignTop);
  grid->addWidget(theme_row, 2, 1, Qt::AlignTop);
  root->addWidget(top_panel_, 0, Qt::AlignHCenter);
  root->addSpacing(kPanelToListGap);  // breathing room between the filter rows and the preview list
  root->addWidget(colors_, 1);

  auto* buttons = new QDialogButtonBox(this);
  buttons->addButton(tr("确认"), QDialogButtonBox::AcceptRole);
  buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
  root->addWidget(buttons);

  connect(counts_, &QButtonGroup::idClicked, this, [this](int id) {
    journal_menu_action("split_picker.count");
    count_ = std::clamp(id, 1, 6);
    state_->count = count_;
    refresh_previews();
  });
  connect(type_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    journal_menu_action("split_picker.type");
    type_ = static_cast<wds::chart_editor::SplitLaneType>(type_combo_->currentData().toInt());
    state_->type = type_;
    refresh_previews();
  });
  connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { rebuild_colors(); });
  connect(clear_btn_, &QPushButton::clicked, this, [this] {
    journal_menu_action("split_picker.filter_clear");
    clear_filters();
  });
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    journal_menu_action("split_picker.ok");
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, [this] {
    journal_menu_action("split_picker.cancel");
    reject();
  });

  rebuild_colors();
  QTimer::singleShot(0, this, [this] {
    sync_aligned_width();
    last_preview_box_w_ = 0;
    schedule_locate_selected();
  });
}

// Both chip rows share one slot width = the widest chip overall, so the 图层 chips line up with the 主题 columns.
void SplitPickerDialog::update_chip_slot_width() {
  int w = 0;
  for (const QPushButton* b : chips_) w = std::max(w, b->sizeHint().width());
  if (ChipFlowLayout* l = flow_layout(layer_flow_)) l->set_min_slot_width(w);
  if (ChipFlowLayout* l = flow_layout(theme_flow_)) l->set_min_slot_width(w);
}

void SplitPickerDialog::changeEvent(QEvent* event) {
  QDialog::changeEvent(event);
  if (event != nullptr && clear_btn_ != nullptr &&
      (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)) {
    clear_btn_->setIcon(themed_named_icon("clear", {}, 16));
  }
}

int SplitPickerDialog::min_body_width() const {
  if (colors_ == nullptr) return 1;
  int sb = 0;
  if (QStyle* st = colors_->style()) {
    sb = st->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, colors_);
  }
  const QMargins cm = colors_->contentsMargins();
  return kPreviewCols * (kPreviewMinBox + kPreviewGap + 1) + sb + colors_->frameWidth() * 2 +
         cm.left() + cm.right() + 2;
}

// Width W of the top panel: label column + grid spacing + the widest of the three rows at its natural width.
//  row 0: the fixed items of the controls row (stretch collapsed -> minimumSizeHint)
//  row 1: single-line layer chips + one slot for the clear button (follower flow size hint)
//  row 2: theme chips are balanced over several justified lines, so they do NOT drive the width; only the widest
//         single chip is a lower bound.
int SplitPickerDialog::content_width() const {
  if (controls_ == nullptr) return 1;
  const int label_w = std::max({
      count_label_ != nullptr ? count_label_->sizeHint().width() : 0,
      layer_label_ != nullptr ? layer_label_->sizeHint().width() : 0,
      theme_label_ != nullptr ? theme_label_->sizeHint().width() : 0,
      1,
  });
  const int row0_w = controls_->minimumSizeHint().width();
  // The follower flow's size hint already contains the slot of the clear button.
  const int layer_w = layer_flow_ != nullptr ? layer_flow_->sizeHint().width() : 0;
  // Balanced flow: minimumSizeHint().width() == widest chip.
  const int theme_w = theme_flow_ != nullptr ? theme_flow_->minimumSizeHint().width() : 0;
  int w = label_w + kGridSpacing + std::max({row0_w, layer_w, theme_w, 1});
  // Never wider than the screen allows (chips wrap instead), but at least the preview body width.
  if (const QScreen* s = screen()) {
    const int cap = std::max(s->availableGeometry().width() - 80, min_body_width());
    w = std::min(w, cap);
  }
  return w;
}

void SplitPickerDialog::sync_aligned_width() {
  if (aligning_ || controls_ == nullptr || search_ == nullptr || colors_ == nullptr) return;
  aligning_ = true;
  update_chip_slot_width();
  const int panel_w = content_width();
  if (top_panel_ != nullptr) top_panel_->setFixedWidth(panel_w);
  const int body_w = std::max(panel_w, min_body_width());
  colors_->setFixedWidth(body_w);

  int chrome = width() - contentsRect().width();
  if (chrome < 0) chrome = 0;
  if (auto* lay = layout()) {
    const auto m = lay->contentsMargins();
    chrome += m.left() + m.right();
  }
  const int win_w = body_w + chrome;
  if (win_w > 0) setFixedWidth(win_w);
  aligning_ = false;
}

int SplitPickerDialog::preview_avail_width() const {
  if (colors_ == nullptr) return 0;
  int widget_w = colors_->width();
  if (widget_w <= 1) widget_w = std::max(content_width(), min_body_width());
  const int frame = colors_->frameWidth() * 2;
  int sb = 0;
  if (QStyle* st = colors_->style()) {
    sb = st->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, colors_);
  }
  const QMargins cm = colors_->contentsMargins();
  return std::max(0, widget_w - frame - sb - cm.left() - cm.right() - 2);
}

int SplitPickerDialog::preview_box_width() const {
  const int avail = preview_avail_width();
  if (avail <= 0) return kPreviewMinBox;
  return std::max(kPreviewMinBox, avail / kPreviewCols - kPreviewGap);
}

void SplitPickerDialog::resizeEvent(QResizeEvent* event) {
  QDialog::resizeEvent(event);
  sync_aligned_width();
  if (colors_ == nullptr) return;
  const int box_w = preview_box_width();
  if (std::abs(box_w - last_preview_box_w_) < 2) return;
  rebuild_colors();
}

void SplitPickerDialog::rebuild_colors() {
  if (colors_host_ == nullptr || colors_grid_ == nullptr || colors_ == nullptr) return;
  const QString query = search_ != nullptr ? search_->text() : QString();
  const auto ids =
      filter_split_picker_color_ids(query.toUtf8().toStdString(), build_tag_filter(chips_));
  update_filter_ui();
  // Detach (not delete) so thumb widgets can be reused; they paint lazily.
  while (QLayoutItem* child = colors_grid_->takeAt(0)) delete child;
  selected_thumb_ = nullptr;
  const int box_w = preview_box_width();
  last_preview_box_w_ = box_w;
  const QFontMetrics fm(colors_->font());
  const int thumb_h = kPreviewPlateH + kPreviewIdGap + std::max(fm.height(), 12);
  constexpr int kCols = kPreviewCols;
  ColorThumb* selected = nullptr;
  int row = 0;
  int col = 0;
  size_t used = 0;
  for (int32_t id : ids) {
    ColorThumb* thumb = nullptr;
    if (used < thumbs_.size()) {
      thumb = static_cast<ColorThumb*>(thumbs_[used]);
      thumb->set_id(id);
    } else {
      thumb = new ColorThumb(id, state_, colors_host_);
      thumb->on_click = [this, thumb] {
        journal_menu_action("split_picker.color");
        color_id_ = thumb->color_id();
        if (selected_thumb_ != nullptr && selected_thumb_ != thumb) {
          static_cast<ColorThumb*>(selected_thumb_)->set_selected(false);
        }
        selected_thumb_ = thumb;
        thumb->set_selected(true);
      };
      thumb->on_dblclick = [this, thumb] {
        color_id_ = thumb->color_id();
        journal_menu_action("split_picker.ok");
        accept();
      };
      thumbs_.push_back(thumb);
    }
    ++used;
    thumb->setFixedHeight(thumb_h);
    thumb->set_box_w(box_w);
    thumb->setToolTip(thumb_tooltip(id));
    const bool is_sel = id == color_id_;
    thumb->set_selected(is_sel);
    if (is_sel) selected = thumb;
    colors_grid_->addWidget(thumb, row, col, Qt::AlignTop);
    thumb->show();
    if (++col >= kCols) {
      col = 0;
      ++row;
    }
  }
  for (size_t i = used; i < thumbs_.size(); ++i) thumbs_[i]->hide();
  // Keep thumb rows packed at their natural height: only the empty row below the last thumb row absorbs spare
  // vertical space (otherwise a few results get stretched evenly over the whole viewport). With no results the
  // empty label's row takes the space instead, so the label stays centred.
  {
    const int thumb_rows = static_cast<int>((used + kCols - 1) / kCols);
    const int clear_rows = std::max(thumb_rows, static_cast<int>((thumbs_.size() + kCols - 1) / kCols)) + 2;
    for (int r = 0; r < clear_rows; ++r) colors_grid_->setRowStretch(r, 0);
    colors_grid_->setRowStretch(used == 0 ? 0 : thumb_rows, 1);
  }
  if (empty_label_ != nullptr) {
    if (used == 0) {
      colors_grid_->addWidget(empty_label_, 0, 0, 1, kCols, Qt::AlignCenter);
      empty_label_->show();
    } else {
      empty_label_->hide();
    }
  }
  active_thumbs_ = used;
  selected_thumb_ = selected;
  schedule_locate_selected();
  restart_prefetch();
}

void SplitPickerDialog::update_filter_ui() {
  if (clear_btn_ != nullptr) {
    const bool any = (search_ != nullptr && !search_->text().isEmpty()) ||
                     !build_tag_filter(chips_).empty();
    clear_btn_->setEnabled(any);
  }
}

void SplitPickerDialog::clear_filters() {
  if (search_ != nullptr) {
    const QSignalBlocker block(search_);
    search_->clear();
  }
  for (QPushButton* b : chips_) static_cast<TagChip*>(b)->set_state(TagChip::State::Off);
  rebuild_colors();
}

void SplitPickerDialog::save_filter_memory() const {
  SplitPickerMemory& mem = picker_memory();
  mem.include = 0;
  mem.exclude = 0;
  for (const QPushButton* b : chips_) {
    const auto* chip = static_cast<const TagChip*>(b);
    if (chip->state() == TagChip::State::Include) mem.include |= chip->bit();
    if (chip->state() == TagChip::State::Exclude) mem.exclude |= chip->bit();
  }
}

void SplitPickerDialog::done(int result) {
  save_filter_memory();
  QDialog::done(result);
}

// Count/type only change how thumbs paint (shared state_); keep layout and scroll position.
void SplitPickerDialog::refresh_previews() {
  for (size_t i = 0; i < active_thumbs_ && i < thumbs_.size(); ++i) thumbs_[i]->update();
  restart_prefetch();
}

void SplitPickerDialog::restart_prefetch() {
  const int gen = ++prefetch_gen_;
  if (!isVisible()) return;
  // One deferred call so the layout and scroll-to-selected settle before we decide what is visible.
  QTimer::singleShot(16, this, [this, gen] { run_prefetch(gen); });
}

void SplitPickerDialog::run_prefetch(int gen) {
  if (gen != prefetch_gen_ || colors_ == nullptr) return;
  const int box_w = preview_box_width();
  std::vector<int32_t> visible;
  std::vector<int32_t> rest;
  const QRect view = colors_->viewport()->rect();
  for (size_t i = 0; i < active_thumbs_ && i < thumbs_.size(); ++i) {
    auto* t = static_cast<ColorThumb*>(thumbs_[i]);
    const QRect r(t->mapTo(colors_->viewport(), QPoint(0, 0)), t->size());
    (r.intersects(view) ? visible : rest).push_back(t->color_id());
  }
  // The worker is LIFO (last request builds first): enqueue the off-screen thumbs first, then the visible ones in
  // reverse so the top-left thumb is the very last request. Requests are cheap; notify = nullptr (the thumbs
  // register their own repaint callback from paintEvent). A returned cluster pointer is not used here.
  // The catalog has ~300 ids but the worker queue holds 256: an unbounded off-screen prefetch would push the visible
  // thumbs' earlier (paint-time) requests out of the queue. Keep headroom; thumbs scrolled into view request
  // themselves from paintEvent anyway.
  constexpr std::size_t kMaxOffscreenPrefetch = 128;
  if (rest.size() > kMaxOffscreenPrefetch) rest.resize(kMaxOffscreenPrefetch);
  for (int32_t id : rest) {
    (void)split_effect_indicator_cluster_request(preview_cluster_key(count_, id, type_, box_w), nullptr, {});
  }
  for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
    (void)split_effect_indicator_cluster_request(preview_cluster_key(count_, *it, type_, box_w), nullptr, {});
  }
}

void SplitPickerDialog::schedule_locate_selected() {
  if (locate_pending_) return;
  locate_pending_ = true;
  QTimer::singleShot(0, this, [this] {
    locate_pending_ = false;
    locate_selected_color();
  });
}

void SplitPickerDialog::locate_selected_color() {
  if (colors_ == nullptr || colors_host_ == nullptr) return;
  if (QLayout* lay = colors_host_->layout()) lay->activate();
  QScrollBar* bar = colors_->verticalScrollBar();
  if (bar == nullptr) return;
  if (selected_thumb_ == nullptr) {
    locate_retries_ = 0;
    bar->setValue(bar->minimum());
    return;
  }
  if (selected_thumb_->height() <= 1 && locate_retries_ < 3) {
    ++locate_retries_;
    schedule_locate_selected();
    return;
  }
  const int y = selected_thumb_->mapTo(colors_host_, QPoint(0, 0)).y();
  if (y > 0 && bar->maximum() <= 0 && locate_retries_ < 3) {
    ++locate_retries_;
    schedule_locate_selected();
    return;
  }
  locate_retries_ = 0;
  bar->setValue(std::clamp(y, bar->minimum(), bar->maximum()));
}

void SplitPickerDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  schedule_locate_selected();
  restart_prefetch();
}

}  // namespace wds::ui

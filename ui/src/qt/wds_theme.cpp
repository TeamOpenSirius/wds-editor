#include "wds/ui/qt/wds_theme.hpp"

#include <wds/interaction/theme.hpp>

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QChildEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QSettings>
#include <QStyleHints>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QKeySequenceEdit>
#ifndef QT_NO_GESTURES
#include <QNativeGestureEvent>
#endif
#include <QHash>
#include <QHeaderView>
#include <QImageReader>
#include <QLibraryInfo>
#include <QMargins>
#include <QMetaEnum>
#include <QPainter>
#include <QPalette>
#include <QPluginLoader>
#include <QPointer>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QStyleOption>
#include <QSvgRenderer>
#include <QScrollBar>
#include <QStyle>
#include <QStyleFactory>
#include <QTextStream>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>
#include <algorithm>

// Compact, self-contained baker for OBS-style .obt themes. Handles the subset
// the bundled Yami theme uses: @OBSThemeVars declarations (color / size /
// number / string / var() alias / calc()/min()/max()), var() substitution in
// the QSS body, palette_* -> QPalette, and theme: / :res/images/ as QDir
// search prefixes (no absolute C:/ or file:/// URLs). This is deliberately
// not the full OBS engine (no watchers/user density).
namespace wds::ui {
namespace {

// Yami QSS paints QScrollBar, which makes QStyleSheetStyle force
// SH_ScrollBar_Transient=0 and PM_ScrollView_ScrollBarOverlap=0. Qt then insets
// the viewport. Negative viewport margins cancel that inset so bars float over
// the content instead of pushing it aside.
class OverlayScrollAccess : public QAbstractScrollArea {
 public:
  using QAbstractScrollArea::setViewportMargins;
  using QAbstractScrollArea::viewportMargins;
};

class OverlayScrollFilter final : public QObject {
  struct AreaState {
    QPointer<QAbstractScrollArea> area;
    QTimer* hide_timer = nullptr;
    bool hovered = false;
  };

 public:
  explicit OverlayScrollFilter(QObject* parent) : QObject(parent) {}

  void attach(QAbstractScrollArea* area) {
    if (area == nullptr || qobject_cast<QHeaderView*>(area) || states_.contains(area)) return;
    area->setProperty("wdsOverlayScroll", true);
    auto* state = new AreaState;
    state->area = area;
    state->hide_timer = new QTimer(area);
    state->hide_timer->setSingleShot(true);
    state->hide_timer->setInterval(800);
    QObject::connect(state->hide_timer, &QTimer::timeout, this, [this, area] { hide_idle(area); });
    states_.insert(area, state);
    QObject::connect(area, &QObject::destroyed, this, [this, area] { delete states_.take(area); });

    const auto sync = [this, area](int, int) {
      apply(area);
      AreaState* state = states_.value(area);
      if (state == nullptr) return;
      set_visual(area, state->hovered || slider_down(area) || state->hide_timer->isActive());
    };
    QObject::connect(area->verticalScrollBar(), &QScrollBar::rangeChanged, area, sync);
    QObject::connect(area->horizontalScrollBar(), &QScrollBar::rangeChanged, area, sync);
    const auto flash = [this, area](int) { reveal(area); };
    QObject::connect(area->verticalScrollBar(), &QScrollBar::valueChanged, area, flash);
    QObject::connect(area->horizontalScrollBar(), &QScrollBar::valueChanged, area, flash);
    QObject::connect(area->verticalScrollBar(), &QScrollBar::sliderPressed, area,
                     [this, area] { reveal(area); });
    QObject::connect(area->horizontalScrollBar(), &QScrollBar::sliderPressed, area,
                     [this, area] { reveal(area); });
    QObject::connect(area->verticalScrollBar(), &QScrollBar::sliderReleased, area,
                     [this, area] { schedule_hide(area); });
    QObject::connect(area->horizontalScrollBar(), &QScrollBar::sliderReleased, area,
                     [this, area] { schedule_hide(area); });

    if (QWidget* viewport = area->viewport()) {
      viewport->setMouseTracking(true);
      viewport->setAttribute(Qt::WA_Hover, true);
    }
    area->setMouseTracking(true);
    apply(area);
    set_visual(area, false);
  }

  void apply(QAbstractScrollArea* area) {
    if (area == nullptr || qobject_cast<QHeaderView*>(area) ||
        area->property("wdsOverlayBusy").toBool()) {
      return;
    }
    area->setProperty("wdsOverlayBusy", true);
    const int vw = area->verticalScrollBar()->sizeHint().width();
    const int hh = area->horizontalScrollBar()->sizeHint().height();
    const bool v = bar_needed(area->verticalScrollBar(), area->verticalScrollBarPolicy());
    const bool h = bar_needed(area->horizontalScrollBar(), area->horizontalScrollBarPolicy());
    const QMargins want(0, 0, v ? -vw : 0, h ? -hh : 0);
    auto* access = static_cast<OverlayScrollAccess*>(area);
    if (access->viewportMargins() != want) access->setViewportMargins(want);
    if (QWidget* parent = area->verticalScrollBar()->parentWidget()) parent->raise();
    if (QWidget* parent = area->horizontalScrollBar()->parentWidget()) parent->raise();
    area->setProperty("wdsOverlayBusy", false);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    const auto type = event->type();
    if (type == QEvent::Polish || type == QEvent::Show) {
      if (auto* area = qobject_cast<QAbstractScrollArea*>(watched)) attach(area);
      return false;
    }
    if (type == QEvent::StyleChange) {
      if (auto* area = qobject_cast<QAbstractScrollArea*>(watched)) apply(area);
      return false;
    }
    if (states_.isEmpty()) return false;
    if (type != QEvent::MouseMove && type != QEvent::HoverMove && type != QEvent::Wheel &&
        type != QEvent::Leave && type != QEvent::Enter && type != QEvent::HoverEnter &&
        type != QEvent::HoverLeave) {
      return false;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) return false;
    QAbstractScrollArea* area = enclosing_area(widget);
    if (area == nullptr) return false;
    if (type == QEvent::Wheel) reveal(area);
    else update_hover(area);
    return false;
  }

 private:
  static bool bar_needed(QScrollBar* bar, Qt::ScrollBarPolicy policy) {
    if (bar == nullptr || policy == Qt::ScrollBarAlwaysOff) return false;
    if (policy == Qt::ScrollBarAlwaysOn) return true;
    return bar->minimum() < bar->maximum();
  }

  static bool slider_down(const QAbstractScrollArea* area) {
    return area->verticalScrollBar()->isSliderDown() ||
           area->horizontalScrollBar()->isSliderDown();
  }

  static QWidget* bar_host(QScrollBar* bar) {
    if (bar == nullptr) return nullptr;
    return bar->parentWidget() != nullptr ? bar->parentWidget() : bar;
  }

  static bool point_over_bar(QAbstractScrollArea* area, const QPoint& global) {
    auto hit = [&](QScrollBar* bar, Qt::ScrollBarPolicy policy) {
      if (!bar_needed(bar, policy)) return false;
      QWidget* host = bar_host(bar);
      return host != nullptr && host->rect().contains(host->mapFromGlobal(global));
    };
    return hit(area->verticalScrollBar(), area->verticalScrollBarPolicy()) ||
           hit(area->horizontalScrollBar(), area->horizontalScrollBarPolicy());
  }

  static void set_bar_idle(QScrollBar* bar, bool idle) {
    if (bar->property("wdsIdle").toBool() == idle) return;
    bar->setProperty("wdsIdle", idle);
    if (QStyle* style = bar->style()) {
      style->unpolish(bar);
      style->polish(bar);
    }
    bar->update();
  }

  static void set_bar_visual(QScrollBar* bar, bool needed, bool shown) {
    if (bar == nullptr) return;
    QWidget* host = bar_host(bar);
    if (!needed) {
      bar->setVisible(false);
      return;
    }
    bar->setVisible(true);
    bar->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    bar->setAttribute(Qt::WA_Hover, true);
    bar->setMouseTracking(true);
    if (host != nullptr) {
      host->setAttribute(Qt::WA_TransparentForMouseEvents, false);
      host->setAttribute(Qt::WA_Hover, true);
      host->setMouseTracking(true);
      host->raise();
    }
    set_bar_idle(bar, !shown);
  }

  static void set_visual(QAbstractScrollArea* area, bool shown) {
    set_bar_visual(area->verticalScrollBar(),
                   bar_needed(area->verticalScrollBar(), area->verticalScrollBarPolicy()), shown);
    set_bar_visual(area->horizontalScrollBar(),
                   bar_needed(area->horizontalScrollBar(), area->horizontalScrollBarPolicy()),
                   shown);
  }

  QAbstractScrollArea* enclosing_area(QWidget* widget) const {
    for (QWidget* parent = widget; parent != nullptr; parent = parent->parentWidget()) {
      if (auto* area = qobject_cast<QAbstractScrollArea*>(parent))
        return states_.contains(area) ? area : nullptr;
    }
    return nullptr;
  }

  void reveal(QAbstractScrollArea* area) {
    AreaState* state = states_.value(area);
    if (state == nullptr) return;
    set_visual(area, true);
    if (state->hovered || slider_down(area)) state->hide_timer->stop();
    else state->hide_timer->start();
  }

  void schedule_hide(QAbstractScrollArea* area) {
    AreaState* state = states_.value(area);
    if (state == nullptr || state->hovered || slider_down(area)) return;
    state->hide_timer->start();
  }

  void hide_idle(QAbstractScrollArea* area) {
    AreaState* state = states_.value(area);
    if (state == nullptr || state->hovered || slider_down(area)) return;
    set_visual(area, false);
  }

  void update_hover(QAbstractScrollArea* area) {
    AreaState* state = states_.value(area);
    if (state == nullptr) return;
    const bool over = point_over_bar(area, QCursor::pos());
    if (state->hovered == over) {
      if (over) reveal(area);
      return;
    }
    state->hovered = over;
    if (over) reveal(area);
    else schedule_hide(area);
  }

  QHash<QAbstractScrollArea*, AreaState*> states_;
};

void install_overlay_scrollbars(QApplication& app) {
  static OverlayScrollFilter* filter = nullptr;
  if (filter == nullptr) {
    filter = new OverlayScrollFilter(&app);
    app.installEventFilter(filter);
  }
  const auto widgets = app.allWidgets();
  for (QWidget* widget : widgets) {
    if (auto* area = qobject_cast<QAbstractScrollArea*>(widget)) filter->attach(area);
  }
}

// Hover+wheel must not step or focus numeric / combo / shortcut fields. Those
// sit on the toolbar and in the settings scroll area, so a page scroll over
// the field is otherwise an accidental value change. QApplication::notify
// calls giveFocusAccordingToFocusPolicy *before* event filters, so the real
// intercept lives in WdsApplication::notify; this filter only keeps
// WheelFocus stripped (and is a fallback if notify is bypassed).
bool is_wheel_value_input(const QWidget* widget) {
  return qobject_cast<const QAbstractSpinBox*>(widget) != nullptr ||
         qobject_cast<const QComboBox*>(widget) != nullptr ||
         qobject_cast<const QKeySequenceEdit*>(widget) != nullptr;
}

void drop_wheel_focus_bit(QWidget* widget) {
  if (widget == nullptr) return;
  if ((widget->focusPolicy() & Qt::WheelFocus) == Qt::WheelFocus) {
    widget->setFocusPolicy(Qt::StrongFocus);
  }
}

void strip_wheel_focus(QWidget* widget) {
  if (widget == nullptr) return;
  QWidget* owner = nullptr;
  for (QWidget* cursor = widget; cursor != nullptr; cursor = cursor->parentWidget()) {
    if (is_wheel_value_input(cursor)) {
      owner = cursor;
      break;
    }
  }
  if (owner == nullptr) return;
  drop_wheel_focus_bit(owner);
  const auto children = owner->findChildren<QWidget*>();
  for (QWidget* child : children) drop_wheel_focus_bit(child);
}

bool steals_wheel_value(const QWidget* widget) {
  for (const QWidget* cursor = widget; cursor != nullptr; cursor = cursor->parentWidget()) {
    if (is_wheel_value_input(cursor)) return true;
  }
  return false;
}

QAbstractScrollArea* enclosing_scroll_area(QWidget* widget) {
  for (QWidget* parent = widget->parentWidget(); parent != nullptr;
       parent = parent->parentWidget()) {
    if (auto* area = qobject_cast<QAbstractScrollArea*>(parent)) return area;
  }
  return nullptr;
}

bool inside_active_popup(const QWidget* widget) {
  const QWidget* popup = QApplication::activePopupWidget();
  return popup != nullptr && widget != nullptr && widget->window() == popup;
}

void forward_wheel_to_scroll_area(QWidget* widget, QWheelEvent* wheel) {
  QAbstractScrollArea* area = enclosing_scroll_area(widget);
  if (area == nullptr) return;
  QWidget* viewport = area->viewport();
  if (viewport == nullptr || viewport == widget) return;
  const QPointF local = viewport->mapFromGlobal(wheel->globalPosition());
  QWheelEvent forwarded(local, wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
                        wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(),
                        wheel->source());
  QCoreApplication::sendEvent(viewport, &forwarded);
}

bool intercept_no_wheel_value(QObject* receiver, QEvent* event) {
  const auto type = event->type();
#ifndef QT_NO_GESTURES
  const bool pan_gesture =
      type == QEvent::NativeGesture &&
      static_cast<const QNativeGestureEvent*>(event)->gestureType() == Qt::PanNativeGesture;
#else
  const bool pan_gesture = false;
#endif
  if (type != QEvent::Wheel && !pan_gesture) return false;
  auto* widget = qobject_cast<QWidget*>(receiver);
  if (widget == nullptr) return false;
  strip_wheel_focus(widget);
  if (inside_active_popup(widget) || !steals_wheel_value(widget)) return false;
  if (type == QEvent::Wheel) {
    auto* wheel = static_cast<QWheelEvent*>(event);
    if (!wheel->spontaneous()) return false;
    forward_wheel_to_scroll_area(widget, wheel);
    return true;
  }
#ifndef QT_NO_GESTURES
  auto* gesture = static_cast<QNativeGestureEvent*>(event);
  const QPoint pixel = gesture->delta().toPoint();
  if (pixel.isNull()) return true;
  QAbstractScrollArea* area = enclosing_scroll_area(widget);
  if (area == nullptr) return true;
  QWidget* viewport = area->viewport();
  if (viewport == nullptr || viewport == widget) return true;
  const QPointF local = viewport->mapFromGlobal(gesture->globalPosition());
  QWheelEvent to_view(local, gesture->globalPosition(), pixel, QPoint(), Qt::NoButton,
                      gesture->modifiers(), Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(viewport, &to_view);
  return true;
#else
  return false;
#endif
}

class NoWheelValueFilter final : public QObject {
 public:
  explicit NoWheelValueFilter(QObject* parent) : QObject(parent) {}

  bool eventFilter(QObject* watched, QEvent* event) override {
    const auto type = event->type();
    if (type == QEvent::Polish || type == QEvent::Show || type == QEvent::StyleChange ||
        type == QEvent::ChildPolished) {
      if (type == QEvent::ChildPolished) {
        if (auto* child = qobject_cast<QWidget*>(static_cast<QChildEvent*>(event)->child())) {
          strip_wheel_focus(child);
        }
      } else if (auto* widget = qobject_cast<QWidget*>(watched)) {
        strip_wheel_focus(widget);
      }
    } else if (type == QEvent::ChildAdded) {
      if (auto* child = qobject_cast<QWidget*>(static_cast<QChildEvent*>(event)->child())) {
        QPointer<QWidget> later(child);
        QTimer::singleShot(0, child, [later] {
          if (later) strip_wheel_focus(later);
        });
      }
    }
    // WdsApplication::notify already consumes steal-wheels before filters.
    // This path only matters if a plain QApplication is used.
    return intercept_no_wheel_value(watched, event);
  }
};

bool ensure_svg_image_plugin() {
  if (QImageReader::supportedImageFormats().contains("svg")) return true;
#if defined(Q_OS_WIN)
  const QString plugin_name = QStringLiteral("qsvg.dll");
#elif defined(Q_OS_MACOS)
  const QString plugin_name = QStringLiteral("libqsvg.dylib");
#else
  const QString plugin_name = QStringLiteral("libqsvg.so");
#endif
  QStringList roots = QCoreApplication::libraryPaths();
  const QString plugins = QLibraryInfo::path(QLibraryInfo::PluginsPath);
  if (!plugins.isEmpty()) roots.prepend(plugins);
  static QList<QPluginLoader*> pinned;
  for (const QString& root : roots) {
    const QString path = QDir(root).filePath(QStringLiteral("imageformats/") + plugin_name);
    if (!QFile::exists(path)) continue;
    auto* loader = new QPluginLoader(path);
    if (loader->load()) {
      pinned.append(loader);
      break;
    }
    delete loader;
  }
  return QImageReader::supportedImageFormats().contains("svg");
}

// QSS image:url(theme:...) has failed three Windows attempts (C: scheme,
// file:///, search-path + qsvg). QStyleSheetStyle::loadPixmap feeds the raw
// string to QPixmap; if the rule is present and the pixmap is null the box
// stays blank, and if the rule is stripped Fusion paints Highlight/Accent
// blue. Draw the Yami SVGs here with Qt6::Svg so neither path is used.
class YamiIndicatorStyle final : public QProxyStyle {
 public:
  explicit YamiIndicatorStyle(QStyle* base) : QProxyStyle(base) {}

  void load_from(const QString& theme_dir, bool light) {
    qDeleteAll(renderers_);
    renderers_.clear();
    const QString folder = light ? QStringLiteral("Light") : QStringLiteral("Yami");
    const QDir dir(QDir(theme_dir).filePath(folder));
    const QStringList names = {QStringLiteral("checkbox_unchecked"),
                               QStringLiteral("checkbox_unchecked_focus"),
                               QStringLiteral("checkbox_checked"),
                               QStringLiteral("checkbox_checked_focus"),
                               QStringLiteral("checkbox_checked_disabled"),
                               QStringLiteral("checkbox_unchecked_disabled")};
    for (const QString& name : names) {
      const QString path = dir.filePath(name + QStringLiteral(".svg"));
      QFile file(path);
      auto* renderer = new QSvgRenderer(this);
      if (file.open(QIODevice::ReadOnly)) renderer->load(file.readAll());
      if (!renderer->isValid()) {
        qWarning("WDS theme: missing checkbox SVG %s", qUtf8Printable(path));
        delete renderer;
        continue;
      }
      renderers_.insert(name, renderer);
    }
  }

  void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override {
    if ((element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck) &&
        option != nullptr && painter != nullptr) {
      if (QSvgRenderer* renderer = renderer_for(*option)) {
        // option->rect is often not square: empty wrap-row boxes are
        // line-height tall, and QSS margin-right inflates the width.
        // Stretching the 1:1 SVG into that rect makes a rectangle.
        const QRect bounds = option->rect;
        const int side = std::min(bounds.width(), bounds.height());
        QRect square(0, 0, side, side);
        square.moveCenter(bounds.center());
        // Keep the 1:1 box on the left so QSS margin-right stays as
        // caption gap (settings wrap rows use 6px). Centering ate it.
        square.moveLeft(bounds.left());
        renderer->render(painter, QRectF(square));
        return;
      }
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
  }

  int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override {
    if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight) return 16;
    return QProxyStyle::pixelMetric(metric, option, widget);
  }

 private:
  QSvgRenderer* renderer_for(const QStyleOption& option) const {
    const bool on = option.state.testFlag(State_On) || option.state.testFlag(State_NoChange);
    const bool enabled = option.state.testFlag(State_Enabled);
    const bool hot = option.state.testFlag(State_MouseOver) ||
                     option.state.testFlag(State_HasFocus) || option.state.testFlag(State_Sunken);
    QString name;
    if (!enabled) {
      name = on ? QStringLiteral("checkbox_checked_disabled")
                : QStringLiteral("checkbox_unchecked_disabled");
    } else if (hot) {
      name = on ? QStringLiteral("checkbox_checked_focus")
                : QStringLiteral("checkbox_unchecked_focus");
    } else {
      name = on ? QStringLiteral("checkbox_checked") : QStringLiteral("checkbox_unchecked");
    }
    return renderers_.value(name);
  }

  QHash<QString, QSvgRenderer*> renderers_;
};

// Runtime values OBS injects; fixed here (no density/font-scale UI).
constexpr double kFontScale = 12.0;  // pt — matches QApplication and edit-canvas labels
constexpr double kPadding = 4.0;

struct Var {
  enum Kind { Color, Expr, String, Alias } kind = String;
  QString str;    // String / Alias key / raw numeric-or-calc expression
  QColor color;   // Color
};

QColor parse_color(const QString& raw) {
  QString s = raw.trimmed();
  static const QRegularExpression rgbRe(
      R"(rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([\d.]+)\s*)?\))");
  auto m = rgbRe.match(s);
  if (m.hasMatch()) {
    const int r = m.captured(1).toInt();
    const int g = m.captured(2).toInt();
    const int b = m.captured(3).toInt();
    const double a = m.captured(4).isEmpty() ? 1.0 : m.captured(4).toDouble();
    return QColor(r, g, b, static_cast<int>(std::lround(a * 255.0)));
  }
  return QColor(s);
}

Var parse_value(QString value) {
  Var v;
  value = value.trimmed();
  if (value.endsWith(';')) value.chop(1);
  value = value.trimmed();

  if (value.startsWith("var(") && value.endsWith(")") &&
      value.indexOf('(', 4) < 0) {
    v.kind = Var::Alias;
    v.str = value.mid(4, value.size() - 5).trimmed();  // --name
    return v;
  }
  if (value.startsWith('#') || value.startsWith("rgb(") || value.startsWith("rgba(")) {
    v.kind = Var::Color;
    v.color = parse_color(value);
    return v;
  }
  // Numeric literal, or any calc()/min()/max()/var() arithmetic expression.
  static const QRegularExpression exprRe(R"(^-?[\d.].*|^(calc|min|max|var)\()");
  if (exprRe.match(value).hasMatch()) {
    v.kind = Var::Expr;
    v.str = value;
    return v;
  }
  v.kind = Var::String;
  v.str = value;
  if ((v.str.startsWith('"') && v.str.endsWith('"')) ||
      (v.str.startsWith('\'') && v.str.endsWith('\'')))
    v.str = v.str.mid(1, v.str.size() - 2);
  return v;
}

// Resolve an alias chain to a concrete variable.
Var resolve(const QHash<QString, Var>& vars, Var v, int depth = 0) {
  while (v.kind == Var::Alias && depth < 40) {
    if (!vars.contains(v.str)) return v;
    v = vars.value(v.str);
    ++depth;
  }
  return v;
}

// --- Recursive-descent evaluator for the numeric expression grammar OBS uses:
//   expr   := term (('+'|'-') term)*
//   term   := factor (('*'|'/') factor)*
//   factor := number[suffix] | '(' expr ')' | 'calc(' expr ')'
//           | 'min(' expr ',' expr ')' | 'max(' expr ',' expr ')'
//           | 'var(' name ')' | name
// Values carry an optional unit suffix (px/pt/%); the suffix of a non-empty
// operand propagates (OBS requires matching or single suffixes).
struct Num {
  double value = 0.0;
  QString suffix;
};

class Eval {
 public:
  Eval(const QHash<QString, Var>& vars, int depth) : vars_(vars), depth_(depth) {}

  Num run(const QString& text) {
    s_ = text;
    i_ = 0;
    return parse_expr();
  }

 private:
  void skip() {
    while (i_ < s_.size() && s_[i_].isSpace()) ++i_;
  }
  bool eat(QChar c) {
    skip();
    if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
    return false;
  }
  bool peek_word(const char* w) {
    skip();
    const QString word = QString::fromLatin1(w);
    return s_.mid(i_, word.size()) == word;
  }

  Num combine(Num a, QChar op, Num b) {
    Num r;
    r.suffix = !a.suffix.isEmpty() ? a.suffix : b.suffix;
    switch (op.toLatin1()) {
      case '+': r.value = a.value + b.value; break;
      case '-': r.value = a.value - b.value; break;
      case '*': r.value = a.value * b.value; break;
      case '/': r.value = b.value != 0.0 ? a.value / b.value : 0.0; break;
    }
    return r;
  }

  Num parse_expr() {
    Num a = parse_term();
    for (;;) {
      skip();
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
        const QChar op = s_[i_++];
        a = combine(a, op, parse_term());
      } else {
        break;
      }
    }
    return a;
  }
  Num parse_term() {
    Num a = parse_factor();
    for (;;) {
      skip();
      if (i_ < s_.size() && (s_[i_] == '*' || s_[i_] == '/')) {
        const QChar op = s_[i_++];
        a = combine(a, op, parse_factor());
      } else {
        break;
      }
    }
    return a;
  }
  Num parse_factor() {
    skip();
    if (eat('(')) {
      Num r = parse_expr();
      eat(')');
      return r;
    }
    if (peek_word("calc(")) {
      i_ += 5;
      Num r = parse_expr();
      eat(')');
      return r;
    }
    if (peek_word("min(") || peek_word("max(")) {
      const bool isMin = s_[i_] == 'm' && s_[i_ + 1] == 'i';
      i_ += 4;
      Num a = parse_expr();
      eat(',');
      Num b = parse_expr();
      eat(')');
      Num r;
      r.suffix = !a.suffix.isEmpty() ? a.suffix : b.suffix;
      r.value = isMin ? std::min(a.value, b.value) : std::max(a.value, b.value);
      return r;
    }
    if (peek_word("var(")) {
      i_ += 4;
      int start = i_;
      while (i_ < s_.size() && s_[i_] != ')') ++i_;
      const QString name = s_.mid(start, i_ - start).trimmed();
      eat(')');
      return resolve_name(name);
    }
    // number [+ suffix]
    skip();
    int start = i_;
    if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
    while (i_ < s_.size() && (s_[i_].isDigit() || s_[i_] == '.')) ++i_;
    if (i_ > start) {
      Num r;
      r.value = s_.mid(start, i_ - start).toDouble();
      int sufStart = i_;
      while (i_ < s_.size() && (s_[i_].isLetter() || s_[i_] == '%')) ++i_;
      r.suffix = s_.mid(sufStart, i_ - sufStart);
      return r;
    }
    // bare identifier → variable name
    while (i_ < s_.size() && (s_[i_].isLetterOrNumber() || s_[i_] == '_' || s_[i_] == '-')) ++i_;
    const QString name = s_.mid(start, i_ - start).trimmed();
    return resolve_name(name);
  }

  Num resolve_name(QString name) {
    if (!name.startsWith("--")) name = "--" + name;
    if (depth_ > 40 || !vars_.contains(name)) return {};
    Var v = resolve(vars_, vars_.value(name), depth_);
    if (v.kind == Var::Expr) return Eval(vars_, depth_ + 1).run(v.str);
    return {};
  }

  const QHash<QString, Var>& vars_;
  int depth_;
  QString s_;
  int i_ = 0;
};

QString format_num(const Num& n) {
  const bool isInt = std::ceil(n.value) == n.value;
  if (n.suffix == "px") {
    return QString::number(static_cast<int>(std::lround(n.value))) + "px";
  }
  return QString::number(n.value, 'f', isInt ? 0 : 2) + n.suffix;
}

QString resolved_string(const QHash<QString, Var>& vars, const QString& key) {
  Var v = resolve(vars, vars.value(key));
  switch (v.kind) {
    case Var::Color:
      return v.color.name(v.color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
    case Var::Expr:
      return format_num(Eval(vars, 0).run(v.str));
    default:
      return v.str;
  }
}

QString read_file(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  return QTextStream(&file).readAll();
}

// Reads a value from an @OBSThemeMeta block (name/id/extends/dark).
QString meta_field(const QString& content, const QString& field) {
  const int metaStart = content.indexOf(QStringLiteral("@OBSThemeMeta"));
  if (metaStart < 0) return {};
  const int metaEnd = content.indexOf('}', metaStart);
  const QString block = content.mid(metaStart, metaEnd - metaStart);
  const QRegularExpression re(field + R"(\s*:\s*'([^']*)')");
  return re.match(block).captured(1);
}

// Merges @OBSThemeVars declarations from `content` into `vars`.
void collect_vars(const QString& content, QHash<QString, Var>& vars) {
  const int varsStart = content.indexOf(QStringLiteral("@OBSThemeVars"));
  if (varsStart < 0) return;
  const int open = content.indexOf('{', varsStart);
  int depth = 0, end = open;
  for (int i = open; i < content.size(); ++i) {
    if (content[i] == '{') ++depth;
    else if (content[i] == '}' && --depth == 0) { end = i; break; }
  }
  const QString block = content.mid(open + 1, end - open - 1);
  static const QRegularExpression declRe(R"(--([A-Za-z0-9_]+)\s*:\s*([^;]+);)");
  auto it = declRe.globalMatch(block);
  while (it.hasNext()) {
    auto m = it.next();
    vars.insert("--" + m.captured(1), parse_value(m.captured(2)));
  }
}

// QSS body = everything after the last OBS metadata section.
QString qss_body(const QString& content) {
  int bodyStart = 0;
  for (const QString& section :
       {QStringLiteral("OBSThemeMeta"), QStringLiteral("OBSThemeVars"), QStringLiteral("OBSTheme")}) {
    const int idx = content.indexOf(section);
    if (idx > bodyStart) bodyStart = content.indexOf('}', idx) + 1;
  }
  return content.mid(bodyStart);
}

// Resolves a theme id to its file, walking .obt (base) + .ovt (variants).
QString file_for_theme(const QString& theme_dir, const QString& theme_id) {
  QDir dir(theme_dir);
  for (const QString& name : dir.entryList({"*.obt", "*.ovt"}, QDir::Files)) {
    if (meta_field(read_file(dir.filePath(name)), "id") == theme_id)
      return dir.filePath(name);
  }
  return {};
}

constexpr auto kPrefSystem = "system";
constexpr auto kPrefLight = "light";
constexpr auto kPrefDark = "dark";
constexpr auto kIdYami = "com.obsproject.Yami";
constexpr auto kIdLight = "com.obsproject.Yami.Light";

}  // namespace

void install_no_wheel_value_inputs(QApplication& app) {
  static NoWheelValueFilter* filter = nullptr;
  if (filter == nullptr) {
    filter = new NoWheelValueFilter(&app);
    app.installEventFilter(filter);
  }
  const auto widgets = app.allWidgets();
  for (QWidget* widget : widgets) strip_wheel_focus(widget);
}

bool WdsApplication::notify(QObject* receiver, QEvent* event) {
  if (intercept_no_wheel_value(receiver, event)) return true;
  return QApplication::notify(receiver, event);
}

QString normalize_theme_preference(const QString& stored) {
  if (stored == QLatin1String(kPrefLight) || stored == QLatin1String(kIdLight))
    return QStringLiteral("light");
  if (stored == QLatin1String(kPrefDark) || stored == QLatin1String(kIdYami))
    return QStringLiteral("dark");
  if (stored.startsWith(QLatin1String("com.obsproject.Yami"))) return QStringLiteral("dark");
  return QStringLiteral("system");
}

QString resolve_obs_theme_id(const QString& preference) {
  const QString pref = normalize_theme_preference(preference);
  if (pref == QLatin1String(kPrefLight)) return QStringLiteral("com.obsproject.Yami.Light");
  if (pref == QLatin1String(kPrefDark)) return QStringLiteral("com.obsproject.Yami");
  const auto scheme = QGuiApplication::styleHints()->colorScheme();
  return scheme == Qt::ColorScheme::Light ? QStringLiteral("com.obsproject.Yami.Light")
                                          : QStringLiteral("com.obsproject.Yami");
}

void watch_system_color_scheme(QApplication& app, const QString& theme_dir) {
  static QString dir;
  dir = theme_dir;
  static bool connected = false;
  if (connected) return;
  connected = true;
  QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app,
                   [&app](Qt::ColorScheme) {
                     const QString pref = normalize_theme_preference(
                         QSettings(QStringLiteral("WDS"), QStringLiteral("WDS Editor"))
                             .value(QStringLiteral("appearance/theme"))
                             .toString());
                     if (pref == QLatin1String(kPrefSystem)) apply_wds_theme(app, dir, pref);
                   });
}

QVector<ThemeInfo> available_themes(const QString& theme_dir) {
  QVector<ThemeInfo> out;
  QDir dir(theme_dir);
  // Variant themes first (they populate the picker); skip System passthrough.
  for (const QString& name : dir.entryList({"*.obt", "*.ovt"}, QDir::Files, QDir::Name)) {
    const QString content = read_file(dir.filePath(name));
    const QString id = meta_field(content, "id");
    if (id.isEmpty() || id == QStringLiteral("com.obsproject.System")) continue;
    // Only Yami-family (base + variants) — that's what we ship icons/vars for.
    const bool isBase = id == QStringLiteral("com.obsproject.Yami");
    if (!isBase && meta_field(content, "extends") != QStringLiteral("com.obsproject.Yami"))
      continue;
    ThemeInfo info;
    info.id = id;
    info.name = meta_field(content, "name");
    info.dark = meta_field(content, "dark") != QStringLiteral("false");
    out.push_back(info);
  }
  return out;
}

void apply_wds_theme(QApplication& app, const QString& theme_dir, const QString& theme_id) {
  watch_system_color_scheme(app, theme_dir);
  const QString obs_id = resolve_obs_theme_id(theme_id);
  wds::interaction::theme::apply_color_scheme(
      obs_id == QLatin1String("com.obsproject.Yami.Light")
          ? wds::interaction::theme::ColorScheme::Light
          : wds::interaction::theme::ColorScheme::Dark);
  const bool light = obs_id == QLatin1String("com.obsproject.Yami.Light");
  auto* indicators = new YamiIndicatorStyle(QStyleFactory::create("Fusion"));
  indicators->load_from(theme_dir, light);
  QApplication::setStyle(indicators);

  const QString base_content = read_file(QDir(theme_dir).filePath(QStringLiteral("Yami.obt")));
  if (base_content.isEmpty()) return;

  // Optional variant layered on the base (overrides vars, appends QSS).
  QString variant_content;
  if (!obs_id.isEmpty() && obs_id != QStringLiteral("com.obsproject.Yami")) {
    const QString path = file_for_theme(theme_dir, obs_id);
    if (!path.isEmpty()) variant_content = read_file(path);
  }

  // 1. Collect vars: base first, then variant overrides.
  QHash<QString, Var> vars;
  collect_vars(base_content, vars);
  if (!variant_content.isEmpty()) collect_vars(variant_content, vars);
  // Runtime-injected values.
  vars["--obsFontScale"] = parse_value(QString::number(kFontScale));
  vars["--obsPadding"] = parse_value(QString::number(kPadding));

  // 2. QSS body: base then variant appended.
  QString qss = qss_body(base_content);
  if (!variant_content.isEmpty()) qss += "\n" + qss_body(variant_content);

  // 3. Substitute var(--x) with concrete values (longest names first so
  // --primary_light isn't clipped by --primary).
  QStringList names = vars.keys();
  std::sort(names.begin(), names.end(),
            [](const QString& a, const QString& b) { return a.size() > b.size(); });
  for (const QString& name : names) {
    qss.replace(QStringLiteral("var(%1)").arg(name), resolved_string(vars, name));
  }

  // 4. Keep theme: urls as QDir search-path prefixes so QSS never sees a
  // Windows drive letter (url("C:/...") → scheme "C") or a file:/// string
  // that QImage treats as a missing file. :res/images/ is OBS's Common/.
  const QString base = QDir(theme_dir).absolutePath();
  QDir::setSearchPaths(QStringLiteral("theme"), {base});
  QDir::setSearchPaths(QStringLiteral("resimg"), {QDir(base).filePath(QStringLiteral("Common"))});
  qss.replace(QStringLiteral(":res/images/"), QStringLiteral("resimg:"));

  // Checkbox SVGs are drawn by YamiIndicatorStyle. Leave those image rules
  // in QSS and QStyleSheetStyle treats hasImage as true even when the pixmap
  // is null, which paints an empty box and never reaches the proxy.
  qss.remove(QRegularExpression(
      QStringLiteral(R"(image:\s*url\(\s*"?(?:theme:Yami|theme:Light)/checkbox_[^")]+"?\s*\);)")));
  ensure_svg_image_plugin();

  // 5. Force the bundled Noto Sans SC as the primary family so CJK renders and
  // the theme's 'Open Sans' fallback (unbundled, Latin-only) doesn't win.
  qss.replace(QStringLiteral("'Open Sans'"), QStringLiteral("'Noto Sans SC'"));

  // 5. Palette from palette_* vars (names are lowercase-first, e.g.
  // palette_windowText -> QPalette::WindowText).
  QPalette pal(app.palette());
  QHash<QString, QPalette::ColorRole> roleMap;
  {
    const QMetaEnum roleEnum = QMetaEnum::fromType<QPalette::ColorRole>();
    for (int i = 0; i < roleEnum.keyCount(); ++i)
      roleMap.insert(QString::fromLatin1(roleEnum.key(i)).toLower(),
                     static_cast<QPalette::ColorRole>(roleEnum.value(i)));
  }
  for (auto it = vars.constBegin(); it != vars.constEnd(); ++it) {
    if (!it.key().startsWith("--palette_")) continue;
    const QString roleName = it.key().mid(QStringLiteral("--palette_").size()).toLower();
    if (!roleMap.contains(roleName)) continue;
    Var v = resolve(vars, it.value());
    if (v.kind == Var::Color) pal.setColor(roleMap.value(roleName), v.color);
  }
  app.setPalette(pal);
  app.setStyleSheet(qss);
  install_overlay_scrollbars(app);
  install_no_wheel_value_inputs(app);
  const auto widgets = app.allWidgets();
  for (QWidget* widget : widgets) widget->update();
}

}  // namespace wds::ui

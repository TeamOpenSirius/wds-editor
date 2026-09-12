#include "wds/ui/qt/wds_theme.hpp"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QMetaEnum>
#include <QPalette>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QTextStream>

#include <cmath>
#include <algorithm>

// Compact, self-contained baker for OBS-style .obt themes. Handles the subset
// the bundled Yami theme uses: @OBSThemeVars declarations (color / size /
// number / string / var() alias / calc()/min()/max()), var() substitution in
// the QSS body, palette_* -> QPalette, and url(theme:...) rewriting. This is
// deliberately not the full OBS engine (no watchers/user density).
namespace wds::ui {
namespace {

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

}  // namespace

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
  QApplication::setStyle(QStyleFactory::create("Fusion"));

  const QString base_content = read_file(QDir(theme_dir).filePath(QStringLiteral("Yami.obt")));
  if (base_content.isEmpty()) return;

  // Optional variant layered on the base (overrides vars, appends QSS).
  QString variant_content;
  if (!theme_id.isEmpty() && theme_id != QStringLiteral("com.obsproject.Yami")) {
    const QString path = file_for_theme(theme_dir, theme_id);
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

  // 4. Rewrite url(theme:...) to the bundled asset directory.
  const QString base = QDir(theme_dir).absolutePath();
  qss.replace(QRegularExpression(R"(url\(\s*theme:)"),
              QStringLiteral("url(%1/").arg(base));
  qss.replace(QStringLiteral(":res/images/"), base + QStringLiteral("/Common/"));

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
}

}  // namespace wds::ui

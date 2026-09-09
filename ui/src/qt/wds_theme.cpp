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

// Compact, self-contained baker for OBS-style .obt themes. Handles the subset
// the bundled Yami theme uses: @OBSThemeVars declarations (color / size /
// number / string / var() alias / calc()/min()/max()), var() substitution in
// the QSS body, palette_* -> QPalette, and url(theme:...) rewriting. This is
// deliberately not the full OBS engine (no variants/watchers/user density).
namespace wds::ui {
namespace {

// Runtime values OBS injects; fixed here (no density/font-scale UI).
constexpr double kFontScale = 10.0;  // pt
constexpr double kPadding = 4.0;

struct Var {
  enum Kind { Color, Size, Number, String, Alias, Calc, Min, Max } kind = String;
  QString suffix;
  QString str;                 // String / Alias key
  double num = 0.0;            // Number / Size
  QColor color;                // Color
  QStringList calc;            // Calc/Min/Max: [a, op, b]
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

  if (value.startsWith("var(") && value.endsWith(")")) {
    v.kind = Var::Alias;
    v.str = value.mid(4, value.size() - 5).trimmed();  // --name
    return v;
  }
  auto func_args = [&](const QString& fn) -> QStringList {
    QString inner = value.mid(fn.size() + 1, value.size() - fn.size() - 2);
    // Split top-level by ',' and (for calc) by operators.
    return inner.split(QRegularExpression(R"(\s*,\s*)"), Qt::SkipEmptyParts);
  };
  if (value.startsWith("calc(") && value.endsWith(")")) {
    v.kind = Var::Calc;
    QString inner = value.mid(5, value.size() - 6).trimmed();
    static const QRegularExpression opRe(R"(^(.*\S)\s*([+\-*/])\s*(\S.*)$)");
    auto m = opRe.match(inner);
    if (m.hasMatch())
      v.calc = {m.captured(1).trimmed(), m.captured(2), m.captured(3).trimmed()};
    return v;
  }
  if (value.startsWith("min(") && value.endsWith(")")) {
    v.kind = Var::Min;
    const auto a = func_args("min");
    if (a.size() == 2) v.calc = {a[0], ",", a[1]};
    return v;
  }
  if (value.startsWith("max(") && value.endsWith(")")) {
    v.kind = Var::Max;
    const auto a = func_args("max");
    if (a.size() == 2) v.calc = {a[0], ",", a[1]};
    return v;
  }
  if (value.startsWith('#') || value.startsWith("rgb(") || value.startsWith("rgba(")) {
    v.kind = Var::Color;
    v.color = parse_color(value);
    return v;
  }
  // number [+ suffix]
  static const QRegularExpression numRe(R"(^(-?[\d.]+)\s*([a-zA-Z%]*)$)");
  auto m = numRe.match(value);
  if (m.hasMatch()) {
    v.num = m.captured(1).toDouble();
    v.suffix = m.captured(2);
    v.kind = v.suffix.isEmpty() ? Var::Number : Var::Size;
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
  while (v.kind == Var::Alias && depth < 20) {
    if (!vars.contains(v.str)) return v;
    v = vars.value(v.str);
    ++depth;
  }
  return v;
}

double eval_num(const QHash<QString, Var>& vars, const QString& token, QString& suffix,
                int depth);

double operand(const QHash<QString, Var>& vars, const QString& tok, QString& suffix, int depth) {
  const QString t = tok.trimmed();
  static const QRegularExpression numRe(R"(^(-?[\d.]+)\s*([a-zA-Z%]*)$)");
  auto m = numRe.match(t);
  if (m.hasMatch()) {
    if (!m.captured(2).isEmpty()) suffix = m.captured(2);
    return m.captured(1).toDouble();
  }
  QString key = t;
  if (key.startsWith("var(") && key.endsWith(")")) key = key.mid(4, key.size() - 5).trimmed();
  if (!vars.contains(key)) return 0.0;
  return eval_num(vars, key, suffix, depth + 1);
}

double eval_num(const QHash<QString, Var>& vars, const QString& key, QString& suffix, int depth) {
  if (depth > 20) return 0.0;
  Var v = resolve(vars, vars.value(key), depth);
  if (v.kind == Var::Number || v.kind == Var::Size) {
    if (!v.suffix.isEmpty()) suffix = v.suffix;
    return v.num;
  }
  if (v.calc.size() == 3) {
    const double a = operand(vars, v.calc[0], suffix, depth);
    const double b = operand(vars, v.calc[2], suffix, depth);
    const QString& op = v.calc[1];
    if (v.kind == Var::Min) return std::min(a, b);
    if (v.kind == Var::Max) return std::max(a, b);
    if (op == "+") return a + b;
    if (op == "-") return a - b;
    if (op == "*") return a * b;
    if (op == "/") return b != 0.0 ? a / b : 0.0;
  }
  return 0.0;
}

QString format_num(double val, const QString& suffix) {
  const bool isInt = std::ceil(val) == val;
  QString out = QString::number(val, 'f', isInt ? 0 : 2);
  if (suffix == "px") out = QString::number(static_cast<int>(std::lround(val)));
  return out + suffix;
}

QString resolved_string(const QHash<QString, Var>& vars, const QString& key) {
  Var v = resolve(vars, vars.value(key));
  switch (v.kind) {
    case Var::Color:
      return v.color.name(v.color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
    case Var::Size:
    case Var::Number: {
      QString suffix = v.suffix;
      const double n = eval_num(vars, key, suffix, 0);
      return format_num(n, suffix);
    }
    case Var::Calc:
    case Var::Min:
    case Var::Max: {
      QString suffix;
      const double n = eval_num(vars, key, suffix, 0);
      return format_num(n, suffix);
    }
    default:
      return v.str;
  }
}

}  // namespace

void apply_wds_theme(QApplication& app, const QString& theme_dir) {
  QApplication::setStyle(QStyleFactory::create("Fusion"));

  QFile file(QDir(theme_dir).filePath(QStringLiteral("Yami.obt")));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  const QString content = QTextStream(&file).readAll();

  // 1. Collect @OBSThemeVars declarations.
  QHash<QString, Var> vars;
  {
    const int varsStart = content.indexOf(QStringLiteral("@OBSThemeVars"));
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
  // Runtime-injected values.
  vars["--obsFontScale"] = parse_value(QString::number(kFontScale));
  vars["--obsPadding"] = parse_value(QString::number(kPadding));

  // 2. QSS body = everything after the last OBS metadata section.
  int bodyStart = 0;
  for (const QString& section :
       {QStringLiteral("OBSThemeMeta"), QStringLiteral("OBSThemeVars"), QStringLiteral("OBSTheme")}) {
    const int idx = content.indexOf(section);
    if (idx > bodyStart) bodyStart = content.indexOf('}', idx) + 1;
  }
  QString qss = content.mid(bodyStart);

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

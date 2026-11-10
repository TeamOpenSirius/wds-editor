#include "wds/ui/constant_expr.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wds::ui {
namespace {

enum class Kind : std::uint8_t { Number, Add, Sub, Mul, Div, LParen, RParen, UMinus, UPlus };

struct Token {
  Kind kind = Kind::Number;
  double value = 0.0;
};

int precedence(Kind k) noexcept {
  switch (k) {
    case Kind::UMinus:
    case Kind::UPlus:
      return 3;
    case Kind::Mul:
    case Kind::Div:
      return 2;
    case Kind::Add:
    case Kind::Sub:
      return 1;
    default:
      return 0;
  }
}

bool is_right_assoc(Kind k) noexcept { return k == Kind::UMinus || k == Kind::UPlus; }

bool is_operator(Kind k) noexcept {
  return k == Kind::Add || k == Kind::Sub || k == Kind::Mul || k == Kind::Div || k == Kind::UMinus ||
         k == Kind::UPlus;
}

bool parse_number(std::string_view text, std::size_t& i, double& value) noexcept {
  const std::size_t n = text.size();
  const std::size_t start = i;
  bool had_digit = false;
  bool had_dot = false;
  while (i < n) {
    const char c = text[i];
    if (c >= '0' && c <= '9') {
      had_digit = true;
      ++i;
      continue;
    }
    if (c == '.' && !had_dot) {
      had_dot = true;
      ++i;
      continue;
    }
    break;
  }
  if (!had_digit || i == start) {
    i = start;
    return false;
  }
  const std::size_t len = i - start;
  char buf[64];
  if (len >= sizeof(buf)) {
    i = start;
    return false;
  }
  std::memcpy(buf, text.data() + start, len);
  buf[len] = '\0';
  char* end = nullptr;
  errno = 0;
  value = std::strtod(buf, &end);
  if (end != buf + len || errno == ERANGE || !std::isfinite(value)) {
    i = start;
    return false;
  }
  return true;
}

bool tokenize(std::string_view text, std::vector<Token>& out) {
  out.clear();
  bool expect_unary = true;
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    const char c = text[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++i;
      continue;
    }
    if ((c >= '0' && c <= '9') || c == '.') {
      if (!expect_unary) return false;
      double value = 0.0;
      if (!parse_number(text, i, value)) return false;
      out.push_back({Kind::Number, value});
      expect_unary = false;
      continue;
    }
    if (c == '(') {
      if (!expect_unary) return false;
      out.push_back({Kind::LParen, 0.0});
      expect_unary = true;
      ++i;
      continue;
    }
    if (c == ')') {
      if (expect_unary) return false;
      out.push_back({Kind::RParen, 0.0});
      expect_unary = false;
      ++i;
      continue;
    }
    Kind op = Kind::Add;
    if (c == '+') {
      op = expect_unary ? Kind::UPlus : Kind::Add;
    } else if (c == '-') {
      op = expect_unary ? Kind::UMinus : Kind::Sub;
    } else if (c == '*') {
      if (expect_unary) return false;
      op = Kind::Mul;
    } else if (c == '/') {
      if (expect_unary) return false;
      op = Kind::Div;
    } else {
      return false;
    }
    out.push_back({op, 0.0});
    expect_unary = true;
    ++i;
  }
  return !out.empty() && !expect_unary;
}

bool to_rpn(const std::vector<Token>& infix, std::vector<Token>& rpn) {
  rpn.clear();
  rpn.reserve(infix.size());
  std::vector<Kind> ops;
  ops.reserve(infix.size());
  for (const Token& tok : infix) {
    if (tok.kind == Kind::Number) {
      rpn.push_back(tok);
      continue;
    }
    if (tok.kind == Kind::LParen) {
      ops.push_back(tok.kind);
      continue;
    }
    if (tok.kind == Kind::RParen) {
      bool found = false;
      while (!ops.empty()) {
        const Kind top = ops.back();
        ops.pop_back();
        if (top == Kind::LParen) {
          found = true;
          break;
        }
        rpn.push_back({top, 0.0});
      }
      if (!found) return false;
      continue;
    }
    while (!ops.empty()) {
      const Kind top = ops.back();
      if (top == Kind::LParen || !is_operator(top)) break;
      const int pt = precedence(top);
      const int pc = precedence(tok.kind);
      if (is_right_assoc(tok.kind) ? pt <= pc : pt < pc) break;
      ops.pop_back();
      rpn.push_back({top, 0.0});
    }
    ops.push_back(tok.kind);
  }
  while (!ops.empty()) {
    const Kind top = ops.back();
    ops.pop_back();
    if (top == Kind::LParen || top == Kind::RParen) return false;
    rpn.push_back({top, 0.0});
  }
  return !rpn.empty();
}

std::optional<double> eval_rpn(const std::vector<Token>& rpn) {
  std::vector<double> st;
  st.reserve(rpn.size());
  for (const Token& tok : rpn) {
    if (tok.kind == Kind::Number) {
      st.push_back(tok.value);
      continue;
    }
    if (tok.kind == Kind::UMinus || tok.kind == Kind::UPlus) {
      if (st.empty()) return std::nullopt;
      double& x = st.back();
      if (tok.kind == Kind::UMinus) x = -x;
      continue;
    }
    if (st.size() < 2) return std::nullopt;
    const double b = st.back();
    st.pop_back();
    const double a = st.back();
    st.pop_back();
    double r = 0.0;
    switch (tok.kind) {
      case Kind::Add:
        r = a + b;
        break;
      case Kind::Sub:
        r = a - b;
        break;
      case Kind::Mul:
        r = a * b;
        break;
      case Kind::Div:
        if (b == 0.0) return std::nullopt;
        r = a / b;
        break;
      default:
        return std::nullopt;
    }
    if (!std::isfinite(r)) return std::nullopt;
    st.push_back(r);
  }
  if (st.size() != 1 || !std::isfinite(st.front())) return std::nullopt;
  return st.front();
}

double round_to_decimals(double value, int decimals) noexcept {
  if (!std::isfinite(value) || decimals <= 0) return std::round(value);
  double scale = 1.0;
  for (int i = 0; i < decimals; ++i) scale *= 10.0;
  return std::round(value * scale) / scale;
}

int digit_dot_slots(std::string_view s) noexcept {
  int n = 0;
  for (char c : s) {
    if (c == '.' || (c >= '0' && c <= '9')) ++n;
  }
  return n;
}

}  // namespace

std::optional<double> eval_constant_expr(std::string_view text) {
  std::vector<Token> infix;
  if (!tokenize(text, infix)) return std::nullopt;
  std::vector<Token> rpn;
  if (!to_rpn(infix, rpn)) return std::nullopt;
  const auto value = eval_rpn(rpn);
  if (!value) return std::nullopt;
  double rounded = round_to_decimals(*value, kConstantExprMaxDecimals);
  if (!std::isfinite(rounded)) return std::nullopt;
  if (rounded == 0.0) rounded = 0.0;
  return rounded;
}

std::string format_trimmed_decimal(double value, int max_decimals) {
  if (!std::isfinite(value)) return "0";
  if (max_decimals < 0) max_decimals = 0;
  if (max_decimals > 9) max_decimals = 9;
  if (std::fabs(value) < 0.5 * std::pow(10.0, -max_decimals)) return "0";
  char fmt[8];
  std::snprintf(fmt, sizeof(fmt), "%%.%df", max_decimals);
  char buf[64];
  std::snprintf(buf, sizeof(buf), fmt, value);
  std::string s(buf);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") return "0";
  return s.empty() ? "0" : s;
}

std::string format_slot_budget_decimal(double value, int max_slots) {
  if (!std::isfinite(value)) return "0";
  if (max_slots < 1) max_slots = 1;
  if (std::fabs(value) < 1e-15) return "0";
  const bool neg = value < 0.0;
  const double mag = std::fabs(value);
  std::string body = format_trimmed_decimal(mag, kConstantExprMaxDecimals);
  auto with_sign = [&](const std::string& digits) {
    return neg ? ("-" + digits) : digits;
  };
  if (digit_dot_slots(body) <= max_slots) return with_sign(body);
  for (int frac = kConstantExprMaxDecimals - 1; frac >= 0; --frac) {
    body = format_trimmed_decimal(round_to_decimals(mag, frac), frac);
    if (digit_dot_slots(body) <= max_slots) return with_sign(body);
  }
  return with_sign(body);
}

}  // namespace wds::ui

#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace wds::interaction {

inline std::optional<int> parse_positive_int(std::string_view text) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value <= 0) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<int> parse_non_negative_int(std::string_view text) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 0) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<int> parse_signed_int(std::string_view text) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<double> parse_speed(std::string_view text, double min = 1.0) {
  try {
    std::size_t parsed = 0;
    const double value = std::stod(std::string(text), &parsed);
    if (parsed != text.size() || !std::isfinite(value) || value < min) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace wds::interaction

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace wds::interaction::utf8_edit {

inline std::size_t prev_offset(const std::string& text, std::size_t i) noexcept {
  if (i == 0) return 0;
  --i;
  while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
    --i;
  }
  return i;
}

inline std::size_t next_offset(const std::string& text, std::size_t i) noexcept {
  if (i >= text.size()) return text.size();
  const auto b = static_cast<unsigned char>(text[i]);
  std::size_t n = 1;
  if ((b & 0xE0) == 0xC0) {
    n = 2;
  } else if ((b & 0xF0) == 0xE0) {
    n = 3;
  } else if ((b & 0xF8) == 0xF0) {
    n = 4;
  }
  return i + n > text.size() ? text.size() : i + n;
}

inline void insert(std::string& text, std::size_t& caret, std::string_view s) {
  caret = std::min(caret, text.size());
  text.insert(caret, s.data(), s.size());
  caret += s.size();
}

inline void erase_prev(std::string& text, std::size_t& caret) {
  caret = std::min(caret, text.size());
  if (caret == 0) return;
  const std::size_t from = prev_offset(text, caret);
  text.erase(from, caret - from);
  caret = from;
}

inline void erase_next(std::string& text, std::size_t& caret) {
  caret = std::min(caret, text.size());
  if (caret >= text.size()) return;
  const std::size_t to = next_offset(text, caret);
  text.erase(caret, to - caret);
}

}  // namespace wds::interaction::utf8_edit

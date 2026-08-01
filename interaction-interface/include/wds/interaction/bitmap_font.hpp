#pragma once

#include <array>
#include <cstdint>

namespace wds::interaction::bitmap_font {

// Each byte is one row; its low eight bits describe pixels from left to right.
using Glyph = std::array<std::uint8_t, 8>;

// Returns an 8×8 glyph for common ASCII (letters, digits, % and punctuation).
// Unsupported characters use the '?' glyph. The atlas is bundled in-binary so
// every platform renders identical UI text (no system font lookup).
const Glyph& glyph(char character) noexcept;

}  // namespace wds::interaction::bitmap_font

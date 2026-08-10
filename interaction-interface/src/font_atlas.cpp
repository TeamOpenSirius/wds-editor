#include "wds/interaction/font_atlas.hpp"

#include <wds/common/utf8_path.hpp>
#include <wds/interaction/theme.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace wds::interaction {
namespace {

std::vector<std::string> system_font_candidates() {
  // Prefer Unicode / CJK-capable faces so Chinese UI labels render.
#if defined(_WIN32)
  return {
      "C:\\Windows\\Fonts\\msyh.ttc",       // Microsoft YaHei
      "C:\\Windows\\Fonts\\msyh.ttf",
      "C:\\Windows\\Fonts\\simsun.ttc",
      "C:\\Windows\\Fonts\\arialuni.ttf",
      "C:\\Windows\\Fonts\\segoeui.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
  };
#elif defined(__APPLE__)
  return {
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
      "/System/Library/Fonts/Hiragino Sans GB.ttc",
      "/System/Library/Fonts/STHeiti Light.ttc",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
      "/Library/Fonts/Arial.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
  };
#else
  return {
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
  };
#endif
}

bool read_file(const std::string& path, std::vector<unsigned char>& out) {
  return wds::common::read_file_bytes(path, out) && !out.empty();
}

// Decode one UTF-8 codepoint. Advances `i`. Returns 0 on failure / NUL.
int next_utf8_codepoint(const std::string& text, std::size_t& i) noexcept {
  if (i >= text.size()) return 0;
  const auto b0 = static_cast<unsigned char>(text[i++]);
  if (b0 < 0x80) return static_cast<int>(b0);
  if ((b0 & 0xE0) == 0xC0) {
    if (i >= text.size()) return 0;
    const auto b1 = static_cast<unsigned char>(text[i++]);
    return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  if ((b0 & 0xF0) == 0xE0) {
    if (i + 1 >= text.size()) return 0;
    const auto b1 = static_cast<unsigned char>(text[i++]);
    const auto b2 = static_cast<unsigned char>(text[i++]);
    return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
  }
  if ((b0 & 0xF8) == 0xF0) {
    if (i + 2 >= text.size()) return 0;
    const auto b1 = static_cast<unsigned char>(text[i++]);
    const auto b2 = static_cast<unsigned char>(text[i++]);
    const auto b3 = static_cast<unsigned char>(text[i++]);
    return ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
  }
  return static_cast<int>('?');
}

}  // namespace

struct FontAtlas::FontInfo {
  stbtt_fontinfo stb{};
};

FontAtlas::FontAtlas() = default;
FontAtlas::~FontAtlas() = default;

FontAtlas& FontAtlas::instance() noexcept {
  static FontAtlas atlas;
  return atlas;
}

void FontAtlas::rebuild_rgba_from_alpha() {
  pixels_.resize(static_cast<std::size_t>(atlas_w_ * atlas_h_ * 4));
  // 1× bake (logical max font px): milder ~a^1.2 so thin strokes keep coverage.
  // Higher tiers keep alpha² to cut the soft fringe that reads as a second stroke.
  const float bake_1x =
      std::max({theme::kFontSizeMd, theme::kFontSizeGutter, theme::kFontSizeTooltip});
  const bool mild_sharpen = baked_size_ <= bake_1x + 0.001f;
  for (int i = 0; i < atlas_w_ * atlas_h_; ++i) {
    const unsigned char a = alpha_[static_cast<std::size_t>(i)];
    const unsigned int aa = (static_cast<unsigned int>(a) * a) / 255u;
    // lerp(a, a²/255, 0.2) ≈ a^1.2; full square for retina+ tiers.
    const unsigned char sharp = mild_sharpen
                                    ? static_cast<unsigned char>((static_cast<unsigned int>(a) * 4u + aa) / 5u)
                                    : static_cast<unsigned char>(aa);
    pixels_[static_cast<std::size_t>(i) * 4 + 0] = 255;
    pixels_[static_cast<std::size_t>(i) * 4 + 1] = 255;
    pixels_[static_cast<std::size_t>(i) * 4 + 2] = 255;
    pixels_[static_cast<std::size_t>(i) * 4 + 3] = sharp;
  }
}

void FontAtlas::clear() {
  info_.reset();
  fallback_info_.reset();
  font_file_.clear();
  fallback_font_file_.clear();
  alpha_.clear();
  pixels_.clear();
  glyphs_.clear();
  texture_ = {};
  atlas_w_ = 0;
  atlas_h_ = 0;
  baked_size_ = 32.0f;
  ascent_ = 0.0f;
  scale_ = 1.0f;
  fallback_scale_ = 1.0f;
  pack_x_ = 0;
  pack_y_ = 0;
  pack_row_h_ = 0;
  pixels_dirty_ = false;
}

bool FontAtlas::pack_codepoint_from(const FontInfo& face, float face_scale, int codepoint) {
  if (alpha_.empty() || codepoint <= 0 || face_scale <= 0.0f) return false;
  if (stbtt_FindGlyphIndex(&face.stb, codepoint) == 0) return false;

  int advance = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(&face.stb, codepoint, &advance, &lsb);

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  stbtt_GetCodepointBitmapBox(&face.stb, codepoint, face_scale, face_scale, &x0, &y0, &x1, &y1);
  const int gw = std::max(0, x1 - x0);
  const int gh = std::max(0, y1 - y0);
  constexpr int kPad = 1;
  const int cell_w = gw + kPad * 2;
  const int cell_h = gh + kPad * 2;

  if (pack_x_ + cell_w > atlas_w_) {
    pack_x_ = 0;
    pack_y_ += pack_row_h_;
    pack_row_h_ = 0;
  }
  if (pack_y_ + cell_h > atlas_h_) {
    std::fprintf(stderr, "FontAtlas: atlas full, dropping U+%04X\n", codepoint);
    return false;
  }

  const int dst_x = pack_x_ + kPad;
  const int dst_y = pack_y_ + kPad;
  if (gw > 0 && gh > 0) {
    stbtt_MakeCodepointBitmap(&face.stb, alpha_.data() + dst_y * atlas_w_ + dst_x, gw, gh,
                              atlas_w_, face_scale, face_scale, codepoint);
  }

  BakedChar c;
  c.x0 = static_cast<float>(dst_x);
  c.y0 = static_cast<float>(dst_y);
  c.x1 = static_cast<float>(dst_x + gw);
  c.y1 = static_cast<float>(dst_y + gh);
  c.xoff = static_cast<float>(x0);
  c.yoff = static_cast<float>(y0);
  c.xadvance = static_cast<float>(advance) * face_scale;
  glyphs_.emplace(codepoint, c);

  pack_x_ += cell_w;
  pack_row_h_ = std::max(pack_row_h_, cell_h);
  pixels_dirty_ = true;
  return true;
}

bool FontAtlas::pack_codepoint(int codepoint) {
  if (info_ == nullptr || alpha_.empty() || codepoint <= 0) return false;
  if (glyphs_.count(codepoint) != 0) return true;
  if (pack_codepoint_from(*info_, scale_, codepoint)) return true;
  if (fallback_info_ != nullptr &&
      pack_codepoint_from(*fallback_info_, fallback_scale_, codepoint)) {
    return true;
  }
  return false;
}

void FontAtlas::try_load_fallback_font(const std::string& primary_path) {
  fallback_info_.reset();
  fallback_font_file_.clear();
  fallback_scale_ = 1.0f;
  for (const auto& path : system_font_candidates()) {
    if (!primary_path.empty() && path == primary_path) continue;
    if (!read_file(path, fallback_font_file_)) {
      fallback_font_file_.clear();
      continue;
    }
    auto face = std::make_unique<FontInfo>();
    int offset = stbtt_GetFontOffsetForIndex(fallback_font_file_.data(), 0);
    if (offset < 0) offset = 0;
    if (!stbtt_InitFont(&face->stb, fallback_font_file_.data(), offset)) {
      fallback_font_file_.clear();
      continue;
    }
    fallback_scale_ = stbtt_ScaleForPixelHeight(&face->stb, baked_size_);
    fallback_info_ = std::move(face);
    std::fprintf(stderr, "FontAtlas: fallback face %s\n", path.c_str());
    return;
  }
}

bool FontAtlas::bake_font_file(const std::string& path, float pixel_height) {
  clear();
  if (path.empty() || !read_file(path, font_file_)) {
    clear();
    return false;
  }

  info_ = std::make_unique<FontInfo>();
  int offset = stbtt_GetFontOffsetForIndex(font_file_.data(), 0);
  if (offset < 0) offset = 0;
  if (!stbtt_InitFont(&info_->stb, font_file_.data(), offset)) {
    std::fprintf(stderr, "FontAtlas: stbtt_InitFont failed for %s\n", path.c_str());
    clear();
    return false;
  }

  // Bake near display size. Oversized atlases (e.g. 64px → 12px tip) bilinear-blur to mush.
  baked_size_ = std::max(16.0f, pixel_height);
  atlas_w_ = 2048;
  atlas_h_ = 2048;
  alpha_.assign(static_cast<std::size_t>(atlas_w_ * atlas_h_), 0);
  scale_ = stbtt_ScaleForPixelHeight(&info_->stb, baked_size_);
  try_load_fallback_font(path);

  int ascent = 0, descent = 0, line_gap = 0;
  stbtt_GetFontVMetrics(&info_->stb, &ascent, &descent, &line_gap);
  ascent_ = static_cast<float>(ascent) * scale_;

  pack_x_ = 1;
  pack_y_ = 1;
  pack_row_h_ = 0;

  for (int cp = 32; cp <= 126; ++cp) {
    if (!pack_codepoint(cp)) {
      std::fprintf(stderr, "FontAtlas: failed packing ASCII\n");
      clear();
      return false;
    }
  }

  rebuild_rgba_from_alpha();
  pixels_dirty_ = true;
  std::fprintf(stderr, "FontAtlas: loaded %s (%.0fpx)\n", path.c_str(), baked_size_);
  return true;
}

bool FontAtlas::bake_system_font(float pixel_height) {
  for (const auto& path : system_font_candidates()) {
    if (bake_font_file(path, pixel_height)) {
      return true;
    }
  }
  std::fprintf(stderr, "FontAtlas: no system UI font found\n");
  return false;
}

bool FontAtlas::ensure_glyphs(const std::string& text) {
  if (info_ == nullptr || text.empty()) return false;
  bool added = false;
  std::size_t i = 0;
  while (i < text.size()) {
    const int cp = next_utf8_codepoint(text, i);
    if (cp <= 0) continue;
    if (cp == '\n' || cp == '\r' || cp == '\t') continue;
    if (glyphs_.count(cp) != 0) continue;
    if (pack_codepoint(cp)) added = true;
  }
  if (added) {
    rebuild_rgba_from_alpha();
  }
  return added;
}

const FontAtlas::BakedChar* FontAtlas::find_glyph(int codepoint) const noexcept {
  const auto it = glyphs_.find(codepoint);
  if (it != glyphs_.end()) return &it->second;
  const auto fallback = glyphs_.find(static_cast<int>('?'));
  if (fallback != glyphs_.end()) return &fallback->second;
  return nullptr;
}

Vec2 FontAtlas::measure(const std::string& text, float pixel_size) const noexcept {
  if (glyphs_.empty() || text.empty()) return {};
  // ensure_glyphs is non-const; callers should warm glyphs before measuring.
  const float scale = pixel_size / std::max(baked_size_, 1.0f);
  float x = 0.0f;
  float max_x = 0.0f;
  float height = pixel_size;
  std::size_t i = 0;
  while (i < text.size()) {
    const int cp = next_utf8_codepoint(text, i);
    if (cp <= 0) continue;
    if (cp == '\n') {
      max_x = std::max(max_x, x);
      x = 0.0f;
      height += pixel_size;
      continue;
    }
    const BakedChar* c = find_glyph(cp);
    if (c == nullptr) continue;
    x += c->xadvance * scale;
  }
  return {std::max(max_x, x), height};
}

void FontAtlas::build_quads(const std::string& text, float x, float y, float pixel_size,
                            std::vector<GlyphQuad>& out) const {
  out.clear();
  if (glyphs_.empty() || text.empty() || atlas_w_ <= 0 || atlas_h_ <= 0) return;
  const float scale = pixel_size / std::max(baked_size_, 1.0f);
  // Snap the pen to the pixel grid so LINEAR sampling does not re-blur glyph AA.
  float pen_x = std::floor(x + 0.5f);
  float pen_y = std::floor(y + ascent_ * scale + 0.5f);
  const float inv_w = 1.0f / static_cast<float>(atlas_w_);
  const float inv_h = 1.0f / static_cast<float>(atlas_h_);

  std::size_t i = 0;
  while (i < text.size()) {
    const int cp = next_utf8_codepoint(text, i);
    if (cp <= 0) continue;
    if (cp == '\n') {
      pen_x = std::floor(x + 0.5f);
      pen_y = std::floor(pen_y + pixel_size + 0.5f);
      continue;
    }
    const BakedChar* c = find_glyph(cp);
    if (c == nullptr) continue;
    const float gw = (c->x1 - c->x0) * scale;
    const float gh = (c->y1 - c->y0) * scale;
    const float gx = std::floor(pen_x + c->xoff * scale + 0.5f);
    const float gy = std::floor(pen_y + c->yoff * scale + 0.5f);
    if (gw > 0.0f && gh > 0.0f) {
      GlyphQuad q;
      q.dst = {gx, gy, std::max(1.0f, std::floor(gw + 0.5f)), std::max(1.0f, std::floor(gh + 0.5f))};
      q.u0 = c->x0 * inv_w;
      q.u1 = c->x1 * inv_w;
      // DrawBatch maps v0→quad bottom and v1→quad top. Atlas y grows downward, so
      // the glyph's bottom row (y1) must be v0 or text renders upside-down / tofu.
      q.v0 = c->y1 * inv_h;
      q.v1 = c->y0 * inv_h;
      out.push_back(q);
    }
    pen_x += c->xadvance * scale;
  }
}

}  // namespace wds::interaction

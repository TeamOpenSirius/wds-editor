#include "wds/interaction/font_atlas.hpp"
#include "wds/interaction/theme.hpp"

#include <wds/common/utf8_path.hpp>

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

constexpr float kDualSizeEpsilon = 0.5f;

void apply_coverage_inplace(unsigned char* dst, int w, int h, int stride, bool mild) noexcept {
  if (dst == nullptr || w <= 0 || h <= 0) return;
  for (int y = 0; y < h; ++y) {
    unsigned char* row = dst + y * stride;
    for (int x = 0; x < w; ++x) {
      const unsigned int a = row[x];
      const unsigned int aa = (a * a) / 255u;
      // Tip / low-DPI: ≈a^1.2 keeps thin strokes. Body on retina: a² cuts fringe.
      row[x] = mild ? static_cast<unsigned char>((a * 4u + aa) / 5u)
                    : static_cast<unsigned char>(aa);
    }
  }
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
  // Coverage is applied per glyph at pack time (tip stays milder than body).
  for (int i = 0; i < atlas_w_ * atlas_h_; ++i) {
    const unsigned char a = alpha_[static_cast<std::size_t>(i)];
    pixels_[static_cast<std::size_t>(i) * 4 + 0] = 255;
    pixels_[static_cast<std::size_t>(i) * 4 + 1] = 255;
    pixels_[static_cast<std::size_t>(i) * 4 + 2] = 255;
    pixels_[static_cast<std::size_t>(i) * 4 + 3] = a;
  }
}

void FontAtlas::clear() {
  info_.reset();
  fallback_info_.reset();
  font_file_.clear();
  fallback_font_file_.clear();
  alpha_.clear();
  pixels_.clear();
  glyphs_body_.clear();
  glyphs_tip_.clear();
  texture_ = {};
  atlas_w_ = 0;
  atlas_h_ = 0;
  baked_body_ = 32.0f;
  baked_tip_ = 32.0f;
  ascent_body_ = 0.0f;
  ascent_tip_ = 0.0f;
  scale_body_ = 1.0f;
  scale_tip_ = 1.0f;
  fallback_scale_body_ = 1.0f;
  fallback_scale_tip_ = 1.0f;
  pack_x_ = 0;
  pack_y_ = 0;
  pack_row_h_ = 0;
  pixels_dirty_ = false;
  dual_ = false;
  mild_sharpen_ = false;
  line_nudge_at_body_ = 0.0f;
}

FontAtlas::Slot FontAtlas::pick_slot(float pixel_size) const noexcept {
  if (!dual_) {
    return Slot::Body;
  }
  // Draw sizes are logical (window) px; bake sizes are framebuffer px
  // (logical × content_scale). Compare in FB space so Retina Md (26) binds
  // the body atlas (52) instead of the tip atlas (26).
  const float fb_px = pixel_size * std::max(theme::ui_content_scale(), 0.01f);
  const float d_body = std::abs(fb_px - baked_body_);
  const float d_tip = std::abs(fb_px - baked_tip_);
  return d_tip < d_body ? Slot::Tip : Slot::Body;
}

float FontAtlas::slot_baked(Slot slot) const noexcept {
  return (slot == Slot::Tip && dual_) ? baked_tip_ : baked_body_;
}

float FontAtlas::slot_ascent(Slot slot) const noexcept {
  return (slot == Slot::Tip && dual_) ? ascent_tip_ : ascent_body_;
}

bool FontAtlas::pack_codepoint_from(const FontInfo& face, float face_scale, int codepoint,
                                    std::unordered_map<int, BakedChar>& out, bool mild_coverage) {
  if (alpha_.empty() || codepoint <= 0 || face_scale <= 0.0f) return false;
  if (stbtt_FindGlyphIndex(&face.stb, codepoint) == 0) return false;
  if (out.count(codepoint) != 0) return true;

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
    apply_coverage_inplace(alpha_.data() + dst_y * atlas_w_ + dst_x, gw, gh, atlas_w_,
                           mild_coverage);
  }

  BakedChar c;
  c.x0 = static_cast<float>(dst_x);
  c.y0 = static_cast<float>(dst_y);
  c.x1 = static_cast<float>(dst_x + gw);
  c.y1 = static_cast<float>(dst_y + gh);
  // Horizontal bearing stays on the integer bitmap box so side bearings do not
  // shift. Vertical bearings use the unscaled outline (y-up): bitmap y1=1 is
  // usually a 1px AA fringe, and rounding that per glyph at half bake scale
  // was the pre-existing Latin baseline split (n vs u, L vs i).
  c.xoff = static_cast<float>(x0);
  int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
  if (stbtt_GetCodepointBox(&face.stb, codepoint, &ux0, &uy0, &ux1, &uy1)) {
    c.yoff = -static_cast<float>(uy1) * face_scale;
    c.y1off = -static_cast<float>(uy0) * face_scale;
  } else {
    c.yoff = static_cast<float>(y0);
    c.y1off = static_cast<float>(y1);
  }
  c.xadvance = static_cast<float>(advance) * face_scale;
  out.emplace(codepoint, c);

  pack_x_ += cell_w;
  pack_row_h_ = std::max(pack_row_h_, cell_h);
  pixels_dirty_ = true;
  return true;
}

bool FontAtlas::pack_codepoint_into(Slot slot, int codepoint) {
  if (info_ == nullptr || alpha_.empty() || codepoint <= 0) return false;
  auto& map = (slot == Slot::Tip && dual_) ? glyphs_tip_ : glyphs_body_;
  if (map.count(codepoint) != 0) return true;

  const float face_scale = (slot == Slot::Tip && dual_) ? scale_tip_ : scale_body_;
  const float fallback_scale =
      (slot == Slot::Tip && dual_) ? fallback_scale_tip_ : fallback_scale_body_;
  const bool mild = mild_sharpen_ || (dual_ && slot == Slot::Tip);

  if (pack_codepoint_from(*info_, face_scale, codepoint, map, mild)) return true;
  if (fallback_info_ != nullptr &&
      pack_codepoint_from(*fallback_info_, fallback_scale, codepoint, map, mild)) {
    return true;
  }
  return false;
}

bool FontAtlas::pack_codepoint(int codepoint) {
  const bool body_ok = pack_codepoint_into(Slot::Body, codepoint);
  if (!dual_) {
    return body_ok;
  }
  const bool tip_ok = pack_codepoint_into(Slot::Tip, codepoint);
  return body_ok || tip_ok;
}

void FontAtlas::try_load_fallback_font(const std::string& primary_path) {
  fallback_info_.reset();
  fallback_font_file_.clear();
  fallback_scale_body_ = 1.0f;
  fallback_scale_tip_ = 1.0f;
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
    fallback_scale_body_ = stbtt_ScaleForPixelHeight(&face->stb, baked_body_);
    fallback_scale_tip_ =
        dual_ ? stbtt_ScaleForPixelHeight(&face->stb, baked_tip_) : fallback_scale_body_;
    fallback_info_ = std::move(face);
    std::fprintf(stderr, "FontAtlas: fallback face %s\n", path.c_str());
    return;
  }
}

bool FontAtlas::bake_font_file(const std::string& path, float body_px, float tip_px,
                               bool mild_sharpen) {
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

  mild_sharpen_ = mild_sharpen;
  baked_body_ = std::max(16.0f, body_px);
  baked_tip_ = tip_px > 0.0f ? std::max(12.0f, tip_px) : baked_body_;
  dual_ = tip_px > 0.0f && std::abs(baked_tip_ - baked_body_) > kDualSizeEpsilon;
  if (!dual_) {
    baked_tip_ = baked_body_;
  }

  // Dual packs ~1.4× pixels; 2048² stays enough for UI warm + on-demand CJK.
  atlas_w_ = 2048;
  atlas_h_ = 2048;
  alpha_.assign(static_cast<std::size_t>(atlas_w_ * atlas_h_), 0);
  scale_body_ = stbtt_ScaleForPixelHeight(&info_->stb, baked_body_);
  scale_tip_ = dual_ ? stbtt_ScaleForPixelHeight(&info_->stb, baked_tip_) : scale_body_;
  try_load_fallback_font(path);

  int ascent = 0, descent = 0, line_gap = 0;
  stbtt_GetFontVMetrics(&info_->stb, &ascent, &descent, &line_gap);
  ascent_body_ = static_cast<float>(ascent) * scale_body_;
  ascent_tip_ = static_cast<float>(ascent) * scale_tip_;

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

  refresh_line_nudge();
  rebuild_rgba_from_alpha();
  pixels_dirty_ = true;
  if (dual_) {
    std::fprintf(stderr, "FontAtlas: loaded %s (body %.0fpx + tip %.0fpx%s)\n", path.c_str(),
                 baked_body_, baked_tip_, mild_sharpen_ ? ", mild sharpen" : "");
  } else {
    std::fprintf(stderr, "FontAtlas: loaded %s (%.0fpx%s)\n", path.c_str(), baked_body_,
                 mild_sharpen_ ? ", mild sharpen" : "");
  }
  return true;
}

bool FontAtlas::bake_system_font(float body_px, float tip_px, bool mild_sharpen) {
  for (const auto& path : system_font_candidates()) {
    if (bake_font_file(path, body_px, tip_px, mild_sharpen)) {
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
    const bool body_missing = glyphs_body_.count(cp) == 0;
    const bool tip_missing = dual_ && glyphs_tip_.count(cp) == 0;
    if (!body_missing && !tip_missing) continue;
    if (pack_codepoint(cp)) added = true;
  }
  if (added) {
    rebuild_rgba_from_alpha();
  }
  return added;
}

FontAtlas::GlyphRef FontAtlas::resolve_glyph(int codepoint, Slot slot) const noexcept {
  const auto& primary = (slot == Slot::Tip && dual_) ? glyphs_tip_ : glyphs_body_;
  const auto it = primary.find(codepoint);
  if (it != primary.end()) {
    return {&it->second, slot_baked(slot), slot_ascent(slot)};
  }
  if (dual_) {
    const Slot other = (slot == Slot::Tip) ? Slot::Body : Slot::Tip;
    const auto& other_map = (other == Slot::Tip) ? glyphs_tip_ : glyphs_body_;
    const auto ot = other_map.find(codepoint);
    if (ot != other_map.end()) {
      return {&ot->second, slot_baked(other), slot_ascent(other)};
    }
  }
  const auto fallback = glyphs_body_.find(static_cast<int>('?'));
  if (fallback != glyphs_body_.end()) {
    return {&fallback->second, baked_body_, ascent_body_};
  }
  return {};
}

Vec2 FontAtlas::measure(const std::string& text, float pixel_size) const noexcept {
  if (glyphs_body_.empty() || text.empty()) return {};
  // ensure_glyphs is non-const; callers should warm glyphs before measuring.
  const Slot slot = pick_slot(pixel_size);
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
    const GlyphRef g = resolve_glyph(cp, slot);
    if (g.c == nullptr) continue;
    x += g.c->xadvance * (pixel_size / std::max(g.baked, 1.0f));
  }
  return {std::max(max_x, x), height};
}

bool FontAtlas::ink_vertical_bounds(const std::string& text, float pixel_size, float& ink_top,
                                    float& ink_bottom) const noexcept {
  ink_top = 0.0f;
  ink_bottom = std::max(0.0f, pixel_size);
  if (glyphs_body_.empty() || text.empty() || pixel_size <= 0.0f) return false;
  const Slot slot = pick_slot(pixel_size);
  bool any = false;
  float top = 0.0f;
  float bottom = 0.0f;
  std::size_t i = 0;
  while (i < text.size()) {
    const int cp = next_utf8_codepoint(text, i);
    if (cp <= 0 || cp == '\n') continue;
    const GlyphRef g = resolve_glyph(cp, slot);
    if (g.c == nullptr) continue;
    const float glyph_scale = pixel_size / std::max(g.baked, 1.0f);
    const float gy = g.ascent * glyph_scale + g.c->yoff * glyph_scale;
    const float gh = (g.c->y1off - g.c->yoff) * glyph_scale;
    if (gh <= 0.0f) continue;
    if (!any) {
      top = gy;
      bottom = gy + gh;
      any = true;
    } else {
      top = std::min(top, gy);
      bottom = std::max(bottom, gy + gh);
    }
  }
  if (!any) return false;
  ink_top = top;
  ink_bottom = bottom;
  return true;
}

void FontAtlas::refresh_line_nudge() {
  line_nudge_at_body_ = 0.0f;
  if (baked_body_ <= 1.0f) return;
  ensure_glyphs("H国");
  float top = 0.0f;
  float bottom = baked_body_;
  if (!ink_vertical_bounds("H国", baked_body_, top, bottom) &&
      !ink_vertical_bounds("H", baked_body_, top, bottom)) {
    return;
  }
  const float ink_c = 0.5f * (top + bottom);
  const float em_c = 0.5f * baked_body_;
  line_nudge_at_body_ = em_c - ink_c;
}

float FontAtlas::line_nudge(float pixel_size) const noexcept {
  if (baked_body_ <= 1.0f || pixel_size <= 0.0f) return 0.0f;
  return line_nudge_at_body_ * (pixel_size / baked_body_);
}

void FontAtlas::build_quads(const std::string& text, float x, float y, float pixel_size,
                            std::vector<GlyphQuad>& out) const {
  out.clear();
  if (glyphs_body_.empty() || text.empty() || atlas_w_ <= 0 || atlas_h_ <= 0) return;
  const Slot slot = pick_slot(pixel_size);
  float pen_x = std::floor(x + 0.5f);
  float line_y = std::floor(y + 0.5f);
  const float inv_w = 1.0f / static_cast<float>(atlas_w_);
  const float inv_h = 1.0f / static_cast<float>(atlas_h_);

  std::size_t i = 0;
  while (i < text.size()) {
    const int cp = next_utf8_codepoint(text, i);
    if (cp <= 0) continue;
    if (cp == '\n') {
      pen_x = std::floor(x + 0.5f);
      line_y += pixel_size;
      continue;
    }
    const GlyphRef g = resolve_glyph(cp, slot);
    if (g.c == nullptr) continue;
    const float glyph_scale = pixel_size / std::max(g.baked, 1.0f);
    // Snap the line origin once. Rounding (baseline + yoff) per glyph turns a
    // 1px bake AA fringe (Noto 'u' y1=1 vs 'n' y1=0) into a 1px baseline split
    // when drawing at half bake size (Retina 26 logical / 52 bake).
    const float origin =
        std::floor(line_y + slot_ascent(slot) * (pixel_size / std::max(slot_baked(slot), 1.0f)) +
                   0.5f);
    // Outline y1off pins the dest bottom to the shared typographic baseline so
    // extra bitmap AA (Noto 'L' vs 'i' at half bake) grows upward, not below
    // the line. Quad *height* still matches the atlas bitmap or NEAREST
    // vertically compresses glyphs on 1× (Win).
    const float bitmap_h = (g.c->y1 - g.c->y0) * glyph_scale;
    const float gw = (g.c->x1 - g.c->x0) * glyph_scale;
    const float gh = std::max(1.0f, std::floor(bitmap_h + 0.5f));
    const float gy = origin + std::floor(g.c->y1off * glyph_scale + 0.5f) - gh;
    const float gx = std::floor(pen_x + g.c->xoff * glyph_scale + 0.5f);
    if (gw > 0.0f && gh > 0.0f) {
      GlyphQuad q;
      q.dst = {gx, gy, std::max(1.0f, std::floor(gw + 0.5f)), gh};
      q.u0 = g.c->x0 * inv_w;
      q.u1 = g.c->x1 * inv_w;
      // DrawBatch maps v0→quad bottom and v1→quad top. Atlas y grows downward, so
      // the glyph's bottom row (y1) must be v0 or text renders upside-down / tofu.
      q.v0 = g.c->y1 * inv_h;
      q.v1 = g.c->y0 * inv_h;
      out.push_back(q);
    }
    pen_x += g.c->xadvance * glyph_scale;
  }
}

}  // namespace wds::interaction

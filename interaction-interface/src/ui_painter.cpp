#include "wds/interaction/ui_painter.hpp"
#include "wds/interaction/bitmap_font.hpp"
#include "wds/interaction/font_atlas.hpp"
#include "wds/interaction/theme.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wds::interaction {

namespace {

wds::renderer::Vec2 map_point(float px, float py, int framebuffer_width, int framebuffer_height,
                              const wds::renderer::ScreenBounds& screen) {
  const float fw = std::max(framebuffer_width, 1);
  const float fh = std::max(framebuffer_height, 1);
  const float u = px / fw;
  const float v = py / fh;
  return {screen.l + u * screen.w, screen.t - v * screen.h};
}

void draw_bitmap_text(UiPainter& painter, const Rect& bounds, const std::string& text,
                      const Color& color, float z, float scale) {
  const float pixel = std::max(scale, 0.0f);
  float x = bounds.x;
  float y = bounds.y;
  for (const char character : text) {
    if (character == '\n') {
      x = bounds.x;
      y += 8.0f * pixel;
      continue;
    }
    const auto& glyph = bitmap_font::glyph(character);
    for (std::size_t row = 0; row < glyph.size(); ++row) {
      for (int column = 0; column < 8; ++column) {
        if ((glyph[row] & (1u << (7 - column))) != 0) {
          painter.fill_rect({x + static_cast<float>(column) * pixel,
                             y + static_cast<float>(row) * pixel, pixel, pixel},
                            color, 0.0f, z);
        }
      }
    }
    x += 8.0f * pixel;
  }
}

int next_utf8(const std::string& text, std::size_t& i) noexcept {
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

bool is_ascii_word_char(int cp) noexcept {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
         cp == '_';
}

bool is_open_paren(int cp) noexcept { return cp == '(' || cp == 0xFF08; /* （ */ }

// Peek next codepoint without advancing past failure.
int peek_utf8(const std::string& text, std::size_t i) noexcept {
  std::size_t tmp = i;
  return next_utf8(text, tmp);
}

// Unbreakable clusters: Latin words, parenthetical groups, single CJK/other glyphs.
// Whitespace is its own cluster and marks a preferred break.
struct TextCluster {
  std::string text;
  bool soft_break_after = false;  // true for whitespace
};

std::vector<TextCluster> tokenize_clusters(const std::string& text) {
  std::vector<TextCluster> clusters;
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t start = i;
    const int cp = next_utf8(text, i);
    if (cp <= 0) continue;
    if (cp == '\n') {
      clusters.push_back({"\n", true});
      continue;
    }
    if (cp == ' ' || cp == '\t') {
      clusters.push_back({text.substr(start, i - start), true});
      continue;
    }
    // Keep （…） / (…) as one unit so brackets are never split across lines.
    if (is_open_paren(cp)) {
      const int closer = (cp == 0xFF08) ? 0xFF09 : ')';
      while (i < text.size()) {
        const std::size_t mark = i;
        const int next = next_utf8(text, i);
        if (next <= 0) break;
        if (next == closer || next == '\n') {
          if (next != closer) {
            i = mark;  // leave newline for outer loop
          }
          break;
        }
      }
      clusters.push_back({text.substr(start, i - start), false});
      continue;
    }
    // Keep ASCII words intact (Tap / Scratch / Hold / ExTap …).
    if (is_ascii_word_char(cp)) {
      while (i < text.size() && is_ascii_word_char(peek_utf8(text, i))) {
        next_utf8(text, i);
      }
      clusters.push_back({text.substr(start, i - start), false});
      continue;
    }
    // Closing paren alone still stays with previous content when packing; as a
    // single cluster it will not start a line if we attach it when possible.
    clusters.push_back({text.substr(start, i - start), false});
  }
  return clusters;
}

// Pack clusters onto lines. Never splits a cluster (no mid-word / mid-paren breaks).
std::string wrap_text_to_width(FontAtlas& font, const std::string& text, float pixel_size,
                               float max_width) {
  if (text.empty() || max_width <= 1.0f) {
    return text;
  }
  const auto clusters = tokenize_clusters(text);
  std::string out;
  std::string line;
  bool line_has_content = false;

  auto trim_trailing_space = [](std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  };

  auto emit_newline = [&] {
    trim_trailing_space(line);
    out += line;
    out.push_back('\n');
    line.clear();
    line_has_content = false;
  };

  for (const auto& cluster : clusters) {
    if (cluster.text == "\n") {
      emit_newline();
      continue;
    }

    if (cluster.soft_break_after) {
      if (!line_has_content) continue;
      if (font.measure(line + cluster.text, pixel_size).x <= max_width) {
        line += cluster.text;
      } else {
        emit_newline();
      }
      continue;
    }

    const std::string candidate = line + cluster.text;
    if (line_has_content && font.measure(candidate, pixel_size).x > max_width) {
      emit_newline();
      line = cluster.text;
      line_has_content = true;
      continue;
    }
    line = candidate;
    line_has_content = true;
  }

  trim_trailing_space(line);
  out += line;
  if (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

// One size for every wrap label in the same host rect — scale with the cell,
// never shrink per string (long tips wrap instead).
float resolve_wrapped_px(const Rect& bounds, float requested) {
  const float raw =
      requested > 0.0f ? requested : theme::tooltip_px_for_host(std::min(bounds.w, bounds.h));
  // Snap to the tip bake bucket so NEAREST samples 1:1 instead of a soft scale.
  return theme::tooltip_bake_bucket(raw);
}

void draw_centered_lines(UiPainter& painter, const Rect& bounds, const std::string& wrapped,
                         const Color& color, float z, float pixel_size) {
  auto& font = FontAtlas::instance();
  const Vec2 block = font.measure(wrapped, pixel_size);
  float y = bounds.y + std::max(0.0f, (bounds.h - block.y) * 0.5f) + font.line_nudge(pixel_size);
  std::size_t start = 0;
  while (start <= wrapped.size()) {
    const std::size_t end = wrapped.find('\n', start);
    const std::string line =
        wrapped.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const float line_w = line.empty() ? 0.0f : font.measure(line, pixel_size).x;
    const float x = bounds.x + std::max(0.0f, (bounds.w - line_w) * 0.5f);
    if (!line.empty()) {
      painter.text({x, y, std::max(line_w, 1.0f), pixel_size}, line, color, z, pixel_size);
    }
    y += pixel_size;
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

}  // namespace

void UiPainter::clear() noexcept {
  rects_.clear();
  front_rects_.clear();
  sprites_.clear();
  behind_sprites_.clear();
  labels_.clear();
}

void UiPainter::reserve_rects(std::size_t n) { rects_.reserve(rects_.size() + n); }

void UiPainter::fill_rect(const Rect& bounds, const Color& color, float corner_radius, float z) {
  rects_.push_back({bounds, color, corner_radius, z});
}

void UiPainter::fill_rect_front(const Rect& bounds, const Color& color, float corner_radius,
                                float z) {
  front_rects_.push_back({bounds, color, corner_radius, z});
}

void UiPainter::fill_rect_outline(const Rect& bounds, const Color& fill, const Color& outline,
                                  float corner_radius, float z) {
  rects_.push_back({bounds, fill, corner_radius, z});
  const float t = 1.0f;
  rects_.push_back({{bounds.x, bounds.y, bounds.w, t}, outline, 0.0f, z + 0.001f});
  rects_.push_back({{bounds.x, bounds.bottom() - t, bounds.w, t}, outline, 0.0f, z + 0.001f});
  rects_.push_back({{bounds.x, bounds.y, t, bounds.h}, outline, 0.0f, z + 0.001f});
  rects_.push_back({{bounds.right() - t, bounds.y, t, bounds.h}, outline, 0.0f, z + 0.001f});
}

void UiPainter::fill_circle(Vec2 center, float radius, const Color& color, float z) {
  if (radius < 0.5f) {
    return;
  }
  if (soft_disk_) {
    const float d = radius * 2.0f;
    sprite({center.x - radius, center.y - radius, d, d}, soft_disk_, color, z);
    return;
  }
  // Fallback: one horizontal strip per pixel row.
  const int rows = std::max(1, static_cast<int>(std::ceil(radius)));
  const float r2 = radius * radius;
  for (int i = -rows; i <= rows; ++i) {
    const float dy = static_cast<float>(i);
    const float half = std::sqrt(std::max(0.0f, r2 - dy * dy));
    if (half < 0.5f) {
      continue;
    }
    rects_.push_back({{center.x - half, center.y + dy - 0.5f, half * 2.0f, 1.0f}, color, 0.0f, z});
  }
}

void UiPainter::fill_circle_outline(Vec2 center, float radius, const Color& fill,
                                    const Color& outline, float thickness, float z) {
  const float outer = std::max(0.5f, radius);
  const float inner = std::max(0.0f, outer - std::max(1.0f, thickness));
  if (soft_disk_) {
    const float od = outer * 2.0f;
    sprite({center.x - outer, center.y - outer, od, od}, soft_disk_, outline, z);
    if (inner > 0.5f) {
      const float id = inner * 2.0f;
      sprite({center.x - inner, center.y - inner, id, id}, soft_disk_, fill, z + 0.0005f);
    }
    return;
  }
  fill_circle(center, outer, outline, z);
  if (inner > 0.5f) {
    fill_circle(center, inner, fill, z + 0.0005f);
  }
}

void UiPainter::sprite(const Rect& bounds, const wds::renderer::TextureInfo& texture,
                       const Color& tint, float z, bool flip_x) {
  if (!texture || bounds.w <= 0.0f || bounds.h <= 0.0f) {
    return;
  }
  sprites_.push_back({bounds, texture, tint, z, flip_x, false});
}

void UiPainter::sprite_behind(const Rect& bounds, const wds::renderer::TextureInfo& texture,
                              const Color& tint, float z) {
  if (!texture || bounds.w <= 0.0f || bounds.h <= 0.0f) {
    return;
  }
  behind_sprites_.push_back({bounds, texture, tint, z, false, false});
}

void UiPainter::sprite_vfade(const Rect& bounds, const wds::renderer::TextureInfo& texture,
                             const Color& tint, float z, float alpha_bottom, float alpha_top) {
  if (!texture || bounds.w <= 0.0f || bounds.h <= 0.0f) {
    return;
  }
  sprites_.push_back({bounds, texture, tint, z, false, false, alpha_bottom, alpha_top});
}

namespace {

// Atlas path: treat scale as pixel size once it looks like UI px.
// Values below this are legacy bitmap multipliers (typically 1–6).
constexpr float kPixelSizeFloor = 8.0f;

float resolve_pixel_size(float scale) noexcept {
  if (scale >= kPixelSizeFloor) {
    return scale;
  }
  return std::max(theme::kFontSizeMd, scale * 8.0f);
}

}  // namespace

Vec2 UiPainter::measure_text(const std::string& text, float scale) const noexcept {
  auto& font = FontAtlas::instance();
  if (font.atlas_width() > 0) {
    // Pack any missing UTF-8 glyphs (e.g. CJK) before measuring.
    font.ensure_glyphs(text);
    return font.measure(text, resolve_pixel_size(scale));
  }

  const float s = std::max(scale, 0.0f);
  float line_width = 0.0f;
  float max_width = 0.0f;
  float height = 8.0f * s;
  for (const char character : text) {
    if (character == '\n') {
      max_width = std::max(max_width, line_width);
      line_width = 0.0f;
      height += 8.0f * s;
    } else {
      line_width += 8.0f * s;
    }
  }
  return {std::max(max_width, line_width), text.empty() ? 0.0f : height};
}

void UiPainter::text(const Rect& bounds, const std::string& text, const Color& color, float z,
                     float scale) {
  if (text.empty()) {
    return;
  }

  auto& font = FontAtlas::instance();
  font.ensure_glyphs(text);
  if (font.ready()) {
    const float pixel_size = resolve_pixel_size(scale);
    std::vector<FontAtlas::GlyphQuad> quads;
    font.build_quads(text, bounds.x, bounds.y, pixel_size, quads);
    for (const auto& q : quads) {
      wds::renderer::TextureInfo glyph = font.texture();
      glyph.u0 = q.u0;
      glyph.v0 = q.v0;
      glyph.u1 = q.u1;
      glyph.v1 = q.v1;
      if (q.dst.w <= 0.0f || q.dst.h <= 0.0f) {
        continue;
      }
      // font_atlas: resolve GPU id at flush after sync_ui_font_texture.
      sprites_.push_back({q.dst, glyph, color, z, false, true});
    }
    return;
  }

  draw_bitmap_text(*this, bounds, text, color, z, scale);
}

void UiPainter::label(const Rect& bounds, const std::string& text, const Color& color, float z,
                      bool wrap, float pixel_size, bool left_align) {
  if (text.empty()) {
    return;
  }
  labels_.push_back({bounds, text, color, z, wrap, pixel_size, left_align});
  if (defer_glyphs_) {
    return;
  }

  auto& font = FontAtlas::instance();
  font.ensure_glyphs(text);
  if (font.ready() || font.atlas_width() > 0) {
    const float max_w = std::max(1.0f, bounds.w - 4.0f);

    if (wrap) {
      const float tip_px = resolve_wrapped_px(bounds, pixel_size);
      const std::string wrapped = wrap_text_to_width(font, text, tip_px, max_w);
      if (font.ready()) {
        draw_centered_lines(*this, bounds, wrapped, color, z, tip_px);
      } else {
        draw_bitmap_text(*this, bounds, wrapped, color, z, 6.0f);
      }
      return;
    }

    const float px = pixel_size > 0.0f ? pixel_size : theme::kFontSizeMd;
    const Vec2 size = font.measure(text, px);
    const float x =
        left_align ? bounds.x + 2.0f : bounds.x + std::max(0.0f, (bounds.w - size.x) * 0.5f);
    const float y = bounds.y + (bounds.h - size.y) * 0.5f + font.line_nudge(px);
    if (font.ready()) {
      this->text({x, y, bounds.w, bounds.h}, text, color, z, px);
    } else {
      draw_bitmap_text(*this, {x, y, bounds.w, bounds.h}, text, color, z, 6.0f);
    }
    return;
  }

  const Vec2 natural = measure_text(text);
  constexpr float kMaxLabelScale = 6.0f;
  const float scale =
      std::clamp(std::min((bounds.w - 8.0f) / std::max(natural.x, 1.0f), (bounds.h - 6.0f) / 8.0f),
                 3.0f, kMaxLabelScale);
  const Vec2 size = measure_text(text, scale);
  const float x =
      left_align ? bounds.x + 2.0f : bounds.x + std::max(0.0f, (bounds.w - size.x) * 0.5f);
  this->text({x, bounds.y + std::max(0.0f, (bounds.h - size.y) * 0.5f), bounds.w, bounds.h}, text,
             color, z, scale);
}

wds::renderer::Quad rect_to_quad(const Rect& rect, int framebuffer_width, int framebuffer_height,
                                 const wds::renderer::ScreenBounds& screen) {
  // UI layout is logical (window) pixels; Vulkan maps framebuffer pixels.
  const float s = std::max(theme::ui_content_scale(), 0.01f);
  const Rect fb{rect.x * s, rect.y * s, rect.w * s, rect.h * s};
  const auto lb = map_point(fb.x, fb.bottom(), framebuffer_width, framebuffer_height, screen);
  const auto lt = map_point(fb.x, fb.y, framebuffer_width, framebuffer_height, screen);
  const auto rt = map_point(fb.right(), fb.y, framebuffer_width, framebuffer_height, screen);
  const auto rb = map_point(fb.right(), fb.bottom(), framebuffer_width, framebuffer_height, screen);
  return {lb, lt, rt, rb};
}

void UiPainter::flush_to(wds::renderer::DrawBatch& batch, wds::renderer::TextureId solid_texture,
                         int framebuffer_width, int framebuffer_height,
                         const wds::renderer::ScreenBounds& screen) const {
  auto emit_rects = [&](const std::vector<UiPaintRect>& rects, float z_max, bool invert) {
    for (const auto& rect : rects) {
      if (invert ? (rect.z <= z_max) : (rect.z > z_max)) continue;
      Rect draw_bounds = rect.bounds;
      if (rect.corner_radius > 0.0f) {
        // Inset in logical px; rect_to_quad applies content scale.
        const float inset = std::min(rect.corner_radius * 0.25f, 2.0f);
        draw_bounds = rect.bounds.inset(inset, inset);
      }
      const auto quad = rect_to_quad(draw_bounds, framebuffer_width, framebuffer_height, screen);
      batch.add_quad(solid_texture, quad, rect.z, rect.color.a, 0.0f, 0.0f, 1.0f, 1.0f, rect.color.r,
                     rect.color.g, rect.color.b);
    }
  };
  auto emit_sprites = [&](const std::vector<UiPaintSprite>& sprites) {
    const wds::renderer::TextureId font_id = FontAtlas::instance().texture().id;
    for (const auto& sprite : sprites) {
      auto quad = rect_to_quad(sprite.bounds, framebuffer_width, framebuffer_height, screen);
      float u0 = sprite.texture.u0;
      float v0 = sprite.texture.v0;
      float u1 = sprite.texture.u1;
      float v1 = sprite.texture.v1;
      if (sprite.flip_x) {
        std::swap(u0, u1);
      }
      const wds::renderer::TextureId tex_id =
          sprite.font_atlas ? font_id : sprite.texture.id;
      if (sprite.font_atlas &&
          (font_id == wds::renderer::kInvalidTextureId || !FontAtlas::instance().texture())) {
        continue;
      }
      if (sprite.alpha_bottom >= 0.0f || sprite.alpha_top >= 0.0f) {
        const float a_bot = sprite.alpha_bottom >= 0.0f ? sprite.alpha_bottom : sprite.tint.a;
        const float a_top = sprite.alpha_top >= 0.0f ? sprite.alpha_top : sprite.tint.a;
        batch.add_quad_corners(tex_id, quad, sprite.z, a_bot, a_bot, a_top, a_top, u0, v0, u1, v1,
                               sprite.tint.r, sprite.tint.g, sprite.tint.b);
      } else {
        batch.add_quad(tex_id, quad, sprite.z, sprite.tint.a, u0, v0, u1, v1, sprite.tint.r,
                       sprite.tint.g, sprite.tint.b);
      }
    }
  };
  auto emit_all_rects = [&](const std::vector<UiPaintRect>& rects) {
    emit_rects(rects, -1.0f, true);
  };
  // Backdrop fill (edit playfield black is z=0.86) then spectrogram, then grid/UI.
  constexpr float kBehindSpriteZ = 0.865f;
  emit_rects(rects_, kBehindSpriteZ, false);
  emit_sprites(behind_sprites_);
  emit_rects(rects_, kBehindSpriteZ, true);
  emit_sprites(sprites_);
  emit_all_rects(front_rects_);
}

}  // namespace wds::interaction

#pragma once

#include "types.hpp"

#include <wds/renderer/draw_batch.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wds::interaction {

// GPU atlas for UI text (bundled face, then system CJK/Unicode faces).
// ASCII is packed at bake time; other codepoints are packed on demand.
// Codepoints missing from the primary face are taken from a system fallback.
class FontAtlas {
 public:
  struct GlyphQuad {
    Rect dst;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  };

  static FontAtlas& instance() noexcept;

  // Rasterize glyphs into an RGBA atlas.
  // Prefers an explicit font file (bundled UI font), then system CJK/Unicode faces.
  bool bake_font_file(const std::string& path, float pixel_height = 32.0f);
  bool bake_system_font(float pixel_height = 32.0f);
  void clear();

  // Ensure all UTF-8 codepoints in `text` are present in the atlas.
  // Returns true if new glyphs were packed (CPU pixels changed).
  bool ensure_glyphs(const std::string& text);

  const unsigned char* pixels() const noexcept {
    return pixels_.empty() ? nullptr : pixels_.data();
  }
  int atlas_width() const noexcept { return atlas_w_; }
  int atlas_height() const noexcept { return atlas_h_; }
  float baked_size() const noexcept { return baked_size_; }

  // True after new glyphs were packed since the last clear_pixels_dirty().
  bool pixels_dirty() const noexcept { return pixels_dirty_; }
  void clear_pixels_dirty() noexcept { pixels_dirty_ = false; }

  void set_gpu_texture(wds::renderer::TextureInfo texture) noexcept { texture_ = texture; }
  void clear_gpu_texture() noexcept { texture_ = {}; }
  bool ready() const noexcept { return static_cast<bool>(texture_) && !glyphs_.empty(); }
  const wds::renderer::TextureInfo& texture() const noexcept { return texture_; }

  Vec2 measure(const std::string& text, float pixel_size) const noexcept;
  void build_quads(const std::string& text, float x, float y, float pixel_size,
                   std::vector<GlyphQuad>& out) const;

 private:
  FontAtlas();
  ~FontAtlas();
  FontAtlas(const FontAtlas&) = delete;
  FontAtlas& operator=(const FontAtlas&) = delete;

  struct BakedChar {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float xoff = 0, yoff = 0, xadvance = 0;
  };

  struct FontInfo;

  bool pack_codepoint(int codepoint);
  bool pack_codepoint_from(const FontInfo& face, float face_scale, int codepoint);
  void try_load_fallback_font(const std::string& primary_path);
  const BakedChar* find_glyph(int codepoint) const noexcept;
  void rebuild_rgba_from_alpha();

  std::unique_ptr<FontInfo> info_;
  std::unique_ptr<FontInfo> fallback_info_;
  std::vector<unsigned char> font_file_;
  std::vector<unsigned char> fallback_font_file_;
  std::vector<unsigned char> alpha_;
  std::vector<unsigned char> pixels_;
  std::unordered_map<int, BakedChar> glyphs_;
  wds::renderer::TextureInfo texture_{};
  int atlas_w_ = 0;
  int atlas_h_ = 0;
  float baked_size_ = 32.0f;
  float ascent_ = 0.0f;
  float scale_ = 1.0f;
  float fallback_scale_ = 1.0f;
  int pack_x_ = 0;
  int pack_y_ = 0;
  int pack_row_h_ = 0;
  bool pixels_dirty_ = false;
};

}  // namespace wds::interaction

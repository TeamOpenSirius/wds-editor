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
//
// Dual bake: body (Md/Gutter) + tip (Tooltip) sizes share one atlas so each
// draw size stays near 1:1 and avoids NEAREST mush from large downscales.
class FontAtlas {
 public:
  struct GlyphQuad {
    Rect dst;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  };

  static FontAtlas& instance() noexcept;

  // Rasterize glyphs into an RGBA atlas.
  // `body_px`: primary UI size. `tip_px` > 0 and meaningfully different enables
  // a second tip-sized pack of the same codepoints. `mild_sharpen`: softer
  // coverage curve (≈a^1.2) for low DPI tiers; otherwise alpha².
  bool bake_font_file(const std::string& path, float body_px, float tip_px = 0.0f,
                      bool mild_sharpen = false);
  bool bake_system_font(float body_px, float tip_px = 0.0f, bool mild_sharpen = false);
  void clear();

  // Ensure all UTF-8 codepoints in `text` are present (both sizes when dual).
  // Returns true if new glyphs were packed (CPU pixels changed).
  bool ensure_glyphs(const std::string& text);

  const unsigned char* pixels() const noexcept {
    return pixels_.empty() ? nullptr : pixels_.data();
  }
  int atlas_width() const noexcept { return atlas_w_; }
  int atlas_height() const noexcept { return atlas_h_; }
  // Body bake size (largest slot). Kept for callers that only need a reference px.
  float baked_size() const noexcept { return baked_body_; }
  float baked_body_size() const noexcept { return baked_body_; }
  float baked_tip_size() const noexcept { return dual_ ? baked_tip_ : baked_body_; }
  bool dual_bake() const noexcept { return dual_; }

  // True after new glyphs were packed since the last clear_pixels_dirty().
  bool pixels_dirty() const noexcept { return pixels_dirty_; }
  void clear_pixels_dirty() noexcept { pixels_dirty_ = false; }

  void set_gpu_texture(wds::renderer::TextureInfo texture) noexcept { texture_ = texture; }
  void clear_gpu_texture() noexcept { texture_ = {}; }
  bool ready() const noexcept { return static_cast<bool>(texture_) && !glyphs_body_.empty(); }
  const wds::renderer::TextureInfo& texture() const noexcept { return texture_; }

  Vec2 measure(const std::string& text, float pixel_size) const noexcept;
  void build_quads(const std::string& text, float x, float y, float pixel_size,
                   std::vector<GlyphQuad>& out) const;

 private:
  FontAtlas();
  ~FontAtlas();
  FontAtlas(const FontAtlas&) = delete;
  FontAtlas& operator=(const FontAtlas&) = delete;

  enum class Slot : int { Body = 0, Tip = 1 };

  struct BakedChar {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float xoff = 0, yoff = 0, xadvance = 0;
  };

  struct FontInfo;

  bool pack_codepoint(int codepoint);
  bool pack_codepoint_into(Slot slot, int codepoint);
  bool pack_codepoint_from(const FontInfo& face, float face_scale, int codepoint,
                           std::unordered_map<int, BakedChar>& out);
  void try_load_fallback_font(const std::string& primary_path);
  struct GlyphRef {
    const BakedChar* c = nullptr;
    float baked = 1.0f;
    float ascent = 0.0f;
  };

  Slot pick_slot(float pixel_size) const noexcept;
  GlyphRef resolve_glyph(int codepoint, Slot slot) const noexcept;
  float slot_baked(Slot slot) const noexcept;
  float slot_ascent(Slot slot) const noexcept;
  void rebuild_rgba_from_alpha();

  std::unique_ptr<FontInfo> info_;
  std::unique_ptr<FontInfo> fallback_info_;
  std::vector<unsigned char> font_file_;
  std::vector<unsigned char> fallback_font_file_;
  std::vector<unsigned char> alpha_;
  std::vector<unsigned char> pixels_;
  std::unordered_map<int, BakedChar> glyphs_body_;
  std::unordered_map<int, BakedChar> glyphs_tip_;
  wds::renderer::TextureInfo texture_{};
  int atlas_w_ = 0;
  int atlas_h_ = 0;
  float baked_body_ = 32.0f;
  float baked_tip_ = 32.0f;
  float ascent_body_ = 0.0f;
  float ascent_tip_ = 0.0f;
  float scale_body_ = 1.0f;
  float scale_tip_ = 1.0f;
  float fallback_scale_body_ = 1.0f;
  float fallback_scale_tip_ = 1.0f;
  int pack_x_ = 0;
  int pack_y_ = 0;
  int pack_row_h_ = 0;
  bool pixels_dirty_ = false;
  bool dual_ = false;
  bool mild_sharpen_ = false;
};

}  // namespace wds::interaction

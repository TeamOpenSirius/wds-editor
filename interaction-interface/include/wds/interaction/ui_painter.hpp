#pragma once

#include "types.hpp"

#include <wds/renderer/draw_batch.hpp>
#include <wds/renderer/draw_types.hpp>

#include <string>
#include <vector>

namespace wds::interaction {

struct UiPaintRect {
  Rect bounds;
  Color color;
  float corner_radius = 0.0f;
  float z = 0.9f;
};

struct UiPaintSprite {
  Rect bounds;
  wds::renderer::TextureInfo texture;
  Color tint{1, 1, 1, 1};
  float z = 0.92f;
  bool flip_x = false;
  // When true, flush_to uses FontAtlas::texture().id (UVs kept) so a mid-frame
  // atlas upload cannot leave DrawBatch holding a destroyed TextureId.
  bool font_atlas = false;
  // When >= 0, flush uses a vertical alpha gradient (lb/rb = alpha_bottom,
  // lt/rt = alpha_top) instead of uniform tint.a. RGB still comes from tint.
  float alpha_bottom = -1.0f;
  float alpha_top = -1.0f;
};

// Collects screen-space paint commands; flush to DrawBatch with a 1×1 white texture.
class UiPainter {
 public:
  void clear() noexcept;
  void fill_rect(const Rect& bounds, const Color& color, float corner_radius = 0.0f,
                 float z = 0.9f);
  // Same as fill_rect, but flushed after sprites so carets sit on top of text glyphs.
  void fill_rect_front(const Rect& bounds, const Color& color, float corner_radius = 0.0f,
                       float z = 0.9f);
  void fill_rect_outline(const Rect& bounds, const Color& fill, const Color& outline,
                         float corner_radius = 0.0f, float z = 0.9f);
  // Axis-aligned quad pipeline has no true rounded corners — approximate disks with strips.
  void fill_circle(Vec2 center, float radius, const Color& color, float z = 0.9f);
  void fill_circle_outline(Vec2 center, float radius, const Color& fill, const Color& outline,
                           float thickness = 1.5f, float z = 0.9f);
  void sprite(const Rect& bounds, const wds::renderer::TextureInfo& texture,
              const Color& tint = {1, 1, 1, 1}, float z = 0.92f, bool flip_x = false);
  // Vertical alpha gradient: bottom of `bounds` uses alpha_bottom, top uses alpha_top.
  void sprite_vfade(const Rect& bounds, const wds::renderer::TextureInfo& texture,
                    const Color& tint, float z, float alpha_bottom, float alpha_top);
  void text(const Rect& bounds, const std::string& text, const Color& color, float z,
            float scale = 1.0f);
  // Centered label. When `wrap` is true, insert line breaks instead of shrinking
  // when the string is wider than `bounds`. Unspecified wrap size is shared for
  // the host rect (theme::tooltip_px_for_host) so every icon-button tip matches.
  // `pixel_size` <= 0 uses theme::kFontSizeMd (or the wrap host size). `left_align`
  // pins text to the left edge.
  void label(const Rect& bounds, const std::string& text, const Color& color, float z = 0.91f,
             bool wrap = false, float pixel_size = 0.0f, bool left_align = false);
  Vec2 measure_text(const std::string& text, float scale = 1.0f) const noexcept;

  // Optional soft disk from the skin atlas — fill_circle becomes 1 tinted sprite.
  void set_soft_disk(const wds::renderer::TextureInfo& texture) noexcept { soft_disk_ = texture; }
  void clear_soft_disk() noexcept { soft_disk_ = {}; }

  void reserve_rects(std::size_t n);

  const std::vector<UiPaintRect>& rects() const noexcept { return rects_; }
  const std::vector<UiPaintRect>& front_rects() const noexcept { return front_rects_; }
  const std::vector<UiPaintSprite>& sprites() const noexcept { return sprites_; }

  // Appends into `batch` (does not clear). Callers that rebuild a frame must
  // `batch.clear()` first; otherwise prior verts accumulate across frames.
  void flush_to(wds::renderer::DrawBatch& batch, wds::renderer::TextureId solid_texture,
                int framebuffer_width, int framebuffer_height,
                const wds::renderer::ScreenBounds& screen) const;

 private:
  std::vector<UiPaintRect> rects_;
  std::vector<UiPaintRect> front_rects_;
  std::vector<UiPaintSprite> sprites_;
  wds::renderer::TextureInfo soft_disk_{};
};

wds::renderer::Quad rect_to_quad(const Rect& rect, int framebuffer_width, int framebuffer_height,
                                 const wds::renderer::ScreenBounds& screen);

}  // namespace wds::interaction

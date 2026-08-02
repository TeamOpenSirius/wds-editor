#pragma once

#include "draw_batch.hpp"
#include "vulkan_renderer.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace wds::renderer {

// Decode a PNG to tightly packed RGBA8 (CPU). False on I/O or decode failure.
bool load_png_rgba8(const std::string& path, std::vector<unsigned char>& out_rgba, int& width,
                    int& height);

// Load a standalone GPU texture (not packed into the atlas). Safe to call after bake_atlas().
TextureInfo create_texture_from_png(VulkanRenderer& renderer, const std::string& path);

// Rasterize an SVG to RGBA. `target_size` is the longer edge in px.
TextureInfo create_texture_from_svg(VulkanRenderer& renderer, const std::string& path,
                                    int target_size = 256);

// Loads PNGs into CPU memory, then packs them into a single GPU atlas on bake_atlas().
// After bake, all TextureInfo share one TextureId with distinct UV rects → typically 1 bind/frame.
class TextureCache {
 public:
  explicit TextureCache(VulkanRenderer* renderer = nullptr) : renderer_(renderer) {}
  ~TextureCache();

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  void set_renderer(VulkanRenderer* renderer) { renderer_ = renderer; }

  // Queue a PNG for atlas packing. Returns false if the file cannot be decoded.
  bool queue_png(const std::string& path);

  // Queue pre-decoded RGBA8 pixels under a synthetic key (e.g. procedural sprites).
  // Key must be unique; overwrites pending entry with the same key.
  bool queue_rgba(const std::string& key, std::vector<unsigned char> pixels, int width,
                  int height);

  // Pack queued images into one atlas texture and fill path → TextureInfo (with UVs).
  bool bake_atlas();

  // Valid after bake_atlas().
  TextureInfo get(const std::string& path) const;

  bool baked() const noexcept { return baked_; }
  TextureId atlas_id() const noexcept { return atlas_id_; }

  void clear();

 private:
  struct CpuImage {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
  };

  VulkanRenderer* renderer_ = nullptr;
  std::unordered_map<std::string, CpuImage> pending_;
  std::unordered_map<std::string, TextureInfo> cache_;
  TextureId atlas_id_ = kInvalidTextureId;
  bool baked_ = false;
};

}  // namespace wds::renderer

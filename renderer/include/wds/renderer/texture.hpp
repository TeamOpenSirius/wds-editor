#pragma once

#include "draw_batch.hpp"
#include "vulkan_renderer.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// GPU hooks used by TextureCache (atlas + standalone). Production code typically
// wraps VulkanRenderer; tests / alternate uploaders inject the same three calls.
// Backend is enabled only when ready, create, and destroy are all set; a partial
// backend is ignored entirely (no split fallback onto VulkanRenderer*).
struct TextureCacheBackend {
  std::function<bool()> ready;
  std::function<TextureInfo(const unsigned char* pixels, int width, int height)> create;
  std::function<void(TextureId)> destroy;
  // Optional injectors. Production leaves these empty.
  std::function<void()> before_atlas_cpu_cache;
  std::function<void(int)> before_standalone_commit;
};

// CPU bookkeeping for a standalone texture. `before_step` is invoked with 0/1/2
// immediately before keys / ids / cache mutation. On exception, prior mutations
// are rolled back and the exception is rethrown. Does not create or destroy GPU ids.
void commit_standalone_records(std::unordered_set<std::string>& keys,
                               std::vector<TextureId>& ids,
                               std::unordered_map<std::string, TextureInfo>& cache,
                               const std::string& path, const TextureInfo& info,
                               const std::function<void(int)>& before_step = {});

// Loads PNGs into CPU memory, then packs them into a single GPU atlas on bake_atlas().
// After bake, all TextureInfo share one TextureId with distinct UV rects → typically 1 bind/frame.
// CPU atlas sources are retained so a later queue + rebake still includes prior keys.
// Rebake is transactional: create the new atlas, swap cache/atlas id, then destroy the old
// atlas. A failed create leaves the live atlas, UVs, and baked() state intact. Queueing more
// images after a successful bake keeps baked() true so the current atlas stays usable.
class TextureCache {
 public:
  explicit TextureCache(VulkanRenderer* renderer = nullptr);
  explicit TextureCache(TextureCacheBackend backend);
  ~TextureCache();

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  void set_renderer(VulkanRenderer* renderer);

  // Queue a PNG for atlas packing. Returns false if the file cannot be decoded.
  // Already-queued / already-cached keys are treated as success and not reloaded.
  bool queue_png(const std::string& path);

  // Queue pre-decoded RGBA8 pixels under a synthetic key (e.g. procedural sprites).
  // Overwrites the CPU source for an existing key and dirties the atlas. If an atlas
  // is already live, baked() stays true until the next successful bake_atlas().
  bool queue_rgba(const std::string& key, std::vector<unsigned char> pixels, int width,
                  int height);

  // Pack retained atlas CPU sources into one atlas texture and fill path → TextureInfo.
  bool bake_atlas();

  // Full-res GPU texture outside the atlas (large BGs). Safe after bake_atlas().
  TextureInfo load_standalone_png(const std::string& path);

  // Valid after bake_atlas() / load_standalone_png().
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

  bool backend_enabled() const;
  bool gpu_ready() const;
  TextureInfo gpu_create(const unsigned char* pixels, int width, int height);
  void gpu_destroy(TextureId id);

  VulkanRenderer* renderer_ = nullptr;
  TextureCacheBackend backend_{};
  std::unordered_map<std::string, CpuImage> pending_;
  std::unordered_map<std::string, TextureInfo> cache_;
  std::unordered_set<std::string> standalone_keys_;
  std::vector<TextureId> standalone_ids_;
  TextureId atlas_id_ = kInvalidTextureId;
  bool baked_ = false;
  bool needs_rebake_ = false;
};

}  // namespace wds::renderer

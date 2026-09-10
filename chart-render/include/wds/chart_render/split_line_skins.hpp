#pragma once

#include <wds/renderer/texture.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wds::renderer {

// Optional wipe sprites for a suffix. LineColor is official_split_line_color, not PNG.
struct SplitLineVariant {
  TextureInfo base;
  TextureInfo transform1;
  TextureInfo transform2;

  const TextureInfo* for_phase(int32_t anim_phase) const noexcept {
    if (anim_phase == 0 && transform1) {
      return &transform1;
    }
    if (anim_phase == 2 && transform2) {
      return &transform2;
    }
    if (base) {
      return &base;
    }
    if (transform1) {
      return &transform1;
    }
    return transform2 ? &transform2 : nullptr;
  }
};

// Resolves scratchLength IDs → optional wipe sprites. LineColor comes from
// official_split_line_color (1.96.0 _lineColor), not PNG sampling.
class SplitLineSkinBank {
 public:
  // Queue all Sirius Split Line PNGs into cache (call before bake_atlas).
  // SkinCatalog::load no longer calls this — draw uses procedural soft_split_line.
  void queue_all(TextureCache& cache, const std::string& skins_directory);

  // Bind TextureInfo after bake_atlas; build color→suffix resolution tables.
  // Paired with queue_all; also unused on the current startup path.
  void bind_after_bake(TextureCache& cache, const std::string& skins_directory);

  // line_slot matches Sonolus splitLineMemory index: 0=left, 1..N-1=mids, N=right end.
  const TextureInfo* texture_for(int32_t color_id, int32_t line_slot, int32_t anim_phase) const;

  // Official SplitEffectElement._lineColor for this prefab Line index.
  bool color_for(int32_t color_id, int32_t line_slot, float& r, float& g, float& b) const;
  bool color_for(int32_t color_id, int32_t line_slot, float& r, float& g, float& b,
                 float& a) const;

  // Resolved skin suffixes for a color id (multi-sprite colors return multiple entries).
  std::vector<std::string> suffixes_for(int32_t color_id) const;

  // Official 1.96.0 SplitEffects IDs (not disk PNG suffixes).
  std::vector<int32_t> catalog_color_ids() const;

  bool empty() const noexcept { return by_suffix_.empty(); }

 private:
  std::vector<std::string> resolve_suffixes(int32_t color_id) const;
  const SplitLineVariant* variant_for_suffix(const std::string& suffix) const noexcept;

  std::unordered_map<std::string, SplitLineVariant> by_suffix_;
  std::unordered_map<std::string, std::string> path_base_;
  std::unordered_map<std::string, std::string> path_t1_;
  std::unordered_map<std::string, std::string> path_t2_;
  // Discovered suffixes present on disk (for a/b and _N auto-resolve).
  std::unordered_map<std::string, bool> suffix_exists_;
};

}  // namespace wds::renderer

#pragma once

#include <wds/renderer/draw_batch.hpp>

#include <cstdint>

namespace wds::renderer {

// Official-style horizontal 3-slice (Unity Sprite m_DrawMode Sliced).
// border_l / border_r are pixels in the source sprite (e.g. 65 on 268-wide notes).
void add_sliced_note(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                     float border_l_px, float border_r_px, float z, float alpha,
                     float r = 1.0f, float g = 1.0f, float b = 1.0f);

}  // namespace wds::renderer

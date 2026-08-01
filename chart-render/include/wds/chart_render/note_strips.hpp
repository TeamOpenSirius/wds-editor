#pragma once

#include <wds/renderer/draw_batch.hpp>

#include <cstdint>

namespace wds::renderer {

// Left/middle/right note skin strips across a trapezoid (chart preview + edit).
void add_note_strips(DrawBatch& batch, const TextureInfo& left, const TextureInfo& middle,
                     const TextureInfo& right, const Quad& quad, float border_percent,
                     int32_t lane_span, float z, float alpha);

}  // namespace wds::renderer

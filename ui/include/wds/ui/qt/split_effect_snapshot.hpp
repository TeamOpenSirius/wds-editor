#pragma once

#include <wds/core/gimmick.hpp>
#include <wds/core/official_playfield.hpp>

#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QSize>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

class QObject;
class QPainter;

namespace wds::ui {

// Lazy-load skins/split_effect_uvs.png (+ bomb_effect_trail.png). Safe to call more than once.
void ensure_split_effect_uv_atlas(const QString& skins_dir);

// Reference composition of the picker (its minimum box width). The edit area scales it proportionally:
// lane_px / kSplitIndicatorRefLanePx, band_h / kSplitIndicatorRefBandPx.
constexpr float kSplitIndicatorRefLanePx = 14.0f;
constexpr float kSplitIndicatorRefBandPx = 44.0f;
// Extra particle size on top of px_per_unit (picker previews: ~1.5x together with the 1.2x wider plate).
constexpr float kSplitIndicatorSizeScale = 1.25f;
constexpr float kSplitIndicatorRefMinSpritePx = 10.5f;
// The edit area draws its indicator at this fraction of the picker-relative edge length: it multiplies BOTH
// size_scale and min_sprite_px of the editor's cluster key (positions / band / lane mapping are unchanged). The
// picker and the hover card do not use it.
constexpr float kEditorIndicatorSizeFactor = 0.5f;
// The edit area shows denser clusters than the picker: particles with SplitSnapParticle::rank < this (the picker and
// the hover card keep the default max_rank of 2), and its band (the y span of the cluster) is this many times the
// picker-relative band (the bottom stays anchored at the fade-in start).
constexpr int kEditorIndicatorMaxRank = 6;
// The editor's band (vertical length) does NOT depend on the lane width: it follows the time axis. The indicator
// covers this fraction of the fade-in duration, starting at fade0 (bottom anchor), so band_px =
// |y_at_ms(fade0 + fraction * fade_in_ms) - y_at_ms(fade0)|. Horizontal positions and particle sizes still scale
// with the lane width.
constexpr float kEditorIndicatorFadeFraction = 0.6f;
constexpr int kEditorIndicatorMinBandPx = 16;
constexpr int kEditorIndicatorMaxBandPx = 2048;
constexpr int kEditorIndicatorBandStepsPerOctave = 8;

// Geometric band quantization (steps of 2^(1/8), ~9%) limiting cache churn while zooming. Returns 0 when px is
// below kEditorIndicatorMinBandPx (skip), else round(16 * 2^(step/8)) capped at kEditorIndicatorMaxBandPx.
inline int editor_indicator_band_for_step(int step) {
  const double v = kEditorIndicatorMinBandPx *
                   std::exp2(static_cast<double>(step) / kEditorIndicatorBandStepsPerOctave);
  return std::min(kEditorIndicatorMaxBandPx, static_cast<int>(std::lround(v)));
}
inline int editor_indicator_band_step(int band_q) {
  return static_cast<int>(std::lround(kEditorIndicatorBandStepsPerOctave *
                                      std::log2(static_cast<double>(band_q) / kEditorIndicatorMinBandPx)));
}
inline int quantize_editor_indicator_band(float px) {
  if (!(px >= static_cast<float>(kEditorIndicatorMinBandPx))) return 0;
  const double capped = std::min(static_cast<double>(px), static_cast<double>(kEditorIndicatorMaxBandPx));
  const int step = static_cast<int>(std::lround(kEditorIndicatorBandStepsPerOctave *
                                                std::log2(capped / kEditorIndicatorMinBandPx)));
  return editor_indicator_band_for_step(step);
}

struct SplitIndicatorSpec {
  int32_t color_id = 1;
  int32_t split_count = 1;
  wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full;
  float px_per_unit = 16.0f;   // pixels per world unit (lane_px / 0.925)
  // Multiplies quad sizes (w, h) and strip widths. Positions (dx, dy) are NOT scaled by it.
  // Order: size_scale, then min_sprite_px (lower bound on a quad's larger side, picker legibility), then the
  // beam extent caps (those still win), then the band fit (which includes strip half-widths).
  float size_scale = 1.0f;
  float min_sprite_px = 0.0f;
  // Only particles with SplitSnapParticle::rank < max_rank are used (2 = the picker's picks).
  int max_rank = 2;
};

// Paint the indicator for every line that emits under (pinned Normal quality, type, split_count).
// line_x[i] = pixel x of official line i, size split_count+1. Particles of all lines are fitted into the
// vertical band [band_top, band_bottom] (pixels, y-down): the dy extent (sprite half sizes and strip half widths
// included) is centred in the band and, if it does not fit at px_per_unit, dy is compressed to fit (x stays at
// true scale). opacity multiplies everything.
// Draw order = baked order; Add/Other use CompositionMode_Plus, Alpha uses SourceOver. No bloom.
// Shape 5 strips (ribbons / particle trails) are drawn as one textured quad per segment.
// Not meant for per-frame use (it builds items and strip segment images on every call): use the cluster API below.
void paint_split_effect_indicator(QPainter& p, const SplitIndicatorSpec& spec, const float* line_x, int line_count,
                                  float band_top, float band_bottom, float opacity = 1.0f);

// Particles-only pixmap on transparent, 12-lane layout spanning the width with the same inset as the picker plate
// (pad = line_w*0.5+2). Cached. Used by picker thumbnails and the hover card.
struct SplitIndicatorPixmapKey {
  int32_t color_id = 1;
  int32_t split_count = 1;
  wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full;
  QSize size{64, 32};
  float min_sprite_px = 0.0f;
  float size_scale = 1.0f;  // see SplitIndicatorSpec::size_scale (default 1 = unchanged behaviour)
};
QPixmap split_effect_indicator_pixmap(const SplitIndicatorPixmapKey& key);           // builds on miss
QPixmap split_effect_indicator_pixmap_if_ready(const SplitIndicatorPixmapKey& key);  // cache only
int prefetch_split_effect_indicator_pixmaps(const std::vector<int32_t>& ids, SplitIndicatorPixmapKey key,
                                            int max_new);

// ---- Cached, layered cluster (the per-frame path for the edit area) ------------------------------------------
// The whole multi-line indicator rendered once into two premultiplied layers (plus an optional third, see sub_layer).
// Line i sits at
// x = edge_i * lane_px (edge from split_boundaries_12: line 0 -> 0, line k -> mids[k-1]+1, line n -> 12), the band
// is [0, band_h] (same dy fit as paint_split_effect_indicator, px_per_unit = lane_px / 0.925).
struct SplitIndicatorClusterKey {
  int32_t color_id = 1;
  int32_t split_count = 1;
  wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full;
  float lane_px = kSplitIndicatorRefLanePx;  // pixel distance between adjacent lane edges (quantized to 0.25 px)
  int band_h = 44;                           // band height in px (the dy-fit band)
  float size_scale = kSplitIndicatorSizeScale;
  float min_sprite_px = kSplitIndicatorRefMinSpritePx;
  int max_rank = 2;  // particles with rank >= max_rank are skipped; part of the cache / dedupe key
};
struct SplitIndicatorCluster {
  QImage alpha_layer;  // premultiplied; draw with SourceOver (blend 1 particles and strips)
  QImage add_layer;    // premultiplied; draw with CompositionMode_Plus (blend 0/2 particles and strips)
  // Subtractive ("cut-out") particles: additive-class quads whose baked tint has a negative rgb (the bats of colour
  // ids 10620 and 11070). In the preview they subtract light on a float target (dst += tex*tint*a, tint < 0) and the
  // display clamps at 0. The build applies them at their draw-order position as dst.rgb = max(0, dst.rgb - m) to
  // the content of alpha_layer and add_layer drawn before them (m = sprite rgb * |tint| * a). What they could not
  // take from the cluster's own content (the residual) belongs to whatever lies UNDER the cluster and is stored here
  // as a multiply mask: premultiplied (a - m_c, a) with a = max(m_c), so drawing it with CompositionMode_Multiply
  // scales an opaque destination by (1 - m_c) per channel, i.e. it never brightens (an approximation of the
  // subtraction on dark scenes). draw_split_effect_indicator_cluster draws it FIRST. Null when no subtractive item
  // left a residual (the common case: almost every id).
  QImage sub_layer;
  QPoint offset;       // top-left of all layers relative to (x of lane edge 0, band_top)
  bool empty = true;   // no visible content: all layers are null
  // All layers share `offset` and size (the union of their tight content boxes); a layer without content is null.
  // The LRU byte accounting counts all three.
};

// Builds on miss, SYNCHRONOUSLY (GUI thread only; tests / one-off paths, never a paint path: a strip-heavy id costs
// 8-30 ms). Never returns nullptr (an empty indicator is a cached cluster with empty=true).
// Lifetime: the LRU (~48 MB of image data) is GUI-thread only and is evicted from the back whenever an entry is
// inserted: by a synchronous miss and by the delivery of background results (which runs from the event loop). A
// returned pointer is therefore valid only until the next insertion; never hold it across event processing or a
// synchronous miss (use it immediately inside the paint, or copy the QImages, which are implicitly shared).
// _if_ready never builds or evicts; nullptr = not built.
// Budget: a build whose layers would exceed half the budget (~24 MB) is refused: the sync API caches an empty
// cluster for it, the _request API keeps returning nullptr (negative-cached for a few seconds, no callback loop).
// Entries looked up (request / _if_ready) since the previous background delivery are pinned against eviction by the
// delivery, so a visible working set larger than the budget is kept (temporary overshoot) and shrinks back once the
// entries are no longer requested. Synchronous inserts use the plain LRU rule.
const SplitIndicatorCluster* split_effect_indicator_cluster(const SplitIndicatorClusterKey& key);
const SplitIndicatorCluster* split_effect_indicator_cluster_if_ready(const SplitIndicatorClusterKey& key);

// Non-blocking variant for paint paths (GUI thread only). Returns the cached cluster if ready (same LRU touch as
// _if_ready). Otherwise returns nullptr and makes sure the key is being built on the single background worker:
//  * in-flight keys are deduplicated (keys compare after the same quantisation as the cache);
//  * the queue is LIFO (the most recently requested key builds first; requesting a queued key again moves it to the
//    front of the line) and bounded to 256 pending keys (the oldest are dropped, and their callbacks with them: the
//    next paint simply requests again);
//  * when the build is done the result is inserted into the LRU on the GUI thread and `on_ready` is invoked through
//    QMetaObject::invokeMethod(notify, on_ready, Qt::QueuedConnection), i.e. on the GUI thread from the event loop. If
//    `notify` was deleted meanwhile the call is skipped. One callback is kept per (key, notify): repeated requests
//    from every paint do not stack callbacks (the first on_ready wins). notify == nullptr = prefetch without callback.
// A key dropped from the full queue wakes its waiters immediately (they repaint and re-request); a build that threw is
// cached as an empty cluster (no endless rebuild when a paint re-requests it every frame).
// Also returns nullptr (and never calls back) when no QCoreApplication exists, when
// ensure_split_effect_uv_atlas() has not been called yet (the build would bake placeholder sprites), or after the
// worker was shut down at application exit. The worker is joined on aboutToQuit, or at the start of ~QCoreApplication
// (qAddPostRoutine) when no event loop ran, and in a static destructor as a fallback; a build that is running
// finishes first. A new application object (tests) re-arms the worker.
const SplitIndicatorCluster* split_effect_indicator_cluster_request(const SplitIndicatorClusterKey& key,
                                                                    QObject* notify,
                                                                    std::function<void()> on_ready);

// Draws sub_layer (Multiply, only if present), then alpha_layer (SourceOver), then add_layer (Plus), all multiplied by
// `opacity`. `edge0_band_top` is the painter-space position of (lane edge 0 x, band_top); it is rounded to whole
// pixels so the layers are not resampled. At most three drawImage calls, no allocation. The Multiply step assumes an
// opaque destination (widget / opaque image), which is what the picker and the editor paint onto.
// The direct painter paint_split_effect_indicator and the pixmap APIs cannot subtract from an arbitrary target and
// leave subtractive particles out (they only show in clusters).
void draw_split_effect_indicator_cluster(QPainter& p, const SplitIndicatorCluster& c, QPointF edge0_band_top,
                                         float opacity = 1.0f);

// ---- Diagnostics (tests) -----------------------------------------------------------------------------------------
// One shape-5 strip exactly as the cluster build draws it: cropped to its window, ribbon-return-to-line transform
// applied (see make_strip_item in the .cpp), after the band fit. Coordinates are cluster pixels: x = 0 at lane edge 0,
// y = 0 at band_top, y down. Runs the layout synchronously on the calling thread; does not touch the caches.
struct SplitIndicatorDebugStrip {
  int32_t line = 0;         // official line index the strip belongs to
  float line_x = 0.0f;      // x of that line (px)
  float px_per_unit = 1.0f;  // world units -> px (dx_world = (x - line_x) / px_per_unit)
  std::vector<float> x;      // centreline, px
  std::vector<float> y;      // centreline, px (y down)
  std::vector<float> width;  // full width, px
};
std::vector<SplitIndicatorDebugStrip> split_effect_indicator_debug_strips(const SplitIndicatorClusterKey& key);

}  // namespace wds::ui

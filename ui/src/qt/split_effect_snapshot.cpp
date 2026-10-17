#include "wds/ui/qt/split_effect_snapshot.hpp"

#include "wds/ui/qt/split_effect_snapshot_data.hpp"
#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/core/split_effect_gate.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRgb>
#include <QStringList>
#include <QTransform>

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace wds::ui {
namespace {

constexpr float kLanePitch = 0.925f;
constexpr int kMaxSpritePx = 256;       // cached sprite side cap (drawn scaled above this)
constexpr float kRingBand = 0.28f;      // ring wall thickness as a fraction of its radius
constexpr std::size_t kMaxSprites = 2048;
constexpr std::size_t kMaxPixmaps = 512;
constexpr float kPi = 3.14159265358979f;
constexpr qint64 kClusterCacheBytes = 48ll * 1024 * 1024;
constexpr int kMaxStripSegPx = 64;         // strip segment image side cap
constexpr float kMinStripPx = 1.5f;        // widest strip vertex is lifted to this when min_sprite_px > 0
constexpr float kStripOverlapPx = 0.5f;    // far-edge overlap of alpha-layer segments (hides SourceOver AA seams)
constexpr float kStripMinSegPx = 0.25f;    // strip segments shorter than this are skipped
constexpr float kStripMaxCompress = 2.0f;  // K: a strip's dy is compressed at most K x (fs >= 1/K), see crop_strip_window
constexpr int kMaxClusterSidePx = 4096;

enum Shape : uint8_t { kBillboard = 0, kStretch = 1, kRing = 2, kCube = 3, kButterfly = 4, kStrip = 5 };

bool& uv_attempted() {  // GUI thread only (ensure_split_effect_uv_atlas)
  static bool attempted = false;
  return attempted;
}

// Atlas data is published once by ensure_split_effect_uv_atlas() (GUI thread) into SharedAtlas under a mutex and
// bumps `gen`. Every thread that builds sprites (GUI thread for the synchronous APIs, the cluster worker for
// _request) reads it through its own ThreadCaches, which re-copy the (implicitly shared, read-only) QImages when
// `gen` changed and drop their derived tile / sprite caches at the same time. No other state is shared.
struct SharedAtlas {
  std::mutex m;
  QImage atlas;                                 // split_effect_uvs.png, straight ARGB32
  QImage trail;                                 // full BombEffectTrail.png (straight ARGB32, top-down)
  QRgb trail_texel = qRgba(255, 255, 255, 255); // corner texel, see ensure_split_effect_uv_atlas
  bool trail_loaded = false;
  std::atomic<quint64> gen{1};
};

SharedAtlas& shared_atlas() {
  static SharedAtlas s;
  return s;
}

struct ThreadCaches {
  quint64 gen = 0;
  QImage atlas;
  // Full BombEffectTrail.png: the texture of blend-2 strips.
  QImage trail_image;
  // BombEffectTrail texel at UV (0,0): the preview samples Trail-material bodies with every UV 0 on a
  // CLAMP_TO_EDGE sampler, i.e. the corner texel. GPU row 0 is the last PNG row.
  QRgb trail_texel = qRgba(255, 255, 255, 255);
  bool trail_loaded = false;
  QHash<uint16_t, QImage> tiles;      // source_tile cache
  QHash<QByteArray, QImage> sprites;  // cached_sprite cache
};

ThreadCaches& thread_caches() {
  thread_local ThreadCaches c;
  SharedAtlas& s = shared_atlas();
  if (c.gen != s.gen.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(s.m);
    c.atlas = s.atlas;
    c.trail_image = s.trail;
    c.trail_texel = s.trail_texel;
    c.trail_loaded = s.trail_loaded;
    c.tiles.clear();
    c.sprites.clear();
    c.gen = s.gen.load(std::memory_order_relaxed);  // written under `m`
  }
  return c;
}

template <typename T>
QByteArray pod_key(const T& v) {
  return QByteArray(reinterpret_cast<const char*>(&v), static_cast<int>(sizeof(T)));
}

QImage solid_tile(QRgb c) {
  QImage t(1, 1, QImage::Format_ARGB32);
  t.setPixel(0, 0, c);
  return t;
}

// Sprite pixels of one uv (straight ARGB32, 1 px gutter removed, top-down). Null = do not draw.
QImage source_tile(uint16_t uv, uint8_t blend, uint8_t shape) {
  namespace cr = wds::chart_render;
  ThreadCaches& tc = thread_caches();
  const QRgb white = qRgba(255, 255, 255, 255);
  // Cube PSR always samples the unbound (white) _MainTex.
  if (shape == kCube) return solid_tile(white);
  if (uv == cr::kSplitEffectNoUvName) {
    // Strips sample the whole trail texture (the preview's trail material), bodies only its corner texel.
    if (shape == kStrip) {
      if (blend == 2 && !tc.trail_image.isNull()) return tc.trail_image;
      return solid_tile(white);
    }
    if (blend == 2 && shape != kRing && tc.trail_loaded) return solid_tile(tc.trail_texel);
    return solid_tile(white);
  }
  auto& cache = tc.tiles;
  const auto it = cache.constFind(uv);
  if (it != cache.cend()) return it.value();
  QImage out;
  const cr::SplitEffectUvTile* tile = cr::official_split_effect_uv_tile(uv);
  int32_t x = 0;
  int32_t y = 0;
  bool present = false;
  if (tile == nullptr) {
    out = solid_tile(white);
  } else if (cr::official_split_effect_uv_px(uv, x, y, present) && present && !tc.atlas.isNull()) {
    int tw = tile->w;
    int th = tile->h;
    const int g = cr::kSplitEffectUvGutterPx;
    if (tw > 2 * g && th > 2 * g) {
      tw -= 2 * g;
      th -= 2 * g;
      x += g;
      y += g;
    }
    if (tw > 0 && th > 0 && x >= 0 && y >= 0 && x + tw <= tc.atlas.width() && y + th <= tc.atlas.height()) {
      out = tc.atlas.copy(x, y, tw, th);
    }
  }
  cache.insert(uv, out);
  return out;
}

// Mesh "Ring" is a 32-seg cylinder wall. Seen top-down it reads as an annulus; the sprite is wrapped
// around it (u per quadrant, v across the band), see split_effect_ring_mesh.inc.
QImage ring_image(const QImage& tile, int w, int h) {
  QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
  out.fill(0);
  const float rx = static_cast<float>(w) * 0.5f;
  const float ry = static_cast<float>(h) * 0.5f;
  const float rmin = std::min(rx, ry);
  const int tw = tile.width();
  const int th = tile.height();
  for (int y = 0; y < h; ++y) {
    auto* row = reinterpret_cast<QRgb*>(out.scanLine(y));
    const float ny = (static_cast<float>(y) + 0.5f - ry) / ry;
    for (int x = 0; x < w; ++x) {
      const float nx = (static_cast<float>(x) + 0.5f - rx) / rx;
      const float rr = std::sqrt(nx * nx + ny * ny);
      const float cov = std::clamp((1.0f - rr) * rmin + 0.5f, 0.0f, 1.0f) *
                        std::clamp((rr - (1.0f - kRingBand)) * rmin + 0.5f, 0.0f, 1.0f);
      if (cov <= 0.0f) continue;
      float ang = std::atan2(-ny, nx);  // y-up
      if (ang < 0.0f) ang += 2.0f * kPi;
      const float f = std::fmod(ang, 0.5f * kPi) / (0.5f * kPi);
      const float radial = std::clamp((rr - (1.0f - kRingBand)) / kRingBand, 0.0f, 1.0f);
      const int sx = std::clamp(static_cast<int>(f * static_cast<float>(tw)), 0, tw - 1);
      const int sy = std::clamp(static_cast<int>((1.0f - radial) * static_cast<float>(th)), 0, th - 1);
      const QRgb c = qPremultiply(reinterpret_cast<const QRgb*>(tile.constScanLine(sy))[sx]);
      row[x] = qRgba(static_cast<int>(qRed(c) * cov), static_cast<int>(qGreen(c) * cov),
                     static_cast<int>(qBlue(c) * cov), static_cast<int>(qAlpha(c) * cov));
    }
  }
  return out;
}

// Subtractive particle: an additive-class (blend != 1) quad whose baked tint has a negative rgb (ButterflyEffect
// brightness = Custom1.x + 1 < 0, e.g. the bat cut-outs of 10620 / 11070). The preview's HDR float target lets
// SrcAlpha,One with a negative source SUBTRACT light (dst += tex.rgb * tint * a, tint < 0) and only clamps the final
// result at 0. The sprite of such an item is tinted with |rgb| and applied as a subtraction (see apply_subtractive).
// Strips never are (none of the baked strip vertices is negative). Alpha blend (1) is not handled: it stays clamped.
bool is_subtractive(const SplitSnapParticle& sp) {
  return sp.shape != kStrip && sp.blend != 1 && (sp.r < 0.0f || sp.g < 0.0f || sp.b < 0.0f);
}

// SplitParticle shaders: out = texel * vertexColor. Additive (blend 0, and 2 = Trail material with
// colour *= 2 on RGBA) is SrcAlpha,One -> rgb*a; Alpha is SrcAlpha,OneMinusSrcAlpha. Clip: Additive
// a<0.001, Alpha / Trail a<0.01. The preview target is float, here values clamp to 8 bit.
void tint_image(QImage& img, uint8_t blend, float r, float g, float b, float a) {
  const float k = blend == 2 ? wds::chart_render::kOfficialSplitEffectTrailAdditiveIntensity : 1.0f;
  r *= k;
  g *= k;
  b *= k;
  a *= k;
  const float clip = blend == 0 ? 0.001f : 0.01f;
  const bool additive = blend != 1;
  auto to8 = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  for (int y = 0; y < img.height(); ++y) {
    auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < img.width(); ++x) {
      const QRgb c = row[x];
      const int sa = qAlpha(c);
      if (sa == 0) continue;
      const float big_a = static_cast<float>(sa) / 255.0f * a;
      if (big_a < clip) {
        row[x] = 0;
        continue;
      }
      const float inv = 1.0f / static_cast<float>(sa);  // premult -> straight
      // Additive: premult rgb = tex*col*A (A may exceed 1). Alpha: clamp(tex*col) * clamp(A).
      const float oa0 = std::clamp(big_a, 0.0f, 1.0f);
      const float w = additive ? big_a : oa0;
      auto chan = [&](int premult, float col) {
        const float t = std::min(static_cast<float>(premult) * inv, 1.0f) * col;
        return std::clamp(additive ? t * w : std::clamp(t, 0.0f, 1.0f) * w, 0.0f, 1.0f);
      };
      const float orr = chan(qRed(c), r);
      const float og = chan(qGreen(c), g);
      const float ob = chan(qBlue(c), b);
      float oa = oa0;
      if (additive) oa = std::max(oa, std::max(orr, std::max(og, ob)));  // keep premultiplied valid
      row[x] = qRgba(to8(orr), to8(og), to8(ob), to8(oa));
    }
  }
}

QImage flip_image(const QImage& tile, uint8_t flip) {
  if ((flip & 3) == 0) return tile;
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  Qt::Orientations o;
  if ((flip & 1) != 0) o |= Qt::Horizontal;
  if ((flip & 2) != 0) o |= Qt::Vertical;
  return tile.flipped(o);
#else
  return tile.mirrored((flip & 1) != 0, (flip & 2) != 0);
#endif
}

uint16_t q64(float v) {
  return static_cast<uint16_t>(std::clamp(std::lround(v * 64.0f), 0L, 65535L));
}

struct SpriteKey {
  uint16_t uv;
  uint8_t blend, shape, flip, pad;
  uint16_t r, g, b, a;
  int16_t w, h;
};

// Tinted sprite of exactly w x h px (butterfly: one wing), cached by quantised parameters.
QImage cached_sprite(const SplitSnapParticle& sp, uint8_t shape, int w, int h) {
  auto& cache = thread_caches().sprites;
  SpriteKey k{};
  k.uv = sp.uv;
  k.blend = sp.blend;
  k.shape = shape;
  k.flip = (shape == kBillboard || shape == kStretch) ? static_cast<uint8_t>(sp.flip & 3) : 0;
  // Subtractive sprites (see is_subtractive) are tinted with |rgb|; the sign is applied when compositing.
  const bool sub = is_subtractive(sp);
  k.r = q64(sub ? std::fabs(sp.r) : sp.r);
  k.g = q64(sub ? std::fabs(sp.g) : sp.g);
  k.b = q64(sub ? std::fabs(sp.b) : sp.b);
  k.a = q64(sp.a);
  k.w = static_cast<int16_t>(w);
  k.h = static_cast<int16_t>(h);
  const QByteArray key = pod_key(k);
  const auto it = cache.constFind(key);
  if (it != cache.cend()) return it.value();

  QImage tile = source_tile(sp.uv, sp.blend, shape);
  QImage out;
  if (!tile.isNull()) {
    tile = flip_image(tile, k.flip);
    if (shape == kRing) {
      out = ring_image(tile, w, h);
    } else if (tile.width() == 1 && tile.height() == 1) {
      out = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
      out.fill(qPremultiply(tile.pixel(0, 0)));
    } else {
      out = tile.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    tint_image(out, sp.blend, k.r / 64.0f, k.g / 64.0f, k.b / 64.0f, k.a / 64.0f);
  }
  if (cache.size() >= static_cast<qsizetype>(kMaxSprites)) cache.clear();
  cache.insert(key, out);
  return out;
}

// One strip vertex in pixels (x final, dyp y-up before band fit).
struct StripVert {
  float x = 0.0f;
  float dyp = 0.0f;
  float wpx = 0.0f;  // full width
  float t = 0.0f;    // texture coordinate along the strip (SplitSnapStripPoint::u)
  float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

struct Item {
  const SplitSnapParticle* sp = nullptr;
  uint8_t shape = kBillboard;
  int32_t line = 0;       // official line index
  float line_x = 0.0f;    // pixel x of that line
  float x = 0.0f;    // pixel x of the particle centre
  float dyp = 0.0f;  // dy in pixels, y-up (before band fit)
  float wpx = 0.0f;
  float hpx = 0.0f;
  float half_w = 0.0f;  // half of the rotated bounding box width
  float half_h = 0.0f;  // half of the rotated bounding box height
  QImage img;
  std::vector<StripVert> strip;  // shape kStrip only: the cropped window (see crop_strip_window)
  float strip_c = 0.0f;          // shape kStrip only: dy centre (px, y-up, before band fit) of the window
  bool subtractive = false;      // see is_subtractive: img is tinted with |rgb| and subtracted, not drawn
  bool alpha_layer() const { return sp->blend == 1; }
};

struct BuildParams {
  int32_t color_id = 1;
  int32_t split_count = 1;
  wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full;
  float ppu = 16.0f;
  float size_scale = 1.0f;
  float min_sprite_px = 0.0f;
  int max_rank = 2;  // particles with SplitSnapParticle::rank >= max_rank are skipped
};

// Items of every emitting line in baked order plus the band fit (shared by direct paint, pixmaps and clusters).
struct Layout {
  std::vector<Item> items;
  float f = 1.0f;         // dy compression factor (sprites; also places the strip windows)
  float fs = 1.0f;        // dy factor inside a strip window = max(f, 1/kStripMaxCompress)
  float centre_y = 0.0f;  // band centre (Qt y)
  float mid_up = 0.0f;    // centre of the dy extent (y-up)
  float y_of(float dyp) const { return centre_y - (dyp * f - mid_up); }  // +dy is up -> smaller Qt y
  // Strip vertex: the window centre sits where the global fit puts it (c * f); the window itself is scaled by fs.
  float y_of_strip(const Item& it, float dyp) const {
    return centre_y - (it.strip_c * f + (dyp - it.strip_c) * fs - mid_up);
  }
};

void cap_quad(Item& it, float angle_deg, float max_v_extent, float max_h_extent) {
  // Cap the rotated extents (after min_sprite_px, cap wins): shrink only the axis contributing most so
  // long beams stay thin instead of becoming blobs.
  const float th_c = angle_deg * kPi / 180.0f;
  const float s_abs = std::fabs(std::sin(th_c));
  const float c_abs = std::fabs(std::cos(th_c));
  const auto cap_extent = [&](float cw, float ch, float max_ext) {
    const float a = cw * it.wpx;
    const float b = ch * it.hpx;
    if (a + b <= max_ext) return;
    if (a >= b) {
      if (b >= max_ext) {
        const float k = max_ext / (a + b);
        it.wpx *= k;
        it.hpx *= k;
      } else {
        it.wpx *= (max_ext - b) / a;
      }
    } else {
      if (a >= max_ext) {
        const float k = max_ext / (a + b);
        it.wpx *= k;
        it.hpx *= k;
      } else {
        it.hpx *= (max_ext - a) / b;
      }
    }
  };
  cap_extent(s_abs, c_abs, max_v_extent);  // vertical extent = |w sin| + |h cos|
  cap_extent(c_abs, s_abs, max_h_extent);  // horizontal extent = |w cos| + |h sin|
}

// Quad item (shapes 0-4). Returns false when nothing is drawn.
bool make_quad_item(Item& it, const SplitSnapParticle& sp, const BuildParams& bp, float line_x, float max_v_extent,
                    float max_h_extent) {
  it.sp = &sp;
  it.shape = sp.shape <= kButterfly ? sp.shape : static_cast<uint8_t>(kBillboard);
  it.wpx = sp.w * bp.ppu * bp.size_scale;
  it.hpx = sp.h * bp.ppu * bp.size_scale;
  const float big = std::max(it.wpx, it.hpx);
  if (bp.min_sprite_px > 0.0f && big > 0.0f && big < bp.min_sprite_px) {
    const float s = bp.min_sprite_px / big;
    it.wpx *= s;
    it.hpx *= s;
  }
  if (!(it.wpx > 0.0f) || !(it.hpx > 0.0f)) return false;
  cap_quad(it, sp.angle_deg, max_v_extent, max_h_extent);
  if (!(it.wpx > 0.0f) || !(it.hpx > 0.0f)) return false;
  const int iw = std::clamp(static_cast<int>(std::lround(it.shape == kButterfly ? it.wpx * 0.5f : it.wpx)), 1,
                            kMaxSpritePx);
  const int ih = std::clamp(static_cast<int>(std::lround(it.hpx)), 1, kMaxSpritePx);
  it.img = cached_sprite(sp, it.shape, iw, ih);
  if (it.img.isNull()) return false;
  it.subtractive = is_subtractive(sp);
  it.x = line_x + sp.dx * bp.ppu;
  it.dyp = sp.dy * bp.ppu;
  const float th = sp.angle_deg * kPi / 180.0f;
  it.half_w = 0.5f * (std::fabs(it.wpx * std::cos(th)) + std::fabs(it.hpx * std::sin(th)));
  it.half_h = 0.5f * (std::fabs(it.wpx * std::sin(th)) + std::fabs(it.hpx * std::cos(th)));
  return true;
}

StripVert lerp_vert(const StripVert& a, const StripVert& b, float t) {
  const auto mix = [t](float x, float y) { return x + (y - x) * t; };
  StripVert o;
  o.x = mix(a.x, b.x);
  o.dyp = mix(a.dyp, b.dyp);
  o.wpx = mix(a.wpx, b.wpx);
  o.t = mix(a.t, b.t);
  o.r = mix(a.r, b.r);
  o.g = mix(a.g, b.g);
  o.b = mix(a.b, b.b);
  o.a = mix(a.a, b.a);
  return o;
}

// Longest run of `v` starting at vertex `first` whose centreline dy span (max - min) stays <= max_span. The segment
// that would break the limit is cut where the span reaches max_span exactly (x, dy, width, u and rgba interpolated).
void strip_run(const std::vector<StripVert>& v, std::size_t first, float max_span, std::vector<StripVert>& out) {
  out.clear();
  out.push_back(v[first]);
  float lo = v[first].dyp;
  float hi = lo;
  for (std::size_t j = first + 1; j < v.size(); ++j) {
    const float y = v[j].dyp;
    const float nlo = std::min(lo, y);
    const float nhi = std::max(hi, y);
    if (nhi - nlo <= max_span) {
      out.push_back(v[j]);
      lo = nlo;
      hi = nhi;
      continue;
    }
    const StripVert& a = v[j - 1];
    const float target = y > hi ? lo + max_span : hi - max_span;  // the limit that y crosses
    const float den = y - a.dyp;
    const float t = std::fabs(den) > 1e-9f ? std::clamp((target - a.dyp) / den, 0.0f, 1.0f) : 0.0f;
    out.push_back(lerp_vert(a, v[j], t));
    break;
  }
}

// Sum over segments of average alpha x average width x length (px): how much visible ribbon a window holds.
float strip_run_score(const std::vector<StripVert>& w) {
  float s = 0.0f;
  for (std::size_t k = 0; k + 1 < w.size(); ++k) {
    const float len = std::hypot(w[k + 1].x - w[k].x, w[k + 1].dyp - w[k].dyp);
    s += 0.5f * (std::max(0.0f, w[k].a) + std::max(0.0f, w[k + 1].a)) * 0.5f * (w[k].wpx + w[k + 1].wpx) * len;
  }
  return s;
}

// A strip keeps its true shape: instead of squeezing a long ribbon into the band, only a window of its polyline is
// drawn, whose dy span is <= max_span. Candidates are the maximal runs that start at a vertex (extended towards the
// tail) and that end at a vertex (extended towards the head); the best is the one with the highest
// strip_run_score. Vertex 0 is the head (newest, most opaque), candidates are scanned head first and a later one must
// beat the best by > 0.01 %, so ties go to the head end.
std::vector<StripVert> crop_strip_window(const std::vector<StripVert>& v, float max_span) {
  const std::size_t n = v.size();
  float lo = 1e30f;
  float hi = -1e30f;
  for (const StripVert& s : v) {
    lo = std::min(lo, s.dyp);
    hi = std::max(hi, s.dyp);
  }
  if (hi - lo <= max_span) return v;
  const std::vector<StripVert> rev(v.rbegin(), v.rend());
  std::vector<StripVert> best;
  std::vector<StripVert> cand;
  float best_score = 0.0f;
  const auto consider = [&]() {
    if (cand.size() < 2) return;
    const float s = strip_run_score(cand);
    if (best.empty() || s > best_score * 1.0001f + 1e-6f) {
      best = cand;
      best_score = s;
    }
  };
  for (std::size_t k = 0; k < n; ++k) {
    strip_run(rev, n - 1 - k, max_span, cand);  // window ending at vertex k, grown towards the head
    std::reverse(cand.begin(), cand.end());
    consider();
    strip_run(v, k, max_span, cand);  // window starting at vertex k, grown towards the tail
    consider();
  }
  if (best.empty()) best.assign(v.begin(), v.begin() + 2);
  return best;
}

// ---- ribbon return-to-line ---------------------------------------------------------------------------------------
// A ribbon may drift away from its split line, so its head and tail hang in the air. The (cropped) window is bent so
// both ends land on the line while the wiggle in between is kept. With arc length t in [0,1] along the (x, dy)
// polyline (px), x(t) the offset from the line, w(t) the width:
//   b(t) = m0 + (m1 - m0) * t          drift baseline; m0 / m1 = mean of x over t in [0, 0.15] / [0.85, 1]
//   s(t) = 0.5 - 0.5 cos(pi * d),  d = clamp(min(t, 1 - t) / 0.20, 0, 1)
//                                      envelope: 0 at both ends, 1 for t in [0.2, 0.8], C1 raised-cosine ramps
//   x'(t) = (x(t) - b(t)) * s(t)       own wiggle preserved in the middle, drift removed, eased to 0 (= on the line)
//   w'(t) = w(t) * (0.35 + 0.65 * s(t))   ends taper to 35 % so they do not look cut
// dy is unchanged (the ends stay where they are along the line). Every strip gets the same treatment (near-line strips
// barely change), so every head and tail lands exactly on the line. Before the maths the window is re-sampled: Hermite
// (Catmull-Rom, chord-length parameter) on (x, dy), linear on width / u / rgba, inserting points so no segment is
// longer than ~3 px (at the uncompressed scale, an upper bound of the final length), at most 96 points in total
// (the step grows until that holds; original vertices are always kept), so the bend has no sudden fold.
// Baked trails zig-zag from vertex to vertex, which at the ends would be hairpins however smooth the bend is. So two
// fold-removal steps act on the ends only (first / last vertex keep their position along the line, the middle is
// untouched): the interior vertices' (x, dy) are blended into a box average towards the ends before the resampling
// (ribbon_return_to_line), and corners sharper than 75 degrees in the end zones are relaxed afterwards
// (ribbon_relax_folds). This changes dy of the end zones slightly (the jitter is removed) although the bend itself
// keeps dy.
constexpr float kRibbonMaxSegPx = 3.0f;
constexpr std::size_t kRibbonMaxPoints = 96;
constexpr float kRibbonEndFrac = 0.20f;
constexpr float kRibbonMeanFrac = 0.15f;
constexpr float kRibbonWidthFloor = 0.35f;
constexpr float kRibbonSmoothFrac = 0.15f;   // fold removal 1: box half-window as a fraction of the vertex count
constexpr float kRibbonSmoothRamp = 0.50f;   // fold removal 1: ramp length (arc-length fraction) at each end
constexpr float kRibbonRelaxLimitDeg = 75.0f;  // fold removal 2: corners sharper than this are relaxed
constexpr int kRibbonRelaxIters = 80;

// Raised cosine 0 -> 1 over the first `ramp` of arc length at each end (0 at t = 0 and 1, 1 beyond the ramps).
float ribbon_envelope(float t, float ramp) {
  const float d = std::clamp(std::min(t, 1.0f - t) / ramp, 0.0f, 1.0f);
  return 0.5f - 0.5f * std::cos(kPi * d);
}

// Turning angle (degrees) at b between a->b and b->c, with y scaled by fs (the band fit compresses a strip window's
// dy by fs in [0.5, 1]). Segments shorter than 0.05 px have no direction (0).
float ribbon_turn_deg(const StripVert& a, const StripVert& b, const StripVert& c, float fs) {
  const float ax = b.x - a.x;
  const float ay = (b.dyp - a.dyp) * fs;
  const float bx = c.x - b.x;
  const float by = (c.dyp - b.dyp) * fs;
  const float la = std::hypot(ax, ay);
  const float lb = std::hypot(bx, by);
  if (la < 0.05f || lb < 0.05f) return 0.0f;
  return std::acos(std::clamp((ax * bx + ay * by) / (la * lb), -1.0f, 1.0f)) * 180.0f / kPi;
}

// Fold removal, part 2 (after the bend): inside the first / last 20 % of arc length (fading out to 30 %) every vertex
// whose turn exceeds kRibbonRelaxLimitDeg (checked at both ends of the band-fit dy compression, fs = 1 and 0.5) is
// moved halfway towards the midpoint of its neighbours; Jacobi iterations, at most kRibbonRelaxIters, until no corner
// is left. The first and last vertex never move, so the ends stay exactly where they landed.
void ribbon_relax_folds(std::vector<StripVert>& r, const std::vector<float>& t) {
  const std::size_t m = r.size();
  if (m < 3) return;
  const auto lambda = [&](std::size_t i) {
    const float d = std::min(t[i], 1.0f - t[i]);
    if (d <= 0.2f) return 0.5f;
    if (d >= 0.3f) return 0.0f;
    const float u = (d - 0.2f) / 0.1f;
    return 0.5f * (1.0f - u * u * (3.0f - 2.0f * u));
  };
  std::vector<std::pair<float, float>> nxt(m);
  for (int it = 0; it < kRibbonRelaxIters; ++it) {
    bool moved = false;
    for (std::size_t i = 0; i < m; ++i) nxt[i] = {r[i].x, r[i].dyp};
    for (std::size_t i = 1; i + 1 < m; ++i) {
      const float l = lambda(i);
      if (l <= 0.0f) continue;
      if (std::max(ribbon_turn_deg(r[i - 1], r[i], r[i + 1], 1.0f), ribbon_turn_deg(r[i - 1], r[i], r[i + 1], 0.5f)) <=
          kRibbonRelaxLimitDeg) {
        continue;
      }
      nxt[i].first = r[i].x * (1.0f - l) + 0.5f * l * (r[i - 1].x + r[i + 1].x);
      nxt[i].second = r[i].dyp * (1.0f - l) + 0.5f * l * (r[i - 1].dyp + r[i + 1].dyp);
      moved = true;
    }
    if (!moved) break;
    for (std::size_t i = 0; i < m; ++i) {
      r[i].x = nxt[i].first;
      r[i].dyp = nxt[i].second;
    }
  }
}

std::vector<StripVert> ribbon_return_to_line(const std::vector<StripVert>& in, float line_x) {
  // Drop zero-length repeats (they have no direction and would break the chord-length parameter).
  std::vector<StripVert> v;
  v.reserve(in.size());
  for (const StripVert& s : in) {
    if (!v.empty() && std::hypot(s.x - v.back().x, s.dyp - v.back().dyp) < 1e-4f) continue;
    v.push_back(s);
  }
  if (v.size() < 2) return in;
  const std::size_t n = v.size();
  std::vector<float> s(n, 0.0f);
  for (std::size_t i = 1; i < n; ++i) s[i] = s[i - 1] + std::hypot(v[i].x - v[i - 1].x, v[i].dyp - v[i - 1].dyp);
  float total = s.back();
  if (!(total > 1e-3f)) return in;

  // Fold removal, part 1: baked trails zig-zag from vertex to vertex (x and dy jump back and forth), and near the ends
  // every back-and-forth is a hairpin. Towards the ends the (x, dy) of the INTERIOR vertices is blended into a box
  // average (+-max(2, 15 % of the vertex count) vertices, edge-replicated): weight 1 - e2(t), e2 = the raised cosine
  // above with a 50 % ramp. The first and last vertex keep their dy. The middle is untouched.
  if (n >= 4) {
    const int kw = std::max(2, static_cast<int>(std::lround(kRibbonSmoothFrac * static_cast<float>(n))));
    std::vector<float> sx(n), sy(n);
    for (std::size_t i = 0; i < n; ++i) {
      float ax = 0.0f;
      float ay = 0.0f;
      for (int j = -kw; j <= kw; ++j) {
        const std::size_t q = static_cast<std::size_t>(std::clamp<long>(static_cast<long>(i) + j, 0, static_cast<long>(n) - 1));
        ax += v[q].x;
        ay += v[q].dyp;
      }
      sx[i] = ax / static_cast<float>(2 * kw + 1);
      sy[i] = ay / static_cast<float>(2 * kw + 1);
    }
    for (std::size_t i = 1; i + 1 < n; ++i) {
      const float e2 = ribbon_envelope(s[i] / total, kRibbonSmoothRamp);
      v[i].x = v[i].x * e2 + sx[i] * (1.0f - e2);
      v[i].dyp = v[i].dyp * e2 + sy[i] * (1.0f - e2);
    }
    for (std::size_t i = 1; i < n; ++i) s[i] = s[i - 1] + std::hypot(v[i].x - v[i - 1].x, v[i].dyp - v[i - 1].dyp);
    total = s.back();
    if (!(total > 1e-3f)) return in;
  }

  // Hermite tangents (per unit arc length): central differences, one-sided at the ends.
  std::vector<float> mx(n), my(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t a = i > 0 ? i - 1 : 0;
    const std::size_t b = i + 1 < n ? i + 1 : n - 1;
    const float ds = s[b] - s[a];
    mx[i] = (v[b].x - v[a].x) / ds;
    my[i] = (v[b].dyp - v[a].dyp) / ds;
  }

  std::vector<int> pieces(n - 1, 1);
  float step = kRibbonMaxSegPx;
  for (;;) {
    std::size_t tot = 1;
    for (std::size_t i = 0; i + 1 < n; ++i) {
      pieces[i] = std::max(1, static_cast<int>(std::ceil((s[i + 1] - s[i]) / step)));
      tot += static_cast<std::size_t>(pieces[i]);
    }
    if (tot <= kRibbonMaxPoints || step > total) break;
    step *= 1.25f;
  }

  std::vector<StripVert> r;
  r.push_back(v[0]);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const float h = s[i + 1] - s[i];
    const int k = pieces[i];
    for (int j = 1; j <= k; ++j) {
      if (j == k) {
        r.push_back(v[i + 1]);
        continue;
      }
      const float u = static_cast<float>(j) / static_cast<float>(k);
      const float u2 = u * u;
      const float u3 = u2 * u;
      const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
      const float h10 = u3 - 2.0f * u2 + u;
      const float h01 = -2.0f * u3 + 3.0f * u2;
      const float h11 = u3 - u2;
      StripVert o = lerp_vert(v[i], v[i + 1], u);
      o.x = h00 * v[i].x + h10 * h * mx[i] + h01 * v[i + 1].x + h11 * h * mx[i + 1];
      o.dyp = h00 * v[i].dyp + h10 * h * my[i] + h01 * v[i + 1].dyp + h11 * h * my[i + 1];
      r.push_back(o);
    }
  }

  // Normalised arc length of the re-sampled polyline.
  const std::size_t m = r.size();
  std::vector<float> t(m, 0.0f);
  for (std::size_t i = 1; i < m; ++i) t[i] = t[i - 1] + std::hypot(r[i].x - r[i - 1].x, r[i].dyp - r[i - 1].dyp);
  if (!(t.back() > 1e-3f)) return in;
  const float inv_total = 1.0f / t.back();
  for (float& q : t) q *= inv_total;
  t.back() = 1.0f;

  float m0 = 0.0f;
  float m1 = 0.0f;
  {
    const auto x_at = [&](float tt) {  // linear interpolation of the offset at arc length tt
      const std::size_t hi =
          std::clamp<std::size_t>(static_cast<std::size_t>(std::upper_bound(t.begin(), t.end(), tt) - t.begin()), 1, m - 1);
      const float span = t[hi] - t[hi - 1];
      const float f = span > 1e-9f ? std::clamp((tt - t[hi - 1]) / span, 0.0f, 1.0f) : 0.0f;
      return (r[hi - 1].x + (r[hi].x - r[hi - 1].x) * f) - line_x;
    };
    constexpr int kSamples = 16;
    const auto mean_over = [&](float a, float b) {
      float acc = 0.0f;
      for (int k = 0; k < kSamples; ++k) acc += x_at(a + (b - a) * (static_cast<float>(k) + 0.5f) / kSamples);
      return acc / kSamples;
    };
    m0 = mean_over(0.0f, kRibbonMeanFrac);
    m1 = mean_over(1.0f - kRibbonMeanFrac, 1.0f);
  }
  for (std::size_t i = 0; i < m; ++i) {
    const float env = ribbon_envelope(t[i], kRibbonEndFrac);
    const float b = m0 + (m1 - m0) * t[i];
    r[i].x = line_x + (r[i].x - line_x - b) * env;
    r[i].wpx *= kRibbonWidthFloor + (1.0f - kRibbonWidthFloor) * env;
  }
  // Exact landing (the envelope is 0 here; this only removes float noise).
  r.front().x = line_x;
  r.back().x = line_x;
  ribbon_relax_folds(r, t);
  return r;
}

// Strip item (shape 5): vertices to pixels; widths x size_scale, lifted so the widest vertex is >= kMinStripPx when
// min_sprite_px is on, and capped to the horizontal extent. The polyline is then cropped to a window whose dy span,
// compressed by at most kStripMaxCompress, plus the widest half-widths, fits the band (crop_strip_window).
// No texture work here (done when drawn).
bool make_strip_item(Item& it, const SplitSnapParticle& sp, const BuildParams& bp, const SplitSnapStripPoint* pool,
                     float line_x, float max_h_extent, float band_h) {
  if (pool == nullptr || sp.strip_count < 2) return false;
  it.sp = &sp;
  it.shape = kStrip;
  const SplitSnapStripPoint* pts = pool + sp.strip_first;
  const std::size_t n = sp.strip_count;
  it.strip.resize(n);
  float max_w = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    StripVert& v = it.strip[i];
    v.x = line_x + pts[i].dx * bp.ppu;
    v.dyp = pts[i].dy * bp.ppu;
    v.wpx = std::max(0.0f, pts[i].width) * bp.ppu * bp.size_scale;
    v.t = pts[i].u;
    v.r = pts[i].r;
    v.g = pts[i].g;
    v.b = pts[i].b;
    v.a = pts[i].a;
    max_w = std::max(max_w, v.wpx);
  }
  if (!(max_w > 1e-4f)) return false;
  float k = 1.0f;
  if (bp.min_sprite_px > 0.0f && max_w < kMinStripPx) k = kMinStripPx / max_w;
  if (max_w * k > max_h_extent) k = max_h_extent / max_w;
  float half_w = 0.0f;
  for (StripVert& v : it.strip) {
    v.wpx *= k;
    half_w = std::max(half_w, 0.5f * v.wpx);
  }
  // Window span: at fs = 1/K the window plus its (uncompressed) widest width must still fit the band, so the
  // global fit never has to push f to 0 because of a strip: K * (band_h - widest width).
  const float max_span = kStripMaxCompress * std::max(band_h - 2.0f * half_w, 0.25f * band_h);
  it.strip = crop_strip_window(it.strip, std::max(max_span, 1.0f));
  it.strip = ribbon_return_to_line(it.strip, line_x);  // head and tail back onto the line, see above
  float clo = 1e30f;
  float chi = -1e30f;
  for (const StripVert& v : it.strip) {
    clo = std::min(clo, v.dyp);
    chi = std::max(chi, v.dyp);
  }
  it.strip_c = 0.5f * (clo + chi);
  it.x = it.strip.front().x;
  it.dyp = it.strip.front().dyp;
  it.half_w = half_w;
  it.half_h = half_w;
  return true;
}

// keep_subtractive: only the cluster build can apply subtractive items (it owns the layers); the direct painter has
// an arbitrary target, so it passes false and such items are left out (as before: a negative tint drew nothing).
void build_layout(const BuildParams& bp, const float* line_x, int line_count, float band_top, float band_bottom,
                  bool keep_subtractive, Layout& out) {
  out = Layout{};
  const float ppu = bp.ppu;
  const float max_v_extent = std::max(1.0f, 0.9f * (band_bottom - band_top));
  const float max_h_extent = std::max(1.0f, 3.0f * kLanePitch * ppu);
  const float band_h = std::max(0.0f, band_bottom - band_top);
  const SplitSnapStripPoint* pool = split_snap_strip_points();

  wds::chart_editor::official_split_effect_for_each_emit(
      wds::chart_editor::kOfficialSpritEffectNormal, bp.type, bp.split_count, [&](int32_t line) {
        if (line < 0 || line >= line_count) return;
        int count = 0;
        const SplitSnapParticle* snap = split_snap_particles(bp.color_id, bp.split_count, line, &count);
        if (snap == nullptr) return;
        for (int i = 0; i < count; ++i) {
          if (static_cast<int>(snap[i].rank) >= bp.max_rank) continue;
          Item it;
          const bool ok = snap[i].shape == kStrip
                              ? make_strip_item(it, snap[i], bp, pool, line_x[line], max_h_extent, band_h)
                              : make_quad_item(it, snap[i], bp, line_x[line], max_v_extent, max_h_extent);
          if (ok && (keep_subtractive || !it.subtractive)) {
            it.line = line;
            it.line_x = line_x[line];
            out.items.push_back(std::move(it));
          }
        }
      });
  if (out.items.empty()) return;

  // Band fit: centre the dy extent (including half sizes / strip half widths) in the band; compress dy if it
  // overflows. Sprites are compressed by f; a strip window keeps its centre at c * f but is scaled by
  // fs = max(f, 1/K) only (extent is convex in f and <= band_h at f = 0, so the bisection finds the one crossing).
  auto extent = [&](float f, float& lo, float& hi) {
    lo = 1e30f;
    hi = -1e30f;
    const float fs = std::max(f, 1.0f / kStripMaxCompress);
    for (const Item& it : out.items) {
      if (it.shape == kStrip) {
        for (const StripVert& v : it.strip) {
          const float y = it.strip_c * f + (v.dyp - it.strip_c) * fs;
          lo = std::min(lo, y - 0.5f * v.wpx);
          hi = std::max(hi, y + 0.5f * v.wpx);
        }
      } else {
        lo = std::min(lo, it.dyp * f - it.half_h);
        hi = std::max(hi, it.dyp * f + it.half_h);
      }
    }
  };
  float f = 1.0f;
  float lo = 0.0f;
  float hi = 0.0f;
  extent(1.0f, lo, hi);
  if (hi - lo > band_h) {
    float a0 = 0.0f;
    float a1 = 1.0f;
    for (int iter = 0; iter < 24; ++iter) {
      const float m = 0.5f * (a0 + a1);
      extent(m, lo, hi);
      (hi - lo <= band_h ? a0 : a1) = m;
    }
    f = a0;
    extent(f, lo, hi);
  }
  out.f = f;
  out.fs = std::max(f, 1.0f / kStripMaxCompress);
  out.centre_y = 0.5f * (band_top + band_bottom);
  out.mid_up = 0.5f * (lo + hi);
}

// Conservative pixel bounds of an item (padded by 2 px for AA / overlap).
QRectF item_bounds(const Layout& lay, const Item& it) {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  if (it.shape == kStrip) {
    // Every segment rectangle is at most half_w (widest half width) + overlap away from its vertices.
    const float h = it.half_w + kStripOverlapPx;
    for (const StripVert& v : it.strip) {
      const float y = lay.y_of_strip(it, v.dyp);
      x0 = std::min(x0, v.x - h);
      x1 = std::max(x1, v.x + h);
      y0 = std::min(y0, y - h);
      y1 = std::max(y1, y + h);
    }
  } else {
    const float y = lay.y_of(it.dyp);
    x0 = it.x - it.half_w;
    x1 = it.x + it.half_w;
    y0 = y - it.half_h;
    y1 = y + it.half_h;
  }
  return QRectF(QPointF(x0 - 2.0f, y0 - 2.0f), QPointF(x1 + 2.0f, y1 + 2.0f));
}

// ---- strip drawing ---------------------------------------------------------------------------------------------
// Each segment i->i+1 of the (cropped, see crop_strip_window) polyline is one rectangle aligned to the segment
// direction: length = segment length (+ kStripOverlapPx past the far end on the alpha layer, hides SourceOver AA
// seams), width = average of the two vertex widths, texture slice t[i]..t[i+1] along, 0..1 across (left..right of the
// travel direction), colour = average of the two vertex tints. Always affine (a rotated rectangle), never projective,
// so a folded or sharply turning ribbon cannot blow a quad up: its bbox is <= segment length + width in any direction.
// Segments shorter than kStripMinSegPx are skipped. No joint fill: at a sharp turn the outer side keeps a small wedge
// notch (widths are a few px), which is far better than the miter spikes the old quads produced. Every strip is
// clipped to its item bounds (the cluster canvas is the union of those).
// Texture axes follow the preview (emit_clipped_textured_quad(u=0..1 across, v=along)): tile x = across, tile row =
// (1 - t) * height (GPU v up, same convention as trail_texel). flip bit0 mirrors across, bit1 mirrors along.
// Each segment is rasterised to a small tinted image (<= 64 px per side).
// Allocation: segment images and thread_local scratch; only runs while a cluster / pixmap is being built.
QImage strip_segment_image(const QImage& tile, float t0, float t1, int iw, int ih, uint8_t blend, float r, float g,
                           float b, float a) {
  QImage out;
  if (tile.width() == 1 && tile.height() == 1) {
    out = QImage(iw, ih, QImage::Format_ARGB32_Premultiplied);
    out.fill(qPremultiply(tile.pixel(0, 0)));
  } else {
    const int th = tile.height();
    const float y0 = (1.0f - std::clamp(t0, 0.0f, 1.0f)) * static_cast<float>(th);
    const float y1 = (1.0f - std::clamp(t1, 0.0f, 1.0f)) * static_cast<float>(th);
    int lo = std::clamp(static_cast<int>(std::floor(std::min(y0, y1))), 0, th - 1);
    int hi = std::clamp(static_cast<int>(std::ceil(std::max(y0, y1))), lo + 1, th);
    QImage crop = tile.copy(0, lo, tile.width(), hi - lo).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (y0 > y1) {  // row 0 must be the segment's start vertex
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
      crop = crop.flipped(Qt::Vertical);
#else
      crop = crop.mirrored(false, true);
#endif
    }
    out = crop.scaled(iw, ih, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (out.format() != QImage::Format_ARGB32_Premultiplied) {
      out = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
  }
  tint_image(out, blend, r, g, b, a);
  return out;
}

void draw_strip(QPainter& p, const QTransform& base, const Layout& lay, const Item& it) {
  const std::size_t n = it.strip.size();
  if (n < 2) return;
  const SplitSnapParticle& sp = *it.sp;
  const QImage tile = flip_image(source_tile(sp.uv, sp.blend, kStrip), sp.flip);
  if (tile.isNull()) return;

  thread_local std::vector<QPointF> pos;
  pos.resize(n);
  for (std::size_t k = 0; k < n; ++k) {
    pos[k] = QPointF(static_cast<qreal>(it.strip[k].x), static_cast<qreal>(lay.y_of_strip(it, it.strip[k].dyp)));
  }

  p.save();
  p.setWorldTransform(base);
  p.setClipRect(item_bounds(lay, it), Qt::IntersectClip);  // clip is mapped by `base` here, then kept in device space
  const bool overlap = it.alpha_layer();
  const float blend_gain = sp.blend == 2 ? wds::chart_render::kOfficialSplitEffectTrailAdditiveIntensity : 1.0f;
  for (std::size_t k = 0; k + 1 < n; ++k) {
    const StripVert& v0 = it.strip[k];
    const StripVert& v1 = it.strip[k + 1];
    const qreal sdx = pos[k + 1].x() - pos[k].x();
    const qreal sdy = pos[k + 1].y() - pos[k].y();
    const qreal slen = std::sqrt(sdx * sdx + sdy * sdy);
    if (!(slen >= static_cast<qreal>(kStripMinSegPx))) continue;
    const float wavg = 0.5f * (v0.wpx + v1.wpx);
    if (wavg < 0.02f) continue;
    const float sa = 0.5f * (v0.a + v1.a);
    if (sa * blend_gain < 0.0005f) continue;

    const qreal len = slen + ((overlap && k + 2 < n) ? static_cast<qreal>(kStripOverlapPx) : 0.0);
    const int iw = std::clamp(static_cast<int>(std::ceil(wavg)), 1, kMaxStripSegPx);
    const int ih = std::clamp(static_cast<int>(std::ceil(len)), 1, kMaxStripSegPx);
    const QImage seg = strip_segment_image(tile, v0.t, v1.t, iw, ih, sp.blend, 0.5f * (v0.r + v1.r),
                                           0.5f * (v0.g + v1.g), 0.5f * (v0.b + v1.b), sa);
    if (seg.isNull()) continue;

    // d = travel direction, nrm = its left side (Qt y-down). Image x runs left -> right across, image y along.
    const qreal dxn = sdx / slen;
    const qreal dyn = sdy / slen;
    const qreal nx = dyn;
    const qreal ny = -dxn;
    const qreal w = static_cast<qreal>(wavg);
    const QPointF ex(-nx * w / iw, -ny * w / iw);
    const QPointF ey(dxn * len / ih, dyn * len / ih);
    const QPointF origin(pos[k].x() + nx * 0.5 * w, pos[k].y() + ny * 0.5 * w);
    const QTransform m(ex.x(), ex.y(), ey.x(), ey.y(), origin.x(), origin.y());
    p.setWorldTransform(m * base);
    p.drawImage(QPointF(0.0, 0.0), seg);
  }
  p.restore();
}

void draw_item(QPainter& p, const QTransform& base, const Layout& lay, const Item& it) {
  p.setCompositionMode(it.alpha_layer() ? QPainter::CompositionMode_SourceOver : QPainter::CompositionMode_Plus);
  if (it.shape == kStrip) {
    draw_strip(p, base, lay, it);
    return;
  }
  QTransform t = base;
  t.translate(static_cast<qreal>(it.x), static_cast<qreal>(lay.y_of(it.dyp)));
  t.rotate(-static_cast<qreal>(it.sp->angle_deg));  // CCW in y-up == negative in Qt y-down
  p.setWorldTransform(t);
  if (it.shape == kButterfly) {
    // Wing sprite on the right of the spine, mirrored copy on the left (preview flap_wing x_tip +/-).
    const QRectF wing(0.0, -it.hpx * 0.5, it.wpx * 0.5, it.hpx);
    p.drawImage(wing, it.img);
    p.scale(-1.0, 1.0);
    p.drawImage(wing, it.img);
  } else {
    p.drawImage(QRectF(-it.wpx * 0.5, -it.hpx * 0.5, it.wpx, it.hpx), it.img);
  }
}

// Line edge (0..12) of official line i, as the picker / editor place them.
int32_t split_line_edge(int32_t i, int32_t n, const std::vector<int32_t>& mids) {
  constexpr int32_t kLanes = 12;
  if (i <= 0) return 0;
  if (i >= n) return kLanes;
  return static_cast<std::size_t>(i - 1) < mids.size()
             ? mids[static_cast<std::size_t>(i - 1)] + 1
             : static_cast<int32_t>(std::lround(static_cast<float>(i * kLanes) / static_cast<float>(n)));
}

QHash<QByteArray, QPixmap>& pixmap_cache() {
  static QHash<QByteArray, QPixmap> cache;
  return cache;
}

struct PixmapKeyPod {
  int32_t color_id, split_count, type, w, h;
  float min_sprite_px;
  float size_scale;
};

QByteArray pixmap_cache_key(const SplitIndicatorPixmapKey& key) {
  PixmapKeyPod k{};
  k.color_id = key.color_id;
  k.split_count = key.split_count;
  k.type = static_cast<int32_t>(key.type);
  k.w = key.size.width();
  k.h = key.size.height();
  k.min_sprite_px = key.min_sprite_px;
  k.size_scale = key.size_scale;
  return pod_key(k);
}

// Horizontal inset of the 12-lane layout. Must match indicator_h_inset in
// split_picker_dialog.cpp.
static float indicator_h_inset(float line_w, float plate_w) {
  return std::max(line_w * 0.5f + 2.0f, 0.06f * plate_w);
}

QPixmap render_indicator_pixmap(const SplitIndicatorPixmapKey& key) {
  const QSize box(std::max(1, key.size.width()), std::max(1, key.size.height()));
  QImage img(box, QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);
  const int32_t n = std::max(1, key.split_count);
  constexpr int32_t kLanes = 12;
  // Same geometry as split_picker_dialog.cpp split_preview_pixmap.
  std::vector<int32_t> mids;
  split_boundaries_12(n, mids);
  const float line_w =
      std::clamp(static_cast<float>(box.width()) / static_cast<float>(kLanes) * 0.14f, 1.5f, 2.5f);
  const float pad = indicator_h_inset(line_w, static_cast<float>(box.width()));
  const float inner_w = std::max(1.0f, static_cast<float>(box.width()) - pad * 2.0f);
  std::vector<float> line_x(static_cast<std::size_t>(n) + 1);
  for (int32_t i = 0; i <= n; ++i) {
    const int32_t edge = split_line_edge(i, n, mids);
    line_x[static_cast<std::size_t>(i)] =
        std::floor(pad + static_cast<float>(edge) * inner_w / static_cast<float>(kLanes) + 0.5f);
  }
  SplitIndicatorSpec spec;
  spec.color_id = key.color_id;
  spec.split_count = n;
  spec.type = key.type;
  spec.px_per_unit = inner_w / static_cast<float>(kLanes) / kLanePitch;
  spec.min_sprite_px = key.min_sprite_px;
  spec.size_scale = key.size_scale;
  QPainter p(&img);
  paint_split_effect_indicator(p, spec, line_x.data(), static_cast<int>(line_x.size()), 2.0f,
                               static_cast<float>(box.height()) - 2.0f);
  p.end();
  return QPixmap::fromImage(img);
}

void clear_cluster_cache();  // defined with the cluster cache below

}  // namespace

void ensure_split_effect_uv_atlas(const QString& skins_dir) {
  if (uv_attempted()) return;
  uv_attempted() = true;

  QStringList dirs;
  const auto add_dir = [&](QString dir) {
    dir = QDir::cleanPath(dir);
    if (dir.isEmpty() || dirs.contains(dir)) return;
    dirs.push_back(dir);
  };
  add_dir(skins_dir);
  const QString app_dir = QCoreApplication::applicationDirPath();
  add_dir(QDir(app_dir).filePath(QStringLiteral("skins")));
  add_dir(QDir(app_dir).filePath(QStringLiteral("../Resources/skins")));
#ifdef WDS_REPO_ROOT
  add_dir(QDir(QString::fromUtf8(WDS_REPO_ROOT)).filePath(QStringLiteral("skins")));
#endif

  const QString name =
      QString::fromUtf8(wds::chart_render::official_split_effect_uv_atlas_relpath());
  const QString trail_name =
      QString::fromUtf8(wds::chart_render::official_split_effect_trail_tex_relpath());
  QImage atlas;
  QImage trail;
  QRgb trail_texel = qRgba(255, 255, 255, 255);
  bool trail_loaded = false;
  for (const QString& dir : dirs) {
    const QString path = QDir(dir).filePath(name);
    if (!QFileInfo::exists(path)) continue;
    QImage im(path);
    if (!im.isNull()) {
      atlas = im.convertToFormat(QImage::Format_ARGB32);
      break;
    }
  }
  for (const QString& dir : dirs) {
    const QString path = QDir(dir).filePath(trail_name);
    if (!QFileInfo::exists(path)) continue;
    QImage im(path);
    if (im.isNull()) continue;
    im = im.convertToFormat(QImage::Format_ARGB32);
    trail_texel = im.pixel(0, im.height() - 1);
    trail_loaded = true;
    trail = im;
    break;
  }
  // Publish once; any thread picks it up on its next thread_caches() call (see SharedAtlas).
  {
    SharedAtlas& s = shared_atlas();
    std::lock_guard<std::mutex> lock(s.m);
    s.atlas = std::move(atlas);
    s.trail = std::move(trail);
    s.trail_texel = trail_texel;
    s.trail_loaded = trail_loaded;
    s.gen.fetch_add(1, std::memory_order_release);
  }
  // Anything built through the synchronous APIs before the atlas existed baked placeholder / missing sprites and must
  // not stay cached as final (the atlas is published exactly once, so this runs once). GUI thread, no pointers held.
  pixmap_cache().clear();
  clear_cluster_cache();
}

namespace {

BuildParams build_params(const SplitIndicatorSpec& spec) {
  BuildParams bp;
  bp.color_id = spec.color_id;
  bp.split_count = spec.split_count;
  bp.type = spec.type;
  bp.ppu = spec.px_per_unit;
  bp.size_scale = spec.size_scale > 0.0f ? spec.size_scale : 1.0f;
  bp.min_sprite_px = spec.min_sprite_px;
  bp.max_rank = spec.max_rank;
  return bp;
}

void set_item_hints(QPainter& p) {
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.setRenderHint(QPainter::Antialiasing, true);
}

// ---- cluster cache ----------------------------------------------------------------------------------------------
struct ClusterEntry {
  QByteArray key;
  SplitIndicatorCluster cluster;
  qint64 bytes = 0;
};

struct ClusterCache {
  std::list<ClusterEntry> lru;  // front = most recently used; std::list keeps element addresses stable
  QHash<QByteArray, std::list<ClusterEntry>::iterator> map;
  qint64 bytes = 0;
};

ClusterCache& cluster_cache() {
  static ClusterCache cache;
  return cache;
}

void clear_cluster_cache() {
  ClusterCache& cc = cluster_cache();
  cc.map.clear();
  cc.lru.clear();
  cc.bytes = 0;
}

struct ClusterKeyPod {
  int32_t color_id, split_count, type, lane_q4, band_h, scale_q64, min_q16, max_rank;
};

ClusterKeyPod cluster_key_pod(const SplitIndicatorClusterKey& key) {
  ClusterKeyPod k{};
  k.color_id = key.color_id;
  k.split_count = std::max(1, key.split_count);
  k.type = static_cast<int32_t>(key.type);
  k.lane_q4 = std::max(1, static_cast<int32_t>(std::lround(key.lane_px * 4.0f)));  // 0.25 px steps
  k.band_h = std::max(1, key.band_h);
  k.scale_q64 = static_cast<int32_t>(std::lround((key.size_scale > 0.0f ? key.size_scale : 1.0f) * 64.0f));
  k.min_q16 = static_cast<int32_t>(std::lround(std::max(0.0f, key.min_sprite_px) * 16.0f));
  k.max_rank = std::clamp(key.max_rank, 0, 255);
  return k;
}

// Tight alpha bounding box of a premultiplied image (empty rect = nothing).
QRect alpha_bbox(const QImage& img) {
  int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
  for (int y = 0; y < img.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    int first = -1, last = -1;
    for (int x = 0; x < img.width(); ++x) {
      if (qAlpha(row[x]) != 0) {
        if (first < 0) first = x;
        last = x;
      }
    }
    if (first < 0) continue;
    x0 = std::min(x0, first);
    x1 = std::max(x1, last);
    y0 = std::min(y0, y);
    y1 = y;
  }
  if (x1 < 0) return QRect();
  return QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

// Applies one subtractive item (see is_subtractive) at its draw-order position. The preview does
// dst.rgb += tex.rgb * tint * a with tint < 0 on a float target, so the item removes `m = sprite.rgb * |tint| * a`
// (the premultiplied rgb of its |rgb|-tinted sprite) from everything drawn before it, and the display clamps at 0:
//   dst.rgb = max(0, dst.rgb - m).
// QPainter has no subtract mode, so the item is rasterised into a scratch image (same transform as draw_item, Plus so
// overlapping wings subtract twice like two quads) and the layers are processed per pixel over its bounding box only.
// Per channel the amount is taken from the alpha layer first (a premultiplied rgb: alpha stays, so the validity
// alpha >= max(rgb) is kept), then from the add layer (alpha is only coverage there, so it is re-derived as
// max(rgb) for the touched pixels, which keeps the tight bbox honest and the layer valid). Whatever neither layer
// could absorb is the residual: it must come out of what lies UNDER the cluster, and is folded into `trans` (opaque
// RGB, scene transmittance, 255 = untouched) as trans *= (1 - residual). Null layers are skipped.
void apply_subtractive(const Layout& lay, const Item& it, int cx, int cy, int cw, int ch, QImage& alpha_layer,
                       QImage& add_layer, QImage& trans) {
  const QRectF b = item_bounds(lay, it);
  const int bx = static_cast<int>(std::floor(b.left())) - cx;
  const int by = static_cast<int>(std::floor(b.top())) - cy;
  const int bw = static_cast<int>(std::ceil(b.right())) - cx - bx;
  const int bh = static_cast<int>(std::ceil(b.bottom())) - cy - by;
  const QRect r = QRect(bx, by, bw, bh).intersected(QRect(0, 0, cw, ch));
  if (r.isEmpty()) return;

  QImage mask(r.size(), QImage::Format_ARGB32_Premultiplied);
  mask.fill(Qt::transparent);
  {
    QPainter mp(&mask);
    set_item_hints(mp);
    const QTransform base =
        QTransform::fromTranslate(-static_cast<qreal>(cx + r.x()), -static_cast<qreal>(cy + r.y()));
    draw_item(mp, base, lay, it);
    mp.end();
  }

  // Subtract min(m, v) from v (v: one 8-bit channel), return the part of m that was not absorbed.
  const auto take = [](int m, int& v) {
    const int c = std::min(m, v);
    v -= c;
    return m - c;
  };
  for (int y = 0; y < r.height(); ++y) {
    const auto* mrow = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
    const int py = r.y() + y;
    QRgb* arow = alpha_layer.isNull() ? nullptr : reinterpret_cast<QRgb*>(alpha_layer.scanLine(py));
    QRgb* drow = add_layer.isNull() ? nullptr : reinterpret_cast<QRgb*>(add_layer.scanLine(py));
    QRgb* trow = reinterpret_cast<QRgb*>(trans.scanLine(py));
    for (int x = 0; x < r.width(); ++x) {
      const QRgb mp = mrow[x];
      int m[3] = {qRed(mp), qGreen(mp), qBlue(mp)};
      if (m[0] == 0 && m[1] == 0 && m[2] == 0) continue;
      const int px = r.x() + x;
      if (arow != nullptr) {
        const QRgb c = arow[px];
        int v[3] = {qRed(c), qGreen(c), qBlue(c)};
        for (int k = 0; k < 3; ++k) m[k] = take(m[k], v[k]);
        arow[px] = qRgba(v[0], v[1], v[2], qAlpha(c));
      }
      if (drow != nullptr) {
        const QRgb c = drow[px];
        int v[3] = {qRed(c), qGreen(c), qBlue(c)};
        for (int k = 0; k < 3; ++k) m[k] = take(m[k], v[k]);
        drow[px] = qRgba(v[0], v[1], v[2], std::max(v[0], std::max(v[1], v[2])));
      }
      if (m[0] != 0 || m[1] != 0 || m[2] != 0) {
        const QRgb t = trow[px];
        const auto mul = [](int tc, int res) { return (tc * (255 - res) + 127) / 255; };
        trow[px] = qRgb(mul(qRed(t), m[0]), mul(qGreen(t), m[1]), mul(qBlue(t), m[2]));
      }
    }
  }
}

// The layout of a cluster key (lines at their lane edges, band [0, band_h]); shared by the build and the diagnostics.
void cluster_layout(const SplitIndicatorClusterKey& key, const ClusterKeyPod& kp, Layout& lay) {
  const int32_t n = kp.split_count;
  const float lane = static_cast<float>(kp.lane_q4) * 0.25f;
  std::vector<int32_t> mids;
  split_boundaries_12(n, mids);
  std::vector<float> line_x(static_cast<std::size_t>(n) + 1);
  for (int32_t i = 0; i <= n; ++i) {
    line_x[static_cast<std::size_t>(i)] = static_cast<float>(split_line_edge(i, n, mids)) * lane;
  }

  BuildParams bp;
  bp.color_id = key.color_id;
  bp.split_count = n;
  bp.type = key.type;
  bp.ppu = lane / kLanePitch;
  bp.size_scale = static_cast<float>(kp.scale_q64) / 64.0f;
  bp.min_sprite_px = static_cast<float>(kp.min_q16) / 16.0f;
  bp.max_rank = kp.max_rank;
  build_layout(bp, line_x.data(), static_cast<int>(line_x.size()), 0.0f, static_cast<float>(kp.band_h), true, lay);
}

SplitIndicatorCluster build_cluster(const SplitIndicatorClusterKey& key, const ClusterKeyPod& kp) {
  SplitIndicatorCluster out;
  Layout lay;
  cluster_layout(key, kp, lay);
  if (lay.items.empty()) return out;

  // Canvas = union of the conservative item bounds.
  QRectF u;
  for (const Item& it : lay.items) u = u.isNull() ? item_bounds(lay, it) : u.united(item_bounds(lay, it));
  const int cx = static_cast<int>(std::floor(u.left()));
  const int cy = static_cast<int>(std::floor(u.top()));
  const int cw = std::clamp(static_cast<int>(std::ceil(u.right())) - cx, 1, kMaxClusterSidePx);
  const int ch = std::clamp(static_cast<int>(std::ceil(u.bottom())) - cy, 1, kMaxClusterSidePx);

  // Items are applied in baked (= preview draw) order. Normal items are painted into their layer; a subtractive item
  // is applied to what the layers hold at that point (apply_subtractive) and accumulates its residual into `trans`.
  QImage layers[2];  // 0 alpha, 1 add
  QImage trans;      // scene transmittance of the residual subtraction, opaque RGB, 255 = scene untouched
  bool used[2] = {false, false};
  bool has_sub = false;
  for (const Item& it : lay.items) {
    if (it.subtractive) {
      has_sub = true;
    } else {
      used[it.alpha_layer() ? 0 : 1] = true;
    }
  }
  for (int li = 0; li < 2; ++li) {
    if (!used[li]) continue;
    layers[li] = QImage(cw, ch, QImage::Format_ARGB32_Premultiplied);
    layers[li].fill(Qt::transparent);
  }
  if (has_sub) {
    trans = QImage(cw, ch, QImage::Format_RGB32);
    trans.fill(0xFFFFFFFFu);
  }
  const QTransform base = QTransform::fromTranslate(-static_cast<qreal>(cx), -static_cast<qreal>(cy));
  std::unique_ptr<QPainter> painters[2];
  const auto open_painters = [&]() {
    for (int li = 0; li < 2; ++li) {
      if (layers[li].isNull() || painters[li]) continue;
      painters[li] = std::make_unique<QPainter>(&layers[li]);
      set_item_hints(*painters[li]);
    }
  };
  // The per-pixel pass must not run while a QPainter is active on the same image.
  const auto close_painters = [&]() {
    for (auto& pp : painters) {
      if (pp) pp->end();
      pp.reset();
    }
  };
  for (const Item& it : lay.items) {
    if (it.subtractive) {
      close_painters();
      apply_subtractive(lay, it, cx, cy, cw, ch, layers[0], layers[1], trans);
    } else {
      open_painters();
      draw_item(*painters[it.alpha_layer() ? 0 : 1], base, lay, it);
    }
  }
  close_painters();

  // The residual (what the cluster's own content could not absorb) becomes the sub layer, in the form the draw's
  // CompositionMode_Multiply needs: premultiplied (a - m_c, a) with m = 1 - transmittance and a = max(m_c), which
  // multiplies an opaque destination by exactly (1 - m_c) per channel.
  QImage sub;
  if (has_sub) {
    sub = QImage(cw, ch, QImage::Format_ARGB32_Premultiplied);
    bool any = false;
    for (int y = 0; y < ch; ++y) {
      const auto* t = reinterpret_cast<const QRgb*>(trans.constScanLine(y));
      auto* d = reinterpret_cast<QRgb*>(sub.scanLine(y));
      for (int x = 0; x < cw; ++x) {
        const int mr = 255 - qRed(t[x]);
        const int mg = 255 - qGreen(t[x]);
        const int mb = 255 - qBlue(t[x]);
        const int a = std::max(mr, std::max(mg, mb));
        d[x] = qRgba(a - mr, a - mg, a - mb, a);
        any = any || a != 0;
      }
    }
    if (!any) sub = QImage();
  }

  // Crop all layers to the union of their tight boxes (one shared offset).
  QImage* all[3] = {&layers[0], &layers[1], &sub};
  QRect tight;
  for (QImage* img : all) {
    if (img->isNull()) continue;
    const QRect r = alpha_bbox(*img);
    if (r.isNull()) {
      *img = QImage();
      continue;
    }
    tight = tight.isNull() ? r : tight.united(r);
  }
  if (tight.isNull()) return out;
  out.alpha_layer = layers[0].isNull() ? QImage() : layers[0].copy(tight);
  out.add_layer = layers[1].isNull() ? QImage() : layers[1].copy(tight);
  out.sub_layer = sub.isNull() ? QImage() : sub.copy(tight);
  out.offset = QPoint(cx + tight.x(), cy + tight.y());
  out.empty = out.alpha_layer.isNull() && out.add_layer.isNull() && out.sub_layer.isNull();
  return out;
}

// Inserts at the LRU front and evicts from the back (never the entry just inserted). GUI thread only. If the id is
// already cached (a synchronous build raced a worker result) the existing entry is touched and kept.
const SplitIndicatorCluster* cluster_cache_insert(const QByteArray& id, SplitIndicatorCluster&& cluster) {
  ClusterCache& cc = cluster_cache();
  const auto hit = cc.map.constFind(id);
  if (hit != cc.map.cend()) {
    cc.lru.splice(cc.lru.begin(), cc.lru, hit.value());
    return &cc.lru.front().cluster;
  }
  ClusterEntry e;
  e.key = id;
  e.cluster = std::move(cluster);
  e.bytes = e.cluster.alpha_layer.sizeInBytes() + e.cluster.add_layer.sizeInBytes() +
            e.cluster.sub_layer.sizeInBytes();
  cc.lru.push_front(std::move(e));
  cc.map.insert(cc.lru.front().key, cc.lru.begin());
  cc.bytes += cc.lru.front().bytes;
  while (cc.bytes > kClusterCacheBytes && cc.lru.size() > 1) {
    ClusterEntry& victim = cc.lru.back();
    cc.bytes -= victim.bytes;
    cc.map.remove(victim.key);
    cc.lru.pop_back();
  }
  return &cc.lru.front().cluster;
}

// ---- background cluster builder -----------------------------------------------------------------------------------
// One worker thread builds clusters requested through split_effect_indicator_cluster_request().
//  * The worker only reads: the baked tables (constexpr), the catalog (constexpr), its own thread_local
//    ThreadCaches (sprites, tiles, copies of the atlas QImages) and the job key it was handed BY VALUE. It never
//    touches cluster_cache(), pixmap_cache() or any QWidget/QPixmap.
//  * Results travel back in `done_` (mutex-protected) and are applied on the GUI thread by drain(), which is the only
//    place that inserts/evicts the LRU and the only place that reads/writes `waiters_`.
//  * Jobs are LIFO (back of `queue_` = most recently requested).
constexpr std::size_t kMaxQueuedBuilds = 256;

struct QueuedBuild {
  QByteArray id;
  SplitIndicatorClusterKey key;  // by value: the caller's key may be a temporary
};

struct BuildResult {
  QByteArray id;
  SplitIndicatorCluster cluster;
  // false: the build threw (allocation failure). drain() caches an EMPTY cluster for the key so a paint that
  // re-requests it every frame (editor during playback) cannot keep the worker rebuilding a failing key forever.
  bool ok = false;
};

struct Waiter {
  QPointer<QObject> notify;
  std::function<void()> fn;
};

// GUI thread. Queues every live waiter's callback onto its notify object (Qt drops the call if that object is
// deleted before delivery).
void deliver_waiters(std::vector<Waiter>& ws) {
  for (Waiter& w : ws) {
    if (w.notify.isNull() || !w.fn) continue;
    QMetaObject::invokeMethod(w.notify.data(), std::move(w.fn), Qt::QueuedConnection);
  }
}

class ClusterWorker {
 public:
  static ClusterWorker& instance() {
    static ClusterWorker w;
    return w;
  }

  ~ClusterWorker() {
    // Static destruction normally runs after the QCoreApplication is gone: touch the connections only while the
    // application object we hooked is still alive (QPointer is null once it was destroyed).
    if (!hooked_app_.isNull()) {
      for (const QMetaObject::Connection& c : hooks_) QObject::disconnect(c);
    }
    shutdown();
  }

  // GUI thread only.
  const SplitIndicatorCluster* request(const SplitIndicatorClusterKey& key, QObject* notify,
                                       std::function<void()> on_ready) {
    QCoreApplication* app = QCoreApplication::instance();
    // Without the atlas published the build would bake white placeholders: refuse (callers re-request on repaint).
    if (app == nullptr || !uv_attempted()) return nullptr;
    attach(app);

    const QByteArray id = pod_key(cluster_key_pod(key));
    auto it = waiters_.find(id);
    const bool inflight = it != waiters_.end();
    if (!inflight) it = waiters_.insert(id, std::vector<Waiter>());
    if (notify != nullptr && on_ready) {
      std::vector<Waiter>& ws = it.value();
      // One callback per (key, notify): paint re-requests every frame and must not pile callbacks up.
      const bool dup = std::any_of(ws.begin(), ws.end(), [&](const Waiter& w) { return w.notify.data() == notify; });
      if (!dup) ws.push_back(Waiter{QPointer<QObject>(notify), std::move(on_ready)});
    }

    std::vector<QByteArray> dropped;
    bool refused = false;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (!accepting_) {
        refused = true;
      } else if (!inflight) {
        try {
          if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
          queue_.push_back(QueuedBuild{id, key});
          while (queue_.size() > kMaxQueuedBuilds) {  // drop the oldest requests
            dropped.push_back(std::move(queue_.front().id));
            queue_.pop_front();
          }
        } catch (const std::exception&) {  // thread creation / allocation failure
          refused = true;
        }
      } else if (!queue_.empty() && queue_.back().id != id) {
        // Re-requested while queued: it is on screen now, so build it first (LIFO). If the worker already took it,
        // it is not in the queue and nothing happens.
        const auto qit = std::find_if(queue_.rbegin(), queue_.rend(), [&](const QueuedBuild& q) { return q.id == id; });
        if (qit != queue_.rend()) {
          QueuedBuild q = std::move(*qit);
          queue_.erase(std::next(qit).base());
          queue_.push_back(std::move(q));
        }
      }
    }
    if (refused) {
      if (!inflight) waiters_.remove(id);
      return nullptr;
    }
    // A dropped key will never be built and never calls back: wake its waiters now so the widget repaints and
    // re-requests it (otherwise a visible thumb whose queued request was pushed out by a prefetch flood would stay
    // blank until something else repaints it). Bounded: only a request stream of > kMaxQueuedBuilds distinct keys
    // can keep dropping, and every repaint re-requests only what is visible.
    for (const QByteArray& d : dropped) {
      const auto dit = waiters_.find(d);
      if (dit == waiters_.end()) continue;
      std::vector<Waiter> ws = std::move(dit.value());
      waiters_.erase(dit);
      deliver_waiters(ws);
    }
    cv_.notify_one();
    return nullptr;
  }

  // Stops accepting, wakes and joins the worker (a running build finishes first, <= ~30 ms). Safe to call twice and
  // from any thread that is not the worker.
  void shutdown() {
    std::thread t;
    {
      std::lock_guard<std::mutex> lk(m_);
      accepting_ = false;
      stop_ = true;
      queue_.clear();
      t = std::move(thread_);
    }
    cv_.notify_all();
    if (t.joinable()) t.join();
  }

 private:
  ClusterWorker() { (void)shared_atlas(); }  // construct the atlas first => it is destroyed after the worker

  // GUI thread, first request under a given application object. Normal exit: aboutToQuit fires from exec() while the
  // app object is alive. An app torn down without an event loop (tests) is covered by a Qt post routine, which runs
  // at the very start of ~QCoreApplication (app object still intact, so the worker cannot be mid-post to a dying
  // object; the QObject::destroyed signal would fire too late for that). Post routines are one-shot, hence
  // re-registered per application. After either, request() returns nullptr until a NEW application object appears:
  // then (tests that create a QGuiApplication repeatedly, possibly at the same address) the worker is re-armed.
  void attach(QCoreApplication* app) {
    if (hooked_app_.data() == app) return;
    shutdown();  // no-op when the previous app's post routine already ran; joins otherwise
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = false;
      accepting_ = true;
      drain_posted_ = false;
      queue_.clear();
      done_.clear();
    }
    waiters_.clear();  // callbacks of the previous app are meaningless, and stale entries would mask "in flight"
    hooks_.clear();    // handles of the old app's connections (freed with it)
    hooked_app_ = app;
    hooks_.push_back(QObject::connect(app, &QCoreApplication::aboutToQuit, [this] { shutdown(); }));
    qAddPostRoutine(&ClusterWorker::post_routine);
  }

  static void post_routine() { instance().shutdown(); }

  void run() {
    for (;;) {
      QueuedBuild job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        job = std::move(queue_.back());  // newest request first
        queue_.pop_back();
      }
      BuildResult r;
      r.id = std::move(job.id);
      try {
        r.cluster = build_cluster(job.key, cluster_key_pod(job.key));
        r.ok = true;
      } catch (...) {
        r.ok = false;
      }
      publish(std::move(r));
    }
  }

  // Worker thread. The post happens under m_ so shutdown() (which takes m_) cannot return while a post to the
  // application object is in flight.
  void publish(BuildResult&& r) {
    std::lock_guard<std::mutex> lk(m_);
    if (stop_ || !accepting_) return;
    done_.push_back(std::move(r));
    if (drain_posted_) return;  // one drain event serves every result finished before it runs
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) return;
    drain_posted_ = true;
    QMetaObject::invokeMethod(app, [this] { drain(); }, Qt::QueuedConnection);
  }

  // GUI thread (queued onto the application object).
  void drain() {
    std::vector<BuildResult> batch;
    {
      std::lock_guard<std::mutex> lk(m_);
      batch.swap(done_);
      drain_posted_ = false;
    }
    for (BuildResult& r : batch) {
      std::vector<Waiter> ws;
      const auto wit = waiters_.find(r.id);
      if (wit != waiters_.end()) {
        ws = std::move(wit.value());
        waiters_.erase(wit);
      }
      if (!r.ok) r.cluster = SplitIndicatorCluster();  // negative entry (empty=true), see BuildResult::ok
      cluster_cache_insert(r.id, std::move(r.cluster));
      deliver_waiters(ws);
    }
  }

  std::mutex m_;  // guards queue_, done_, stop_, accepting_, drain_posted_, thread_
  std::condition_variable cv_;
  std::deque<QueuedBuild> queue_;
  std::vector<BuildResult> done_;
  bool stop_ = false;
  bool accepting_ = true;
  bool drain_posted_ = false;
  std::thread thread_;
  // GUI thread only:
  QPointer<QCoreApplication> hooked_app_;  // null once that application object was destroyed
  std::vector<QMetaObject::Connection> hooks_;
  QHash<QByteArray, std::vector<Waiter>> waiters_;  // keys in flight (queued or building) and their callbacks
};

}  // namespace

void paint_split_effect_indicator(QPainter& p, const SplitIndicatorSpec& spec, const float* line_x,
                                  int line_count, float band_top, float band_bottom, float opacity) {
  if (line_x == nullptr || line_count < 1 || opacity <= 0.0f || spec.px_per_unit <= 0.0f) return;
  Layout lay;
  build_layout(build_params(spec), line_x, line_count, band_top, band_bottom, false, lay);
  if (lay.items.empty()) return;

  p.save();
  set_item_hints(p);
  p.setOpacity(p.opacity() * static_cast<qreal>(std::min(opacity, 1.0f)));
  const QTransform base = p.worldTransform();
  for (const Item& it : lay.items) draw_item(p, base, lay, it);
  p.restore();
}

const SplitIndicatorCluster* split_effect_indicator_cluster_if_ready(const SplitIndicatorClusterKey& key) {
  ClusterCache& cc = cluster_cache();
  const QByteArray id = pod_key(cluster_key_pod(key));
  const auto it = cc.map.constFind(id);
  if (it == cc.map.cend()) return nullptr;
  cc.lru.splice(cc.lru.begin(), cc.lru, it.value());  // iterators/addresses stay valid
  return &cc.lru.front().cluster;
}

const SplitIndicatorCluster* split_effect_indicator_cluster(const SplitIndicatorClusterKey& key) {
  if (const SplitIndicatorCluster* hit = split_effect_indicator_cluster_if_ready(key)) return hit;
  const ClusterKeyPod kp = cluster_key_pod(key);
  return cluster_cache_insert(pod_key(kp), build_cluster(key, kp));  // GUI-thread state only
}

const SplitIndicatorCluster* split_effect_indicator_cluster_request(const SplitIndicatorClusterKey& key,
                                                                    QObject* notify,
                                                                    std::function<void()> on_ready) {
  if (const SplitIndicatorCluster* hit = split_effect_indicator_cluster_if_ready(key)) return hit;
  return ClusterWorker::instance().request(key, notify, std::move(on_ready));
}

std::vector<SplitIndicatorDebugStrip> split_effect_indicator_debug_strips(const SplitIndicatorClusterKey& key) {
  std::vector<SplitIndicatorDebugStrip> out;
  const ClusterKeyPod kp = cluster_key_pod(key);
  Layout lay;
  cluster_layout(key, kp, lay);
  const float ppu = static_cast<float>(kp.lane_q4) * 0.25f / kLanePitch;
  for (const Item& it : lay.items) {
    if (it.shape != kStrip) continue;
    SplitIndicatorDebugStrip d;
    d.line = it.line;
    d.line_x = it.line_x;
    d.px_per_unit = ppu;
    for (const StripVert& v : it.strip) {
      d.x.push_back(v.x);
      d.y.push_back(lay.y_of_strip(it, v.dyp));
      d.width.push_back(v.wpx);
    }
    out.push_back(std::move(d));
  }
  return out;
}

void draw_split_effect_indicator_cluster(QPainter& p, const SplitIndicatorCluster& c, QPointF edge0_band_top,
                                         float opacity) {
  if (c.empty || opacity <= 0.0f) return;
  p.save();
  p.setOpacity(p.opacity() * static_cast<qreal>(std::min(opacity, 1.0f)));
  p.setRenderHint(QPainter::SmoothPixmapTransform, false);
  const QPointF o(std::round(edge0_band_top.x()) + c.offset.x(), std::round(edge0_band_top.y()) + c.offset.y());
  if (!c.sub_layer.isNull()) {
    // Residual subtraction of the cut-out items: scene *= (1 - mask) (never brightens), before any light is added.
    p.setCompositionMode(QPainter::CompositionMode_Multiply);
    p.drawImage(o, c.sub_layer);
  }
  if (!c.alpha_layer.isNull()) {
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(o, c.alpha_layer);
  }
  if (!c.add_layer.isNull()) {
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.drawImage(o, c.add_layer);
  }
  p.restore();
}

QPixmap split_effect_indicator_pixmap(const SplitIndicatorPixmapKey& key) {
  auto& cache = pixmap_cache();
  const QByteArray id = pixmap_cache_key(key);
  const auto it = cache.constFind(id);
  if (it != cache.cend()) return it.value();
  QPixmap pm = render_indicator_pixmap(key);
  if (cache.size() >= static_cast<qsizetype>(kMaxPixmaps)) cache.clear();
  cache.insert(id, pm);
  return pm;
}

QPixmap split_effect_indicator_pixmap_if_ready(const SplitIndicatorPixmapKey& key) {
  const auto it = pixmap_cache().constFind(pixmap_cache_key(key));
  if (it == pixmap_cache().cend()) return {};
  return it.value();
}

int prefetch_split_effect_indicator_pixmaps(const std::vector<int32_t>& ids, SplitIndicatorPixmapKey key,
                                            int max_new) {
  if (max_new <= 0) return 0;
  int built = 0;
  auto& cache = pixmap_cache();
  for (int32_t id : ids) {
    key.color_id = id;
    const QByteArray ck = pixmap_cache_key(key);
    if (cache.contains(ck)) continue;
    cache.insert(ck, render_indicator_pixmap(key));
    if (++built >= max_new) break;
  }
  return built;
}

}  // namespace wds::ui

// split_effect_snapshot_bake: offline generator for ui/src/qt/split_effect_snapshot_table.inc.
//
// Runs the real split-effect simulation (collect_split_effect_particles, no Vulkan) for every
// split colour id the picker lists, samples it mid fade-in (now_t = 0.5 s) and bakes 1-2 static
// sprite particles per particle kind, plus strips (ribbon chains and particle-mode trails, decimated
// to a per-kind vertex budget), into a compact table used by the editor's static split indicators.
//
// Regenerate (from the repo root, after building wds_split_effect_snapshot_bake):
//   build/chart-render/wds_split_effect_snapshot_bake --out-inc ui/src/qt/split_effect_snapshot_table.inc
// Optional:
//   --only ID[,ID...]     bake a subset (table then only contains those ids; do not commit)
//   --debug-tsv FILE      dump every baked entry (P rows, cols 21/22 = strip_first/strip_count), the strip
//                         point pool (Q rows), used atlas tiles (T rows) and the line/slot map (L rows)
//   --analyze             keying evidence + per-kind budgets (representation, vertices kept/total,
//                         max decimation error) and the list of kinds that stay unrepresented
//   --z-fold K            subtract K*z from dy (default 0; K=tan(60deg) mimics the preview's tilt)
//
// Baked frame (see ui/include/wds/ui/qt/split_effect_snapshot_data.hpp): top-down editor frame,
// +x right across lanes, +y farther along the lane (up on screen), world units, relative to the
// owning split line (dx = world_x - official_split_line_world_x, dy = main_y - line origin main_y).
// Sampling is quality-independent: the sim is run with the pinned Normal quality and type Full.
//
// Strips (shape 5): see the generated .inc header and the Strip* helpers below.
//
// Colour semantics: r,g,b,a are the display-referred values the preview feeds to its vertex
// colour (Custom1 brightness included, unclamped). For blend 2 (trail material, "Other") the
// preview multiplies rgb AND alpha by kOfficialSplitEffectTrailAdditiveIntensity (2) at draw
// time; that x2 is NOT baked, the renderer applies it.
//
// Shape conventions (all quads: w = extent along the sprite's U axis, h = extent along V, the
// quad is rotated by angle_deg CCW in the y-up frame about its centre (dx, dy)):
//   0 billboard  w = drawn width after the max-particle-size clamp, h = w * sol_y (sprite aspect
//                included), angle = roll; pivot offsets and tilt are folded into centre / w / h.
//   1 stretched  sprite U runs tail -> head along the velocity: w = stretch length (|v|*velocityScale
//                + appliedLengthScale*size.y), h = across width (Start Size X), angle = direction
//                of the velocity in the (x, main_y) plane. Draw it exactly like a billboard.
//   2 ring       cylinder-wall ring mesh seen from above: ellipse with full diameters w x h, major
//                axis along w, rotated by angle. UV runs around the circumference, so draw it as
//                an ellipse outline (stroke ~ 0.1*min(w,h)) tinted with the sprite colour.
//   3 cube       flat tinted rectangle (white texture), w x h, rotated by angle.
//   4 butterfly  w = full wing span (both wings, flap foreshortening included), h = wing height;
//                draw the sprite on the right half and mirrored on the left half.
// flip bit0 = mirror sprite U, bit1 = mirror sprite V (applied in sprite space, before rotation).

#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/chart_render/split_line_official_colors.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/official_split_x.hpp>
#include <wds/core/preview_snapshot.hpp>
#include <wds/core/split_effect_gate.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using wds::chart_editor::PreviewSplitLaneInstance;
using wds::chart_editor::SplitLaneType;
using wds::chart_render::SplitEffectLiveParticle;

constexpr int64_t kSentinelStartMs = 10'000'000;
constexpr int kQualityNormal = wds::chart_editor::kOfficialSpritEffectNormal;
constexpr float kViewAspect = 16.0f / 9.0f;  // preview content aspect used for the maxP clamp
constexpr float kPi = 3.14159265358979323846f;

// Tier 0 = the canonical fade-in sample, tier 1 = other fade-in / early steady samples named by
// the plan, tier 2 = last-resort samples (reported separately).
struct SampleTime {
  double t;
  int tier;
};
const SampleTime kTimes[] = {
    {0.5, 0},  {0.25, 1}, {0.75, 1}, {1.0, 1},  {1.5, 1},  {0.05, 2}, {0.1, 2},  {0.15, 2}, {0.2, 2},
    {0.3, 2},  {0.35, 2}, {0.4, 2},  {0.45, 2}, {0.55, 2}, {0.6, 2},  {0.65, 2}, {0.7, 2},  {0.8, 2},
    {0.9, 2},  {1.25, 2}, {1.75, 2}, {2.0, 2}};

float g_z_fold = 0.0f;

// One strip vertex as the preview shades it. r,g,b,a are the RAW trail colours (the trail
// material's x2 intensity for blend 2 is NOT baked; the renderer applies it, like for bodies).
struct StripPt {
  float x = 0, y = 0;  // offset from the line origin (dx, dy)
  float w = 0;         // full width
  float u = 0;         // along-trail coordinate (the preview's v_along)
  float r = 1, g = 1, b = 1, a = 1;
  float ay = 0, az = 0;  // absolute Main y / z (frustum clip only, not emitted)
};

StripPt lerp_pt(const StripPt& a, const StripPt& b, float t) {
  StripPt o;
  auto l = [t](float p, float q) { return p + (q - p) * t; };
  o.x = l(a.x, b.x); o.y = l(a.y, b.y); o.w = l(a.w, b.w); o.u = l(a.u, b.u);
  o.r = l(a.r, b.r); o.g = l(a.g, b.g); o.b = l(a.b, b.b); o.a = l(a.a, b.a);
  o.ay = l(a.ay, b.ay); o.az = l(a.az, b.az);
  return o;
}

// Clips the polyline against the preview's visible volume: in front of the near plane and inside
// the vertical NDC range [-kNdcMargin, kNdcMargin]. Each constraint is linear in (main_y, z)
// (cz -/+ f*wy >= 0), so intersections stay on the 3D segment. Keeps the longest inside run.
// Returns false when nothing is visible.
constexpr float kNdcMargin = 1.1f;

bool clip_to_view(std::vector<StripPt>& pts) {
  const float rad = wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
  const float cs = std::cos(rad), sn = std::sin(rad);
  const float f = wds::chart_editor::official_camera_focal();
  auto cz = [&](const StripPt& p) { return wds::chart_editor::official_main_camera_z(p.ay, p.az); };
  auto wy = [&](const StripPt& p) { return p.ay * cs - p.az * sn; };
  using Fn = std::function<float(const StripPt&)>;
  const Fn planes[3] = {
      [&](const StripPt& p) { return cz(p) - wds::chart_editor::kOfficialCameraNearClip; },
      [&](const StripPt& p) { return cz(p) - kNdcMargin * f * wy(p); },
      [&](const StripPt& p) { return cz(p) + kNdcMargin * f * wy(p); }};
  for (const Fn& g : planes) {
    std::vector<std::vector<StripPt>> runs;
    std::vector<StripPt> cur;
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i) {
      const float gi = g(pts[i]);
      if (i > 0) {
        const float gp = g(pts[i - 1]);
        if ((gp >= 0) != (gi >= 0)) {
          const float t = gp / (gp - gi);
          const StripPt m = lerp_pt(pts[i - 1], pts[i], t);
          if (gp >= 0) {  // leaving
            cur.push_back(m);
            runs.push_back(std::move(cur));
            cur.clear();
          } else {  // entering
            cur.push_back(m);
          }
        }
      }
      if (gi >= 0) cur.push_back(pts[i]);
    }
    if (!cur.empty()) runs.push_back(std::move(cur));
    size_t best = 0;
    float best_len = -1;
    for (size_t r = 0; r < runs.size(); ++r) {
      float len = 0;
      for (size_t i = 1; i < runs[r].size(); ++i) {
        len += std::hypot(runs[r][i].x - runs[r][i - 1].x, runs[r][i].y - runs[r][i - 1].y);
      }
      if (len > best_len) {
        best_len = len;
        best = r;
      }
    }
    if (runs.empty() || runs[best].size() < 2) return false;
    pts = std::move(runs[best]);
  }
  return pts.size() >= 2;
}

struct Baked {
  int32_t catalog_index = 0;
  uint16_t uv = 0xFFFF;
  uint8_t blend = 0;
  uint8_t shape = 0;
  uint8_t flip = 0;
  int8_t sorting_order = 0;
  uint8_t rank = 0;  // selection rank within its kind (0,1 = picker picks, 2.. = editor extras)
  float dx = 0, dy = 0, w = 0, h = 0, angle = 0;
  float r = 1, g = 1, b = 1, a = 1;
  // selection metadata (not emitted)
  float birth_t = 0;
  float score = 0;
  int tier = 0;
  double t = 0;
  uint32_t seed = 0;
  // strip entries (shape 5)
  std::vector<StripPt> pts;  // decimated vertices
  int total_pts = 0;         // vertices before decimation (visible run)
  float err_pos = 0;         // max positional decimation error (world units)
  float err_attr = 0;        // max relative width/alpha/colour/U error (fraction of the strip's range)
  uint8_t group = 0;         // draw-order group: 0 particle (body + own trail), 1 ribbon chain
  uint8_t seq = 0;           // 0 body, 1 trail of the same particle
  bool ribbon = false;
  uint32_t strip_first = 0;  // global point pool index (assigned before output)
};

struct KindStats {
  int seen = 0;          // live particles of this kind over all sample times
  int skip_trail_only = 0;
  int skip_dead_alpha = 0;
  int skip_unnamed_other = 0;
  int skip_no_sprite = 0;
  int skip_zero_size = 0;
  int skip_behind_camera = 0;
  int kept = 0;
  int trail_seen = 0;       // particle-mode trails / ribbon runs examined
  int trail_no_width = 0;   // preview draws nothing (all widths ~0)
  int trail_faint = 0;      // no vertex with meaningful alpha/colour
  int trail_short = 0;      // visible part shorter than the minimum span / too narrow
  int trail_offscreen = 0;  // entirely behind the camera / outside the preview's vertical view
  int trail_ok = 0;
};

// ---- strip candidates -----------------------------------------------------------------------

constexpr int kMaxRank = 6;  // sprite picks per kind per slot (rank 0..5)
constexpr int kRibbonCap = 32;
constexpr int kTrailCapWithBody = 12;
constexpr int kTrailCapOnly = 32;
constexpr int kTrailKeepPerKindTier = 6;
constexpr float kTolPos = 0.025f;  // world units (lane pitch 0.925)

struct TrailCand {
  uint32_t catalog_index = 0;
  uint8_t blend = 2;
  int8_t sorting_order = 1;
  uint8_t group = 0;  // 0 particle-mode trail, 1 ribbon chain
  float birth_t = 0;
  uint32_t seed = 0;
  int tier = 0;
  double t = 0;
  float score = 0;
  float peak_vis = 0, peak_w = 0, span = 0;
  std::vector<StripPt> pts;  // visible run, full resolution
};

float blend_k(uint8_t blend) {
  return blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Other)
             ? wds::chart_render::kOfficialSplitEffectTrailAdditiveIntensity
             : 1.0f;
}

// Perceived strength of a vertex: alpha x brightness after the material intensity (Alpha blend:
// brightness does not matter).
float vertex_vis(const StripPt& p, uint8_t blend) {
  const float k = blend_k(blend);
  const float a = std::min(1.0f, std::max(0.0f, p.a) * k);
  if (blend == static_cast<uint8_t>(wds::chart_render::SplitEffectBlend::Alpha)) return a;
  const float lum = std::min(1.5f, std::max(0.05f, std::max({p.r, p.g, p.b}) * k));
  return a * lum;
}

constexpr float kVisVertexVis = 0.02f;
constexpr float kVisVertexWidth = 0.004f;

enum class TrailReject { Ok, NoWidth, Faint, Short, Offscreen };

// Trims `pts` to the visible run (keeping one hidden neighbour for the fade ramp), computes the
// candidate metrics. Returns Ok when the trail has at least the loose minimum presence.
TrailReject eval_trail_clipped(std::vector<StripPt>& pts, uint8_t blend, TrailCand& c);

TrailReject eval_trail(std::vector<StripPt>& pts, uint8_t blend, TrailCand& c) {
  const int n = static_cast<int>(pts.size());
  if (n < 2) return TrailReject::Short;
  bool any_w = false;
  for (const StripPt& p : pts) any_w = any_w || p.w >= 1e-4f;
  if (!any_w) return TrailReject::NoWidth;
  if (!clip_to_view(pts)) return TrailReject::Offscreen;
  return eval_trail_clipped(pts, blend, c);
}

TrailReject eval_trail_clipped(std::vector<StripPt>& pts, uint8_t blend, TrailCand& c) {
  const int n = static_cast<int>(pts.size());
  int first = -1, last = -1;
  for (int i = 0; i < n; ++i) {
    const StripPt& p = pts[static_cast<size_t>(i)];
    if (p.w >= kVisVertexWidth && vertex_vis(p, blend) >= kVisVertexVis) {
      if (first < 0) first = i;
      last = i;
    }
  }
  if (first < 0) return TrailReject::Faint;
  first = std::max(0, first - 1);
  last = std::min(n - 1, last + 1);
  if (last - first < 1) return TrailReject::Short;
  if (first > 0 || last < n - 1) {
    pts = std::vector<StripPt>(pts.begin() + first, pts.begin() + last + 1);
  }
  float span = 0, area = 0, peak_vis = 0, peak_w = 0, cxs = 0, wsum = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    const StripPt& p = pts[i];
    peak_vis = std::max(peak_vis, vertex_vis(p, blend));
    peak_w = std::max(peak_w, p.w);
    if (i + 1 < pts.size()) {
      const StripPt& q = pts[i + 1];
      const float len = std::hypot(q.x - p.x, q.y - p.y);
      const float aw = std::min(3.0f, 0.5f * (p.w + q.w));
      const float av = 0.5f * (vertex_vis(p, blend) + vertex_vis(q, blend));
      span += len;
      area += len * aw * av;
      cxs += 0.5f * (p.x + q.x) * len * aw * av;
      wsum += len * aw * av;
    }
  }
  const float cx = wsum > 1e-9f ? cxs / wsum : pts[0].x;
  c.span = span;
  c.peak_w = peak_w;
  c.peak_vis = peak_vis;
  c.score = area / (1.0f + std::fabs(cx) / 1.5f);
  if (span < 0.05f || peak_w < 0.008f || peak_vis < 0.03f) return TrailReject::Short;
  return TrailReject::Ok;
}

// Strict test used when the kind already has a visible body (trail must be meaningful on its
// own); the loose test (eval_trail Ok) is used for trail-driven kinds and ribbons.
bool trail_meaningful(const TrailCand& c) {
  return c.span >= 0.12f && c.peak_w >= 0.02f && c.peak_vis >= 0.1f && c.score >= 0.01f;
}

// Douglas-Peucker in the (x, y) frame extended with width / alpha / colour / U channels.
// kept indices are returned ascending. Clamped to [2, cap] by growing the tolerance.
struct Decimation {
  std::vector<int> keep;
  float err_pos = 0, err_attr = 0;
};

Decimation decimate(const std::vector<StripPt>& pts, uint8_t blend, int cap) {
  const int n = static_cast<int>(pts.size());
  Decimation out;
  if (n <= 2) {
    for (int i = 0; i < n; ++i) out.keep.push_back(i);
    return out;
  }
  const float k = blend_k(blend);
  std::vector<float> s(static_cast<size_t>(n), 0.0f);
  float maxw = 0, maxa = 0.05f, maxc = 0.05f, umin = pts[0].u, umax = pts[0].u;
  for (int i = 0; i < n; ++i) {
    const StripPt& p = pts[static_cast<size_t>(i)];
    if (i > 0) {
      const StripPt& q = pts[static_cast<size_t>(i - 1)];
      s[static_cast<size_t>(i)] = s[static_cast<size_t>(i - 1)] + std::hypot(p.x - q.x, p.y - q.y);
    }
    maxw = std::max(maxw, p.w);
    maxa = std::max(maxa, p.a * k);
    maxc = std::max(maxc, std::max({p.r, p.g, p.b}) * k);
    umin = std::min(umin, p.u);
    umax = std::max(umax, p.u);
  }
  const float urange = std::max(umax - umin, 1e-3f);
  const float nw = std::max(maxw, 0.02f);

  // error of vertex j against the chord (i, kk): returns normalised error (1 = at tolerance);
  // also reports the raw position error and relative attribute error.
  auto chord_err = [&](int i, int kk, int j, float scale, float& pos_raw, float& attr_rel) {
    const StripPt& a = pts[static_cast<size_t>(i)];
    const StripPt& b = pts[static_cast<size_t>(kk)];
    const StripPt& p = pts[static_cast<size_t>(j)];
    const float ds = s[static_cast<size_t>(kk)] - s[static_cast<size_t>(i)];
    const float tt = ds > 1e-9f ? (s[static_cast<size_t>(j)] - s[static_cast<size_t>(i)]) / ds
                                : static_cast<float>(j - i) / static_cast<float>(kk - i);
    // distance to the chord segment
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float l2 = vx * vx + vy * vy;
    float proj = l2 > 1e-12f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.0f;
    proj = std::clamp(proj, 0.0f, 1.0f);
    pos_raw = std::hypot(p.x - (a.x + vx * proj), p.y - (a.y + vy * proj));
    auto lerp = [&](float x0, float x1) { return x0 + (x1 - x0) * tt; };
    const float dw = std::fabs(p.w - lerp(a.w, b.w)) / nw;
    const float da = std::fabs(p.a * k - lerp(a.a * k, b.a * k)) / maxa;
    const float dc = std::max({std::fabs(p.r - lerp(a.r, b.r)), std::fabs(p.g - lerp(a.g, b.g)),
                               std::fabs(p.b - lerp(a.b, b.b))}) * k / maxc;
    const float du = std::fabs(p.u - lerp(a.u, b.u)) / urange;
    attr_rel = std::max({dw, da, dc, du});
    return std::max(pos_raw / (kTolPos * scale), attr_rel / (0.10f * scale));
  };
  // The preview shades each segment flat (average of its endpoints), so a long ramp must keep a few
  // stops: force a split when the end-to-end change across a chord is large.
  auto span_change = [&](int i, int kk) {
    const StripPt& a = pts[static_cast<size_t>(i)];
    const StripPt& b = pts[static_cast<size_t>(kk)];
    const float dw = std::fabs(b.w - a.w) / nw;
    const float da = std::fabs(b.a - a.a) * k / maxa;
    const float dc = std::max({std::fabs(b.r - a.r), std::fabs(b.g - a.g), std::fabs(b.b - a.b)}) * k / maxc;
    return std::max({dw, da, dc});
  };

  auto run = [&](float scale) {
    std::vector<char> keep(static_cast<size_t>(n), 0);
    keep[0] = keep[static_cast<size_t>(n - 1)] = 1;
    std::vector<std::pair<int, int>> stack{{0, n - 1}};
    while (!stack.empty()) {
      const auto [i, kk] = stack.back();
      stack.pop_back();
      if (kk - i < 2) continue;
      float best = -1;
      int bj = -1;
      for (int j = i + 1; j < kk; ++j) {
        float pr, ar;
        const float e = chord_err(i, kk, j, scale, pr, ar);
        if (e > best) {
          best = e;
          bj = j;
        }
      }
      const bool force = span_change(i, kk) > 0.35f * scale;
      if (best > 1.0f || force) {
        keep[static_cast<size_t>(bj)] = 1;
        stack.push_back({i, bj});
        stack.push_back({bj, kk});
      }
    }
    std::vector<int> idx;
    for (int i = 0; i < n; ++i) {
      if (keep[static_cast<size_t>(i)]) idx.push_back(i);
    }
    return idx;
  };
  out.keep = run(1.0f);
  if (static_cast<int>(out.keep.size()) > cap) {
    // Over budget: grow the tolerance geometrically until it fits, then bisect back down so the
    // budget is used as fully as possible.
    float lo = 1.0f, hi = 1.25f;
    std::vector<int> fit = run(hi);
    for (int g = 0; g < 60 && static_cast<int>(fit.size()) > cap; ++g) {
      lo = hi;
      hi *= 1.25f;
      fit = run(hi);
    }
    if (static_cast<int>(fit.size()) <= cap) {
      for (int it = 0; it < 12; ++it) {
        const float mid = 0.5f * (lo + hi);
        std::vector<int> m = run(mid);
        if (static_cast<int>(m.size()) <= cap) {
          hi = mid;
          fit = std::move(m);
        } else {
          lo = mid;
        }
      }
      out.keep = std::move(fit);
    }
  }
  if (static_cast<int>(out.keep.size()) > cap) {  // pathological: uniform fallback
    out.keep.clear();
    for (int q = 0; q < cap; ++q) out.keep.push_back(q * (n - 1) / (cap - 1));
  }
  // measure the achieved error on every dropped vertex
  for (size_t q = 0; q + 1 < out.keep.size(); ++q) {
    for (int j = out.keep[q] + 1; j < out.keep[q + 1]; ++j) {
      float pr, ar;
      chord_err(out.keep[q], out.keep[q + 1], j, 1.0f, pr, ar);
      out.err_pos = std::max(out.err_pos, pr);
      out.err_attr = std::max(out.err_attr, ar);
    }
  }
  return out;
}

PreviewSplitLaneInstance make_split(int32_t effect_id, int32_t split_count) {
  PreviewSplitLaneInstance split;
  split.apply_identity(effect_id, split_count, SplitLaneType::Full, effect_id, kSentinelStartMs,
                       kSentinelStartMs + 30000);
  split.apply_state(true, false, split_count + 1);
  split.apply_animation(1.0f, 0.0f, 1.0f, 0.0f, 1);
  split.should_show = true;
  split.split_line_alpha = 1.0f;
  return split;
}

double now_sec_at(double now_t) {
  return static_cast<double>(kSentinelStartMs) / 1000.0 -
         static_cast<double>(wds::chart_editor::kOfficialSplitFadeInSec) + now_t;
}

float line_origin_y(int32_t color_id) {
  return wds::chart_editor::split_fade_grows_from_tip(color_id)
             ? wds::chart_editor::official_split_z180_origin_y(color_id)
             : wds::chart_editor::official_split_identity_origin_y();
}

// Same maths as StageGeometry::split_particle_oriented_world_corners, reduced to the 2D plane the
// editor shows: View alignment is camera-facing (on-screen offset == (px, py)), World/Local
// alignment lives in the lane plane (== (px, py) top-down). Both reduce to (px, py).
void oriented_corners_xy(float size_unity, float height_scale, float pivot_x, float pivot_y,
                         float pivot_z, float roll, float tilt_x, float tilt_y, float* ox,
                         float* oy) {
  const float width = std::max(size_unity, 1e-4f);
  const float height = width * std::max(height_scale, 1e-4f);
  const float cr = std::cos(roll), sr = std::sin(roll);
  const float cxr = std::cos(tilt_x), sxr = std::sin(tilt_x);
  const float cyr = std::cos(tilt_y), syr = std::sin(tilt_y);
  const float oxp = pivot_x * width;
  const float oyp = -pivot_y * height;
  const float ozp = -pivot_z * width;
  const float hw = width * 0.5f, hh = height * 0.5f;
  const float lx[4] = {-hw, hw, hw, -hw};
  const float ly[4] = {-hh, -hh, hh, hh};
  for (int i = 0; i < 4; ++i) {
    const float mx = lx[i] + oxp, my = ly[i] + oyp, mz = ozp;
    const float x = mx * cr - my * sr;
    const float y = mx * sr + my * cr;
    const float z = mz;
    const float x2 = x * cyr + z * syr;
    const float z2 = -x * syr + z * cyr;
    (void)z2;
    ox[i] = x2;
    oy[i] = y * cxr - z * sxr;
  }
}

enum class Skip { None, TrailOnly, DeadAlpha, UnnamedOther, NoSprite, ZeroSize, BehindCamera };

// Converts one live particle. Returns Skip::None when `out` is valid.
Skip convert(const SplitEffectLiveParticle& p, float line_x, float origin_y, Baked& out) {
  using namespace wds::chart_render;
  const bool unnamed = p.uv_name_index == kSplitEffectNoUvName;
  const bool cube = (p.flags & kSplitEffectFlagCubeMesh) != 0;
  const bool ring = (p.flags & kSplitEffectFlagRingMesh) != 0;
  const bool butterfly = (p.flags & kSplitEffectFlagButterfly) != 0;
  const bool trails = (p.flags & kSplitEffectFlagTrails) != 0;
  const bool other = p.blend == static_cast<uint8_t>(SplitEffectBlend::Other);
  const bool mesh_render = p.render_mode == kSplitEffectRenderMesh;
  const bool stretch_render = p.render_mode == kSplitEffectRenderStretch;

  // playback_preview.cpp emit_one skip rules.
  if (unnamed && !cube && !ring && trails && !other) {
    return Skip::TrailOnly;  // unnamed ParticleAdd/Alpha + Trails: trail only
  }
  if (p.remaining_lifetime <= 1e-5f && trails && p.trail_count < 2 && p.trail_points >= 2) {
    return Skip::TrailOnly;  // dead particle whose trail is still fading
  }
  if (p.a < 0.001f) {
    return Skip::DeadAlpha;
  }
  if (unnamed && !cube && !ring && other) {
    return Skip::UnnamedOther;  // BombEffectTrail corner texel (alpha 0), clipped by the shader
  }
  if (!unnamed && !cube) {
    const SplitEffectUvTile* tile = official_split_effect_uv_tile(p.uv_name_index);
    if (tile == nullptr || tile->present == 0 || tile->w == 0 || tile->h == 0) {
      return Skip::NoSprite;  // named sprite missing from the atlas: preview draws nothing
    }
  }
  if (!wds::chart_editor::official_main_in_front_of_camera(p.main_y, p.z)) {
    return Skip::BehindCamera;
  }

  float size_unity = p.size;
  if (!mesh_render && !stretch_render) {
    const float view_z = wds::chart_editor::official_main_camera_z(p.main_y, p.z);
    const float tan_half = 1.0f / std::max(wds::chart_editor::official_camera_focal(), 1e-6f);
    size_unity = split_effect_billboard_draw_width(p.current_size, p.size, p.max_particle_size,
                                                   view_z, tan_half, kViewAspect, p.render_mode);
  }
  if (!mesh_render && size_unity <= 1e-6f) {
    return Skip::ZeroSize;  // maxP == 0 or zero size: preview draws no body
  }

  out = Baked{};
  out.catalog_index = static_cast<int32_t>(p.catalog_index);
  out.blend = p.blend;
  out.sorting_order = static_cast<int8_t>(p.sorting_order);
  out.flip = static_cast<uint8_t>((p.flip_u ? 1 : 0) | (p.flip_v ? 2 : 0));
  out.r = p.r;
  out.g = p.g;
  out.b = p.b;
  out.a = p.a;
  out.birth_t = p.birth_t;
  out.seed = p.seed;
  out.uv = (cube || unnamed) ? 0xFFFF : p.uv_name_index;
  if (cube) {
    out.flip = 0;
  }

  float cx = p.world_x;
  float cy = p.main_y - g_z_fold * p.z;
  float w = 0, h = 0, ang = 0;

  if (butterfly) {
    // Flapping wings (playback_preview.cpp flap_wing): wing half-span 0.2*size, flapped about the
    // spine by theta = custom1_y * mesh_x * 7.5.
    const float wing = p.butterfly_wing_x > 1e-6f ? p.butterfly_wing_x : kSplitEffectButterflyMeshHalfX;
    const float theta = p.custom1_y * wing * 7.5f;
    w = 2.0f * wing * size_unity * std::fabs(std::cos(theta));
    h = 2.0f * kSplitEffectButterflyMeshHalfY * size_unity;
    ang = p.roll;
    out.shape = 4;
  } else if (ring && mesh_render) {
    // Ring mesh radius 1, scaled by (size_x, size_y), Euler Z,X,Y; project onto the lane plane.
    const float cr = std::cos(p.roll), sr = std::sin(p.roll);
    const float cxr = std::cos(p.tilt_x), sxr = std::sin(p.tilt_x);
    const float cyr = std::cos(p.tilt_y), syr = std::sin(p.tilt_y);
    auto project = [&](float mx, float my, float& ox, float& oy) {
      const float x = mx * cr - my * sr;
      const float y = mx * sr + my * cr;
      const float z = 0.0f;
      const float y2 = y * cxr - z * sxr;
      const float z2 = y * sxr + z * cxr;
      ox = x * cyr + z2 * syr;
      oy = y2;
    };
    float ax, ay, bx, by;
    project(p.size, 0.0f, ax, ay);
    project(0.0f, p.size_y, bx, by);
    // M = [a b] (columns). SVD -> ellipse semi axes and major-axis angle.
    const float E = 0.5f * (ax + by), F = 0.5f * (ax - by);
    const float G = 0.5f * (ay + bx), H = 0.5f * (ay - bx);
    const float Q = std::sqrt(E * E + H * H), R = std::sqrt(F * F + G * G);
    const float a1 = std::atan2(G, F), a2 = std::atan2(H, E);
    w = 2.0f * (Q + R);
    h = 2.0f * std::fabs(Q - R);
    ang = 0.5f * (a2 + a1);
    out.shape = 2;
  } else if (cube && mesh_render) {
    // Axis-aligned-in-roll bounding rectangle of the projected box (8 corners).
    const float cr = std::cos(p.roll), sr = std::sin(p.roll);
    const float cxr = std::cos(p.tilt_x), sxr = std::sin(p.tilt_x);
    const float cyr = std::cos(p.tilt_y), syr = std::sin(p.tilt_y);
    float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
    for (int i = 0; i < 8; ++i) {
      const float mx = ((i & 1) ? 0.5f : -0.5f) * p.size;
      const float my = ((i & 2) ? 0.5f : -0.5f) * p.size_y;
      const float mz = ((i & 4) ? 0.5f : -0.5f) * p.size_z;
      const float x = mx * cr - my * sr;
      const float y = mx * sr + my * cr;
      const float y2 = y * cxr - mz * sxr;
      const float z2 = y * sxr + mz * cxr;
      const float wx = x * cyr + z2 * syr;
      // un-roll so we measure the extent in the rolled frame
      const float u = wx * cr + y2 * sr;
      const float v = -wx * sr + y2 * cr;
      umin = std::min(umin, u);
      umax = std::max(umax, u);
      vmin = std::min(vmin, v);
      vmax = std::max(vmax, v);
    }
    w = umax - umin;
    h = vmax - vmin;
    const float uc = 0.5f * (umax + umin), vc = 0.5f * (vmax + vmin);
    cx += uc * cr - vc * sr;
    cy += uc * sr + vc * cr;
    ang = p.roll;
    out.shape = 3;
  } else if (stretch_render) {
    // Kettlewell strip (stage_geometry.cpp split_particle_stretch_world_corners): head at the
    // particle (shifted by pivot), tail = head - (v*velocityScale + dir*appliedLS*size.y).
    float vx = p.total_vx, vy = p.total_vy, vz = p.total_vz;
    if (vx * vx + vy * vy + vz * vz <= 1e-20f) {
      vx = p.vel_x;
      vy = p.vel_y;
      vz = p.vel_z;
    }
    const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (speed <= 1e-10f) {
      // Preview falls back to the oriented billboard (width = size, height = along).
      const float along = wds::chart_render::official_split_effect_stretch_height(size_unity, p.size_y, p.size_3d);
      float qx[4], qy[4];
      oriented_corners_xy(std::max(size_unity, 1e-4f), along / std::max(size_unity, 1e-4f), p.pivot_x,
                          p.pivot_y, 0.0f, p.roll, 0.0f, 0.0f, qx, qy);
      cx += 0.25f * (qx[0] + qx[1] + qx[2] + qx[3]);
      cy += 0.25f * (qy[0] + qy[1] + qy[2] + qy[3]);
      w = std::max(size_unity, 1e-4f);
      h = along;
      ang = p.roll;
      out.shape = 0;
    } else {
      const float along = wds::chart_render::official_split_effect_stretch_height(size_unity, p.size_y, p.size_3d);
      const float ls = wds::chart_render::official_split_effect_stretch_applied_length_scale(
          p.stored_vx, p.stored_vy, p.stored_vz, p.total_vx, p.total_vy, p.total_vz, p.length_scale,
          p.effect_id, p.render_mode, p.uv_name_index);
      const float inv = 1.0f / speed;
      const float dxv = vx * inv, dyv = vy * inv;
      // lane-plane projection of the 3D tail vector
      const float tail_x = vx * p.velocity_scale + vx * inv * ls * along;
      const float tail_y = vy * p.velocity_scale + vy * inv * ls * along;
      const float len = std::sqrt(tail_x * tail_x + tail_y * tail_y);
      const float across = std::max(size_unity, 1e-4f);
      // pivot_y shifts the head along the velocity, pivot_x across it.
      float hx = p.world_x + dxv * p.pivot_y * along;
      float hy = p.main_y - g_z_fold * p.z + dyv * p.pivot_y * along;
      hx += (-dyv) * p.pivot_x * across * 0.5f;
      hy += (dxv) * p.pivot_x * across * 0.5f;
      cx = hx - 0.5f * tail_x;
      cy = hy - 0.5f * tail_y;
      w = len;
      h = across;
      ang = std::atan2(tail_y, tail_x);
      out.shape = 1;
      if (len < 1e-5f) {
        return Skip::ZeroSize;
      }
    }
  } else {
    float qx[4], qy[4];
    oriented_corners_xy(size_unity, p.sol_y, p.pivot_x, p.pivot_y, p.pivot_z, p.roll, p.tilt_x, p.tilt_y,
                        qx, qy);
    const float ccx = 0.25f * (qx[0] + qx[1] + qx[2] + qx[3]);
    const float ccy = 0.25f * (qy[0] + qy[1] + qy[2] + qy[3]);
    cx += ccx;
    cy += ccy;
    const float e1x = qx[1] - qx[0], e1y = qy[1] - qy[0];
    const float e2x = qx[3] - qx[0], e2y = qy[3] - qy[0];
    w = std::sqrt(e1x * e1x + e1y * e1y);
    h = std::sqrt(e2x * e2x + e2y * e2y);
    ang = std::atan2(e1y, e1x);
    out.shape = 0;
  }
  // Numerically degenerate only; the renderer's min_sprite_px keeps small sparks legible.
  if (!(std::max(w, h) >= 0.005f) || !(std::min(w, h) >= 0.001f)) {
    return Skip::ZeroSize;
  }
  out.dx = cx - line_x;
  out.dy = cy - origin_y;
  out.w = w;
  out.h = h;
  out.angle = ang * (180.0f / kPi);
  // Normalise to (-180, 180].
  while (out.angle > 180.0f) out.angle -= 360.0f;
  while (out.angle <= -180.0f) out.angle += 360.0f;
  // Visibility score: alpha x area (blend 2 gets its x2), clamped so huge fog quads do not
  // swamp small crisp sprites, damped by horizontal distance from the line.
  const float a_eff = std::min(out.a * (out.blend == 2 ? 2.0f : 1.0f), 1.0f);
  const float lum = std::max({out.r, out.g, out.b, 0.05f});
  const float area = std::min(out.w * out.h, 6.0f);
  out.score = a_eff * std::min(lum, 2.0f) * area / (1.0f + std::fabs(out.dx) / 1.5f);
  return Skip::None;
}

std::vector<SplitEffectLiveParticle>& pool() {
  static std::vector<SplitEffectLiveParticle> p(wds::chart_render::kSplitEffectParticlePool);
  return p;
}

// Runs the sim for `count` lines (type Full) at now_t and returns {line_index -> particles}.
// Only scalars survive; trail pointers are transient.
int collect(int32_t id, int count, double now_t) {
  const PreviewSplitLaneInstance split = make_split(id, count);
  return wds::chart_render::collect_split_effect_particles(split, kQualityNormal, now_sec_at(now_t),
                                                           pool().data(), static_cast<int>(pool().size()));
}

struct SlotPlan {
  int slot = 0;
  int count = 0;  // rep split_count
  int line = 0;   // rep line index
};

std::vector<SlotPlan> plan_slots(int32_t id, int element_count) {
  std::map<int, SlotPlan> by_slot;
  for (int c = 1; c <= wds::chart_editor::kOfficialSplitLineMaxCount; ++c) {
    for (int l = 0; l <= c; ++l) {
      const int slot = wds::chart_editor::split_color_slot(id, c, l) % element_count;
      if (by_slot.find(slot) == by_slot.end()) {
        by_slot[slot] = {slot, c, l};
      }
    }
  }
  std::vector<SlotPlan> out;
  for (auto& kv : by_slot) out.push_back(kv.second);
  return out;
}

struct KindReport {
  uint32_t kind = 0;
  std::string repr;    // sprite | sprite+trail | sprite+ribbon | trail | trail+trail | ribbon | missing
  std::string reason;  // why missing / note
  int kept = 0, total = 0;
  float err_pos = 0, err_attr = 0;
  bool round1_missing = false;  // no sprite body baked (round-1 rules)
  bool represented = false;
};

struct SlotResult {
  SlotPlan plan;
  std::vector<Baked> particles;           // sorted in preview order
  std::map<uint32_t, KindStats> kinds;    // by catalog_index
  std::set<uint32_t> expected_kinds;
  std::set<uint32_t> missing_kinds;
  std::vector<KindReport> reports;        // per kind that had anything to say
  int tier0_kinds = 0, tier1_kinds = 0, tier2_kinds = 0;
};

bool differs_enough(const Baked& a, const Baked& b) {
  if (a.uv != b.uv) return true;
  const float sa = std::max(a.w, a.h), sb = std::max(b.w, b.h);
  const float ratio = sa > sb ? sa / std::max(sb, 1e-6f) : sb / std::max(sa, 1e-6f);
  if (ratio > 1.5f) return true;
  const float dist = std::hypot(a.dx - b.dx, a.dy - b.dy);
  if (dist > 1.5f * std::max(std::max(sa, sb), 0.25f)) return true;
  return false;
}

SlotResult bake_slot(int32_t id, const wds::chart_render::SplitEffectCatalogEntry& entry,
                     const SlotPlan& plan) {
  SlotResult res;
  res.plan = plan;
  uint16_t pcount = 0;
  const auto* parts = wds::chart_render::official_split_effect_particles(entry, &pcount);
  for (uint16_t i = 0; i < pcount; ++i) {
    if (parts[i].element_index == plan.slot && parts[i].go_active != 0) {
      res.expected_kinds.insert(static_cast<uint32_t>(entry.particle_begin) + i);
    }
  }
  float line_x = 0;
  wds::chart_editor::official_split_line_world_x(plan.count, plan.line, line_x);
  const float origin_y = line_origin_y(id);

  // kind -> candidates by tier
  std::map<uint32_t, std::vector<Baked>> cands;
  // kind -> tier -> best trail candidates (full resolution, copied right after each collect)
  std::map<uint32_t, std::array<std::vector<TrailCand>, 3>> trails;
  std::vector<StripPt> scratch;
  auto offer_trail = [&](TrailCand&& c, std::vector<StripPt>& pts, KindStats& ks, const SampleTime& st) {
    ++ks.trail_seen;
    c.tier = st.tier;
    c.t = st.t;
    switch (eval_trail(pts, c.blend, c)) {
      case TrailReject::NoWidth: ++ks.trail_no_width; return;
      case TrailReject::Faint: ++ks.trail_faint; return;
      case TrailReject::Short: ++ks.trail_short; return;
      case TrailReject::Offscreen: ++ks.trail_offscreen; return;
      case TrailReject::Ok: break;
    }
    ++ks.trail_ok;
    auto& vec = trails[c.catalog_index][static_cast<size_t>(st.tier)];
    if (static_cast<int>(vec.size()) >= kTrailKeepPerKindTier &&
        c.score <= vec.back().score) {
      return;
    }
    c.pts = pts;
    vec.push_back(std::move(c));
    std::stable_sort(vec.begin(), vec.end(),
                     [](const TrailCand& a, const TrailCand& b) { return a.score > b.score; });
    if (static_cast<int>(vec.size()) > kTrailKeepPerKindTier) vec.pop_back();
  };
  for (const SampleTime& st : kTimes) {
    const int n = collect(id, plan.count, st.t);
    // Ribbon chains of this line (one stitched chain = one strip). Same maths as the preview's
    // process_chain: chain sorted by trail_vertex (0 = oldest), U = arc fraction of the whole
    // chain, drawn in contiguous trail_vertex runs.
    {
      std::map<uint32_t, std::vector<int>> chains;
      for (int i = 0; i < n; ++i) {
        const SplitEffectLiveParticle& rp = pool()[static_cast<size_t>(i)];
        if (rp.line_index == plan.line && rp.trail_chain != 0 && rp.trail_count >= 2) {
          chains[rp.trail_chain].push_back(i);
        }
      }
      for (auto& kv : chains) {
        std::vector<int>& idxs = kv.second;
        std::sort(idxs.begin(), idxs.end(), [&](int a, int b) {
          const auto& pa = pool()[static_cast<size_t>(a)];
          const auto& pb = pool()[static_cast<size_t>(b)];
          if (pa.trail_vertex != pb.trail_vertex) return pa.trail_vertex < pb.trail_vertex;
          return a < b;
        });
        if (idxs.size() < 2) continue;
        const SplitEffectLiveParticle& marker = pool()[static_cast<size_t>(idxs.back())];
        const auto mat = wds::chart_render::official_split_effect_trail_material(marker.catalog_index);
        const uint8_t tblend = wds::chart_render::official_split_effect_trail_blend(mat);
        const int cn = static_cast<int>(idxs.size());
        std::vector<float> uu(static_cast<size_t>(cn), 0.0f);
        float total = 0;
        for (int k2 = 1; k2 < cn; ++k2) {
          const auto& a = pool()[static_cast<size_t>(idxs[static_cast<size_t>(k2 - 1)])];
          const auto& b = pool()[static_cast<size_t>(idxs[static_cast<size_t>(k2)])];
          total += std::sqrt((b.world_x - a.world_x) * (b.world_x - a.world_x) +
                             (b.main_y - a.main_y) * (b.main_y - a.main_y) + (b.z - a.z) * (b.z - a.z));
        }
        float acc = 0;
        for (int k2 = 1; k2 < cn; ++k2) {
          const auto& a = pool()[static_cast<size_t>(idxs[static_cast<size_t>(k2 - 1)])];
          const auto& b = pool()[static_cast<size_t>(idxs[static_cast<size_t>(k2)])];
          acc += std::sqrt((b.world_x - a.world_x) * (b.world_x - a.world_x) +
                           (b.main_y - a.main_y) * (b.main_y - a.main_y) + (b.z - a.z) * (b.z - a.z));
          uu[static_cast<size_t>(k2)] = total > 1e-6f ? acc / total : 1.0f;
        }
        KindStats& ks = res.kinds[marker.catalog_index];
        int run0 = 0;
        for (int k2 = 0; k2 < cn; ++k2) {
          const bool brk =
              k2 + 1 >= cn ||
              pool()[static_cast<size_t>(idxs[static_cast<size_t>(k2 + 1)])].trail_vertex !=
                  static_cast<uint16_t>(pool()[static_cast<size_t>(idxs[static_cast<size_t>(k2)])].trail_vertex + 1u);
          if (!brk) continue;
          if (k2 + 1 - run0 >= 2) {
            scratch.clear();
            for (int q = run0; q <= k2; ++q) {
              const auto& v = pool()[static_cast<size_t>(idxs[static_cast<size_t>(q)])];
              StripPt sp;
              sp.x = v.world_x - line_x;
              sp.y = v.main_y - g_z_fold * v.z - origin_y;
              sp.w = v.trail_width;
              sp.u = uu[static_cast<size_t>(q)];
              sp.r = v.trail_r;
              sp.g = v.trail_g;
              sp.b = v.trail_b;
              sp.a = v.trail_a;
              sp.ay = v.main_y;
              sp.az = v.z;
              scratch.push_back(sp);
            }
            TrailCand c;
            c.catalog_index = marker.catalog_index;
            c.blend = tblend;
            c.sorting_order = static_cast<int8_t>(marker.sorting_order);
            c.group = 1;
            c.birth_t = marker.birth_t;
            c.seed = marker.seed;
            offer_trail(std::move(c), scratch, ks, st);
          }
          run0 = k2 + 1;
        }
      }
    }
    for (int i = 0; i < n; ++i) {
      const SplitEffectLiveParticle& p = pool()[static_cast<size_t>(i)];
      if (p.line_index != plan.line) continue;
      KindStats& ks = res.kinds[p.catalog_index];
      ++ks.seen;
      // Particle-mode trail (copy now: the channel pointers are transient).
      if (p.trail_count < 2 && p.trail_points >= 2) {
        const int pn = static_cast<int>(p.trail_points);
        const bool xyz_ok = static_cast<int>(p.trail_x.size()) >= pn && static_cast<int>(p.trail_y.size()) >= pn &&
                            static_cast<int>(p.trail_z.size()) >= pn;
        if (xyz_ok) {
          const auto mat = wds::chart_render::official_split_effect_trail_material(p.catalog_index);
          const uint8_t tblend = wds::chart_render::official_split_effect_trail_blend(mat);
          const bool wide = static_cast<int>(p.trail_widths.size()) == pn;
          const bool cols = static_cast<int>(p.trail_color_r.size()) == pn &&
                            static_cast<int>(p.trail_color_g.size()) == pn &&
                            static_cast<int>(p.trail_color_b.size()) == pn &&
                            static_cast<int>(p.trail_color_a.size()) == pn;
          const float denom = std::max(static_cast<float>(pn - 1), 1.0f);
          scratch.clear();
          for (int q = 0; q < pn; ++q) {
            StripPt sp;
            sp.x = p.trail_x[static_cast<size_t>(q)] - line_x;
            sp.y = p.trail_y[static_cast<size_t>(q)] - g_z_fold * p.trail_z[static_cast<size_t>(q)] - origin_y;
            sp.w = wide ? p.trail_widths[static_cast<size_t>(q)] : p.trail_width;
            sp.u = static_cast<float>(q) / denom;  // index 0 = head
            sp.r = cols ? p.trail_color_r[static_cast<size_t>(q)] : p.trail_r;
            sp.g = cols ? p.trail_color_g[static_cast<size_t>(q)] : p.trail_g;
            sp.b = cols ? p.trail_color_b[static_cast<size_t>(q)] : p.trail_b;
            sp.a = cols ? p.trail_color_a[static_cast<size_t>(q)] : p.trail_a;
            sp.ay = p.trail_y[static_cast<size_t>(q)];
            sp.az = p.trail_z[static_cast<size_t>(q)];
            scratch.push_back(sp);
          }
          TrailCand c;
          c.catalog_index = p.catalog_index;
          c.blend = tblend;
          c.sorting_order = static_cast<int8_t>(p.sorting_order);
          c.group = 0;
          c.birth_t = p.birth_t;
          c.seed = p.seed;
          offer_trail(std::move(c), scratch, ks, st);
        }
      }
      Baked b;
      switch (convert(p, line_x, origin_y, b)) {
        case Skip::None:
          b.tier = st.tier;
          b.t = st.t;
          cands[p.catalog_index].push_back(b);
          ++ks.kept;
          break;
        case Skip::TrailOnly: ++ks.skip_trail_only; break;
        case Skip::DeadAlpha: ++ks.skip_dead_alpha; break;
        case Skip::UnnamedOther: ++ks.skip_unnamed_other; break;
        case Skip::NoSprite: ++ks.skip_no_sprite; break;
        case Skip::ZeroSize: ++ks.skip_zero_size; break;
        case Skip::BehindCamera: ++ks.skip_behind_camera; break;
      }
    }
  }
  for (auto& kv : cands) {
    std::vector<Baked>& v = kv.second;
    int best_tier = 99;
    for (const Baked& b : v) best_tier = std::min(best_tier, b.tier);
    std::vector<const Baked*> pick;
    for (const Baked& b : v) {
      if (b.tier == best_tier) pick.push_back(&b);
    }
    std::sort(pick.begin(), pick.end(), [](const Baked* a, const Baked* b) { return a->score > b->score; });
    if (pick.empty()) continue;
    const Baked* first = pick[0];
    std::vector<Baked> chosen;  // picks of this kind (rank 0,1 first, then extras)
    chosen.push_back(*first);
    chosen.back().rank = 0;
    if (best_tier == 0) ++res.tier0_kinds;
    else if (best_tier == 1) ++res.tier1_kinds;
    else ++res.tier2_kinds;
    // Second one only when it adds information and is still clearly visible.
    for (size_t i = 1; i < pick.size() && i < 400; ++i) {
      if (pick[i]->score >= 0.25f * first->score && differs_enough(*first, *pick[i])) {
        chosen.push_back(*pick[i]);
        chosen.back().rank = 1;
        break;
      }
    }
    // Extras (rank 2..kMaxRank-1) for the editor: visibility-ranked, spread along dy by greedy
    // farthest-point selection in (dx, dy * 0.5). Sample 0.5 s first, other tiers only as fallback.
    // Extras always rank >= 2 (a kind with a single pick leaves rank 1 unused so the picker set is unchanged).
    int next_rank = 2;
    for (int tier = best_tier; tier < 3 && next_rank < kMaxRank; ++tier) {
      std::vector<const Baked*> cand;
      for (const Baked& b : v) {
        if (b.tier != tier) continue;
        bool dup = false;
        for (const Baked& c : chosen) {
          if (c.uv == b.uv && std::fabs(c.dx - b.dx) <= 0.05f && std::fabs(c.dy - b.dy) <= 0.05f) {
            dup = true;
            break;
          }
        }
        if (!dup) cand.push_back(&b);
      }
      std::stable_sort(cand.begin(), cand.end(), [](const Baked* a, const Baked* b) { return a->score > b->score; });
      const size_t want = static_cast<size_t>(kMaxRank - next_rank);
      if (cand.size() > want * 3) cand.resize(want * 3);
      while (next_rank < kMaxRank && !cand.empty()) {
        size_t bi = 0;
        float bd = -1.0f;
        for (size_t i = 0; i < cand.size(); ++i) {
          float md = 1e30f;
          for (const Baked& c : chosen) {
            const float ddx = cand[i]->dx - c.dx, ddy = (cand[i]->dy - c.dy) * 0.5f;
            md = std::min(md, ddx * ddx + ddy * ddy);
          }
          if (md > bd) {
            bd = md;
            bi = i;
          }
        }
        // Skip near-duplicates of an already chosen pick (same frame, same spot).
        const Baked* pk = cand[bi];
        cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(bi));
        bool dup = false;
        for (const Baked& c : chosen) {
          if (c.uv == pk->uv && std::fabs(c.dx - pk->dx) <= 0.05f && std::fabs(c.dy - pk->dy) <= 0.05f) dup = true;
        }
        if (dup) continue;
        chosen.push_back(*pk);
        chosen.back().rank = static_cast<uint8_t>(next_rank++);
      }
    }
    for (const Baked& c : chosen) res.particles.push_back(c);
  }
  // ---- strips -----------------------------------------------------------------------------
  std::set<uint32_t> body_kinds;
  for (const Baked& b : res.particles) body_kinds.insert(static_cast<uint32_t>(b.catalog_index));
  std::map<uint32_t, KindReport> reports;
  auto make_strip = [&](const TrailCand& c, int cap, bool has_body) {
    const Decimation d = decimate(c.pts, c.blend, cap);
    Baked s;
    s.catalog_index = static_cast<int32_t>(c.catalog_index);
    s.uv = 0xFFFF;
    s.blend = c.blend;
    s.shape = 5;
    s.flip = 0;
    s.sorting_order = c.sorting_order;
    s.birth_t = c.birth_t;
    s.seed = c.seed;
    s.tier = c.tier;
    s.t = c.t;
    s.score = c.score;
    s.group = c.group;
    s.seq = 1;
    s.ribbon = c.group == 1;
    for (int idx : d.keep) s.pts.push_back(c.pts[static_cast<size_t>(idx)]);
    s.total_pts = static_cast<int>(c.pts.size());
    s.err_pos = d.err_pos;
    s.err_attr = d.err_attr;
    s.dx = s.pts[0].x;
    s.dy = s.pts[0].y;
    s.w = c.peak_w;  // informational: peak strip width
    s.h = c.span;    // informational: polyline length (undecimated)
    (void)has_body;
    return s;
  };
  for (auto& kv : trails) {
    const uint32_t kind = kv.first;
    const bool has_body = body_kinds.count(kind) != 0;
    std::vector<const TrailCand*> q;
    for (size_t tier = 0; tier < 3 && q.empty(); ++tier) {
      for (const TrailCand& c : kv.second[tier]) {
        const bool ok = c.group == 1 || !has_body || trail_meaningful(c);
        if (ok) q.push_back(&c);
      }
    }
    KindReport& rep = reports[kind];
    rep.kind = kind;
    if (q.empty()) {
      rep.reason = "trail_below_threshold";
      continue;
    }
    std::stable_sort(q.begin(), q.end(), [](const TrailCand* a, const TrailCand* b) { return a->score > b->score; });
    const TrailCand* first = q[0];
    if (has_body && first->group == 0) {
      // Prefer the trail of the very particle whose body was baked (head sits on the sprite).
      for (const Baked& b : res.particles) {
        if (static_cast<uint32_t>(b.catalog_index) != kind) continue;
        for (const TrailCand* c : q) {
          if (c->seed == b.seed && c->t == b.t) {
            first = c;
            break;
          }
        }
      }
    }
    const int cap = first->group == 1 ? kRibbonCap : (has_body ? kTrailCapWithBody : kTrailCapOnly);
    res.particles.push_back(make_strip(*first, cap, has_body));
    if (first->group == 0 && !has_body) {
      // A second, clearly different trail for trail-driven particle kinds.
      for (const TrailCand* c : q) {
        if (c == first || c->score < 0.4f * first->score) continue;
        const float d = std::hypot(c->pts[0].x - first->pts[0].x, c->pts[0].y - first->pts[0].y);
        const float ratio = std::max(c->span, first->span) / std::max(std::min(c->span, first->span), 1e-4f);
        if (d > 0.6f * std::max(std::max(c->span, first->span), 0.3f) || ratio > 1.8f) {
          res.particles.push_back(make_strip(*c, cap, has_body));
          break;
        }
      }
    }
  }
  // ---- per-kind reports -------------------------------------------------------------------
  for (uint32_t k : res.expected_kinds) {
    reports[k].kind = k;
  }
  for (auto& kv : reports) {
    KindReport& rep = kv.second;
    int nb = 0, ns = 0;
    bool ribbon = false;
    for (const Baked& b : res.particles) {
      if (static_cast<uint32_t>(b.catalog_index) != rep.kind) continue;
      if (b.shape == 5) {
        ++ns;
        ribbon = ribbon || b.ribbon;
        rep.kept += static_cast<int>(b.pts.size());
        rep.total += b.total_pts;
        rep.err_pos = std::max(rep.err_pos, b.err_pos);
        rep.err_attr = std::max(rep.err_attr, b.err_attr);
      } else {
        ++nb;
      }
    }
    rep.round1_missing = nb == 0;
    rep.represented = nb + ns > 0;
    if (nb > 0 && ns == 0) rep.repr = "sprite";
    else if (nb > 0) rep.repr = ribbon ? "sprite+ribbon" : "sprite+trail";
    else if (ns > 0) rep.repr = ribbon ? "ribbon" : (ns > 1 ? "trail+trail" : "trail");
    else rep.repr = "missing";
    if (!rep.represented) {
      const auto ks = res.kinds.count(rep.kind) ? res.kinds.at(rep.kind) : KindStats{};
      if (ks.seen == 0) rep.reason = "never_alive";
      else if (ks.trail_seen == 0) rep.reason = "invisible_no_trails";
      else if (ks.trail_no_width == ks.trail_seen) rep.reason = "trail_zero_width";
      else if (ks.trail_ok == 0 && ks.trail_offscreen >= ks.trail_faint && ks.trail_offscreen >= ks.trail_short)
        rep.reason = "trail_offscreen";
      else if (ks.trail_ok == 0 && ks.trail_faint >= ks.trail_short) rep.reason = "trail_faint";
      else if (ks.trail_ok == 0) rep.reason = "trail_too_short_or_narrow";
      else rep.reason = "trail_below_threshold";
    }
  }
  for (uint32_t k : res.expected_kinds) {
    if (!reports[k].represented) res.missing_kinds.insert(k);
  }
  for (auto& kv : reports) {
    if (!kv.second.repr.empty()) res.reports.push_back(kv.second);
  }
  // Preview draw order (playback_preview.cpp): all particles sorted by (sorting_order,
  // catalog_index, birth_t, seed), each particle's body immediately followed by its own particle-mode
  // trail; ribbon chains are appended after the particles of the line and the whole list is then
  // stable-sorted by sorting_order only. => (sorting_order, group[0 particle,1 ribbon],
  // catalog_index, birth_t, seed, seq[0 body,1 trail]).
  std::stable_sort(res.particles.begin(), res.particles.end(), [](const Baked& a, const Baked& b) {
    if (a.sorting_order != b.sorting_order) return a.sorting_order < b.sorting_order;
    if (a.group != b.group) return a.group < b.group;
    if (a.catalog_index != b.catalog_index) return a.catalog_index < b.catalog_index;
    if (a.birth_t != b.birth_t) return a.birth_t < b.birth_t;
    if (a.seed != b.seed) return a.seed < b.seed;
    return a.seq < b.seq;
  });
  return res;
}

void fmt_f(std::string& s, float v) {
  char buf[48];
  if (v == 0.0f) v = 0.0f;  // squash -0
  std::snprintf(buf, sizeof buf, "%.4g", v);
  s += buf;
}

// ---- keying analysis ------------------------------------------------------------------------

void analyze(const std::vector<int32_t>& ids) {
  int groups = 0, multi = 0, same_kinds = 0, same_exact = 0, same_exact_ignoring_birth = 0;
  double mirror_num = 0, mirror_den = 0;
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    if (entry == nullptr || entry->particle_count == 0) continue;
    const int ec = std::max<int>(entry->element_count, 1);
    wds::chart_render::reset_split_effect_auto_seed_session(true);
    // slot -> list of (count,line, kind->count, positions)
    struct LineSig {
      int count, line;
      std::map<uint32_t, int> kinds;
      std::vector<std::array<float, 3>> pos;  // kind, dx, dy (sorted)
    };
    std::map<int, std::vector<LineSig>> by_slot;
    for (int c = 1; c <= wds::chart_editor::kOfficialSplitLineMaxCount; ++c) {
      const int n = collect(id, c, 0.5);
      for (int l = 0; l <= c; ++l) {
        float lx = 0;
        wds::chart_editor::official_split_line_world_x(c, l, lx);
        LineSig sig{c, l, {}, {}};
        for (int i = 0; i < n; ++i) {
          const auto& p = pool()[static_cast<size_t>(i)];
          if (p.line_index != l) continue;
          ++sig.kinds[p.catalog_index];
          sig.pos.push_back({static_cast<float>(p.catalog_index), p.world_x - lx, p.main_y});
          if (lx != 0.0f) {
            mirror_num += static_cast<double>(p.world_x - lx) * (lx > 0 ? 1.0 : -1.0);
            mirror_den += std::fabs(static_cast<double>(p.world_x - lx));
          }
        }
        std::sort(sig.pos.begin(), sig.pos.end());
        by_slot[wds::chart_editor::split_color_slot(id, c, l) % ec].push_back(std::move(sig));
      }
    }
    for (auto& kv : by_slot) {
      ++groups;
      if (kv.second.size() < 2) continue;
      ++multi;
      bool kinds_eq = true, exact = true;
      for (size_t i = 1; i < kv.second.size(); ++i) {
        if (kv.second[i].kinds != kv.second[0].kinds) kinds_eq = false;
        const auto& a = kv.second[0].pos;
        const auto& b = kv.second[i].pos;
        if (a.size() != b.size()) {
          exact = false;
          continue;
        }
        for (size_t j = 0; j < a.size(); ++j) {
          for (int k = 0; k < 3; ++k) {
            if (std::fabs(a[j][static_cast<size_t>(k)] - b[j][static_cast<size_t>(k)]) > 1e-3f) exact = false;
          }
        }
      }
      if (kinds_eq) ++same_kinds;
      if (exact) ++same_exact;
      if (!exact && id < 0) ++same_exact_ignoring_birth;
    }
  }
  std::printf("analyze: (id,slot) groups=%d, with >=2 lines sharing the slot=%d\n", groups, multi);
  std::printf("  identical per-kind particle counts across lines: %d/%d\n", same_kinds, multi);
  std::printf("  identical (kind,dx,main_y) lists across lines  : %d/%d\n", same_exact, multi);
  std::printf("  side-mirroring index sum(dx*sgn(line_x))/sum|dx| : %.4f (0 = no dependence on side)\n",
              mirror_den > 0 ? mirror_num / mirror_den : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_inc, debug_tsv;
  std::vector<int32_t> only;
  bool do_analyze = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--out-inc") out_inc = next();
    else if (a == "--debug-tsv") debug_tsv = next();
    else if (a == "--analyze") do_analyze = true;
    else if (a == "--z-fold") g_z_fold = static_cast<float>(std::atof(next()));
    else if (a == "--only") {
      std::string s = next();
      size_t pos = 0;
      while (pos < s.size()) {
        size_t e = s.find(',', pos);
        if (e == std::string::npos) e = s.size();
        only.push_back(std::atoi(s.substr(pos, e - pos).c_str()));
        pos = e + 1;
      }
    } else {
      std::fprintf(stderr, "unknown arg %s\n", a.c_str());
      return 2;
    }
  }
  std::vector<int32_t> ids = wds::chart_render::official_split_color_ids();
  std::sort(ids.begin(), ids.end());
  if (!only.empty()) {
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](int32_t id) { return std::find(only.begin(), only.end(), id) == only.end(); }),
              ids.end());
  }
  if (do_analyze) {
    analyze(ids);  // keying evidence; the per-kind budget report follows the bake below
  }

  struct IdResult {
    int32_t id;
    int element_count;
    std::vector<SlotResult> slots;
  };
  std::vector<IdResult> results;
  int no_effect = 0;
  for (int32_t id : ids) {
    const auto* entry = wds::chart_render::official_split_effect(id);
    if (entry == nullptr || entry->particle_count == 0) {
      ++no_effect;
      continue;
    }
    wds::chart_render::reset_split_effect_auto_seed_session(true);
    IdResult ir;
    ir.id = id;
    ir.element_count = std::max<int>(entry->element_count, 1);
    for (const SlotPlan& plan : plan_slots(id, ir.element_count)) {
      ir.slots.push_back(bake_slot(id, *entry, plan));
    }
    results.push_back(std::move(ir));
    std::fprintf(stderr, "\rbaked %zu/%zu", results.size(), ids.size());
  }
  std::fprintf(stderr, "\n");

  // ---- strip point pool (same order as the .inc rows: id, slot 0..n-1, particle order) ----
  std::vector<StripPt> strip_pool;
  for (IdResult& ir : results) {
    for (int slot = 0; slot < ir.element_count; ++slot) {
      for (SlotResult& sr : ir.slots) {
        if (sr.plan.slot != slot) continue;
        for (Baked& b : sr.particles) {
          if (b.shape != 5) continue;
          b.strip_first = static_cast<uint32_t>(strip_pool.size());
          strip_pool.insert(strip_pool.end(), b.pts.begin(), b.pts.end());
        }
      }
    }
  }

  // ---- summary ----
  size_t total_particles = 0, slot_rows = 0, empty_slots = 0, missing_total = 0, expected_total = 0;
  size_t t0 = 0, t1 = 0, t2 = 0;
  size_t shape_n[6] = {0, 0, 0, 0, 0, 0};
  float wmin = 1e9f, wmax = 0, hmin = 1e9f, hmax = 0, dymin = 1e9f, dymax = -1e9f, dxmin = 1e9f, dxmax = -1e9f;
  for (const IdResult& ir : results) {
    for (const SlotResult& sr : ir.slots) {
      ++slot_rows;
      total_particles += sr.particles.size();
      if (sr.particles.empty()) ++empty_slots;
      missing_total += sr.missing_kinds.size();
      expected_total += sr.expected_kinds.size();
      t0 += static_cast<size_t>(sr.tier0_kinds);
      t1 += static_cast<size_t>(sr.tier1_kinds);
      t2 += static_cast<size_t>(sr.tier2_kinds);
      for (const Baked& b : sr.particles) {
        ++shape_n[b.shape];
        if (b.shape == 5) continue;
        wmin = std::min(wmin, b.w); wmax = std::max(wmax, b.w);
        hmin = std::min(hmin, b.h); hmax = std::max(hmax, b.h);
        dymin = std::min(dymin, b.dy); dymax = std::max(dymax, b.dy);
        dxmin = std::min(dxmin, b.dx); dxmax = std::max(dxmax, b.dx);
      }
    }
  }
  std::printf("ids requested=%zu, without sim effect=%d, baked ids=%zu, slot rows=%zu (empty %zu)\n",
              ids.size(), no_effect, results.size(), slot_rows, empty_slots);
  std::printf("particles=%zu (avg %.2f per slot-row); kinds from tier0=%zu tier1=%zu tier2=%zu\n", total_particles,
              slot_rows ? static_cast<double>(total_particles) / static_cast<double>(slot_rows) : 0.0, t0, t1, t2);
  std::printf("expected kinds=%zu, unrepresented=%zu\n", expected_total, missing_total);
  std::printf("shapes: billboard=%zu stretch=%zu ring=%zu cube=%zu butterfly=%zu strip=%zu\n", shape_n[0],
              shape_n[1], shape_n[2], shape_n[3], shape_n[4], shape_n[5]);
  std::printf("sprites: w[%.4g..%.4g] h[%.4g..%.4g] dx[%.4g..%.4g] dy[%.4g..%.4g]\n", wmin, wmax, hmin, hmax, dxmin,
              dxmax, dymin, dymax);
  {
    // Sprite picks per rank (0,1 = picker picks, 2.. = editor extras); strips are rank 0 and excluded.
    size_t rank_n[kMaxRank] = {};
    for (const IdResult& ir : results) {
      for (const SlotResult& sr : ir.slots) {
        for (const Baked& b : sr.particles) {
          if (b.shape != 5 && b.rank < kMaxRank) ++rank_n[b.rank];
        }
      }
    }
    std::printf("sprite picks per rank:");
    for (int r = 0; r < kMaxRank; ++r) std::printf(" r%d=%zu", r, rank_n[r]);
    std::printf("\n");
  }
  {
    // Strip statistics.
    size_t n_strip = 0, pts_total = 0, pts_pre = 0;
    std::map<int, size_t> hist;  // vertex count histogram (ribbon / trail)
    size_t n_ribbon = 0, n_trail = 0;
    float max_epos = 0, max_eattr = 0, len_min = 1e9f, len_max = 0, wmx = 0;
    std::vector<float> lens, widths;
    for (const IdResult& ir : results) {
      for (const SlotResult& sr : ir.slots) {
        for (const Baked& b : sr.particles) {
          if (b.shape != 5) continue;
          ++n_strip;
          (b.ribbon ? n_ribbon : n_trail)++;
          pts_total += b.pts.size();
          pts_pre += static_cast<size_t>(b.total_pts);
          ++hist[static_cast<int>(b.pts.size())];
          max_epos = std::max(max_epos, b.err_pos);
          max_eattr = std::max(max_eattr, b.err_attr);
          len_min = std::min(len_min, b.h);
          len_max = std::max(len_max, b.h);
          wmx = std::max(wmx, b.w);
          lens.push_back(b.h);
          widths.push_back(b.w);
        }
      }
    }
    std::sort(lens.begin(), lens.end());
    std::sort(widths.begin(), widths.end());
    auto pct = [](const std::vector<float>& v, double q) {
      return v.empty() ? 0.0f : v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
    };
    std::printf("strips=%zu (ribbon=%zu trail=%zu), vertices kept=%zu of %zu (pool %zu), avg %.2f/strip\n", n_strip,
                n_ribbon, n_trail, pts_total, pts_pre, strip_pool.size(),
                n_strip ? static_cast<double>(pts_total) / static_cast<double>(n_strip) : 0.0);
    std::printf("strip length (world, undecimated) min %.3g p10 %.3g p50 %.3g p90 %.3g max %.3g; peak width p10 %.3g "
                "p50 %.3g p90 %.3g max %.3g\n",
                len_min, pct(lens, 0.1), pct(lens, 0.5), pct(lens, 0.9), len_max, pct(widths, 0.1), pct(widths, 0.5),
                pct(widths, 0.9), wmx);
    std::printf("decimation max error: pos %.4g world units, attr %.1f%% of range\n", max_epos, max_eattr * 100.0f);
    std::printf("vertex-count histogram:");
    for (const auto& kv : hist) std::printf(" %d:%zu", kv.first, kv.second);
    std::printf("\n");
  }
  {
    // Representation of expected kinds, vs. round-1 rules (sprite bodies only).
    size_t r_sprite = 0, r_sprite_trail = 0, r_sprite_ribbon = 0, r_trail = 0, r_ribbon = 0, r_missing = 0;
    size_t round1_missing = 0, now_represented = 0;
    std::map<std::string, size_t> reasons;
    std::map<std::string, size_t> recovered_by;
    for (const IdResult& ir : results) {
      for (const SlotResult& sr : ir.slots) {
        for (const KindReport& rep : sr.reports) {
          if (!sr.expected_kinds.count(rep.kind)) continue;
          if (rep.repr == "sprite") ++r_sprite;
          else if (rep.repr == "sprite+trail") ++r_sprite_trail;
          else if (rep.repr == "sprite+ribbon") ++r_sprite_ribbon;
          else if (rep.repr == "trail" || rep.repr == "trail+trail") ++r_trail;
          else if (rep.repr == "ribbon") ++r_ribbon;
          else ++r_missing;
          if (rep.round1_missing) {
            ++round1_missing;
            if (rep.represented) {
              ++now_represented;
              ++recovered_by[rep.repr];
            } else {
              ++reasons[rep.reason];
            }
          }
          if (do_analyze) {
            std::printf("KIND id=%d slot=%d kind=%u repr=%s vertices=%d/%d err_pos=%.4f err_attr=%.1f%%%s%s\n", ir.id,
                        sr.plan.slot, rep.kind, rep.repr.c_str(), rep.kept, rep.total, rep.err_pos,
                        rep.err_attr * 100.0f, rep.reason.empty() ? "" : " note=", rep.reason.c_str());
          }
        }
      }
    }
    std::printf("representation of expected kinds: sprite=%zu sprite+trail=%zu sprite+ribbon=%zu trail=%zu ribbon=%zu "
                "missing=%zu\n",
                r_sprite, r_sprite_trail, r_sprite_ribbon, r_trail, r_ribbon, r_missing);
    std::printf("round-1 unrepresented kinds=%zu, now represented=%zu, still missing=%zu\n", round1_missing,
                now_represented, round1_missing - now_represented);
    for (const auto& kv : recovered_by) std::printf("  recovered as %s: %zu\n", kv.first.c_str(), kv.second);
    for (const auto& kv : reasons) std::printf("  still missing (%s): %zu\n", kv.first.c_str(), kv.second);
  }
  if (do_analyze) {
    for (const IdResult& ir : results) {
      for (const SlotResult& sr : ir.slots) {
        for (uint32_t k : sr.missing_kinds) {
          const KindStats ks = sr.kinds.count(k) ? sr.kinds.at(k) : KindStats{};
          std::string why;
          for (const KindReport& rep : sr.reports) {
            if (rep.kind == k) why = rep.reason;
          }
          std::printf("MISSING %s id=%d slot=%d kind=%u seen=%d trail_only=%d dead_alpha=%d unnamed_other=%d "
                      "no_sprite=%d zero_size=%d behind=%d trails=%d(no_width=%d faint=%d short=%d offscreen=%d)\n",
                      why.c_str(), ir.id, sr.plan.slot, k, ks.seen, ks.skip_trail_only, ks.skip_dead_alpha,
                      ks.skip_unnamed_other, ks.skip_no_sprite, ks.skip_zero_size, ks.skip_behind_camera,
                      ks.trail_seen, ks.trail_no_width, ks.trail_faint, ks.trail_short, ks.trail_offscreen);
        }
      }
    }
  }

  // ---- debug tsv ----
  if (!debug_tsv.empty()) {
    FILE* f = std::fopen(debug_tsv.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "cannot write %s\n", debug_tsv.c_str());
      return 1;
    }
    for (const IdResult& ir : results) {
      for (int c : {1, 2, 3, 4, 5, 6}) {
        for (int l = 0; l <= c; ++l) {
          float lx = 0;
          wds::chart_editor::official_split_line_world_x(c, l, lx);
          std::fprintf(f, "L\t%d\t%d\t%d\t%d\t%.4f\n", ir.id, c, l,
                       wds::chart_editor::split_color_slot(ir.id, c, l) % ir.element_count, lx);
        }
      }
      for (const SlotResult& sr : ir.slots) {
        for (const Baked& b : sr.particles) {
          // Columns 20/21 (strip_first, strip_count) are new; Q rows give the strip point pool.
          std::fprintf(f,
                       "P\t%d\t%d\t%d\t%u\t%u\t%u\t%u\t%d\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%d\t%.2f\t%u\t%zu\n",
                       ir.id, sr.plan.slot, b.catalog_index, b.uv, b.blend, b.shape, b.flip, b.sorting_order, b.dx,
                       b.dy, b.w, b.h, b.angle, b.r, b.g, b.b, b.a, b.tier, b.t, b.strip_first,
                       b.shape == 5 ? b.pts.size() : size_t{0});
        }
      }
    }
    {
      std::set<uint16_t> uvs;
      for (const IdResult& ir : results) {
        for (const SlotResult& sr : ir.slots) {
          for (const Baked& b : sr.particles) {
            if (b.shape != 5 && b.uv != 0xFFFF) uvs.insert(b.uv);
          }
        }
      }
      for (uint16_t uv : uvs) {
        const wds::chart_render::SplitEffectUvTile* t = wds::chart_render::official_split_effect_uv_tile(uv);
        if (t != nullptr && t->present != 0) {
          std::fprintf(f, "T\t%u\t%u\t%u\t%u\t%u\n", uv, t->x, t->y, t->w, t->h);
        }
      }
    }
    for (size_t i = 0; i < strip_pool.size(); ++i) {
      const StripPt& q = strip_pool[i];
      std::fprintf(f, "Q\t%zu\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\t%.5g\n", i, q.x, q.y, q.w, q.u, q.r, q.g, q.b,
                   q.a);
    }
    std::fclose(f);
  }

  // ---- .inc ----
  if (!out_inc.empty()) {
    std::string s;
    s += "// GENERATED FILE - do not edit. Produced by chart-render/tools/split_effect_snapshot_bake.cpp\n";
    s += "// from the real split-effect simulation (collect_split_effect_particles, quality pinned to Normal,\n";
    s += "// type Full, now_t = 0.5 s into fade-in, sentinel start_ms 10000000).\n";
    s += "// Regenerate:\n";
    s += "//   cmake --build build --target wds_split_effect_snapshot_bake\n";
    s += "//   build/chart-render/wds_split_effect_snapshot_bake --out-inc ui/src/qt/split_effect_snapshot_table.inc\n";
    s += "// Keying: one block of particles per (color_id, slot); slot = split_color_slot(color_id, split_count,\n";
    s += "// line) % slot_count. Offsets are relative to the owning line (dx = world_x - line x, dy = main_y -\n";
    s += "// line origin main_y); the sim result is line-position independent apart from per-line RNG seeds.\n";
    s += "// Colour: r,g,b,a as the preview shades them (unclamped). For blend 2 the preview's x2 trail intensity\n";
    s += "// (rgb AND alpha) is NOT baked; the renderer applies it. This holds for strips too: strip vertex\n";
    s += "// colours are the RAW trail colours (trail_color_* / trail_r.. as the sim delivers them); the preview\n";
    s += "// multiplies them by official_split_effect_trail_intensity (2 for blend 2, else 1) at draw time, the\n";
    s += "// renderer must apply the same x2 to rgb AND alpha of every blend-2 strip vertex.\n";
    s += "// Row layout: {catalog_index, uv, blend, shape, flip, sorting_order, rank, dx, dy, w, h, angle_deg, r, g, b, a,\n";
    s += "//              strip_first, strip_count}\n";
    s += "// shape: 0 billboard, 1 stretched (w = length along U), 2 ring, 3 cube, 4 butterfly, 5 strip.\n";
    s += "// Strips (shape 5): vertices live in kSnapStripPoints {dx, dy, width, u, r, g, b, a}; dx,dy are offsets\n";
    s += "// from the line origin like sprites, width is the FULL width in world units, entry r,g,b,a = 1,\n";
    s += "// entry w/h are informational (peak width / polyline length). Vertex order = the preview's vertex order:\n";
    s += "// ribbon chains run oldest -> newest (u = arc-length fraction of the whole 3D chain, 0 oldest, 1 newest),\n";
    s += "// particle-mode trails run head -> tail (u = vertex index / (n-1) of the undecimated trail, 0 head).\n";
    s += "// u is the preview's along-trail coordinate (it feeds it as texture V; across the strip the texture U\n";
    s += "// runs 0..1 from the +perp edge to the -perp edge). Positions drop the 3D z (top-down frame like sprites);\n";
    s += "// u keeps the 3D arc length. Strips are decimated per kind (Douglas-Peucker in dx,dy with width / alpha /\n";
    s += "// colour / u preserved to ~10% of their range, tolerance 0.025 world units, [2, 32] vertices; <= 12 for a\n";
    s += "// particle-mode trail that accompanies a sprite body). Blend/uv of a strip are the trail material's:\n";
    s += "// blend 0/1 draw with the white texture (uv = 0xFFFF), blend 2 with bomb_effect_trail.png.\n";
    s += "// Draw order inside a slot = preview order: (sorting_order, group, catalog_index, birth_t, seed, seq) with\n";
    s += "// group 0 = particles (a particle's own trail strip directly after its body), group 1 = ribbon chains\n";
    s += "// (the preview emits ribbons after the particles of the line, then stable-sorts by sorting_order only).\n";
    std::vector<std::pair<uint32_t, uint16_t>> slot_rows_v;  // begin, count
    std::string particles;
    uint32_t pidx = 0;
    std::string ids_s;
    uint32_t slot_row = 0;
    std::string slots_s;
    for (const IdResult& ir : results) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "    {%d, %d, %u},\n", ir.id, ir.element_count, slot_row);
      ids_s += buf;
      // Rows for slots 0..element_count-1 (slots never reached stay empty).
      for (int slot = 0; slot < ir.element_count; ++slot) {
        const SlotResult* found = nullptr;
        for (const SlotResult& sr : ir.slots) {
          if (sr.plan.slot == slot) found = &sr;
        }
        const uint32_t begin = pidx;
        const size_t n = found ? found->particles.size() : 0;
        if (found) {
          for (const Baked& b : found->particles) {
            std::snprintf(buf, sizeof buf, "{%d,%u,%u,%u,%u,%d,%u,", b.catalog_index, b.uv, b.blend, b.shape, b.flip,
                          b.sorting_order, static_cast<unsigned>(b.rank));
            particles += buf;
            const float v[9] = {b.dx, b.dy, b.w, b.h, b.angle, b.r, b.g, b.b, b.a};
            for (int k = 0; k < 9; ++k) {
              fmt_f(particles, v[k]);
              particles += ",";
            }
            std::snprintf(buf, sizeof buf, "%u,%zu},\n", b.strip_first, b.shape == 5 ? b.pts.size() : size_t{0});
            particles += buf;
            ++pidx;
          }
        }
        std::snprintf(buf, sizeof buf, "    {%u, %zu},\n", begin, n);
        slots_s += buf;
        ++slot_row;
      }
    }
    std::string strip_s;
    for (const StripPt& q : strip_pool) {
      strip_s += "{";
      const float v[8] = {q.x, q.y, q.w, q.u, q.r, q.g, q.b, q.a};
      for (int k = 0; k < 8; ++k) {
        fmt_f(strip_s, v[k]);
        strip_s += (k == 7) ? "},\n" : ",";
      }
    }
    s += "constexpr SplitSnapStripPoint kSnapStripPoints[] = {\n";
    s += strip_s.empty() ? std::string("    {}\n") : strip_s;
    s += "};\n";
    s += "constexpr SplitSnapParticle kSnapParticles[] = {\n";
    s += particles.empty() ? std::string("    {}\n") : particles;
    s += "};\n";
    s += "constexpr SnapSlot kSnapSlots[] = {\n" + slots_s + "};\n";
    s += "constexpr SnapId kSnapIds[] = {\n" + ids_s + "};\n";
    FILE* f = std::fopen(out_inc.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "cannot write %s\n", out_inc.c_str());
      return 1;
    }
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    std::printf("wrote %s (%zu bytes, %u entries, %zu strip points, %u slot rows)\n", out_inc.c_str(), s.size(), pidx,
                strip_pool.size(), slot_row);
  }
  return 0;
}

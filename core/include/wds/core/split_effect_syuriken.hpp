#pragma once

#include <wds/core/official_playfield.hpp>
#include <wds/core/official_split_x.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_editor {

// CPU port of OpenWDS SplitEffectSyuriken.shader. Does not sample SplitLine.
// inner = uv.xxyx → iz is V, iw is U (with edge, both sides fade).
// alpha = (pulse * tail + colorMask) * a; blend is SrcAlpha One.
//
// Only pulse uses time_sec. RGB / color_mask / tail / local_y are static for
// a given (u,v,local_y,rgb) so the preview can cache them and re-apply pulse
// each frame (bit-exact vs calling official_syuriken_fragment).
struct OfficialSyurikenStatic {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float color_mask = 0.0f;
  float tail = 0.0f;
  float local_y = 0.0f;
};

inline OfficialSyurikenStatic official_syuriken_static(float u, float v, float local_y, float cr,
                                                       float cg, float cb) noexcept {
  auto saturate = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
  auto hermite = [&](float x) {
    const float s = saturate(x);
    return s * s * (3.0f - 2.0f * s);
  };
  auto pow_sat = [](float x, float exp) {
    const float a = std::fabs(x);
    if (a <= 0.0f) {
      return 0.0f;
    }
    if (a >= 1.0f) {
      return 1.0f;
    }
    return std::pow(a, exp);
  };

  const float ox = hermite((u - 0.425000012f) * 1.73913050f);
  const float oy = hermite((u - 0.574999988f) * -1.73913050f);
  const float oz = hermite((v - 1.02500010f) * 1.73913050f);
  const float ow = hermite((v - 1.17499995f) * -1.73913050f);
  const float outer_pair_x = 1.0f - (ow + oz);
  const float outer_pair_y = 1.0f - (oy + ox);
  float color_mask = 1.0f - outer_pair_x * outer_pair_y;
  color_mask = std::min(pow_sat(color_mask, 4.0f), 1.0f);

  const float ix = hermite((u - 0.25f) * 1.33333337f);
  const float iy = hermite((u - 0.75f) * -1.33333337f);
  const float iz = hermite((v - 0.25f) * 1.33333337f);
  const float iw = hermite((u - 0.544999957f) * 2.19780207f);
  const float inner_pair = 1.0f - (iy + ix);
  const float inner_reverse = 1.0f - iz;
  float checker = 1.0f - inner_reverse * inner_pair;
  checker = 1.0f - std::min(pow_sat(checker, 1200.0f), 1.0f);
  color_mask = (1.0f - color_mask) * checker;

  const float max_c = std::max(cr, std::max(cg, cb));
  float edge = hermite((u - 0.455000013f) * -2.19780207f);
  edge = std::pow(std::max(edge + iw, 0.0f), 0.4f);
  const float tail = (1.0f - edge) * (1.0f - std::min(pow_sat(iz, 80.0f), 1.0f));

  OfficialSyurikenStatic out;
  out.r = cr + (max_c - cr) * color_mask;
  out.g = cg + (max_c - cg) * color_mask;
  out.b = cb + (max_c - cb) * color_mask;
  out.color_mask = color_mask;
  out.tail = tail;
  out.local_y = local_y;
  return out;
}

// hop / live dump darkband-orig-v1 (2026-10-17). Do not label 2022.3.
//
// OpenWDS SplitEffectSyuriken.shader writes
//   pulse = sin((localY - 20 t) * length(GetObjectToWorldMatrix()[1]))
// with localY = TransformWorldToObject(positionWS).y. That is the unbatched
// object matrix (GPU probe align2-u3-unity-v1 G=13.49, analog 43 Hz). The
// original SpriteRenderer path does not see that matrix: Unity 2022.3
// DrawCallBatching lists Sprite Renderers; dynamic batching transforms
// vertices to world space on the CPU
// (https://docs.unity3d.com/2022.3/Documentation/Manual/DrawCallBatching.html
// https://docs.unity3d.com/2022.3/Documentation/Manual/dynamic-batching.html)
// so the shader ObjectToWorld is identity, localY = positionWS.y, wys = 1,
// pulse = sin(positionWS.y - 20 t) ≈ 3.18 Hz. SRP Batcher requires a mesh /
// skinned mesh (https://docs.unity3d.com/2022.3/Documentation/Manual/SRPBatcher.html)
// so SpriteRenderer stays on the standard path; URP m_SupportsDynamicBatching=0
// still batches sprites (Hamming-1 srp_off / dyn_on did not switch to 43 Hz).
// Prior GPU probe assigned r.material.shader, which copies the material and
// breaks batching (same DrawCallBatching page).
//
// Original-path Game-view luma (no shader/material/renderer swap),
// docs/split-effect-parity/results/darkband-orig-v1/:
//   default_60hz 1.48 s per-pixel Goertzel peak 3.20 Hz,
//   amp@3.18 wmean 0.026 vs amp@43 0.001 vs 17 Hz alias 0.001;
//   captureFramerate=1000 79 ms window cannot resolve 3.18, but
//   amp@43 stays ~4–5× below 3.18 (Hamming-1 srp_off / dyn_on same).
// Native installed capture-preview (paused seeks 4.0+k/60, 90 frames)
// matches: peak 3.20 Hz, amp ratio 38×, xcorr −16 px/frame = up.
// Transform.position.y of _Line1 = -4.45 = identity origin main_y (−8.9) ×
// cos(kOfficialLaneTiltDeg). LaneGroup-local z=0 → Unity world Y = main_y ×
// cos(tilt) = project_main_xyz wy. Shared path, not per ID.
//
// Vertex alpha packing: DrawBatch culls quads with all a<=0, so emit stores
// (worldY + kOfficialSyurikenPulseWorldYBias) * q. Preview plates are already
// projected 2D trapezoids drawn with an ortho MVP (clip w=1); GPU interpolation
// of raw worldY is screen-linear and makes far-lane dark bands travel ~8× the
// official screen speed. q is the same Heckbert / parallel-edge ratio
// fill_quad_corner_geom already writes for UV (hop: 2D overlay stand-in for
// Unity perspective-correct positionWS.y). Fragment: packed / q − bias.
// Do not label 2022.3.
inline constexpr float kOfficialSyurikenPulseWorldYBias = 64.0f;

inline float official_syuriken_pulse_q(float q) noexcept {
  return std::max(q, 1e-6f);
}

inline float official_syuriken_pulse_position_ws_y(float main_y) noexcept {
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  return main_y * std::cos(rad);
}

inline float official_syuriken_pulse_pack_alpha(float position_ws_y, float q) noexcept {
  return (position_ws_y + kOfficialSyurikenPulseWorldYBias) * official_syuriken_pulse_q(q);
}

inline float official_syuriken_pulse_unpack_ws_y(float packed_alpha, float q) noexcept {
  return packed_alpha / official_syuriken_pulse_q(q) - kOfficialSyurikenPulseWorldYBias;
}

// Unbatched ObjectToWorld row-1 length. Kept for the GPU-probe record; pulse
// no longer multiplies 20 t by this (see hop above).
inline float official_syuriken_pulse_world_y_scale(int32_t color_id, float fade_scale) noexcept {
  float ex_x = kOfficialSplitLineLocalScaleX;
  float ex_y = 0.0f;
  float ex_z = 0.0f;
  float ey_x = 0.0f;
  float ey_y = 1.0f;
  float ey_z = 0.0f;
  float ez_x = 0.0f;
  float ez_y = 0.0f;
  float ez_z = 1.0f;
  ey_y *= fade_scale;
  if (split_fade_grows_from_tip(color_id)) {
    ex_x = -ex_x;
    ex_y = -ex_y;
    ey_x = -ey_x;
    ey_y = -ey_y;
    ez_x = -ez_x;
    ez_y = -ez_y;
  }
  ex_y *= kOfficialLineHeightScale;
  ey_y *= kOfficialLineHeightScale;
  ez_y *= kOfficialLineHeightScale;
  const float rad = official_deg_to_rad(kOfficialLaneTiltDeg);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float r1x = ex_y * c - ex_z * s;
  const float r1y = ey_y * c - ey_z * s;
  const float r1z = ez_y * c - ez_z * s;
  return std::sqrt(r1x * r1x + r1y * r1y + r1z * r1z);
}

inline void official_syuriken_apply_pulse(const OfficialSyurikenStatic& st, float time_sec,
                                          float position_ws_y, float ca, float& out_r, float& out_g,
                                          float& out_b, float& out_a) noexcept {
  out_r = st.r;
  out_g = st.g;
  out_b = st.b;
  const float pulse = std::min(std::sin(position_ws_y - 20.0f * time_sec) + 1.7f, 1.0f);
  out_a = (pulse * st.tail + st.color_mask) * ca;
}

inline void official_syuriken_fragment(float u, float v, float local_y, float time_sec,
                                       float position_ws_y, float cr, float cg, float cb,
                                       float ca, float& out_r, float& out_g, float& out_b,
                                       float& out_a) noexcept {
  official_syuriken_apply_pulse(official_syuriken_static(u, v, local_y, cr, cg, cb), time_sec,
                                position_ws_y, ca, out_r, out_g, out_b, out_a);
}

}  // namespace wds::chart_editor

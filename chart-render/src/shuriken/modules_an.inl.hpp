#pragma once

#include "internal.hpp"
#include "simd_dispatch.hpp"
#include "split_wall_profile.hpp"

#include <wds/core/official_playfield.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#ifndef WDS_AN_OP
#define WDS_AN_OP 1
#endif

// hop (not 2022.3): analytic curl inlined into modules.cpp / integrate.cpp
// on Clang. Contraction is forced off ONLY around these definitions:
//   Clang: #pragma clang fp contract(off) survives always_inline into a
//          contract-on caller (probe: fmul+fadd, not fmadd).
//   GCC:   #pragma STDC FP_CONTRACT is ignored in C++. #pragma GCC
//          optimize("fp-contract=off") and optimize attr do NOT survive
//          always_inline into -ffp-contract=fast (probe: fmadd). Public
//          AN entry points are noinline+optimize("fp-contract=off") so
//          MinGW compiles the whole body contract-off. Helpers still
//          inline into those entries.
//   MSVC:  #pragma float_control(precise, on, push)
// LTO across a contract-off TU is not used: GCC LTO may drop per-TU
// fp-contract when inlining, so it is not provable on MinGW + Clang.
// FD stays in modules.cpp with default contraction (golden-grid-v2-ref).

// WDS_AN_OP: 1 = explicit IEEE fma / vfmaq / fmadd (adopted). Contraction
// stays off around everything else so Clang/GCC cannot fuse a different
// set. 0 = contract-off fmul+fadd (previous golden). 2 = old compiler-
// contracted add (pragma contract on, no explicit fma). hop, not 2022.3.
#if WDS_AN_OP != 2
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif
#endif

namespace wds::chart_render::shuriken {

#if defined(__clang__) || defined(__GNUC__)
#define WDS_NOISE_INLINE inline __attribute__((always_inline))
#else
#define WDS_NOISE_INLINE inline
#endif
// Clang: always_inline; #pragma clang fp contract(off) survives inlining
// into a contract-on caller (probe: fmul+fadd).
// GCC: #pragma GCC optimize / optimize("fp-contract=off") do NOT survive
// always_inline into a -ffp-contract=fast caller (probe: fmadd). Public
// AN entry points stay noinline+optimize so the whole body, including
// inlined helpers, is compiled contract-off. hop, not 2022.3.
#if defined(__clang__)
#define WDS_AN_ENTRY inline __attribute__((always_inline))
#elif defined(__GNUC__)
#define WDS_AN_ENTRY inline __attribute__((noinline, optimize("fp-contract=off")))
#else
#define WDS_AN_ENTRY inline
#endif

namespace an_detail {

// IEEE r = a*b + c. Kind 1 is exact fma; 0/2 are separate mul+add.
WDS_NOISE_INLINE float an_madd(float a, float b, float c) noexcept {
#if WDS_AN_OP == 1
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_fmaf(a, b, c);
#else
  return std::fma(a, b, c);
#endif
#else
  return a * b + c;
#endif
}

WDS_NOISE_INLINE float fade(float t) noexcept {
  const float inner = an_madd(t, an_madd(t, 6.0f, -15.0f), 10.0f);
  return (t * t * t) * inner;
}

WDS_NOISE_INLINE int perm(int i) noexcept {
  // Ken Perlin permutation (256), folded.
  static constexpr uint8_t kP[256] = {
      151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,
      103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,
      0,   26,  197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149,
      56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166,
      77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,
      245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187,
      208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186,
      3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212,
      207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248,
      152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
      19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,
      242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107,
      49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,
      150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,
      215, 61,  156, 180};
  return kP[i & 255];
}

struct NoiseAxis {
  int cell;
  float frac;
  float w;
};

WDS_NOISE_INLINE NoiseAxis make_noise_axis(float p) noexcept {
  const float fl = std::floor(p);
  NoiseAxis a;
  a.cell = static_cast<int>(fl) & 255;
  a.frac = p - fl;
  a.w = fade(a.frac);
  return a;
}

struct CornerHashes {
  int aa, ba, ab, bb, aa1, ba1, ab1, bb1;
};

WDS_NOISE_INLINE CornerHashes corner_hashes(int X, int Y, int Z) noexcept {
  const int A = perm(X) + Y;
  const int AA = perm(A) + Z;
  const int AB = perm(A + 1) + Z;
  const int B = perm(X + 1) + Y;
  const int BA = perm(B) + Z;
  const int BB = perm(B + 1) + Z;
  CornerHashes h;
  h.aa = perm(AA);
  h.ba = perm(BA);
  h.ab = perm(AB);
  h.bb = perm(BB);
  h.aa1 = perm(AA + 1);
  h.ba1 = perm(BA + 1);
  h.ab1 = perm(AB + 1);
  h.bb1 = perm(BB + 1);
  return h;
}

// hop (not 2022.3): analytic ∇ of Ken Perlin. fade(t)=6t^5-15t^4+10t^3,
// fade'(t)=30t^2(t-1)^2. Same lattice / grad / remap as perlin3.
WDS_NOISE_INLINE float fade_deriv(float t) noexcept {
  const float a = t * (t - 1.0f);
  return 30.0f * a * a;
}

WDS_NOISE_INLINE void grad_with_d(int hash, float x, float y, float z, float& v, float& dx,
                                  float& dy, float& dz) noexcept {
  const int h = hash & 15;
  const float u = h < 8 ? x : y;
  const float w = h < 4 ? y : (h == 12 || h == 14 ? x : z);
  const float su = ((h & 1) == 0) ? 1.0f : -1.0f;
  const float sv = ((h & 2) == 0) ? 1.0f : -1.0f;
  v = su * u + sv * w;
  dx = 0.0f;
  dy = 0.0f;
  dz = 0.0f;
  if (h < 8) {
    dx = su;
  } else {
    dy = su;
  }
  if (h < 4) {
    dy += sv;
  } else if (h == 12 || h == 14) {
    dx += sv;
  } else {
    dz += sv;
  }
}

// Returns L = perlin3*2-1 (the fbm octave) and dL/d(x,y,z).
WDS_NOISE_INLINE void perlin3_value_grad(float x, float y, float z, float& value, float& dx,
                                         float& dy, float& dz) noexcept {
  const NoiseAxis X = make_noise_axis(x);
  const NoiseAxis Y = make_noise_axis(y);
  const NoiseAxis Z = make_noise_axis(z);
  const CornerHashes h = corner_hashes(X.cell, Y.cell, Z.cell);
  const float xf = X.frac;
  const float yf = Y.frac;
  const float zf = Z.frac;
  const float u = X.w;
  const float v = Y.w;
  const float w = Z.w;
  const float du = fade_deriv(xf);
  const float dv = fade_deriv(yf);
  const float dw = fade_deriv(zf);

  float gAA, aax, aay, aaz;
  grad_with_d(h.aa, xf, yf, zf, gAA, aax, aay, aaz);
  float gBA, bax, bay, baz;
  grad_with_d(h.ba, xf - 1.0f, yf, zf, gBA, bax, bay, baz);
  float gAB, abx, aby, abz;
  grad_with_d(h.ab, xf, yf - 1.0f, zf, gAB, abx, aby, abz);
  float gBB, bbx, bby, bbz;
  grad_with_d(h.bb, xf - 1.0f, yf - 1.0f, zf, gBB, bbx, bby, bbz);
  float gAA1, aa1x, aa1y, aa1z;
  grad_with_d(h.aa1, xf, yf, zf - 1.0f, gAA1, aa1x, aa1y, aa1z);
  float gBA1, ba1x, ba1y, ba1z;
  grad_with_d(h.ba1, xf - 1.0f, yf, zf - 1.0f, gBA1, ba1x, ba1y, ba1z);
  float gAB1, ab1x, ab1y, ab1z;
  grad_with_d(h.ab1, xf, yf - 1.0f, zf - 1.0f, gAB1, ab1x, ab1y, ab1z);
  float gBB1, bb1x, bb1y, bb1z;
  grad_with_d(h.bb1, xf - 1.0f, yf - 1.0f, zf - 1.0f, gBB1, bb1x, bb1y, bb1z);

  const float x1 = an_madd(u, gBA - gAA, gAA);
  const float x1x = an_madd(du, gBA - gAA, an_madd(u, bax - aax, aax));
  const float x1y = an_madd(u, bay - aay, aay);
  const float x1z = an_madd(u, baz - aaz, aaz);
  const float x2 = an_madd(u, gBB - gAB, gAB);
  const float x2x = an_madd(du, gBB - gAB, an_madd(u, bbx - abx, abx));
  const float x2y = an_madd(u, bby - aby, aby);
  const float x2z = an_madd(u, bbz - abz, abz);
  const float y1 = an_madd(v, x2 - x1, x1);
  const float y1x = an_madd(v, x2x - x1x, x1x);
  const float y1y = an_madd(dv, x2 - x1, an_madd(v, x2y - x1y, x1y));
  const float y1z = an_madd(v, x2z - x1z, x1z);
  const float x3 = an_madd(u, gBA1 - gAA1, gAA1);
  const float x3x = an_madd(du, gBA1 - gAA1, an_madd(u, ba1x - aa1x, aa1x));
  const float x3y = an_madd(u, ba1y - aa1y, aa1y);
  const float x3z = an_madd(u, ba1z - aa1z, aa1z);
  const float x4 = an_madd(u, gBB1 - gAB1, gAB1);
  const float x4x = an_madd(du, gBB1 - gAB1, an_madd(u, bb1x - ab1x, ab1x));
  const float x4y = an_madd(u, bb1y - ab1y, ab1y);
  const float x4z = an_madd(u, bb1z - ab1z, ab1z);
  const float y2 = an_madd(v, x4 - x3, x3);
  const float y2x = an_madd(v, x4x - x3x, x3x);
  const float y2y = an_madd(dv, x4 - x3, an_madd(v, x4y - x3y, x3y));
  const float y2z = an_madd(v, x4z - x3z, x3z);
  value = an_madd(w, y2 - y1, y1);
  dx = an_madd(w, y2x - y1x, y1x);
  dy = an_madd(w, y2y - y1y, y1y);
  dz = an_madd(dw, y2 - y1, an_madd(w, y2z - y1z, y1z));
}

WDS_NOISE_INLINE void fbm_value_grad(float x, float y, float z, int octaves, float lac, float gain,
                                     float& value, float& dx, float& dy, float& dz) noexcept {
  const int n = std::clamp(octaves, 1, 8);
  if (n == 1) {
    perlin3_value_grad(x, y, z, value, dx, dy, dz);
    return;
  }
  float sum = 0.0f;
  float sdx = 0.0f;
  float sdy = 0.0f;
  float sdz = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    float lv = 0.0f, lx = 0.0f, ly = 0.0f, lz = 0.0f;
    perlin3_value_grad(x * freq, y * freq, z * freq, lv, lx, ly, lz);
    sum = an_madd(lv, amp, sum);
    const float fam = freq * amp;
    sdx = an_madd(lx, fam, sdx);
    sdy = an_madd(ly, fam, sdy);
    sdz = an_madd(lz, fam, sdz);
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  if (norm <= 1e-6f) {
    value = 0.0f;
    dx = dy = dz = 0.0f;
    return;
  }
  const float inv = 1.0f / norm;
  value = sum * inv;
  dx = sdx * inv;
  dy = sdy * inv;
  dz = sdz * inv;
}

#if defined(__clang__) || defined(__GNUC__)
typedef float wds_v4f __attribute__((vector_size(16)));

WDS_NOISE_INLINE wds_v4f v4_load(const float* p) noexcept {
  wds_v4f r;
  std::memcpy(&r, p, 16);
  return r;
}

WDS_NOISE_INLINE void v4_store(float* p, wds_v4f v) noexcept {
  std::memcpy(p, &v, 16);
}

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
// noinline: Clang refuses to always_inline a target("avx2,fma") callee into
// a default-ISA v4_madd. Runtime Avx2 check is in the caller. hop.
__attribute__((target("avx2,fma"), noinline)) inline wds_v4f v4_madd_avx2_fma(wds_v4f a, wds_v4f b,
                                                                              wds_v4f c) noexcept {
  const __m128 r = _mm_fmadd_ps(*reinterpret_cast<const __m128*>(&a),
                                *reinterpret_cast<const __m128*>(&b),
                                *reinterpret_cast<const __m128*>(&c));
  wds_v4f out;
  *reinterpret_cast<__m128*>(&out) = r;
  return out;
}
#endif

WDS_NOISE_INLINE wds_v4f v4_madd(wds_v4f a, wds_v4f b, wds_v4f c) noexcept {
#if WDS_AN_OP == 1
#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
  const float32x4_t r = vfmaq_f32(*reinterpret_cast<const float32x4_t*>(&c),
                                  *reinterpret_cast<const float32x4_t*>(&a),
                                  *reinterpret_cast<const float32x4_t*>(&b));
  wds_v4f out;
  *reinterpret_cast<float32x4_t*>(&out) = r;
  return out;
#else
#if defined(__x86_64__) || defined(_M_X64)
  if (split_simd_path() == SplitSimdPath::Avx2) {
    return v4_madd_avx2_fma(a, b, c);
  }
#endif
  wds_v4f r;
  r[0] = an_madd(a[0], b[0], c[0]);
  r[1] = an_madd(a[1], b[1], c[1]);
  r[2] = an_madd(a[2], b[2], c[2]);
  r[3] = an_madd(a[3], b[3], c[3]);
  return r;
#endif
#else
  return a * b + c;
#endif
}

WDS_NOISE_INLINE void perlin3_value_grad_x4_vec(const float x[4], const float y[4], const float z[4],
                                                float value[4], float dx[4], float dy[4],
                                                float dz[4]) noexcept {
  float xf[4], yf[4], zf[4], u[4], v[4], w[4], du[4], dv[4], dw[4];
  int Xc[4], Yc[4], Zc[4];
  for (int i = 0; i < 4; ++i) {
    const float flx = std::floor(x[i]);
    const float fly = std::floor(y[i]);
    const float flz = std::floor(z[i]);
    Xc[i] = static_cast<int>(flx) & 255;
    Yc[i] = static_cast<int>(fly) & 255;
    Zc[i] = static_cast<int>(flz) & 255;
    xf[i] = x[i] - flx;
    yf[i] = y[i] - fly;
    zf[i] = z[i] - flz;
    u[i] = fade(xf[i]);
    v[i] = fade(yf[i]);
    w[i] = fade(zf[i]);
    du[i] = fade_deriv(xf[i]);
    dv[i] = fade_deriv(yf[i]);
    dw[i] = fade_deriv(zf[i]);
  }

  float gAA[4], aax[4], aay[4], aaz[4];
  float gBA[4], bax[4], bay[4], baz[4];
  float gAB[4], abx[4], aby[4], abz[4];
  float gBB[4], bbx[4], bby[4], bbz[4];
  float gAA1[4], aa1x[4], aa1y[4], aa1z[4];
  float gBA1[4], ba1x[4], ba1y[4], ba1z[4];
  float gAB1[4], ab1x[4], ab1y[4], ab1z[4];
  float gBB1[4], bb1x[4], bb1y[4], bb1z[4];
  for (int i = 0; i < 4; ++i) {
    const CornerHashes h = corner_hashes(Xc[i], Yc[i], Zc[i]);
    grad_with_d(h.aa, xf[i], yf[i], zf[i], gAA[i], aax[i], aay[i], aaz[i]);
    grad_with_d(h.ba, xf[i] - 1.0f, yf[i], zf[i], gBA[i], bax[i], bay[i], baz[i]);
    grad_with_d(h.ab, xf[i], yf[i] - 1.0f, zf[i], gAB[i], abx[i], aby[i], abz[i]);
    grad_with_d(h.bb, xf[i] - 1.0f, yf[i] - 1.0f, zf[i], gBB[i], bbx[i], bby[i], bbz[i]);
    grad_with_d(h.aa1, xf[i], yf[i], zf[i] - 1.0f, gAA1[i], aa1x[i], aa1y[i], aa1z[i]);
    grad_with_d(h.ba1, xf[i] - 1.0f, yf[i], zf[i] - 1.0f, gBA1[i], ba1x[i], ba1y[i], ba1z[i]);
    grad_with_d(h.ab1, xf[i], yf[i] - 1.0f, zf[i] - 1.0f, gAB1[i], ab1x[i], ab1y[i], ab1z[i]);
    grad_with_d(h.bb1, xf[i] - 1.0f, yf[i] - 1.0f, zf[i] - 1.0f, gBB1[i], bb1x[i], bb1y[i],
                bb1z[i]);
  }

  const wds_v4f vu = v4_load(u);
  const wds_v4f vv = v4_load(v);
  const wds_v4f vw = v4_load(w);
  const wds_v4f vdu = v4_load(du);
  const wds_v4f vdv = v4_load(dv);
  const wds_v4f vdw = v4_load(dw);

  const wds_v4f vgAA = v4_load(gAA);
  const wds_v4f vgBA = v4_load(gBA);
  const wds_v4f vgAB = v4_load(gAB);
  const wds_v4f vgBB = v4_load(gBB);
  const wds_v4f vaax = v4_load(aax);
  const wds_v4f vbax = v4_load(bax);
  const wds_v4f vaay = v4_load(aay);
  const wds_v4f vbay = v4_load(bay);
  const wds_v4f vaaz = v4_load(aaz);
  const wds_v4f vbaz = v4_load(baz);
  const wds_v4f vabx = v4_load(abx);
  const wds_v4f vbbx = v4_load(bbx);
  const wds_v4f vaby = v4_load(aby);
  const wds_v4f vbby = v4_load(bby);
  const wds_v4f vabz = v4_load(abz);
  const wds_v4f vbbz = v4_load(bbz);

  const wds_v4f x1 = v4_madd(vu, vgBA - vgAA, vgAA);
  const wds_v4f x1x = v4_madd(vdu, vgBA - vgAA, v4_madd(vu, vbax - vaax, vaax));
  const wds_v4f x1y = v4_madd(vu, vbay - vaay, vaay);
  const wds_v4f x1z = v4_madd(vu, vbaz - vaaz, vaaz);
  const wds_v4f x2 = v4_madd(vu, vgBB - vgAB, vgAB);
  const wds_v4f x2x = v4_madd(vdu, vgBB - vgAB, v4_madd(vu, vbbx - vabx, vabx));
  const wds_v4f x2y = v4_madd(vu, vbby - vaby, vaby);
  const wds_v4f x2z = v4_madd(vu, vbbz - vabz, vabz);
  const wds_v4f y1 = v4_madd(vv, x2 - x1, x1);
  const wds_v4f y1x = v4_madd(vv, x2x - x1x, x1x);
  const wds_v4f y1y = v4_madd(vdv, x2 - x1, v4_madd(vv, x2y - x1y, x1y));
  const wds_v4f y1z = v4_madd(vv, x2z - x1z, x1z);

  const wds_v4f vgAA1 = v4_load(gAA1);
  const wds_v4f vgBA1 = v4_load(gBA1);
  const wds_v4f vgAB1 = v4_load(gAB1);
  const wds_v4f vgBB1 = v4_load(gBB1);
  const wds_v4f vaa1x = v4_load(aa1x);
  const wds_v4f vba1x = v4_load(ba1x);
  const wds_v4f vaa1y = v4_load(aa1y);
  const wds_v4f vba1y = v4_load(ba1y);
  const wds_v4f vaa1z = v4_load(aa1z);
  const wds_v4f vba1z = v4_load(ba1z);
  const wds_v4f vab1x = v4_load(ab1x);
  const wds_v4f vbb1x = v4_load(bb1x);
  const wds_v4f vab1y = v4_load(ab1y);
  const wds_v4f vbb1y = v4_load(bb1y);
  const wds_v4f vab1z = v4_load(ab1z);
  const wds_v4f vbb1z = v4_load(bb1z);

  const wds_v4f x3 = v4_madd(vu, vgBA1 - vgAA1, vgAA1);
  const wds_v4f x3x = v4_madd(vdu, vgBA1 - vgAA1, v4_madd(vu, vba1x - vaa1x, vaa1x));
  const wds_v4f x3y = v4_madd(vu, vba1y - vaa1y, vaa1y);
  const wds_v4f x3z = v4_madd(vu, vba1z - vaa1z, vaa1z);
  const wds_v4f x4 = v4_madd(vu, vgBB1 - vgAB1, vgAB1);
  const wds_v4f x4x = v4_madd(vdu, vgBB1 - vgAB1, v4_madd(vu, vbb1x - vab1x, vab1x));
  const wds_v4f x4y = v4_madd(vu, vbb1y - vab1y, vab1y);
  const wds_v4f x4z = v4_madd(vu, vbb1z - vab1z, vab1z);
  const wds_v4f y2 = v4_madd(vv, x4 - x3, x3);
  const wds_v4f y2x = v4_madd(vv, x4x - x3x, x3x);
  const wds_v4f y2y = v4_madd(vdv, x4 - x3, v4_madd(vv, x4y - x3y, x3y));
  const wds_v4f y2z = v4_madd(vv, x4z - x3z, x3z);

  v4_store(value, v4_madd(vw, y2 - y1, y1));
  v4_store(dx, v4_madd(vw, y2x - y1x, y1x));
  v4_store(dy, v4_madd(vw, y2y - y1y, y1y));
  v4_store(dz, v4_madd(vdw, y2 - y1, v4_madd(vw, y2z - y1z, y1z)));
}
#endif

#if defined(_MSC_VER) && !defined(__clang__)
WDS_NOISE_INLINE void perlin3_value_grad_x4_vec(const float x[4], const float y[4], const float z[4],
                                                float value[4], float dx[4], float dy[4],
                                                float dz[4]) noexcept {
  for (int i = 0; i < 4; ++i) {
    perlin3_value_grad(x[i], y[i], z[i], value[i], dx[i], dy[i], dz[i]);
  }
}
#endif

WDS_NOISE_INLINE void perlin3_value_grad_x4(const float x[4], const float y[4], const float z[4],
                                            float value[4], float dx[4], float dy[4],
                                            float dz[4]) noexcept {
  if (split_simd_path() == SplitSimdPath::Scalar) {
    for (int i = 0; i < 4; ++i) {
      perlin3_value_grad(x[i], y[i], z[i], value[i], dx[i], dy[i], dz[i]);
    }
    return;
  }
  perlin3_value_grad_x4_vec(x, y, z, value, dx, dy, dz);
}

WDS_NOISE_INLINE void fbm_world_grad(float wx, float wy, float wz, float freq, float scroll,
                                     float phase_x, float phase_y, float phase_z, int oct,
                                     float lac, float gain, float& dwx, float& dwy,
                                     float& dwz) noexcept {
  float val = 0.0f, dcx = 0.0f, dcy = 0.0f, dcz = 0.0f;
  fbm_value_grad(an_madd(wx, freq, scroll + phase_x), an_madd(wy, freq, phase_y),
                 an_madd(wz, freq, phase_z), oct, lac, gain, val, dcx, dcy, dcz);
  dwx = dcx * freq;
  dwy = dcy * freq;
  dwz = dcz * freq;
}

WDS_NOISE_INLINE void fbm_value_grad_x4(const float x[4], const float y[4], const float z[4],
                                        int octaves, float lac, float gain, float value[4],
                                        float dx[4], float dy[4], float dz[4]) noexcept {
  const int n = std::clamp(octaves, 1, 8);
  if (n == 1) {
    perlin3_value_grad_x4(x, y, z, value, dx, dy, dz);
    return;
  }
  float sum[4] = {0, 0, 0, 0};
  float sdx[4] = {0, 0, 0, 0};
  float sdy[4] = {0, 0, 0, 0};
  float sdz[4] = {0, 0, 0, 0};
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    float px[4], py[4], pz[4], lv[4], lx[4], ly[4], lz[4];
    for (int k = 0; k < 4; ++k) {
      px[k] = x[k] * freq;
      py[k] = y[k] * freq;
      pz[k] = z[k] * freq;
    }
    perlin3_value_grad_x4(px, py, pz, lv, lx, ly, lz);
    const float fam = freq * amp;
    for (int k = 0; k < 4; ++k) {
      sum[k] = an_madd(lv[k], amp, sum[k]);
      sdx[k] = an_madd(lx[k], fam, sdx[k]);
      sdy[k] = an_madd(ly[k], fam, sdy[k]);
      sdz[k] = an_madd(lz[k], fam, sdz[k]);
    }
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  if (norm <= 1e-6f) {
    value[0] = value[1] = value[2] = value[3] = 0.0f;
    dx[0] = dx[1] = dx[2] = dx[3] = 0.0f;
    dy[0] = dy[1] = dy[2] = dy[3] = 0.0f;
    dz[0] = dz[1] = dz[2] = dz[3] = 0.0f;
    return;
  }
  const float inv = 1.0f / norm;
  for (int k = 0; k < 4; ++k) {
    value[k] = sum[k] * inv;
    dx[k] = sdx[k] * inv;
    dy[k] = sdy[k] * inv;
    dz[k] = sdz[k] * inv;
  }
}


}  // namespace an_detail


WDS_AN_ENTRY void eval_noise_velocity_an(const ParticleState& st, float age, float lx, float ly,
                                        float lz, float& vx, float& vy, float& vz, float* unit_x,
                                        float* unit_y, float* unit_z) noexcept {
  using an_detail::an_madd;
  using an_detail::fbm_value_grad_x4;
  using an_detail::fbm_world_grad;
  vx = vy = vz = 0.0f;
  if (st.p == nullptr) {
    return;
  }
  const SplitEffectParticle& p = *st.p;
  if ((p.flags & kSplitEffectFlagNoise) == 0) {
    return;
  }
  const float t01 = std::clamp(age / (st.life > 1e-6f ? st.life : 1.0f), 0.0f, 1.0f);
  float str_x = 0.0f, str_y = 0.0f, str_z = 0.0f;
  eval_noise_strength_axes(st, t01, str_x, str_y, str_z);
  if (std::fabs(str_x) < 1e-8f && std::fabs(str_y) < 1e-8f && std::fabs(str_z) < 1e-8f &&
      p.noise_strength_count == 0 && p.noise_strength_y_count == 0 &&
      p.noise_strength_z_count == 0 && unit_x == nullptr) {
    return;
  }
  const float freq = p.noise_frequency > 1e-6f ? p.noise_frequency : 1.0f;
  const NoisePhaseExperiment phase_experiment = noise_phase_experiment();
  const float scroll = p.noise_scroll * age;
  const float amt = p.noise_position_amount;
  if (!official_split_effect_noise_position_curl_on(amt)) {
    return;
  }
  SplitWallScope wall_noise(SplitWallSec::Noise);
  float px = lx;
  float py = ly;
  float pz = lz;
  if (p.simulation_space != static_cast<uint8_t>(SplitEffectSimulationSpace::Local)) {
    const float origin = emitter_main_y(st.color_id, st.birth_fade) + st.birth_origin_y_delta;
    px = st.line_x + lx;
    py = origin + emitter_main_y_from_ly_slope(st.color_id) * ly;
    pz = lz + st.birth_parent_z;
  }
  const float oct_scale = p.noise_octave_scale > 1e-4f ? p.noise_octave_scale : 2.0f;
  const float oct_mult = p.noise_octave_mult > 1e-4f ? p.noise_octave_mult : 0.5f;
  const int oct = p.noise_octaves;
  const float phase_x =
      phase_experiment == NoisePhaseExperiment::Seed ? noise_seed_phase(st.seed, 0u) : 0.0f;
  const float phase_y =
      phase_experiment == NoisePhaseExperiment::Seed ? noise_seed_phase(st.seed, 1u) : 0.0f;
  const float phase_z =
      phase_experiment == NoisePhaseExperiment::Seed ? noise_seed_phase(st.seed, 2u) : 0.0f;
  constexpr float kOffA = 19.0f;
  constexpr float kOffB = 47.0f;
  float sx = str_x;
  float sy = str_y;
  float sz = str_z;
  if (p.noise_damping != 0 && noise_experiment() != NoiseExperiment::BridsonVectorPotential) {
    sx /= freq;
    sy /= freq;
    sz /= freq;
  }
  if (split_noise_vol_enabled()) {
    eval_noise_velocity_vol_q(px, py, pz, freq, scroll, phase_x, phase_y, phase_z, oct, oct_scale,
                              oct_mult, 1.0f, 1.0f, 1.0f, amt, vx, vy, vz);
    finish_noise_vel(vx, vy, vz, sx, sy, sz, vx, vy, vz, unit_x, unit_y, unit_z);
    return;
  }
  if (split_noise_an_simd_enabled()) {
    const float gx[4] = {an_madd(px, freq, scroll + phase_x),
                         an_madd(px + kOffA, freq, scroll + phase_x),
                         an_madd(px, freq, scroll + phase_x), an_madd(px, freq, scroll + phase_x)};
    const float gy[4] = {an_madd(py, freq, phase_y), an_madd(py, freq, phase_y),
                         an_madd(py + kOffB, freq, phase_y), an_madd(py, freq, phase_y)};
    const float gz[4] = {an_madd(pz, freq, phase_z), an_madd(pz, freq, phase_z),
                         an_madd(pz, freq, phase_z), an_madd(pz, freq, phase_z)};
    float val[4], dcx[4], dcy[4], dcz[4];
    fbm_value_grad_x4(gx, gy, gz, oct, oct_scale, oct_mult, val, dcx, dcy, dcz);
    finish_noise_vel((dcy[2] - dcz[1]) * freq * amt, (dcz[0] - dcx[2]) * freq * amt,
                     (dcx[1] - dcy[0]) * freq * amt, sx, sy, sz, vx, vy, vz, unit_x, unit_y,
                     unit_z);
    (void)val;
    return;
  }
  float dxx = 0.0f, dxy = 0.0f, dxz = 0.0f;
  float dyx = 0.0f, dyy = 0.0f, dyz = 0.0f;
  float dzx = 0.0f, dzy = 0.0f, dzz = 0.0f;
  fbm_world_grad(px, py, pz, freq, scroll, phase_x, phase_y, phase_z, oct, oct_scale, oct_mult,
                 dxx, dxy, dxz);
  fbm_world_grad(px + kOffA, py, pz, freq, scroll, phase_x, phase_y, phase_z, oct, oct_scale,
                 oct_mult, dyx, dyy, dyz);
  fbm_world_grad(px, py + kOffB, pz, freq, scroll, phase_x, phase_y, phase_z, oct, oct_scale,
                 oct_mult, dzx, dzy, dzz);
  finish_noise_vel((dzy - dyz) * amt, (dxz - dzx) * amt, (dyx - dxy) * amt, sx, sy, sz, vx, vy, vz,
                   unit_x, unit_y, unit_z);
  (void)dxx;
  (void)dyy;
  (void)dzz;
}


#undef WDS_AN_ENTRY
#undef WDS_NOISE_INLINE

}  // namespace wds::chart_render::shuriken

#if WDS_AN_OP != 2
#if defined(__clang__)
#pragma clang fp contract(on)
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
#endif

#include "internal.hpp"
#include "modules_an.inl.hpp"
#include "simd_dispatch.hpp"
#include "split_eval_cache.hpp"
#include "split_wall_profile.hpp"

#include <wds/core/official_playfield.hpp>
#include <wds/core/official_split_x.hpp>
#include <wds/core/split_fade.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#define WDS_NOISE_HAS_NEON 1
#else
#define WDS_NOISE_HAS_NEON 0
#endif

namespace wds::chart_render::shuriken {

namespace {
thread_local const ParticleCacheKey* g_rol_cache_key = nullptr;
}

void set_rol_cache_key(const ParticleCacheKey* key) noexcept {
  g_rol_cache_key = key;
}

namespace {

float fade(float t) noexcept {
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

int perm(int i) noexcept {
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

float grad(int hash, float x, float y, float z) noexcept {
  const int h = hash & 15;
  const float u = h < 8 ? x : y;
  const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
  return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

float perlin3_reference_impl(float x, float y, float z) noexcept {
  const int X = static_cast<int>(std::floor(x)) & 255;
  const int Y = static_cast<int>(std::floor(y)) & 255;
  const int Z = static_cast<int>(std::floor(z)) & 255;
  const float xf = x - std::floor(x);
  const float yf = y - std::floor(y);
  const float zf = z - std::floor(z);
  const float u = fade(xf);
  const float v = fade(yf);
  const float w = fade(zf);
  const int A = perm(X) + Y;
  const int AA = perm(A) + Z;
  const int AB = perm(A + 1) + Z;
  const int B = perm(X + 1) + Y;
  const int BA = perm(B) + Z;
  const int BB = perm(B + 1) + Z;
  const float x1 = grad(perm(AA), xf, yf, zf) +
                   u * (grad(perm(BA), xf - 1.0f, yf, zf) - grad(perm(AA), xf, yf, zf));
  const float x2 = grad(perm(AB), xf, yf - 1.0f, zf) +
                   u * (grad(perm(BB), xf - 1.0f, yf - 1.0f, zf) - grad(perm(AB), xf, yf - 1.0f, zf));
  const float y1 = x1 + v * (x2 - x1);
  const float x3 = grad(perm(AA + 1), xf, yf, zf - 1.0f) +
                   u * (grad(perm(BA + 1), xf - 1.0f, yf, zf - 1.0f) -
                        grad(perm(AA + 1), xf, yf, zf - 1.0f));
  const float x4 = grad(perm(AB + 1), xf, yf - 1.0f, zf - 1.0f) +
                   u * (grad(perm(BB + 1), xf - 1.0f, yf - 1.0f, zf - 1.0f) -
                        grad(perm(AB + 1), xf, yf - 1.0f, zf - 1.0f));
  const float y2 = x3 + v * (x4 - x3);
  return (y1 + w * (y2 - y1)) * 0.5f + 0.5f;
}

struct NoiseAxis {
  int cell;
  float frac;
  float w;
};

#if defined(__clang__) || defined(__GNUC__)
#define WDS_NOISE_INLINE inline __attribute__((always_inline))
#else
#define WDS_NOISE_INLINE inline
#endif

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

WDS_NOISE_INLINE float perlin_lerp_corners(const CornerHashes& h, float xf, float yf, float zf,
                                           float u, float v, float w) noexcept {
  const float gAA = grad(h.aa, xf, yf, zf);
  const float gBA = grad(h.ba, xf - 1.0f, yf, zf);
  const float x1 = gAA + u * (gBA - gAA);
  const float gAB = grad(h.ab, xf, yf - 1.0f, zf);
  const float gBB = grad(h.bb, xf - 1.0f, yf - 1.0f, zf);
  const float x2 = gAB + u * (gBB - gAB);
  const float y1 = x1 + v * (x2 - x1);
  const float gAA1 = grad(h.aa1, xf, yf, zf - 1.0f);
  const float gBA1 = grad(h.ba1, xf - 1.0f, yf, zf - 1.0f);
  const float x3 = gAA1 + u * (gBA1 - gAA1);
  const float gAB1 = grad(h.ab1, xf, yf - 1.0f, zf - 1.0f);
  const float gBB1 = grad(h.bb1, xf - 1.0f, yf - 1.0f, zf - 1.0f);
  const float x4 = gAB1 + u * (gBB1 - gAB1);
  const float y2 = x3 + v * (x4 - x3);
  return (y1 + w * (y2 - y1)) * 0.5f + 0.5f;
}

WDS_NOISE_INLINE float perlin3_from_axes(const NoiseAxis& X, const NoiseAxis& Y,
                                         const NoiseAxis& Z) noexcept {
  return perlin_lerp_corners(corner_hashes(X.cell, Y.cell, Z.cell), X.frac, Y.frac, Z.frac, X.w,
                             Y.w, Z.w);
}

WDS_NOISE_INLINE float perlin3(float x, float y, float z) noexcept {
  return perlin3_from_axes(make_noise_axis(x), make_noise_axis(y), make_noise_axis(z));
}

WDS_NOISE_INLINE void perlin3_pair_x(float x0, float x1, float y, float z, float& o0,
                                     float& o1) noexcept {
  const NoiseAxis X0 = make_noise_axis(x0);
  const NoiseAxis X1 = make_noise_axis(x1);
  const NoiseAxis Y = make_noise_axis(y);
  const NoiseAxis Z = make_noise_axis(z);
  if (X0.cell == X1.cell) {
    const CornerHashes h = corner_hashes(X0.cell, Y.cell, Z.cell);
    o0 = perlin_lerp_corners(h, X0.frac, Y.frac, Z.frac, X0.w, Y.w, Z.w);
    o1 = perlin_lerp_corners(h, X1.frac, Y.frac, Z.frac, X1.w, Y.w, Z.w);
  } else {
    o0 = perlin3_from_axes(X0, Y, Z);
    o1 = perlin3_from_axes(X1, Y, Z);
  }
}

WDS_NOISE_INLINE void perlin3_pair_y(float x, float y0, float y1, float z, float& o0,
                                     float& o1) noexcept {
  const NoiseAxis X = make_noise_axis(x);
  const NoiseAxis Y0 = make_noise_axis(y0);
  const NoiseAxis Y1 = make_noise_axis(y1);
  const NoiseAxis Z = make_noise_axis(z);
  if (Y0.cell == Y1.cell) {
    const CornerHashes h = corner_hashes(X.cell, Y0.cell, Z.cell);
    o0 = perlin_lerp_corners(h, X.frac, Y0.frac, Z.frac, X.w, Y0.w, Z.w);
    o1 = perlin_lerp_corners(h, X.frac, Y1.frac, Z.frac, X.w, Y1.w, Z.w);
  } else {
    o0 = perlin3_from_axes(X, Y0, Z);
    o1 = perlin3_from_axes(X, Y1, Z);
  }
}

WDS_NOISE_INLINE void perlin3_pair_z(float x, float y, float z0, float z1, float& o0,
                                     float& o1) noexcept {
  const NoiseAxis X = make_noise_axis(x);
  const NoiseAxis Y = make_noise_axis(y);
  const NoiseAxis Z0 = make_noise_axis(z0);
  const NoiseAxis Z1 = make_noise_axis(z1);
  if (Z0.cell == Z1.cell) {
    const CornerHashes h = corner_hashes(X.cell, Y.cell, Z0.cell);
    o0 = perlin_lerp_corners(h, X.frac, Y.frac, Z0.frac, X.w, Y.w, Z0.w);
    o1 = perlin_lerp_corners(h, X.frac, Y.frac, Z1.frac, X.w, Y.w, Z1.w);
  } else {
    o0 = perlin3_from_axes(X, Y, Z0);
    o1 = perlin3_from_axes(X, Y, Z1);
  }
}

float fbm(float x, float y, float z, int octaves, float lac, float gain) noexcept {
  const int n = std::clamp(octaves, 1, 8);
  if (n == 1) {
    return perlin3(x, y, z) * 2.0f - 1.0f;
  }
  float sum = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    sum += (perlin3(x * freq, y * freq, z * freq) * 2.0f - 1.0f) * amp;
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  return norm > 1e-6f ? sum / norm : 0.0f;
}

float fbm_reference_impl(float x, float y, float z, int octaves, float lac, float gain) noexcept {
  float sum = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  const int n = std::clamp(octaves, 1, 8);
  for (int i = 0; i < n; ++i) {
    sum += (perlin3_reference_impl(x * freq, y * freq, z * freq) * 2.0f - 1.0f) * amp;
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  return norm > 1e-6f ? sum / norm : 0.0f;
}

WDS_NOISE_INLINE void fbm_pair_x(float x0, float x1, float y, float z, int octaves, float lac,
                                 float gain, float& o0, float& o1) noexcept {
  const int n = std::clamp(octaves, 1, 8);
  if (n == 1) {
    float p0 = 0.0f;
    float p1 = 0.0f;
    perlin3_pair_x(x0, x1, y, z, p0, p1);
    o0 = p0 * 2.0f - 1.0f;
    o1 = p1 * 2.0f - 1.0f;
    return;
  }
  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    float p0 = 0.0f;
    float p1 = 0.0f;
    perlin3_pair_x(x0 * freq, x1 * freq, y * freq, z * freq, p0, p1);
    sum0 += (p0 * 2.0f - 1.0f) * amp;
    sum1 += (p1 * 2.0f - 1.0f) * amp;
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  o0 = norm > 1e-6f ? sum0 / norm : 0.0f;
  o1 = norm > 1e-6f ? sum1 / norm : 0.0f;
}

WDS_NOISE_INLINE void fbm_pair_y(float x, float y0, float y1, float z, int octaves, float lac,
                                 float gain, float& o0, float& o1) noexcept {
  const int n = std::clamp(octaves, 1, 8);
  if (n == 1) {
    float p0 = 0.0f;
    float p1 = 0.0f;
    perlin3_pair_y(x, y0, y1, z, p0, p1);
    o0 = p0 * 2.0f - 1.0f;
    o1 = p1 * 2.0f - 1.0f;
    return;
  }
  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    float p0 = 0.0f;
    float p1 = 0.0f;
    perlin3_pair_y(x * freq, y0 * freq, y1 * freq, z * freq, p0, p1);
    sum0 += (p0 * 2.0f - 1.0f) * amp;
    sum1 += (p1 * 2.0f - 1.0f) * amp;
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  o0 = norm > 1e-6f ? sum0 / norm : 0.0f;
  o1 = norm > 1e-6f ? sum1 / norm : 0.0f;
}

WDS_NOISE_INLINE void fbm_pair_z(float x, float y, float z0, float z1, int octaves, float lac,
                                 float gain, float& o0, float& o1) noexcept {
  const int n = std::clamp(octaves, 1, 8);
  if (n == 1) {
    float p0 = 0.0f;
    float p1 = 0.0f;
    perlin3_pair_z(x, y, z0, z1, p0, p1);
    o0 = p0 * 2.0f - 1.0f;
    o1 = p1 * 2.0f - 1.0f;
    return;
  }
  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    float p0 = 0.0f;
    float p1 = 0.0f;
    perlin3_pair_z(x * freq, y * freq, z0 * freq, z1 * freq, p0, p1);
    sum0 += (p0 * 2.0f - 1.0f) * amp;
    sum1 += (p1 * 2.0f - 1.0f) * amp;
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  o0 = norm > 1e-6f ? sum0 / norm : 0.0f;
  o1 = norm > 1e-6f ? sum1 / norm : 0.0f;
}

#if WDS_NOISE_HAS_NEON
// hop (not 2022.3): 4-wide world-sample batch for the FD+NEON test hook.
// Each lane calls the same perlin_lerp_corners as scalar so FD stays
// bit-exact vs pair. Keep this in modules.cpp (default contraction).
WDS_NOISE_INLINE void perlin3_x4(const float x[4], const float y[4], const float z[4],
                                 float o[4]) noexcept {
  for (int i = 0; i < 4; ++i) {
    o[i] = perlin3(x[i], y[i], z[i]);
  }
}

WDS_NOISE_INLINE void fbm_x4(const float x[4], const float y[4], const float z[4], int octaves,
                             float lac, float gain, float o[4]) noexcept {
  const int n = std::clamp(octaves, 1, 8);
  if (n == 1) {
    float p[4];
    perlin3_x4(x, y, z, p);
    o[0] = p[0] * 2.0f - 1.0f;
    o[1] = p[1] * 2.0f - 1.0f;
    o[2] = p[2] * 2.0f - 1.0f;
    o[3] = p[3] * 2.0f - 1.0f;
    return;
  }
  float sum[4] = {0, 0, 0, 0};
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int i = 0; i < n; ++i) {
    float px[4], py[4], pz[4], p[4];
    for (int k = 0; k < 4; ++k) {
      px[k] = x[k] * freq;
      py[k] = y[k] * freq;
      pz[k] = z[k] * freq;
    }
    perlin3_x4(px, py, pz, p);
    for (int k = 0; k < 4; ++k) {
      sum[k] += (p[k] * 2.0f - 1.0f) * amp;
    }
    norm += amp;
    freq *= lac;
    amp *= gain;
  }
  if (norm <= 1e-6f) {
    o[0] = o[1] = o[2] = o[3] = 0.0f;
    return;
  }
  const float inv = 1.0f / norm;
  o[0] = sum[0] * inv;
  o[1] = sum[1] * inv;
  o[2] = sum[2] * inv;
  o[3] = sum[3] * inv;
}

WDS_NOISE_INLINE void fbm_world_x4(const float wx[4], const float wy[4], const float wz[4],
                                   float freq, float scroll, float phase_x, float phase_y,
                                   float phase_z, int oct, float lac, float gain,
                                   float o[4]) noexcept {
  float x[4], y[4], z[4];
  for (int i = 0; i < 4; ++i) {
    x[i] = wx[i] * freq + scroll + phase_x;
    y[i] = wy[i] * freq + phase_y;
    z[i] = wz[i] * freq + phase_z;
  }
  fbm_x4(x, y, z, oct, lac, gain, o);
}
#endif

#undef WDS_NOISE_INLINE

}  // namespace

NoiseExperiment noise_experiment() noexcept {
  static const NoiseExperiment mode = [] {
    const char* raw = std::getenv("OPENWDS_NOISE_EXPERIMENT");
    if (raw == nullptr) return NoiseExperiment::NativeCurrent;
    const std::string_view name(raw);
    if (name == "bridson_vector_potential") return NoiseExperiment::BridsonVectorPotential;
    if (name == "frequency_damped_curl") return NoiseExperiment::FrequencyDampedCurl;
    if (name == "quality_shared_sample") return NoiseExperiment::QualitySharedSample;
    return NoiseExperiment::NativeCurrent;
  }();
  return mode;
}

NoisePhaseExperiment noise_phase_experiment() noexcept {
  static const NoisePhaseExperiment mode = [] {
    const char* raw = std::getenv("OPENWDS_NOISE_PHASE_EXPERIMENT");
    if (raw == nullptr) return NoisePhaseExperiment::Current;
    const std::string_view name(raw);
    if (name == "age") return NoisePhaseExperiment::Age;
    if (name == "seed") return NoisePhaseExperiment::Seed;
    return NoisePhaseExperiment::Current;
  }();
  return mode;
}

// hop (not 2022.3): analytic-gradient Bridson curl vs 12-sample FD
// (ε=1e-4/frequency). Default is analytic. WDS_SPLIT_NOISE_EXACT=1 restores
// FD (bit-exact vs golden-grid-v2-ref). Override -1 follows env.
int g_noise_exact_override = -1;
int g_noise_neon_override = -1;
int g_noise_vol_override = -1;
int g_noise_hold_override = -1;
int g_walk_hz_override = -1;

bool split_noise_exact_enabled() noexcept {
  if (g_noise_exact_override >= 0) {
    return g_noise_exact_override != 0;
  }
  static const bool exact = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_EXACT");
    return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
  }();
  return exact;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((unused))
#endif
bool split_noise_neon_enabled() noexcept {
  if (g_noise_neon_override >= 0) {
    return g_noise_neon_override != 0;
  }
  static const bool neon = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_NEON");
    return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
  }();
  return neon;
}

// hop: 4-wide analytic ∇ of one particle's 3 potentials + pad. Bit-exact vs
// 3× scalar on this TU, but the dummy lane + gather is slower (~62 vs ~53
// ns). Stay opt-in (WDS_SPLIT_NOISE_NEON=1 or test override) until a
// 4-particle SoA batch pays.
bool split_noise_an_simd_enabled() noexcept {
  if (g_noise_neon_override >= 0) {
    return g_noise_neon_override != 0;
  }
  static const bool on = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_NEON");
    return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
  }();
  return on;
}

void set_split_noise_exact_for_tests(int on) noexcept {
  g_noise_exact_override = on;
}

void set_split_noise_neon_for_tests(int on) noexcept {
  g_noise_neon_override = on;
}

void set_split_noise_vol_for_tests(int on) noexcept {
  g_noise_vol_override = on;
}

void set_split_noise_hold_for_tests(int steps) noexcept {
  g_noise_hold_override = steps;
}

void set_split_walk_hz_for_tests(int hz) noexcept {
  g_walk_hz_override = hz;
}

// hop (not 2022.3): tileable 3D curl volume sampled trilinearly. Opt-in
// (WDS_SPLIT_NOISE_VOL=1). Default cheap path is AN + hold (below). EXACT=1
// is FD. Volume 32/64^3 looked too smooth on 11711 (waves flattened).
bool split_noise_vol_enabled() noexcept {
  if (split_noise_exact_enabled()) {
    return false;
  }
  if (g_noise_vol_override >= 0) {
    return g_noise_vol_override != 0;
  }
  static const bool on = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_VOL");
    if (raw == nullptr || raw[0] == '\0') {
      return false;
    }
    return raw[0] == '1' && raw[1] == '\0';
  }();
  return on;
}

int split_noise_hold() noexcept {
  if (split_noise_exact_enabled()) {
    return 1;
  }
  if (g_noise_hold_override > 0) {
    return g_noise_hold_override;
  }
  static const int hold = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_HOLD");
    if (raw == nullptr || raw[0] == '\0') {
      // hop: hold-4 vs hold-2 stills/10-frame 11711+10380 matched by eye.
      return 4;
    }
    char* end = nullptr;
    const long v = std::strtol(raw, &end, 10);
    if (end == raw || v < 1 || v > 16) {
      return 4;
    }
    return static_cast<int>(v);
  }();
  return hold;
}

namespace {

bool split_noise_hold_env_forced() noexcept {
  // Cache the process env. getenv takes a process-wide lock; four workers
  // calling it from every run_step serialized on that lock (10380 cold
  // ~66% of sampled time). WDS_SPLIT_NOISE_HOLD still works; first read
  // wins, same as the other WDS_SPLIT_* switches. Tests use the override.
  static const bool forced = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_HOLD");
    return raw != nullptr && raw[0] != '\0';
  }();
  return forced;
}

bool split_noise_hold_adapt() noexcept {
  if (split_noise_exact_enabled() || g_noise_hold_override > 0 || split_noise_hold_env_forced()) {
    return false;
  }
  static const bool on = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_HOLD_ADAPT");
    if (raw == nullptr || raw[0] == '\0') {
      return true;
    }
    return raw[0] != '0';
  }();
  return on;
}

// hop (not 2022.3): RAISE unset = cost-aware densify; 0 = never N<4; 1 = rate-implied.
enum class NoiseHoldDensifyPolicy : uint8_t { CostAware = 0, ForceOff = 1, ForceOn = 2 };

NoiseHoldDensifyPolicy split_noise_hold_densify_policy() noexcept {
  static const NoiseHoldDensifyPolicy p = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_HOLD_RAISE");
    if (raw == nullptr || raw[0] == '\0') {
      return NoiseHoldDensifyPolicy::CostAware;
    }
    if (raw[0] == '0' && raw[1] == '\0') {
      return NoiseHoldDensifyPolicy::ForceOff;
    }
    if (raw[0] == '1' && raw[1] == '\0') {
      return NoiseHoldDensifyPolicy::ForceOn;
    }
    return NoiseHoldDensifyPolicy::CostAware;
  }();
  return p;
}

float axis_peak_abs(float max_v, float min_v, uint8_t random, uint16_t begin, uint8_t count,
                    uint16_t min_begin, uint8_t min_count) noexcept {
  float peak = std::max(std::fabs(max_v), std::fabs(min_v));
  static constexpr float kT[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
  for (float t : kT) {
    if (count > 0) {
      peak = std::max(peak, std::fabs(official_split_effect_eval_curve_keys(begin, count, t, max_v)));
    }
    if (random != 0 && min_count > 0) {
      peak = std::max(
          peak, std::fabs(official_split_effect_eval_curve_keys(min_begin, min_count, t, min_v)));
    }
  }
  return peak;
}

// hop (not 2022.3): |dq/dt| ≈ freq×|v_bound| + |scroll|.
// v_bound is a catalog-only conservative envelope (VOL / force / gravity /
// inherit×startSpeed / drag terminal). No live emitter, no noise feedback.
float coord_rate(const SplitEffectParticle& p) noexcept {
  if ((p.flags & kSplitEffectFlagNoise) == 0) {
    return 0.0f;
  }
  const float freq = p.noise_frequency > 1e-6f ? p.noise_frequency : 1.0f;
  const float scroll = std::fabs(p.noise_scroll);
  float v0 = std::max(std::fabs(p.start_speed), std::fabs(p.start_speed_min));
  v0 = std::max(v0, axis_peak_abs(p.start_speed, p.start_speed_min, p.speed_random,
                                  p.speed_curve_begin, p.speed_curve_count, p.speed_min_curve_begin,
                                  p.speed_min_curve_count));
  float vol_m = 0.0f;
  if ((p.flags & kSplitEffectFlagVelocity) != 0) {
    const float vx = axis_peak_abs(p.vel_x, p.vel_x_min, p.vel_x_random, p.vel_x_begin, p.vel_x_count,
                                   p.vel_x_min_begin, p.vel_x_min_count);
    const float vy = axis_peak_abs(p.vel_y, p.vel_y_min, p.vel_y_random, p.vel_y_begin, p.vel_y_count,
                                   p.vel_y_min_begin, p.vel_y_min_count);
    const float vz = axis_peak_abs(p.vel_z, p.vel_z_min, p.vel_z_random, p.vel_z_begin, p.vel_z_count,
                                   p.vel_z_min_begin, p.vel_z_min_count);
    vol_m = std::sqrt(vx * vx + vy * vy + vz * vz);
  }
  float fx = 0.0f, fy = 0.0f, fz = 0.0f;
  if (p.force_enabled != 0) {
    fx = axis_peak_abs(p.force_x, p.force_x_min, p.force_x_random, p.force_x_begin, p.force_x_count,
                       p.force_x_min_begin, p.force_x_min_count);
    fy = axis_peak_abs(p.force_y, p.force_y_min, p.force_y_random, p.force_y_begin, p.force_y_count,
                       p.force_y_min_begin, p.force_y_min_count);
    fz = axis_peak_abs(p.force_z, p.force_z_min, p.force_z_random, p.force_z_begin, p.force_z_count,
                       p.force_z_min_begin, p.force_z_min_count);
  }
  float gy = 0.0f, gz = 0.0f;
  if (std::fabs(p.gravity_modifier) > 1e-8f) {
    wds::chart_editor::official_gravity_lane_accel(p.gravity_modifier, gy, gz);
  }
  const float a = std::sqrt(fx * fx + (fy + gy) * (fy + gy) + (fz + gz) * (fz + gz));
  float inh = 0.0f;
  if (p.inherit_enabled != 0) {
    inh = std::max(std::fabs(p.inherit_mult), std::fabs(p.inherit_mult_min));
  }
  const float T = std::max(std::max(p.lifetime, p.lifetime_min), 1e-3f);
  float v_bound = v0 + vol_m + inh * v0;
  if (p.clamp_enabled != 0) {
    const float drag = axis_peak_abs(p.clamp_drag, p.clamp_drag_min, p.clamp_drag_random,
                                     p.clamp_drag_begin, p.clamp_drag_count, 0, 0);
    if (drag > 1e-8f) {
      v_bound = std::max(v_bound, a / std::max(drag, 1e-4f));
    } else {
      v_bound += a * T;
    }
  } else {
    v_bound += a * T;
  }
  return freq * v_bound + scroll;
}

float target_travel() noexcept {
  // hop: hold-4 travel of the fastest accepted visual (10380/11711/10660/11621).
  static const float t = []() {
    constexpr int32_t kIds[] = {10380, 11711, 10660, 11621};
    float max_rate = 0.0f;
    for (int32_t id : kIds) {
      const SplitEffectCatalogEntry* e = official_split_effect(id);
      if (e == nullptr) {
        continue;
      }
      uint16_t n = 0;
      const SplitEffectParticle* ps = official_split_effect_particles(*e, &n);
      if (ps == nullptr) {
        continue;
      }
      for (uint16_t i = 0; i < n; ++i) {
        max_rate = std::max(max_rate, coord_rate(ps[i]));
      }
    }
    if (max_rate < 1e-8f) {
      return 1.251032f;
    }
    return max_rate * 4.0f / 60.0f;
  }();
  return t;
}

// hop (not 2022.3): hold-4 travel above which densify changed the overall
// picture. Calibrated from /tmp/noise-densify hold-4 vs RAISE stills and
// 10-frame strips. Named fast systems were not under-sampled (n_rate≥4).
// The only n_rate<4 cluster is 11740 (t4 up to 12.075): same teardrop
// layout, no extra flicker. Threshold sits just above that same-picture
// travel. Not 2022.3.
float tvis_travel() noexcept {
  return 12.1f;
}

constexpr float kHoldHz = 60.0f;
// hop: one analytic curl eval vs one integrate step (3 potentials, NEON x4
// with unused lane). Not 2022.3.
constexpr float kNoiseEvalCostHop = 8.0f;
constexpr float kDensifyBudget = 0.03f;

int n_from_rate(float rate, int lo) noexcept {
  if (rate <= 1e-8f) {
    return 16;
  }
  const int n = static_cast<int>(std::floor(target_travel() * kHoldHz / rate));
  return std::clamp(n, lo, 16);
}

float rod_fade_travel(const SplitEffectParticle& p, int32_t effect_id) noexcept {
  // hop: same LineHight-scale closed form as emit authored_rod_travel at fade=1.
  float axis = wds::chart_editor::kOfficialZ180LineHightY + p.local_pos_y;
  if (effect_id != 0) {
    axis = emit_local_y(effect_id) + p.local_pos_y;
  }
  return std::fabs(axis) * wds::chart_editor::kOfficialLineHeightScale;
}

float est_live_own(const SplitEffectParticle& p, int32_t effect_id) noexcept {
  if (p.go_active == 0) {
    return 0.0f;
  }
  const float life = std::max(std::max(p.lifetime, p.lifetime_min), 1e-3f);
  const float length = p.length_in_sec > 1e-6f ? p.length_in_sec : 1.0f;
  const bool looping = (p.flags & kSplitEffectFlagLooping) != 0;
  const float window = looping ? life : std::min(life, length);
  float rot = std::max(p.rate_over_time, 0.0f);
  if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverTime)) {
    rot = std::max(rot, p.emission);
  }
  if (p.rot_curve_count > 0) {
    static constexpr float kT[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float t : kT) {
      rot = std::max(rot, official_split_effect_eval_rate_over_time(p, t));
    }
  }
  float live = rot * window;
  float burst = 0.0f;
  uint8_t burst_n = 0;
  const SplitEffectBurst* bursts = official_split_effect_bursts(p, &burst_n);
  if (bursts != nullptr) {
    for (uint8_t i = 0; i < burst_n; ++i) {
      const SplitEffectBurst& b = bursts[i];
      const float count = std::max(b.count, 0.0f);
      const float prob = std::clamp(b.probability, 0.0f, 1.0f);
      const float cycles = std::max(static_cast<float>(b.cycles), 1.0f);
      float overlap = cycles;
      if (b.interval > 1e-6f) {
        overlap = std::min(cycles, std::floor(life / b.interval) + 1.0f);
        overlap = std::max(overlap, 1.0f);
      }
      burst += count * prob * overlap;
    }
  }
  if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::Burst)) {
    burst += std::max(p.emission, 0.0f);
  }
  live += burst;
  float rod = std::max(p.rate_over_distance, 0.0f);
  if (p.rod_random != 0) {
    rod = std::max(rod, p.rate_over_distance_min);
  }
  if (p.emission_kind == static_cast<uint8_t>(SplitEffectEmission::RateOverDistance)) {
    rod = std::max(rod, p.emission);
  }
  if (p.rod_curve_count > 0) {
    static constexpr float kT[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float t : kT) {
      rod = std::max(rod, official_split_effect_eval_curve_keys(p.rod_curve_begin, p.rod_curve_count,
                                                               t, p.rate_over_distance));
    }
  }
  if (rod > 1e-8f) {
    // hop: RoD seeds ≈ RoD × fade-in world travel (live dump v1 path).
    live += split_effect_rod_seeds_from_ps_world_path(rod, rod_fade_travel(p, effect_id));
  }
  const float cap = p.max_particles > 0 ? static_cast<float>(p.max_particles) : live;
  live = std::min(live, cap);
  if (live < 1.0f && (rot > 1e-8f || burst > 1e-8f || rod > 1e-8f)) {
    live = 1.0f;
  }
  return live;
}

float noise_work(const SplitEffectParticle& p, float live, int hold_n) noexcept {
  if ((p.flags & kSplitEffectFlagNoise) == 0 ||
      !official_split_effect_noise_position_curl_on(p.noise_position_amount)) {
    return 0.0f;
  }
  const int n = std::max(hold_n, 1);
  return live * (1.0f / static_cast<float>(n)) * kNoiseEvalCostHop;
}

float est_work(const SplitEffectParticle& p, float live, int hold_n) noexcept {
  return live + noise_work(p, live, hold_n);
}

int decide_n(const SplitEffectParticle& p, float live, float work0, float extra_acc,
             NoiseHoldDensifyPolicy policy, float* extra_out) noexcept {
  if (extra_out != nullptr) {
    *extra_out = 0.0f;
  }
  const float rate = coord_rate(p);
  const int n4 = n_from_rate(rate, 4);
  if (policy == NoiseHoldDensifyPolicy::ForceOn) {
    return n_from_rate(rate, 1);
  }
  if (policy == NoiseHoldDensifyPolicy::ForceOff) {
    return n4;
  }
  const int n_rate = n_from_rate(rate, 1);
  const float travel4 = rate * 4.0f / kHoldHz;
  if (travel4 <= tvis_travel() || n_rate >= 4) {
    return n4;
  }
  if (!official_split_effect_noise_position_curl_on(p.noise_position_amount)) {
    return n4;
  }
  for (int cand = n_rate; cand < 4; ++cand) {
    const float extra = live * (1.0f / static_cast<float>(cand) - 0.25f) * kNoiseEvalCostHop;
    if (work0 > 1e-8f && (extra_acc + extra) / work0 <= kDensifyBudget) {
      if (extra_out != nullptr) {
        *extra_out = extra;
      }
      return cand;
    }
  }
  return n4;
}

struct HoldCache {
  std::vector<int> n;
};

HoldCache build_hold_cache() {
  using split_effect_catalog_detail::kOfficialSplitEffects;
  using split_effect_catalog_detail::kSplitEffectParticles;
  HoldCache c;
  c.n.assign(std::size(kSplitEffectParticles), 4);
  const NoiseHoldDensifyPolicy policy = split_noise_hold_densify_policy();
  for (const SplitEffectCatalogEntry& e : kOfficialSplitEffects) {
    uint16_t n = 0;
    const SplitEffectParticle* ps = official_split_effect_particles(e, &n);
    if (ps == nullptr || n == 0) {
      continue;
    }
    std::vector<float> live(n, 0.0f);
    for (uint16_t i = 0; i < n; ++i) {
      live[i] = est_live_own(ps[i], e.id);
    }
    for (uint16_t i = 0; i < n; ++i) {
      const uint16_t parent_idx = static_cast<uint16_t>(e.particle_begin + i);
      uint16_t nsub = 0;
      const SplitEffectSubEmitter* subs =
          official_split_effect_subemitters_for_parent(parent_idx, &nsub);
      if (subs == nullptr) {
        continue;
      }
      for (uint16_t s = 0; s < nsub; ++s) {
        if (subs[s].type != kSplitEffectSubEmitBirth && subs[s].type != kSplitEffectSubEmitDeath) {
          continue;
        }
        const uint16_t child_idx = subs[s].child_index;
        if (child_idx < e.particle_begin ||
            child_idx >= static_cast<uint16_t>(e.particle_begin + n)) {
          continue;
        }
        const uint16_t ci = static_cast<uint16_t>(child_idx - e.particle_begin);
        uint8_t burst_n = 0;
        const SplitEffectBurst* bursts = official_split_effect_bursts(ps[ci], &burst_n);
        float event_n = 0.0f;
        if (bursts != nullptr) {
          for (uint8_t bi = 0; bi < burst_n; ++bi) {
            event_n += std::max(bursts[bi].count, 0.0f) *
                       std::clamp(bursts[bi].probability, 0.0f, 1.0f);
          }
        } else if (ps[ci].emission_kind == static_cast<uint8_t>(SplitEffectEmission::Burst)) {
          event_n = std::max(ps[ci].emission, 0.0f);
        }
        const float cap =
            ps[ci].max_particles > 0 ? static_cast<float>(ps[ci].max_particles) : live[ci];
        live[ci] = std::min(
            cap, live[ci] + live[i] * event_n * std::max(subs[s].emit_probability, 0.0f));
      }
    }
    float work0 = 0.0f;
    for (uint16_t i = 0; i < n; ++i) {
      work0 += est_work(ps[i], live[i], n_from_rate(coord_rate(ps[i]), 4));
    }
    float extra_acc = 0.0f;
    for (uint16_t i = 0; i < n; ++i) {
      float extra = 0.0f;
      const int hold = decide_n(ps[i], live[i], work0, extra_acc, policy, &extra);
      extra_acc += extra;
      c.n[static_cast<size_t>(e.particle_begin + i)] = hold;
    }
  }
  return c;
}

const HoldCache& hold_cache() {
  static const HoldCache c = build_hold_cache();
  return c;
}

bool catalog_particle_index(const SplitEffectParticle& p, uint32_t& idx) noexcept {
  using split_effect_catalog_detail::kSplitEffectParticles;
  const auto* begin = kSplitEffectParticles;
  const ptrdiff_t n = static_cast<ptrdiff_t>(std::size(kSplitEffectParticles));
  const ptrdiff_t i = &p - begin;
  if (i < 0 || i >= n) {
    return false;
  }
  idx = static_cast<uint32_t>(i);
  return true;
}

int lookup_or_decide(const SplitEffectParticle& p, int32_t effect_id) noexcept {
  const HoldCache& c = hold_cache();
  uint32_t idx = 0;
  if (catalog_particle_index(p, idx) && idx < c.n.size()) {
    return c.n[idx];
  }
  if (effect_id != 0) {
    const SplitEffectCatalogEntry* e = official_split_effect(effect_id);
    uint16_t n = 0;
    const SplitEffectParticle* ps = e != nullptr ? official_split_effect_particles(*e, &n) : nullptr;
    if (ps != nullptr) {
      for (uint16_t i = 0; i < n; ++i) {
        if (std::memcmp(&ps[i], &p, sizeof(SplitEffectParticle)) == 0) {
          const size_t gi = static_cast<size_t>(e->particle_begin + i);
          if (gi < c.n.size()) {
            return c.n[gi];
          }
        }
      }
    }
  }
  const float live = est_live_own(p, effect_id);
  const float work0 = est_work(p, live, n_from_rate(coord_rate(p), 4));
  return decide_n(p, live, work0, 0.0f, split_noise_hold_densify_policy(), nullptr);
}

}  // namespace

float split_noise_coord_rate(const SplitEffectParticle& p) noexcept {
  return coord_rate(p);
}

float split_noise_hold_target_travel() noexcept {
  return target_travel();
}

float split_noise_hold_tvis() noexcept {
  return tvis_travel();
}

float split_noise_est_live(const SplitEffectParticle& p, int32_t effect_id) noexcept {
  return est_live_own(p, effect_id);
}

float split_noise_est_frame_work(const SplitEffectParticle& p, int hold_n,
                                 int32_t effect_id) noexcept {
  return est_work(p, est_live_own(p, effect_id), hold_n);
}

int split_noise_hold_for(const SplitEffectParticle& p) noexcept {
  return split_noise_hold_for(p, 0);
}

int split_noise_hold_for(const SplitEffectParticle& p, int32_t effect_id) noexcept {
  if (split_noise_exact_enabled()) {
    return 1;
  }
  if (!split_noise_hold_adapt()) {
    return split_noise_hold();
  }
  // Bind copies miss catalog_particle_index; lookup_or_decide then memcmp's
  // the whole row. Cache by row CONTENT + effect + overrides so hash / ctor /
  // hold share one decide per system on the worker. Not keyed on &p: p is
  // often a reused task slot (fs.tasks) that holds different rows over time,
  // so an address key returned stale counts depending on which worker ran
  // which task. The result is a pure function of (row bytes, effect_id,
  // overrides); padding differences can only cause a miss, never a wrong hit.
  thread_local bool have_row = false;
  alignas(SplitEffectParticle) thread_local unsigned char last_row[sizeof(SplitEffectParticle)];
  thread_local int32_t last_id = 0;
  thread_local int last_n = 4;
  thread_local int last_exact = -2;
  thread_local int last_hold_ov = -3;
  if (have_row && last_id == effect_id && last_exact == g_noise_exact_override &&
      last_hold_ov == g_noise_hold_override &&
      std::memcmp(last_row, &p, sizeof(SplitEffectParticle)) == 0) {
    return last_n;
  }
  last_n = lookup_or_decide(p, effect_id);
  std::memcpy(last_row, &p, sizeof(SplitEffectParticle));
  have_row = true;
  last_id = effect_id;
  last_exact = g_noise_exact_override;
  last_hold_ov = g_noise_hold_override;
  return last_n;
}

bool split_noise_hold_spread() noexcept {
  if (split_noise_exact_enabled()) {
    return false;
  }
  static const bool on = [] {
    const char* raw = std::getenv("WDS_SPLIT_NOISE_HOLD_SPREAD");
    if (raw == nullptr || raw[0] == '\0') {
      return true;
    }
    return raw[0] != '0';
  }();
  return on;
}

namespace {

bool axis_time_varying(uint8_t count, uint8_t min_count) noexcept {
  return count > 0 || min_count > 0;
}

bool axis_can_be_nonzero(float a, float b, uint8_t count, uint8_t min_count) noexcept {
  return std::fabs(a) > 1e-8f || std::fabs(b) > 1e-8f || axis_time_varying(count, min_count);
}

}  // namespace

bool split_particle_trail_path_straight(const SplitEffectParticle& p) noexcept {
  // hop (not 2022.3): lerp is exact iff velocity is constant on a line.
  // Speed change also breaks equal-age vertex spacing along a lerp.
  if (std::fabs(p.gravity_modifier) > 1e-8f) {
    return false;
  }
  if (p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Custom)) {
    return false;
  }
  if (p.inherit_enabled != 0 && p.inherit_mode != 0) {
    return false;
  }
  if (p.force_enabled != 0) {
    if (p.force_randomize != 0) {
      return false;
    }
    if (axis_can_be_nonzero(p.force_x, p.force_x_min, p.force_x_count, p.force_x_min_count) ||
        axis_can_be_nonzero(p.force_y, p.force_y_min, p.force_y_count, p.force_y_min_count) ||
        axis_can_be_nonzero(p.force_z, p.force_z_min, p.force_z_count, p.force_z_min_count)) {
      return false;
    }
  }
  if ((p.flags & kSplitEffectFlagNoise) != 0 &&
      official_split_effect_noise_position_curl_on(p.noise_position_amount)) {
    if (axis_can_be_nonzero(p.noise_strength, p.noise_strength_min, p.noise_strength_count,
                           p.noise_strength_min_count) ||
        axis_can_be_nonzero(p.noise_strength_y, p.noise_strength_y_min, p.noise_strength_y_count,
                           p.noise_strength_y_min_count) ||
        axis_can_be_nonzero(p.noise_strength_z, p.noise_strength_z_min, p.noise_strength_z_count,
                           p.noise_strength_z_min_count)) {
      return false;
    }
  }
  if ((p.flags & kSplitEffectFlagVelocity) != 0) {
    if (axis_time_varying(p.vel_x_count, p.vel_x_min_count) ||
        axis_time_varying(p.vel_y_count, p.vel_y_min_count) ||
        axis_time_varying(p.vel_z_count, p.vel_z_min_count)) {
      return false;
    }
    if (axis_can_be_nonzero(p.orbital_x, p.orbital_x_min, 0, 0) ||
        axis_can_be_nonzero(p.orbital_y, p.orbital_y_min, 0, 0) ||
        axis_can_be_nonzero(p.orbital_z, p.orbital_z_min, 0, 0)) {
      return false;
    }
    if (std::fabs(p.radial) > 1e-8f) {
      return false;
    }
  }
  if (p.clamp_enabled != 0) {
    if (p.clamp_dampen > 1e-8f) {
      return false;
    }
    if (axis_can_be_nonzero(p.clamp_drag, p.clamp_drag_min, p.clamp_drag_count, 0)) {
      return false;
    }
  }
  return true;
}

bool split_particle_trail_path_straight(const ParticleState& st) noexcept {
  if (st.p == nullptr) {
    return true;
  }
  const SplitEffectParticle& p = *st.p;
  if (std::fabs(p.gravity_modifier) > 1e-8f) {
    return false;
  }
  if (p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Custom)) {
    return false;
  }
  if (p.inherit_enabled != 0 && p.inherit_mode != 0) {
    return false;
  }
  if (p.force_enabled != 0) {
    if (p.force_randomize != 0) {
      return false;
    }
    if (axis_can_be_nonzero(p.force_x, p.force_x_min, p.force_x_count, p.force_x_min_count) ||
        axis_can_be_nonzero(p.force_y, p.force_y_min, p.force_y_count, p.force_y_min_count) ||
        axis_can_be_nonzero(p.force_z, p.force_z_min, p.force_z_count, p.force_z_min_count)) {
      return false;
    }
  }
  if ((p.flags & kSplitEffectFlagNoise) != 0 &&
      official_split_effect_noise_position_curl_on(p.noise_position_amount)) {
    if (axis_can_be_nonzero(p.noise_strength, p.noise_strength_min, p.noise_strength_count,
                           p.noise_strength_min_count) ||
        axis_can_be_nonzero(p.noise_strength_y, p.noise_strength_y_min, p.noise_strength_y_count,
                           p.noise_strength_y_min_count) ||
        axis_can_be_nonzero(p.noise_strength_z, p.noise_strength_z_min, p.noise_strength_z_count,
                           p.noise_strength_z_min_count)) {
      return false;
    }
  }
  if ((p.flags & kSplitEffectFlagVelocity) != 0) {
    if (axis_time_varying(p.vel_x_count, p.vel_x_min_count) ||
        axis_time_varying(p.vel_y_count, p.vel_y_min_count) ||
        axis_time_varying(p.vel_z_count, p.vel_z_min_count)) {
      return false;
    }
  }
  if (std::fabs(st.orb_x) > 1e-8f || std::fabs(st.orb_y) > 1e-8f || std::fabs(st.orb_z) > 1e-8f ||
      std::fabs(st.radial) > 1e-8f) {
    return false;
  }
  if (p.clamp_enabled != 0) {
    if (p.clamp_dampen > 1e-8f) {
      return false;
    }
    if (axis_can_be_nonzero(p.clamp_drag, p.clamp_drag_min, p.clamp_drag_count, 0)) {
      return false;
    }
  }
  return true;
}

bool split_closed_form() noexcept {
  static const bool on = [] {
    const char* raw = std::getenv("WDS_SPLIT_CLOSED_FORM");
    if (raw == nullptr || raw[0] == '\0') {
      return false;
    }
    return raw[0] == '1';
  }();
  return on;
}

int split_walk_hz() noexcept {
  if (g_walk_hz_override > 0) {
    return g_walk_hz_override;
  }
  static const int hz = [] {
    const char* raw = std::getenv("WDS_SPLIT_WALK_HZ");
    if (raw == nullptr || raw[0] == '\0') {
      // hop (not 2022.3): 60Hz parent walk. WDS_SPLIT_WALK_HZ=30 thins RoD
      // births visibly (11711/11621/10660 A/B); 120 is the finer march.
      return 60;
    }
    char* end = nullptr;
    const long v = std::strtol(raw, &end, 10);
    if (end == raw) {
      return 60;
    }
    if (v == 30 || v == 60 || v == 120) {
      return static_cast<int>(v);
    }
    return 60;
  }();
  return hz;
}

namespace {

constexpr int kVolN = 64;
constexpr float kVolT = 32.0f;
constexpr float kVolOffA = 19.0f;
constexpr float kVolOffB = 47.0f;

struct VolKey {
  int oct = 1;
  uint32_t scale_q = 0;
  uint32_t mult_q = 0;
  bool operator==(const VolKey& o) const noexcept {
    return oct == o.oct && scale_q == o.scale_q && mult_q == o.mult_q;
  }
};

struct VolKeyHash {
  std::size_t operator()(const VolKey& k) const noexcept {
    std::size_t h = static_cast<std::size_t>(k.oct);
    h = h * 1315423911u + k.scale_q;
    h = h * 1315423911u + k.mult_q;
    return h;
  }
};

uint32_t quant_vol_f(float v) noexcept {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
}

struct VolField {
  std::vector<float> xyz;
};

const VolField* bake_vol(const VolKey& key, int oct, float lac, float gain) {
  static std::shared_mutex mu;
  static std::unordered_map<VolKey, std::unique_ptr<VolField>, VolKeyHash> map;
  thread_local VolKey tls_key{};
  thread_local const VolField* tls_field = nullptr;
  if (tls_field != nullptr && tls_key == key) {
    return tls_field;
  }
  {
    std::shared_lock<std::shared_mutex> rd(mu);
    const auto it = map.find(key);
    if (it != map.end()) {
      tls_key = key;
      tls_field = it->second.get();
      return tls_field;
    }
  }
  std::unique_lock<std::shared_mutex> wr(mu);
  const auto it = map.find(key);
  if (it != map.end()) {
    tls_key = key;
    tls_field = it->second.get();
    return tls_field;
  }
  auto field = std::make_unique<VolField>();
  field->xyz.resize(static_cast<std::size_t>(kVolN * kVolN * kVolN * 3));
  const float cell = kVolT / static_cast<float>(kVolN);
  const int used_oct = std::clamp(oct, 1, 8);
  for (int z = 0; z < kVolN; ++z) {
    const float qz = (static_cast<float>(z) + 0.5f) * cell;
    for (int y = 0; y < kVolN; ++y) {
      const float qy = (static_cast<float>(y) + 0.5f) * cell;
      for (int x = 0; x < kVolN; ++x) {
        const float qx = (static_cast<float>(x) + 0.5f) * cell;
        float vx = 0, dx = 0, dy = 0, dz = 0;
        float dyx = 0, dyy = 0, dyz = 0;
        float dzx = 0, dzy = 0, dzz = 0;
        an_detail::fbm_value_grad(qx, qy, qz, used_oct, lac, gain, vx, dx, dy, dz);
        an_detail::fbm_value_grad(qx + kVolOffA, qy, qz, used_oct, lac, gain, vx, dyx, dyy, dyz);
        an_detail::fbm_value_grad(qx, qy + kVolOffB, qz, used_oct, lac, gain, vx, dzx, dzy, dzz);
        const std::size_t i =
            (static_cast<std::size_t>(z) * kVolN * kVolN + static_cast<std::size_t>(y) * kVolN +
             static_cast<std::size_t>(x)) *
            3;
        field->xyz[i + 0] = dzy - dyz;
        field->xyz[i + 1] = dz - dzx;
        field->xyz[i + 2] = dyx - dy;
      }
    }
  }
  auto [ins, _] = map.emplace(key, std::move(field));
  tls_key = key;
  tls_field = ins->second.get();
  return tls_field;
}

int wrap_vol_i(int i) noexcept {
  const int n = kVolN;
  i %= n;
  if (i < 0) {
    i += n;
  }
  return i;
}

void sample_vol(const VolField& f, float qx, float qy, float qz, float& cx, float& cy,
                float& cz) noexcept {
  const float s = static_cast<float>(kVolN) / kVolT;
  float ux = qx * s - 0.5f;
  float uy = qy * s - 0.5f;
  float uz = qz * s - 0.5f;
  const int i0 = wrap_vol_i(static_cast<int>(std::floor(ux)));
  const int j0 = wrap_vol_i(static_cast<int>(std::floor(uy)));
  const int k0 = wrap_vol_i(static_cast<int>(std::floor(uz)));
  const int i1 = wrap_vol_i(i0 + 1);
  const int j1 = wrap_vol_i(j0 + 1);
  const int k1 = wrap_vol_i(k0 + 1);
  const float fx = ux - std::floor(ux);
  const float fy = uy - std::floor(uy);
  const float fz = uz - std::floor(uz);
  const auto at = [&](int x, int y, int z) -> const float* {
    const std::size_t i = (static_cast<std::size_t>(z) * kVolN * kVolN +
                           static_cast<std::size_t>(y) * kVolN + static_cast<std::size_t>(x)) *
                          3;
    return &f.xyz[i];
  };
  const float* c000 = at(i0, j0, k0);
  const float* c100 = at(i1, j0, k0);
  const float* c010 = at(i0, j1, k0);
  const float* c110 = at(i1, j1, k0);
  const float* c001 = at(i0, j0, k1);
  const float* c101 = at(i1, j0, k1);
  const float* c011 = at(i0, j1, k1);
  const float* c111 = at(i1, j1, k1);
  const float ifx = 1.0f - fx;
  const float ify = 1.0f - fy;
  const float ifz = 1.0f - fz;
  cx = cy = cz = 0.0f;
  const float w000 = ifx * ify * ifz;
  const float w100 = fx * ify * ifz;
  const float w010 = ifx * fy * ifz;
  const float w110 = fx * fy * ifz;
  const float w001 = ifx * ify * fz;
  const float w101 = fx * ify * fz;
  const float w011 = ifx * fy * fz;
  const float w111 = fx * fy * fz;
  cx = c000[0] * w000 + c100[0] * w100 + c010[0] * w010 + c110[0] * w110 + c001[0] * w001 +
       c101[0] * w101 + c011[0] * w011 + c111[0] * w111;
  cy = c000[1] * w000 + c100[1] * w100 + c010[1] * w010 + c110[1] * w110 + c001[1] * w001 +
       c101[1] * w101 + c011[1] * w011 + c111[1] * w111;
  cz = c000[2] * w000 + c100[2] * w100 + c010[2] * w010 + c110[2] * w110 + c001[2] * w001 +
       c101[2] * w101 + c011[2] * w011 + c111[2] * w111;
}

}  // namespace

void eval_noise_velocity_vol_q(float px, float py, float pz, float freq, float scroll, float phase_x,
                               float phase_y, float phase_z, int oct, float oct_scale, float oct_mult,
                               float sx, float sy, float sz, float amt, float& vx, float& vy,
                               float& vz) noexcept {
  VolKey key;
  key.oct = std::clamp(oct, 1, 8);
  key.scale_q = quant_vol_f(oct_scale);
  key.mult_q = quant_vol_f(oct_mult);
  const VolField* field = bake_vol(key, key.oct, oct_scale, oct_mult);
  const float qx = px * freq + scroll + phase_x;
  const float qy = py * freq + phase_y;
  const float qz = pz * freq + phase_z;
  float cx = 0.0f, cy = 0.0f, cz = 0.0f;
  sample_vol(*field, qx, qy, qz, cx, cy, cz);
  vx = cx * freq * sx * amt;
  vy = cy * freq * sy * amt;
  vz = cz * freq * sz * amt;
}

float noise_seed_phase(uint32_t seed, uint32_t axis) noexcept {
  // A bounded domain offset is only a diagnostic hypothesis.  The hash is
  // not presented as Unity's random stream or as an authored seed mapping.
  return (u01(mix3(seed, 0x4e4f4953u + axis * 0x9e3779b9u, 0x50484153u)) -
          0.5f) * 2.0f;
}


float perlin3_reference(float x, float y, float z) noexcept {
  return perlin3_reference_impl(x, y, z);
}

float perlin3_optimized(float x, float y, float z) noexcept {
  return perlin3(x, y, z);
}

float fbm_reference(float x, float y, float z, int octaves, float lac, float gain) noexcept {
  return fbm_reference_impl(x, y, z, octaves, lac, gain);
}

float fbm_optimized(float x, float y, float z, int octaves, float lac, float gain) noexcept {
  return fbm(x, y, z, octaves, lac, gain);
}

void apply_noise(const ParticleState& st, float age, float& lx, float& ly, float& lz) noexcept {
  float vx = 0.0f, vy = 0.0f, vz = 0.0f;
  eval_noise_velocity(st, age, lx, ly, lz, vx, vy, vz);
  lx += vx;
  ly += vy;
  lz += vz;
}

void eval_noise_velocity_reference(const ParticleState& st, float age, float lx, float ly, float lz,
                                   float& vx, float& vy, float& vz) noexcept {
  vx = vy = vz = 0.0f;
  const SplitEffectParticle& p = *st.p;
  if ((p.flags & kSplitEffectFlagNoise) == 0) {
    return;
  }
  const float t01 = std::clamp(age / (st.life > 1e-6f ? st.life : 1.0f), 0.0f, 1.0f);
  float str_x = 0.0f, str_y = 0.0f, str_z = 0.0f;
  eval_noise_strength_axes(st, t01, str_x, str_y, str_z);
  if (std::fabs(str_x) < 1e-8f && std::fabs(str_y) < 1e-8f && std::fabs(str_z) < 1e-8f &&
      p.noise_strength_count == 0 && p.noise_strength_y_count == 0 &&
      p.noise_strength_z_count == 0) {
    return;
  }
  const float freq = p.noise_frequency > 1e-6f ? p.noise_frequency : 1.0f;
  const NoisePhaseExperiment phase_experiment = noise_phase_experiment();
  const float scroll = p.noise_scroll * age;
  // Unity 2022.3 Position Amount is a multiplier on particle positions
  // (Manual/PartSysNoiseModule.html, NoiseModule-positionAmount). The API
  // example slider includes 0. Hamming-1 iso_noise_size / iso_noise_rot set
  // positionAmount=0; Unity v19 dump vel/pose stay at birth. v23: Unity
  // |anim| scales through 1e-6; 0 still turns curl off. Do not rewrite 0
  // to 1. Curl ε stays the 2020 hop.
  const float amt = p.noise_position_amount;
  if (!official_split_effect_noise_position_curl_on(amt)) {
    return;
  }
  // Unity 2022.3 Noise samples the current particle position in Simulation
  // Space. World (1796): world metres — origin already includes the 27-unit
  // fade travel, do not multiply again. Local (76): PS-local lx/ly/lz.
  float px = lx;
  float py = ly;
  float pz = lz;
  // EVAL-GAP(Custom): the catalog preserves value 2, but this evaluator has
  // no MainModule.customSimulationSpace Transform source (Unity 2022.3:
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.MainModule-simulationSpace.html).
  // Keep the existing World path until that source is available; do not
  // invent a transform formula here.
  if (p.simulation_space != static_cast<uint8_t>(SplitEffectSimulationSpace::Local)) {
    const float origin = emitter_main_y(st.color_id, st.birth_fade) + st.birth_origin_y_delta;
    px = st.line_x + lx;
    py = origin + emitter_main_y_from_ly_slope(st.color_id) * ly;
    pz = lz + st.birth_parent_z;
  }
  const float oct_scale = p.noise_octave_scale > 1e-4f ? p.noise_octave_scale : 2.0f;
  const float oct_mult = p.noise_octave_mult > 1e-4f ? p.noise_octave_mult : 0.5f;
  const int oct = p.noise_octaves;
  const NoiseExperiment experiment = noise_experiment();
  // Bridson SIGGRAPH 2007 Curl-Noise: ψ = three uncorrelated Perlin samples
  // (same N at large offsets), v = ∇×ψ. Unity 2022.3 Noise names this
  // "Curl Noise" / "multiple samples of Perlin"; official quality=2 is High
  // (3D). Two-sided differences = 2 samples per axis (units/s after Strength).
  const float phase_x = phase_experiment == NoisePhaseExperiment::Seed
                            ? noise_seed_phase(st.seed, 0u)
                            : 0.0f;
  const float phase_y = phase_experiment == NoisePhaseExperiment::Seed
                            ? noise_seed_phase(st.seed, 1u)
                            : 0.0f;
  const float phase_z = phase_experiment == NoisePhaseExperiment::Seed
                            ? noise_seed_phase(st.seed, 2u)
                            : 0.0f;
  auto n_at = [&](float x, float y, float z) {
    return fbm_reference_impl(x * freq + scroll + phase_x, y * freq + phase_y,
                              z * freq + phase_z, oct, oct_scale, oct_mult);
  };
  constexpr float kOffA = 19.0f;
  constexpr float kOffB = 47.0f;
  auto psi_x = [&](float x, float y, float z) { return n_at(x, y, z); };
  auto psi_y = [&](float x, float y, float z) {
    if (experiment == NoiseExperiment::QualitySharedSample) return n_at(x, y, z);
    return n_at(x + kOffA, y, z);
  };
  auto psi_z = [&](float x, float y, float z) {
    if (experiment == NoiseExperiment::QualitySharedSample) return n_at(x + kOffA, y, z);
    return n_at(x, y + kOffB, z);
  };
  // Approved Bridson SIGGRAPH 2007 hop (not a Unity page): finite-difference
  // offset is 1e-4 × domain. Domain = 1/frequency (Unity Frequency period).
  // synth-mesh-measure-v14 Wave 1/4: this curl+ε vs Unity animatedVelocity
  // failed the gate (frac≈0 on all Hamming-1 bins). Unconfirmed. Do not
  // invent a replacement mesh. Keep this hop. Not 2022.3.
  // Damping: "Higher frequency noise reduces the strength by a proportional
  // amount" (NoiseModule-damping) so the field can be scaled "while
  // maintaining the same behaviour, but at a different size." That is
  // strength/frequency against the extra ∇ factor from ψ(freq·x).
  // Strength: "move faster and further" — the speed scale, not ×freq.
  // The native path uses a frequency-normalized offset.  The Bridson and
  // shared-sample variants retain that canonical domain scaling; the
  // frequency-damped variant deliberately holds the finite-difference step
  // fixed so damping is represented only by strength/frequency.
  const float eps = experiment == NoiseExperiment::FrequencyDampedCurl ? 1e-4f : 1e-4f / freq;
  const float inv_2eps = 1.0f / (2.0f * eps);
  float sx = str_x;
  float sy = str_y;
  float sz = str_z;
  if (p.noise_damping != 0 &&
      experiment != NoiseExperiment::BridsonVectorPotential &&
      experiment != NoiseExperiment::QualitySharedSample) {
    sx /= freq;
    sy /= freq;
    sz /= freq;
  }
  vx = ((psi_z(px, py + eps, pz) - psi_z(px, py - eps, pz)) -
        (psi_y(px, py, pz + eps) - psi_y(px, py, pz - eps))) *
       inv_2eps * sx * amt;
  vy = ((psi_x(px, py, pz + eps) - psi_x(px, py, pz - eps)) -
        (psi_z(px + eps, py, pz) - psi_z(px - eps, py, pz))) *
       inv_2eps * sy * amt;
  vz = ((psi_y(px + eps, py, pz) - psi_y(px - eps, py, pz)) -
        (psi_x(px, py + eps, pz) - psi_x(px, py - eps, pz))) *
       inv_2eps * sz * amt;
}

void eval_noise_velocity(const ParticleState& st, float age, float lx, float ly, float lz,
                         float& vx, float& vy, float& vz, float* unit_x, float* unit_y,
                         float* unit_z) noexcept {
  vx = vy = vz = 0.0f;
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
  const NoiseExperiment experiment = noise_experiment();
  const float phase_x = phase_experiment == NoisePhaseExperiment::Seed
                            ? noise_seed_phase(st.seed, 0u)
                            : 0.0f;
  const float phase_y = phase_experiment == NoisePhaseExperiment::Seed
                            ? noise_seed_phase(st.seed, 1u)
                            : 0.0f;
  const float phase_z = phase_experiment == NoisePhaseExperiment::Seed
                            ? noise_seed_phase(st.seed, 2u)
                            : 0.0f;
  constexpr float kOffA = 19.0f;
  constexpr float kOffB = 47.0f;
  const bool quality_shared = experiment == NoiseExperiment::QualitySharedSample;
  auto n_pair_x = [&](float xa, float xb, float y, float z, float& a, float& b) {
    fbm_pair_x(xa * freq + scroll + phase_x, xb * freq + scroll + phase_x, y * freq + phase_y,
               z * freq + phase_z, oct, oct_scale, oct_mult, a, b);
  };
  auto n_pair_y = [&](float x, float ya, float yb, float z, float& a, float& b) {
    fbm_pair_y(x * freq + scroll + phase_x, ya * freq + phase_y, yb * freq + phase_y,
               z * freq + phase_z, oct, oct_scale, oct_mult, a, b);
  };
  auto n_pair_z = [&](float x, float y, float za, float zb, float& a, float& b) {
    fbm_pair_z(x * freq + scroll + phase_x, y * freq + phase_y, za * freq + phase_z,
               zb * freq + phase_z, oct, oct_scale, oct_mult, a, b);
  };
  const float eps = experiment == NoiseExperiment::FrequencyDampedCurl ? 1e-4f : 1e-4f / freq;
  const float inv_2eps = 1.0f / (2.0f * eps);
  float sx = str_x;
  float sy = str_y;
  float sz = str_z;
  if (p.noise_damping != 0 &&
      experiment != NoiseExperiment::BridsonVectorPotential &&
      experiment != NoiseExperiment::QualitySharedSample) {
    sx /= freq;
    sy /= freq;
    sz /= freq;
  }
  if (!split_noise_exact_enabled() && !quality_shared) {
    eval_noise_velocity_an(st, age, lx, ly, lz, vx, vy, vz, unit_x, unit_y, unit_z);
    return;
  }
#if WDS_NOISE_HAS_NEON
  if (split_noise_neon_enabled() && !quality_shared) {
    auto n4 = [&](const float wx[4], const float wy[4], const float wz[4], float o[4]) {
      fbm_world_x4(wx, wy, wz, freq, scroll, phase_x, phase_y, phase_z, oct, oct_scale, oct_mult, o);
    };
    float wx[4], wy[4], wz[4], o[4];
    wx[0] = px;
    wy[0] = py + eps + kOffB;
    wz[0] = pz;
    wx[1] = px;
    wy[1] = py - eps + kOffB;
    wz[1] = pz;
    wx[2] = px + kOffA;
    wy[2] = py;
    wz[2] = pz + eps;
    wx[3] = px + kOffA;
    wy[3] = py;
    wz[3] = pz - eps;
    n4(wx, wy, wz, o);
    const float pz_yp = o[0];
    const float pz_ym = o[1];
    const float py_zp = o[2];
    const float py_zm = o[3];
    wx[0] = px;
    wy[0] = py;
    wz[0] = pz + eps;
    wx[1] = px;
    wy[1] = py;
    wz[1] = pz - eps;
    wx[2] = px + eps;
    wy[2] = py + kOffB;
    wz[2] = pz;
    wx[3] = px - eps;
    wy[3] = py + kOffB;
    wz[3] = pz;
    n4(wx, wy, wz, o);
    const float px_zp = o[0];
    const float px_zm = o[1];
    const float pz_xp = o[2];
    const float pz_xm = o[3];
    wx[0] = px + eps + kOffA;
    wy[0] = py;
    wz[0] = pz;
    wx[1] = px - eps + kOffA;
    wy[1] = py;
    wz[1] = pz;
    wx[2] = px;
    wy[2] = py + eps;
    wz[2] = pz;
    wx[3] = px;
    wy[3] = py - eps;
    wz[3] = pz;
    n4(wx, wy, wz, o);
    const float py_xp = o[0];
    const float py_xm = o[1];
    const float px_yp = o[2];
    const float px_ym = o[3];
    finish_noise_vel(((pz_yp - pz_ym) - (py_zp - py_zm)) * inv_2eps * amt,
                     ((px_zp - px_zm) - (pz_xp - pz_xm)) * inv_2eps * amt,
                     ((py_xp - py_xm) - (px_yp - px_ym)) * inv_2eps * amt, sx, sy, sz, vx, vy, vz,
                     unit_x, unit_y, unit_z);
    return;
  }
#endif
  float pz_yp = 0.0f, pz_ym = 0.0f;
  float py_zp = 0.0f, py_zm = 0.0f;
  float px_zp = 0.0f, px_zm = 0.0f;
  float pz_xp = 0.0f, pz_xm = 0.0f;
  float py_xp = 0.0f, py_xm = 0.0f;
  float px_yp = 0.0f, px_ym = 0.0f;
  if (quality_shared) {
    n_pair_y(px + kOffA, py + eps, py - eps, pz, pz_yp, pz_ym);
    n_pair_z(px, py, pz + eps, pz - eps, py_zp, py_zm);
    n_pair_x((px + eps) + kOffA, (px - eps) + kOffA, py, pz, pz_xp, pz_xm);
    n_pair_x(px + eps, px - eps, py, pz, py_xp, py_xm);
  } else {
    n_pair_y(px, (py + eps) + kOffB, (py - eps) + kOffB, pz, pz_yp, pz_ym);
    n_pair_z(px + kOffA, py, pz + eps, pz - eps, py_zp, py_zm);
    n_pair_x(px + eps, px - eps, py + kOffB, pz, pz_xp, pz_xm);
    n_pair_x((px + eps) + kOffA, (px - eps) + kOffA, py, pz, py_xp, py_xm);
  }
  n_pair_z(px, py, pz + eps, pz - eps, px_zp, px_zm);
  n_pair_y(px, py + eps, py - eps, pz, px_yp, px_ym);
  finish_noise_vel(((pz_yp - pz_ym) - (py_zp - py_zm)) * inv_2eps * amt,
                   ((px_zp - px_zm) - (pz_xp - pz_xm)) * inv_2eps * amt,
                   ((py_xp - py_xm) - (px_yp - px_ym)) * inv_2eps * amt, sx, sy, sz, vx, vy, vz,
                   unit_x, unit_y, unit_z);
}

float eval_noise_n(const ParticleState& st, float age, float world_x, float main_y,
                   float world_z) noexcept {
  const SplitEffectParticle& p = *st.p;
  if ((p.flags & kSplitEffectFlagNoise) == 0) {
    return 0.0f;
  }
  // Same n_at as eval_noise_velocity (dimensionless Perlin / fbm).
  const float freq = p.noise_frequency > 1e-6f ? p.noise_frequency : 1.0f;
  const float scroll = p.noise_scroll * age;
  const float oct_scale = p.noise_octave_scale > 1e-4f ? p.noise_octave_scale : 2.0f;
  const float oct_mult = p.noise_octave_mult > 1e-4f ? p.noise_octave_mult : 0.5f;
  const int oct = p.noise_octaves;
  float px = world_x;
  float py = main_y;
  float pz = world_z;
  // EVAL-GAP(Custom): without a customSimulationSpace Transform (Unity
  // 2022.3 MainModule.simulationSpace), value 2 remains on the existing
  // World sample path. No transform is inferred.
  if (p.simulation_space == static_cast<uint8_t>(SplitEffectSimulationSpace::Local)) {
    if (st.event_emit != 0) {
      // Unity 2022.3 Sub Emitters emit at the parent particle pose
      // (https://docs.unity3d.com/2022.3/Manual/PartSysSubEmitModule.html).
      // integrate_pose applies that event translation after the child has
      // been integrated in its own Local simulation space. Undo the same
      // translation before sampling Local Noise, so the parent pose is not
      // accidentally sampled as a second child-space displacement.
      float loc_x = p.local_pos_x * wds::chart_editor::kOfficialSplitLineLocalScaleX;
      float loc_y = p.local_pos_y;
      loc_y *= wds::chart_editor::kOfficialLineHeightScale * std::max(st.fade_scale, 0.0f);
      const float origin = emitter_main_y(st.color_id, st.fade_scale);
      const float origin_with_pose = origin + st.birth_origin_y_delta;
      const float wy0 =
          origin_with_pose + emitter_main_y_from_ly_slope(st.color_id) * loc_y;
      world_x -= st.event_wx - (st.line_x + loc_x);
      main_y -= st.event_wy - wy0;
      world_z -= st.event_wz - p.local_pos_z - st.birth_parent_z;
    }
    // invert apply_epilogue Local: Main → integrator lx/ly/lz (Noise samples
    // Simulation Space, Manual/PartSysNoiseModule.html). Hierarchy divides
    // out parent 27×fade. Local scaling only subtracts Transform
    // localPos×(parent_scale−1); Shape/motion stay PS-local.
    px = world_x - st.line_x;
    pz = world_z;
    const float live_y_scale =
        wds::chart_editor::kOfficialLineHeightScale * std::max(st.fade_scale, 0.0f);
    const float origin = emitter_main_y(st.color_id, st.fade_scale) + st.birth_origin_y_delta;
    float ly = emitter_main_y_from_ly_slope(st.color_id) * (main_y - origin);
    if (p.scaling_mode != static_cast<uint8_t>(SplitEffectScalingMode::Local)) {
      if (live_y_scale > 1e-8f) {
        ly /= live_y_scale;
      }
    } else {
      ly -= p.local_pos_y * (live_y_scale - 1.0f);
    }
    py = ly;
  }
  return fbm(px * freq + scroll, py * freq, pz * freq, oct, oct_scale, oct_mult);
}

void eval_sol_axes(const SplitEffectParticle& p, float t01, float ux, float uy, float uz, float& sx,
                   float& sy, float& sz) noexcept {
  sx = 1.0f;
  sy = 1.0f;
  sz = 1.0f;
  if ((p.flags & kSplitEffectFlagSizeOverLifetime) == 0) {
    return;
  }
  sx = official_split_effect_eval_sol(p, t01);
  // Unity 2022.3 TwoCurves: Evaluate(t, lerpFactor) between min and max.
  // live dump dump-pending-v2 11350 hop (not 2022.3): separateAxes TwoCurves
  // draws an independent u per axis. Non-separate copies sx onto Y/Z.
  if (p.sol_min_count > 0) {
    const float lo =
        official_split_effect_eval_sol_axis(p.sol_min_begin, p.sol_min_count, t01, sx);
    sx = lo + (sx - lo) * std::clamp(ux, 0.0f, 1.0f);
  }
  // Unity 1D Size-over-Lifetime (separateAxes off) applies the same curve to
  // every axis. Catalog sol_y is 1 when unused; a non-1 sol_y without Y keys
  // is separateAxes constant Y (e.g. thorns).
  if (p.sol_y_count > 0) {
    sy = official_split_effect_eval_sol_axis(p.sol_y_begin, p.sol_y_count, t01, p.sol_y);
    if (p.sol_y_min_count > 0) {
      const float lo =
          official_split_effect_eval_sol_axis(p.sol_y_min_begin, p.sol_y_min_count, t01, sy);
      sy = lo + (sy - lo) * std::clamp(uy, 0.0f, 1.0f);
    }
  } else if (p.sol_y > 1e-4f && std::fabs(p.sol_y - 1.0f) > 1e-4f) {
    sy = p.sol_y;
  } else {
    sy = sx;
  }
  if (p.sol_z_count > 0) {
    sz = official_split_effect_eval_sol_axis(p.sol_z_begin, p.sol_z_count, t01, 1.0f);
    if (p.sol_z_min_count > 0) {
      const float lo =
          official_split_effect_eval_sol_axis(p.sol_z_min_begin, p.sol_z_min_count, t01, sz);
      sz = lo + (sz - lo) * std::clamp(uz, 0.0f, 1.0f);
    }
  } else {
    sz = sx;
  }
}

void eval_col(const SplitEffectParticle& p, float t01, float col_u, float& r, float& g, float& b,
              float& a) noexcept {
  r = g = b = a = 1.0f;
  if ((p.flags & kSplitEffectFlagColorOverLifetime) == 0) {
    return;
  }
  official_split_effect_eval_col(p, t01, col_u, r, g, b, a);
}

uint32_t catalog_index(const SplitEffectParticle& p) noexcept {
  using split_effect_catalog_detail::kSplitEffectParticles;
  const auto* begin = kSplitEffectParticles;
  const ptrdiff_t n = static_cast<ptrdiff_t>(std::size(kSplitEffectParticles));
  const ptrdiff_t i = &p - begin;
  if (i < 0 || i >= n) {
    return 0;
  }
  return static_cast<uint32_t>(i);
}

float eval_minmax_axis(float max_v, float min_v, uint8_t random, uint16_t begin, uint8_t count,
                       uint16_t min_begin, uint8_t min_count, float t01, float u) noexcept {
  float hi = max_v;
  if (count > 0) {
    hi = official_split_effect_eval_curve_keys(begin, count, t01, max_v);
  }
  if (random == 0) {
    return hi;
  }
  float lo = min_v;
  if (min_count > 0) {
    lo = official_split_effect_eval_curve_keys(min_begin, min_count, t01, min_v);
  }
  return lo + (hi - lo) * std::clamp(u, 0.0f, 1.0f);
}

void eval_noise_strength_axes(const ParticleState& st, float t01, float& sx, float& sy,
                              float& sz) noexcept {
  const SplitEffectParticle& p = *st.p;
  sx = eval_minmax_axis(p.noise_strength, p.noise_strength_min, p.noise_strength_random,
                        p.noise_strength_begin, p.noise_strength_count, p.noise_strength_min_begin,
                        p.noise_strength_min_count, t01, st.noise_ux);
  if (p.noise_separate_axes != 0) {
    sy = eval_minmax_axis(p.noise_strength_y, p.noise_strength_y_min, p.noise_strength_y_random,
                          p.noise_strength_y_begin, p.noise_strength_y_count,
                          p.noise_strength_y_min_begin, p.noise_strength_y_min_count, t01,
                          st.noise_uy);
    sz = eval_minmax_axis(p.noise_strength_z, p.noise_strength_z_min, p.noise_strength_z_random,
                          p.noise_strength_z_begin, p.noise_strength_z_count,
                          p.noise_strength_z_min_begin, p.noise_strength_z_min_count, t01,
                          st.noise_uz);
  } else {
    sy = sx;
    sz = sx;
  }
}

void world_force_to_lane(float wx, float wy, float wz, float& lx, float& ly, float& lz) noexcept {
  static const float rad =
      wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
  static const float c = std::cos(rad);
  static const float s = std::sin(rad);
  lx = wx;
  ly = wy * c + wz * s;
  lz = -wy * s + wz * c;
}

void lane_to_world(float lx, float ly, float lz, float& wx, float& wy, float& wz) noexcept {
  static const float rad =
      wds::chart_editor::official_deg_to_rad(wds::chart_editor::kOfficialLaneTiltDeg);
  static const float c = std::cos(rad);
  static const float s = std::sin(rad);
  wx = lx;
  wy = ly * c - lz * s;
  wz = ly * s + lz * c;
}

float emitter_inherit_speed(int32_t color_id, float play_t) noexcept {
  // d(emitter_main_y)/dt from the same pose helper as local_y_to_main_y.
  // live dump refix-d 11700_1 hop, not 2022.3.
  float origin = 0.0f;
  float signed_lever = 0.0f;
  emitter_main_pose(color_id, emit_local_y(color_id), origin, signed_lever);
  (void)origin;
  const float deriv =
      wds::chart_editor::official_split_fade_in_scale_deriv(play_t - kSplitEffectAnimatorLagSec);
  return signed_lever * deriv;
}

float eval_rotation_over_life(const SplitEffectParticle& p, float age, float life, float u01,
                              uint8_t axis) noexcept {
  if (p.rot_ol_enabled == 0) {
    return 0.0f;
  }
  // Unity 2022.3 RotationOverLifetimeModule.separateAxes: "When disabled,
  // only the z-axis rotation curve is used."
  if (p.rot_ol_separate == 0 && axis != 2) {
    return 0.0f;
  }
  auto omega_at = [&](float t01) noexcept {
    if (axis == 0 && p.rot_ol_separate != 0) {
      return eval_minmax_axis(p.rot_ol_x, p.rot_ol_x_min, p.rot_ol_x_random, p.rot_ol_x_begin,
                              p.rot_ol_x_count, p.rot_ol_x_min_begin, p.rot_ol_x_min_count, t01,
                              u01);
    }
    if (axis == 1 && p.rot_ol_separate != 0) {
      return eval_minmax_axis(p.rot_ol_y, p.rot_ol_y_min, p.rot_ol_y_random, p.rot_ol_y_begin,
                              p.rot_ol_y_count, p.rot_ol_y_min_begin, p.rot_ol_y_min_count, t01,
                              u01);
    }
    return eval_minmax_axis(p.rot_ol, p.rot_ol_min, p.rot_ol_random, p.rot_ol_begin, p.rot_ol_count,
                            p.rot_ol_min_begin, p.rot_ol_min_count, t01, u01);
  };
  // Unity 2022.3 Rotation over Lifetime: "Angular Velocity … degrees per
  // second" / "how the velocity changes throughout the lifetime". Catalog
  // scalars are radians (π/2 = startRotation 90°). ω(t)×age reverses when
  // the curve falls to 0; the angle is ∫ω dt.
  const bool curved =
      (axis == 0 && p.rot_ol_separate != 0)
          ? (p.rot_ol_x_count > 0 || p.rot_ol_x_min_count > 0)
          : (axis == 1 && p.rot_ol_separate != 0)
                ? (p.rot_ol_y_count > 0 || p.rot_ol_y_min_count > 0)
                : (p.rot_ol_count > 0 || p.rot_ol_min_count > 0);
  if (!curved || age <= 1e-8f) {
    return omega_at(0.0f) * std::max(age, 0.0f);
  }
  const float t_end = std::max(integration_candidate_age(age), 0.0f);
  const float hz = integration_candidate_hz();
  const float life_s = life > 1e-6f ? life : 1.0f;
  if (split_integrator_is_legacy()) {
    const int steps = std::max(1, static_cast<int>(std::ceil(t_end * hz)));
    const float dt = t_end / static_cast<float>(steps);
    float acc = 0.0f;
    for (int i = 0; i < steps; ++i) {
      const float t = dt * (static_cast<float>(i) + 0.5f);
      acc += omega_at(std::clamp(t / life_s, 0.0f, 1.0f)) * dt;
    }
    return acc;
  }
  const float h = 1.0f / hz;
  const int k = t_end <= 0.0f ? 0 : static_cast<int>(std::floor(t_end * hz + 1e-4f));
  float r = t_end - static_cast<float>(k) * h;
  if (r <= 1e-7f) {
    r = 0.0f;
  }
  float acc = 0.0f;
  int i0 = 0;
  const ParticleCacheKey* ck = g_rol_cache_key;
  if (ck != nullptr && ck->valid != 0) {
    int have = -1;
    float have_acc = 0.0f;
    if (split_eval_cache_lookup_rol(*ck, axis, k, have, have_acc) && have >= 0) {
      acc = have_acc;
      i0 = have;
    }
  }
  for (int i = i0; i < k; ++i) {
    const float t = h * (static_cast<float>(i) + 0.5f);
    acc += omega_at(std::clamp(t / life_s, 0.0f, 1.0f)) * h;
  }
  if (ck != nullptr && ck->valid != 0 && k - i0 >= 2) {
    split_eval_cache_store_rol(*ck, axis, k, acc);
  }
  if (r > 0.0f) {
    const float t = static_cast<float>(k) * h + 0.5f * r;
    acc += omega_at(std::clamp(t / life_s, 0.0f, 1.0f)) * r;
  }
  return acc;
}

void fill_trails(PoseIntegrator& integ, const ParticleState& st, float age,
                 SplitEffectLiveParticle& out) noexcept {
  const SplitEffectParticle& p = *st.p;
  split_effect_clear_live_trails(out);
  out.trail_chain = 0;
  out.trail_vertex = 0;
  out.trail_count = 0;
  // Unity 2022.3 Trails: "Size affects Width" multiplies trail width by the
  // particle size. That is the current size (startSize × Size-over-Lifetime),
  // same value written to out.size. Birth startSize alone skipped SOL (10120
  // Two Curves peak ~0.5) and made Ribbon currents thicker than OpenWDS.
  const float size_mul = p.trail_size_affects_width != 0 ? out.size : 1.0f;
  // Unity 2022.3 Trails widthOverTrail: "the width of each trail point"
  // (ScriptReference/ParticleSystem.TrailModule-widthOverTrail.html).
  // Ribbon stitch_ribbons evals at 3D arc/total (oldest living particle = 0;
  // hop, ribbon-jitter-v1 / align2-n6). Particle-mode playback uses one
  // LiveParticle.trail_width for every vertex — t=0 is the head (newest).
  // Keep the head value for cheap callers; Particle-mode fills trail_widths
  // below once the number of points is known. Ribbon stitch_ribbons replaces
  // this value per particle after age ordering.
  out.trail_width = p.trail_width * size_mul;
  out.trail_over_alpha_begin = p.trail_over_alpha_begin;
  out.trail_over_alpha_count = p.trail_over_alpha_count;
  out.trail_over_color_begin = p.trail_over_color_begin;
  out.trail_over_color_count = p.trail_over_color_count;
  out.trail_over_min_alpha_begin = p.trail_over_min_alpha_begin;
  out.trail_over_min_alpha_count = p.trail_over_min_alpha_count;
  out.trail_over_min_color_begin = p.trail_over_min_color_begin;
  out.trail_over_min_color_count = p.trail_over_min_color_count;
  out.trail_over_state = p.trail_over_state;
  out.trail_over_mode = p.trail_over_mode;
  out.trail_over_min_mode = p.trail_over_min_mode;
  out.trail_over_r = p.trail_over_r;
  out.trail_over_g = p.trail_over_g;
  out.trail_over_b = p.trail_over_b;
  out.trail_over_r_min = p.trail_over_r_min;
  out.trail_over_g_min = p.trail_over_g_min;
  out.trail_over_b_min = p.trail_over_b_min;
  out.trail_color_u = st.col_u;
  out.trail_over_a0 = p.trail_over_a0;
  out.trail_over_a1 = p.trail_over_a1;
  // Unity 2022.3 Trails Color over Lifetime: "the color of the entire Trail
  // over the lifetime of the particle" (Manual/PartSysTrailsModule.html).
  // t01 = particle age / startLifetime, clamped at death so dieWith=false
  // does not invent a post-death fade. Color-over-Trail (stitch / playback)
  // modulates along the strip. inheritParticleColor sits on top.
  const float part_life = st.life > 1e-6f ? st.life : age;
  const float head_age = std::min(age, part_life);
  const float t01 = st.life > 1e-6f ? head_age / st.life : 1.0f;
  float tr = p.trail_r;
  float tg = p.trail_g;
  float tb = p.trail_b;
  float ta = p.trail_a;
  official_split_effect_eval_trail_col(p, t01, st.col_u, tr, tg, tb, ta);
  if (p.trail_inherit_particle_color != 0) {
    // Unity 2022.3 inheritParticleColor: trail overrides sit on the particle
    // color (GetCurrentColor = start × color modules, Color32). Custom1 is a
    // vertex stream, not a color module — do not multiply the ParticleAdd
    // draw hop on out.r.
    // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.TrailModule-inheritParticleColor.html
    // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.Particle.GetCurrentColor.html
    out.trail_r = tr * out.current_r;
    out.trail_g = tg * out.current_g;
    out.trail_b = tb * out.current_b;
    out.trail_a = ta * out.current_a;
  } else {
    out.trail_r = tr;
    out.trail_g = tg;
    out.trail_b = tb;
    out.trail_a = ta;
  }
  if ((p.flags & kSplitEffectFlagTrails) == 0) {
    return;
  }
  // Ribbon (mode 1) connects living particles by age in stitch_ribbons.
  if (p.trail_mode == 1) {
    return;
  }
  if (p.trail_ratio > 0.0f && u01(mix3(st.seed, 70, 1u)) > p.trail_ratio) {
    return;
  }
  // Unity 2022.3 Trails Lifetime: "The lifetime of each vertex in the Trail,
  // expressed as a multiplier of the lifetime of the particle it belongs to.
  // When each new vertex is added to the Trail, it disappears after it has
  // been in existence longer than its total lifetime."
  // https://docs.unity3d.com/2022.3/Documentation/Manual/PartSysTrailsModule.html
  // API: "This value is relative to the particle lifetime."
  // https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ParticleSystem.TrailModule-lifetime.html
  // Vertex lifetime = Lifetime × startLifetime (2022.3 TrailModule.lifetime).
  // dieWithParticles=false: caller passes wall-clock age (may exceed life);
  // head freezes at death, no new vertices, remaining points expire at
  // t_rec + L×life. Window = [max(0, clock − L×life), min(clock, life)].
  // live dump dump-pending-v2 hop (not 2022.3): Curve L = scalar ×
  // curve(age/startLifetime) every frame. TwoConstants stay the birth pick.
  const float trail_mul =
      p.trail_lifetime_count > 0
          ? eval_minmax_axis(p.trail_lifetime, p.trail_lifetime_min, p.trail_lifetime_random,
                             p.trail_lifetime_begin, p.trail_lifetime_count,
                             p.trail_lifetime_min_begin, p.trail_lifetime_min_count, t01, 0.0f)
          : (st.trail_life_mul >= 0.0f ? st.trail_life_mul : std::max(p.trail_lifetime, 0.0f));
  const float vert_life = std::max(trail_mul, 0.0f) * part_life;
  // Official minVertexDistance is 0.1 or 0.2 on all 59 Particle trails.
  // No published vertex cap; worst official path needs ~481. The 8-slot
  // array plus the 0.012–0.12s step clamp are storage, not Unity.
  const float min_d = p.trail_min_vertex > 1e-6f ? p.trail_min_vertex : 0.0f;
  const float window_start = std::max(0.0f, age - vert_life);
  const float trail_age = std::max(0.0f, head_age - window_start);
  if (trail_age <= 1e-8f) {
    return;
  }
  float step = trail_age;
  if (min_d > 1e-6f) {
    // Particle-trail minVertexDistance consumes this same triple
    // (out.vel_* = grid FD of pose). Not trace.total: that omits orbital /
    // radial displacement and the speed modifier.
    const float speed =
        std::sqrt(out.vel_x * out.vel_x + out.vel_y * out.vel_y + out.vel_z * out.vel_z);
    if (speed > 1e-4f) {
      step = min_d / speed;
    }
  } else {
    step = 1.0f / 60.0f;
  }
  int n = 1;
  if (step > 1e-6f) {
    n = static_cast<int>(std::floor(trail_age / step)) + 1;
  }
  n = std::clamp(n, 3, kSplitEffectTrailMaxPoints);
  split_effect_alloc_live_trails_xyz(out, static_cast<uint16_t>(n));
  split_effect_alloc_live_trails_attrs(out, static_cast<uint16_t>(n));
  const float denom = static_cast<float>(n - 1);
  auto vertex_age = [&](int i) noexcept -> float {
    if (i == 0) {
      return head_age;
    }
    if (i == n - 1 && window_start <= 1e-6f) {
      return 0.0f;
    }
    return window_start + trail_age * static_cast<float>(n - 1 - i) / denom;
  };
  // hop (not 2022.3): lerp is exact on a constant-velocity line. Otherwise one
  // forward PoseIntegrator walk (vertex ages ascending, same run_step as
  // integrate-each-vertex). Keep equal-age n = clamp(floor(trail_age/step)+1,
  // 3, 512). Evidence: WDS_SPLIT_TRAIL_LERP=0 10770/10660 matches Unity braid;
  // default lerp drew straight lines. Do not sample(0) on the live integrator.
  if (split_particle_trail_path_straight(st)) {
    float bx = 0.0f;
    float by = 0.0f;
    float bz = 0.0f;
    integ.birth_world(bx, by, bz);
    if (window_start > 1e-6f) {
      const ParticleCacheKey* trail_rol = g_rol_cache_key;
      set_rol_cache_key(nullptr);
      PoseIntegrator tail_integ(st);
      tail_integ.set_cache_key(nullptr);
      tail_integ.sample(window_start, bx, by, bz, nullptr);
      set_rol_cache_key(trail_rol);
    }
    const float hx = out.world_x;
    const float hy = out.main_y;
    const float hz = out.z;
    for (int i = 0; i < n; ++i) {
      const float u = static_cast<float>(n - 1 - i) / denom;
      out.trail_x[static_cast<size_t>(i)] = bx + (hx - bx) * u;
      out.trail_y[static_cast<size_t>(i)] = by + (hy - by) * u;
      out.trail_z[static_cast<size_t>(i)] = bz + (hz - bz) * u;
    }
  } else {
    const ParticleCacheKey* trail_rol = g_rol_cache_key;
    set_rol_cache_key(nullptr);
    const float hz = integration_candidate_hz();
    auto grid_k = [hz](float ta) noexcept -> int {
      const float t_end = std::max(integration_candidate_age(ta), 0.0f);
      return t_end <= 0.0f ? 0 : static_cast<int>(std::floor(t_end * hz + 1e-4f));
    };
    const int k0 = grid_k(window_start <= 1e-6f ? 0.0f : window_start);
    const int k1 = grid_k(head_age);
    const IntegratorGridState* log = integ.grid_log_data();
    const int log_n = integ.grid_log_size();
    const int log_origin = integ.grid_log_origin_step();
    const bool log_covers =
        log != nullptr && log_n > 0 && log_origin >= 0 && log_origin <= k0 &&
        log_origin + log_n > k1;
    PoseIntegrator path_integ(st);
    path_integ.set_cache_key(nullptr);
    auto emit_at = [&](PoseIntegrator& src, int i, float ta) noexcept {
      const size_t ii = static_cast<size_t>(i);
      if (ta <= 1e-8f) {
        src.birth_world(out.trail_x[ii], out.trail_y[ii], out.trail_z[ii]);
        return;
      }
      src.sample(ta, out.trail_x[ii], out.trail_y[ii], out.trail_z[ii], nullptr);
    };
    if (log_covers) {
      for (int i = n - 1; i >= 0; --i) {
        const float ta = vertex_age(i);
        if (ta <= 1e-8f) {
          emit_at(path_integ, i, ta);
          continue;
        }
        const int k = grid_k(ta);
        path_integ.import_grid(log[k - log_origin]);
        emit_at(path_integ, i, ta);
      }
      const float dx = out.trail_x[0] - out.world_x;
      const float dy = out.trail_y[0] - out.main_y;
      const float dz = out.trail_z[0] - out.z;
      if (dx * dx + dy * dy + dz * dz > 1e-6f) {
        PoseIntegrator replay(st);
        replay.set_cache_key(nullptr);
        for (int i = n - 1; i >= 0; --i) {
          emit_at(replay, i, vertex_age(i));
        }
      }
    } else {
      for (int i = n - 1; i >= 0; --i) {
        emit_at(path_integ, i, vertex_age(i));
      }
    }
    set_rol_cache_key(trail_rol);
  }
  out.trail_points = static_cast<uint16_t>(n);
  const float span = std::max(static_cast<float>(n - 1), 1.0f);
  // MinMaxCurve random interpolation is sampled once for the particle and
  // then evaluated along the trail. This reuses the existing module seed
  // contract (stable per particle, independent of vertex count).
  const bool width_random = p.trail_width_state == 2 || p.trail_width_state == 3;
  const float width_u = u01(mix3(st.seed, 72, 1u));
  for (int i = 0; i < n; ++i) {
    const float t_head = static_cast<float>(i) / span;
    const float width = eval_minmax_axis(
        p.trail_width, p.trail_width_min, width_random ? 1u : 0u, p.trail_width_begin,
        p.trail_width_count, p.trail_width_min_begin, p.trail_width_min_count, t_head, width_u);
    out.trail_widths[static_cast<size_t>(i)] = std::max(width * size_mul, 0.0f);
    if (i == 0) {
      out.trail_width = out.trail_widths[0];
    }
    float over_r = 1.0f, over_g = 1.0f, over_b = 1.0f, over_a = 1.0f;
    official_split_effect_eval_trail_over(p, t_head, st.col_u, over_r, over_g, over_b, over_a);
    out.trail_color_r[static_cast<size_t>(i)] = out.trail_r * over_r;
    out.trail_color_g[static_cast<size_t>(i)] = out.trail_g * over_g;
    out.trail_color_b[static_cast<size_t>(i)] = out.trail_b * over_b;
    out.trail_color_a[static_cast<size_t>(i)] = out.trail_a * over_a;
  }
}

namespace {
uint32_t fbits_u32(float x) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &x, sizeof(u));
  return u != 0 ? u : 1u;
}
}  // namespace

void stitch_ribbons(const SplitEffectParticle& p, SplitEffectLiveParticle* out, int n,
                    int32_t line_index, uint32_t system_id) noexcept {
  if (out == nullptr || n < 2 || p.trail_mode != 1) {
    return;
  }
  SplitWallScope wall_st(SplitWallSec::Stitch);
  // Unity 2022.3 Ribbon: "connecting each particle based on its age."
  // ribbonCount is 1 on every official system — one chain in birth order.
  // hop (not 2022.3): widthOverTrail / colorOverTrail t is 3D arc prefix /
  // total, same t as Stretch UV (trail_u0). t=0 oldest living particle
  // (BakeTrails station 0 / near field), t=1 newest. live dump
  // ribbon-jitter-v1 11230_1 / 10820: 1422/1422 stations α ≡ Color32(1−u)
  // (mae 0.00093; index mae 0.00464); n-change Δα follows Δu (mae 0.0013)
  // not Δindex (mae 0.0028). align2-n6 11230_1 fi015 station 2 half-width
  // = 0.5×size×curve(uv), not curve(index). Pages say "over its length"
  // and do not name index vs arc or which end is t=0. Old t=i/(n-1) jumped
  // by particle rank when RoD changed n during fade-in. Particle-mode
  // fill_trails still uses t=0 = head (newest).
  // textureMode Stretch maps the texture once along the whole length
  // (3D arc; u=0 oldest). hop, not 2022.3.
  // BakeTrails is the inter-particle 3D quad strip (same class as PTrail).
  // Cross stays on Trails. Do not write Cross into Stretch.
  float total = 0.0f;
  for (int i = 1; i < n; ++i) {
    const float dx = out[i].world_x - out[i - 1].world_x;
    const float dy = out[i].main_y - out[i - 1].main_y;
    const float dz = out[i].z - out[i - 1].z;
    total += std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  const float denom = std::max(p.trail_width, 1e-6f);
  // live dump v21: bind copies make catalog_index(p)==0. Hashing living
  // count n then merged 10121/22/25/26 (all n=26 on one line) into one
  // strip — Unity BakeTrails is one ribbon per ParticleSystem
  // (ribbonCount=1). Identity is catalog GO index when collect provides
  // it, plus authored fields that survive hydrate+bind. Do not hash n.
  const uint32_t authored =
      mix3(mix3(fbits_u32(p.size), fbits_u32(p.shape_pos_y), fbits_u32(p.shape_radius)),
           mix3(fbits_u32(p.shape_pos_z), fbits_u32(p.local_pos_y), fbits_u32(p.trail_width)),
           mix3(static_cast<uint32_t>(p.element_index) + 1u,
                static_cast<uint32_t>(p.uv_name_index) + 1u, fbits_u32(p.rate_over_distance)));
  const uint32_t chain =
      mix3(system_id + 1u, static_cast<uint32_t>(line_index) + 1u, authored);
  // Prefix acc is the same 3D arc used for trail_u0/u1. total==0: t=0 at
  // the oldest station and t=1 elsewhere (same as fill_ribbon_stretch_along).
  float acc = 0.0f;
  for (int i = 0; i < n; ++i) {
    SplitEffectLiveParticle& v = out[i];
    const float t = total > 1e-6f ? acc / total : (i == 0 ? 0.0f : 1.0f);
    const bool width_random = p.trail_width_state == 2 || p.trail_width_state == 3;
    const float width_u = u01(mix3(v.seed, 72, 1u));
    const float curve_w = eval_minmax_axis(
        p.trail_width, p.trail_width_min, width_random ? 1u : 0u, p.trail_width_begin,
        p.trail_width_count, p.trail_width_min_begin, p.trail_width_min_count, t, width_u);
    const float size_mul = v.trail_width / denom;
    v.trail_width = std::max(curve_w * size_mul, 0.0f);
    float over_r = 1.0f, over_g = 1.0f, over_b = 1.0f, over_a = 1.0f;
    official_split_effect_eval_trail_over(p, t, v.trail_color_u, over_r, over_g, over_b, over_a);
    v.trail_r *= over_r;
    v.trail_g *= over_g;
    v.trail_b *= over_b;
    v.trail_a *= std::clamp(over_a, 0.0f, 1.0f);
    v.trail_chain = chain;
    v.trail_vertex = static_cast<uint16_t>(i);
    v.trail_count = static_cast<uint16_t>(n);
    if (i + 1 >= n) {
      continue;
    }
    SplitEffectLiveParticle& nxt = out[i + 1];
    const float dx = nxt.world_x - v.world_x;
    const float dy = nxt.main_y - v.main_y;
    const float dz = nxt.z - v.z;
    const float seg = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float u_old = total > 1e-6f ? acc / total : 0.0f;
    acc += seg;
    const float u_new = total > 1e-6f ? acc / total : 1.0f;
    nxt.trail_points = 2;
    split_effect_alloc_live_trails_xyz(nxt, 2);
    nxt.trail_x[0] = v.world_x;
    nxt.trail_y[0] = v.main_y;
    nxt.trail_z[0] = v.z;
    nxt.trail_x[1] = nxt.world_x;
    nxt.trail_y[1] = nxt.main_y;
    nxt.trail_z[1] = nxt.z;
    nxt.trail_u0 = u_old;
    nxt.trail_u1 = u_new;
  }
}

}  // namespace wds::chart_render::shuriken

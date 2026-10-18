#include <shuriken/internal.hpp>
#include <shuriken/modules_an.inl.hpp>
#include <shuriken/simd_dispatch.hpp>
#include <wds/chart_render/split_effect_catalog.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

namespace {

using wds::chart_render::SplitEffectParticle;
using wds::chart_render::SplitEffectSimulationSpace;
using wds::chart_render::official_split_effect;
using wds::chart_render::official_split_effect_particles;
using wds::chart_render::shuriken::ParticleState;
using wds::chart_render::shuriken::eval_noise_velocity;
using wds::chart_render::shuriken::eval_noise_velocity_reference;
using wds::chart_render::shuriken::fbm_optimized;
using wds::chart_render::shuriken::fbm_reference;
using wds::chart_render::shuriken::perlin3_optimized;
using wds::chart_render::shuriken::perlin3_reference;

int failures = 0;

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

uint32_t fbits(float x) noexcept {
  uint32_t u = 0;
  std::memcpy(&u, &x, sizeof(u));
  return u;
}

bool same_bits(float a, float b) noexcept {
  return fbits(a) == fbits(b);
}

void expect_bits(float got, float ref, const char* what, int line, float x, float y, float z) {
  if (same_bits(got, ref)) {
    return;
  }
  std::fprintf(stderr, "FAIL:%d: %s bits got=%08x ref=%08x (%.9g vs %.9g) @ (%.9g,%.9g,%.9g)\n",
               line, what, fbits(got), fbits(ref), static_cast<double>(got),
               static_cast<double>(ref), static_cast<double>(x), static_cast<double>(y),
               static_cast<double>(z));
  ++failures;
}

float i2f(uint32_t u) noexcept {
  float x = 0.0f;
  std::memcpy(&x, &u, sizeof(x));
  return x;
}

float bounded_rand(std::mt19937& rng, float lo, float hi) {
  std::uniform_real_distribution<float> dist(lo, hi);
  return dist(rng);
}

SplitEffectParticle catalog_noise_particle() {
  const auto* entry = official_split_effect(11621);
  uint16_t n = 0;
  const SplitEffectParticle* ps = official_split_effect_particles(*entry, &n);
  for (uint16_t i = 0; i < n; ++i) {
    if ((ps[i].flags & wds::chart_render::kSplitEffectFlagNoise) != 0) {
      return ps[i];
    }
  }
  return ps[0];
}

void test_perlin_and_fbm_random() {
  std::mt19937 rng(0x4e4f4953u);
  constexpr int kN = 20000;
  for (int i = 0; i < kN; ++i) {
    const float x = bounded_rand(rng, -64.0f, 64.0f);
    const float y = bounded_rand(rng, -64.0f, 64.0f);
    const float z = bounded_rand(rng, -64.0f, 64.0f);
    expect_bits(perlin3_optimized(x, y, z), perlin3_reference(x, y, z), "perlin3", __LINE__, x, y,
                z);
    const int oct = 1 + static_cast<int>(rng() % 8);
    const float lac = bounded_rand(rng, 1.25f, 3.0f);
    const float gain = bounded_rand(rng, 0.25f, 0.85f);
    expect_bits(fbm_optimized(x, y, z, oct, lac, gain), fbm_reference(x, y, z, oct, lac, gain),
                "fbm", __LINE__, x, y, z);
  }
}

void test_perlin_lattice_and_pairs() {
  const float pts[][3] = {
      {0.0f, 0.0f, 0.0f},
      {1.0f, 2.0f, 3.0f},
      {-1.0f, -2.0f, -3.0f},
      {0.5f, 0.5f, 0.5f},
      {-0.25f, 1.75f, 8.125f},
      {1e-4f, -1e-4f, 2e-4f},
      {2.0f - 1e-4f, 2.0f + 1e-4f, 2.0f},
      {16.0f, -16.0f, 31.999f},
  };
  for (const auto& p : pts) {
    expect_bits(perlin3_optimized(p[0], p[1], p[2]), perlin3_reference(p[0], p[1], p[2]), "perlin3",
                __LINE__, p[0], p[1], p[2]);
  }
  // Same-cell and cell-crossing +/- 1e-4 pairs (curl octave-0 spacing).
  const float anchors[] = {-4.0f, -1.0f, -1e-4f, 0.0f, 1e-4f, 1.0f, 2.0f, 7.5f};
  for (float a : anchors) {
    const float b = a + 2e-4f;
    const float c = a - 2e-4f;
    expect_bits(perlin3_optimized(a, b, 0.3f), perlin3_reference(a, b, 0.3f), "perlin3+", __LINE__,
                a, b, 0.3f);
    expect_bits(perlin3_optimized(a, c, 0.3f), perlin3_reference(a, c, 0.3f), "perlin3-", __LINE__,
                a, c, 0.3f);
    expect_bits(fbm_optimized(a, b, 0.3f, 1, 2.0f, 0.5f), fbm_reference(a, b, 0.3f, 1, 2.0f, 0.5f),
                "fbm+", __LINE__, a, b, 0.3f);
  }
}

void test_eval_noise_velocity_catalog_and_random() {
  wds::chart_render::shuriken::set_split_noise_exact_for_tests(1);
  SplitEffectParticle base = catalog_noise_particle();
  std::mt19937 rng(0x4355524cu);
  constexpr int kN = 4000;
  for (int i = 0; i < kN; ++i) {
    SplitEffectParticle p = base;
    if ((rng() & 1u) != 0u) {
      p.simulation_space = static_cast<uint8_t>(SplitEffectSimulationSpace::Local);
    }
    if ((rng() & 3u) == 0u) {
      p.noise_octaves = static_cast<uint8_t>(1 + (rng() % 4));
    }
    if ((rng() & 3u) == 1u) {
      p.noise_frequency = bounded_rand(rng, 0.1f, 4.0f);
    }
    if ((rng() & 7u) == 0u) {
      p.noise_separate_axes = 1;
      p.noise_strength_y = bounded_rand(rng, 0.1f, 2.0f);
      p.noise_strength_z = bounded_rand(rng, 0.1f, 2.0f);
    }
    if ((rng() & 7u) == 1u) {
      p.noise_scroll = bounded_rand(rng, -1.0f, 1.0f);
    }
    ParticleState st;
    st.p = &p;
    st.color_id = 11621;
    st.line_x = bounded_rand(rng, -2.0f, 2.0f);
    st.birth_fade = bounded_rand(rng, 0.2f, 1.0f);
    st.fade_scale = st.birth_fade;
    st.birth_origin_y_delta = bounded_rand(rng, -0.5f, 0.5f);
    st.birth_parent_z = bounded_rand(rng, -1.0f, 1.0f);
    st.seed = rng();
    const float age = bounded_rand(rng, 0.0f, 2.5f);
    const float lx = bounded_rand(rng, -3.0f, 3.0f);
    const float ly = bounded_rand(rng, -3.0f, 3.0f);
    const float lz = bounded_rand(rng, -3.0f, 3.0f);
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    eval_noise_velocity(st, age, lx, ly, lz, ox, oy, oz);
    eval_noise_velocity_reference(st, age, lx, ly, lz, rx, ry, rz);
    if (!same_bits(ox, rx) || !same_bits(oy, ry) || !same_bits(oz, rz)) {
      std::fprintf(stderr,
                   "FAIL:%d: eval_noise_velocity bits opt=(%08x,%08x,%08x) ref=(%08x,%08x,%08x) "
                   "age=%.9g p=(%.9g,%.9g,%.9g) sim=%u oct=%u freq=%.9g\n",
                   __LINE__, fbits(ox), fbits(oy), fbits(oz), fbits(rx), fbits(ry), fbits(rz),
                   static_cast<double>(age), static_cast<double>(lx), static_cast<double>(ly),
                   static_cast<double>(lz), p.simulation_space, p.noise_octaves,
                   static_cast<double>(p.noise_frequency));
      ++failures;
      if (failures > 20) {
        break;
      }
    }
  }
  // Explicit cell-crossing curl sample (11621 freq=0.5 → eps=2e-4).
  SplitEffectParticle p = base;
  p.simulation_space = static_cast<uint8_t>(SplitEffectSimulationSpace::Local);
  ParticleState st;
  st.p = &p;
  st.color_id = 11621;
  st.seed = 1;
  float ox = 0.0f, oy = 0.0f, oz = 0.0f;
  float rx = 0.0f, ry = 0.0f, rz = 0.0f;
  eval_noise_velocity(st, 0.4f, 0.0f, 4.0f, 0.0f, ox, oy, oz);
  eval_noise_velocity_reference(st, 0.4f, 0.0f, 4.0f, 0.0f, rx, ry, rz);
  CHECK(same_bits(ox, rx) && same_bits(oy, ry) && same_bits(oz, rz));

  wds::chart_render::shuriken::set_split_noise_exact_for_tests(1);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(1);
  int neon_mismatch = 0;
  for (int i = 0; i < 2000; ++i) {
    const float age = bounded_rand(rng, 0.0f, 2.5f);
    const float lx = bounded_rand(rng, -3.0f, 3.0f);
    const float ly = bounded_rand(rng, -3.0f, 3.0f);
    const float lz = bounded_rand(rng, -3.0f, 3.0f);
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    wds::chart_render::shuriken::set_split_noise_neon_for_tests(1);
    eval_noise_velocity(st, age, lx, ly, lz, nx, ny, nz);
    wds::chart_render::shuriken::set_split_noise_neon_for_tests(0);
    eval_noise_velocity(st, age, lx, ly, lz, sx, sy, sz);
    if (!same_bits(nx, sx) || !same_bits(ny, sy) || !same_bits(nz, sz)) {
      ++neon_mismatch;
      if (neon_mismatch <= 4) {
        std::fprintf(stderr,
                     "FAIL:%d: NEON FD bits (%08x,%08x,%08x) vs scalar (%08x,%08x,%08x)\n",
                     __LINE__, fbits(nx), fbits(ny), fbits(nz), fbits(sx), fbits(sy), fbits(sz));
      }
    }
  }
  CHECK(neon_mismatch == 0);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(-1);

  wds::chart_render::shuriken::set_split_noise_exact_for_tests(0);
  int an_simd_mismatch = 0;
  for (int i = 0; i < 2000; ++i) {
    const float age = bounded_rand(rng, 0.0f, 2.5f);
    const float lx = bounded_rand(rng, -3.0f, 3.0f);
    const float ly = bounded_rand(rng, -3.0f, 3.0f);
    const float lz = bounded_rand(rng, -3.0f, 3.0f);
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    wds::chart_render::shuriken::set_split_noise_neon_for_tests(1);
    eval_noise_velocity(st, age, lx, ly, lz, nx, ny, nz);
    wds::chart_render::shuriken::set_split_noise_neon_for_tests(0);
    eval_noise_velocity(st, age, lx, ly, lz, sx, sy, sz);
    if (!same_bits(nx, sx) || !same_bits(ny, sy) || !same_bits(nz, sz)) {
      ++an_simd_mismatch;
      if (an_simd_mismatch <= 4) {
        std::fprintf(stderr,
                     "FAIL:%d: AN SIMD bits (%08x,%08x,%08x) vs scalar (%08x,%08x,%08x)\n",
                     __LINE__, fbits(nx), fbits(ny), fbits(nz), fbits(sx), fbits(sy), fbits(sz));
      }
    }
  }
  CHECK(an_simd_mismatch == 0);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(-1);

  wds::chart_render::shuriken::set_split_noise_exact_for_tests(0);
  int far = 0;
  double max_abs = 0.0;
  for (int i = 0; i < 800; ++i) {
    const float age = bounded_rand(rng, 0.0f, 2.5f);
    const float lx = bounded_rand(rng, -3.0f, 3.0f);
    const float ly = bounded_rand(rng, -3.0f, 3.0f);
    const float lz = bounded_rand(rng, -3.0f, 3.0f);
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    eval_noise_velocity(st, age, lx, ly, lz, ax, ay, az);
    eval_noise_velocity_reference(st, age, lx, ly, lz, fx, fy, fz);
    const double dx = std::fabs(static_cast<double>(ax) - static_cast<double>(fx));
    const double dy = std::fabs(static_cast<double>(ay) - static_cast<double>(fy));
    const double dz = std::fabs(static_cast<double>(az) - static_cast<double>(fz));
    const double mag = std::max(dx, std::max(dy, dz));
    max_abs = std::max(max_abs, mag);
    const double scale = std::max(1.0, std::fabs(static_cast<double>(fx)) +
                                           std::fabs(static_cast<double>(fy)) +
                                           std::fabs(static_cast<double>(fz)));
    if (mag > 0.05 * scale && mag > 1e-3) {
      ++far;
    }
  }
  CHECK(far < 16);
  wds::chart_render::shuriken::set_split_noise_exact_for_tests(-1);
  (void)max_abs;
}

void test_bit_patterns() {
  std::mt19937 rng(0x5045524cu);
  for (int i = 0; i < 4000; ++i) {
    const float x = i2f(rng());
    const float y = i2f(rng());
    const float z = i2f(rng());
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
      continue;
    }
    if (std::fabs(x) > 1.0e6f || std::fabs(y) > 1.0e6f || std::fabs(z) > 1.0e6f) {
      continue;
    }
    expect_bits(perlin3_optimized(x, y, z), perlin3_reference(x, y, z), "perlin3-bits", __LINE__, x,
                y, z);
  }
}

template <class Fn>
double ns_per_call(int iters, Fn&& fn) {
  fn();
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    fn();
  }
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(iters);
}

void run_microbench() {
  std::mt19937 rng(7);
  constexpr int kN = 256;
  float xs[kN], ys[kN], zs[kN];
  for (int i = 0; i < kN; ++i) {
    xs[i] = bounded_rand(rng, -8.0f, 8.0f);
    ys[i] = bounded_rand(rng, -8.0f, 8.0f);
    zs[i] = bounded_rand(rng, -8.0f, 8.0f);
  }
  volatile float sink = 0.0f;
  constexpr int kIters = 40000;
  const double pref = ns_per_call(kIters, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      s += perlin3_reference(xs[i], ys[i], zs[i]);
    }
    sink = s;
  }) / static_cast<double>(kN);
  const double popt = ns_per_call(kIters, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      s += perlin3_optimized(xs[i], ys[i], zs[i]);
    }
    sink = s;
  }) / static_cast<double>(kN);
  const double fref = ns_per_call(kIters, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      s += fbm_reference(xs[i], ys[i], zs[i], 1, 2.0f, 0.5f);
    }
    sink = s;
  }) / static_cast<double>(kN);
  const double fopt = ns_per_call(kIters, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      s += fbm_optimized(xs[i], ys[i], zs[i], 1, 2.0f, 0.5f);
    }
    sink = s;
  }) / static_cast<double>(kN);

  SplitEffectParticle p = catalog_noise_particle();
  p.simulation_space = static_cast<uint8_t>(SplitEffectSimulationSpace::Local);
  ParticleState st;
  st.p = &p;
  st.color_id = 11621;
  st.seed = 1;
  const double cref = ns_per_call(8000, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      eval_noise_velocity_reference(st, 0.4f, xs[i], ys[i], zs[i], vx, vy, vz);
      s += vx + vy + vz;
    }
    sink = s;
  }) / static_cast<double>(kN);
  const double copt = ns_per_call(8000, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      eval_noise_velocity(st, 0.4f, xs[i], ys[i], zs[i], vx, vy, vz);
      s += vx + vy + vz;
    }
    sink = s;
  }) / static_cast<double>(kN);
  wds::chart_render::shuriken::set_split_noise_exact_for_tests(1);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(1);
  const double cneon = ns_per_call(8000, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      eval_noise_velocity(st, 0.4f, xs[i], ys[i], zs[i], vx, vy, vz);
      s += vx + vy + vz;
    }
    sink = s;
  }) / static_cast<double>(kN);
  wds::chart_render::shuriken::set_split_noise_exact_for_tests(0);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(0);
  const double canal = ns_per_call(8000, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      eval_noise_velocity(st, 0.4f, xs[i], ys[i], zs[i], vx, vy, vz);
      s += vx + vy + vz;
    }
    sink = s;
  }) / static_cast<double>(kN);
  wds::chart_render::shuriken::set_split_noise_exact_for_tests(0);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(1);
  const double canneon = ns_per_call(8000, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      eval_noise_velocity(st, 0.4f, xs[i], ys[i], zs[i], vx, vy, vz);
      s += vx + vy + vz;
    }
    sink = s;
  }) / static_cast<double>(kN);
  wds::chart_render::shuriken::set_split_noise_exact_for_tests(-1);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(-1);

  // hop: eval_noise_velocity_an NEON (3 curl potentials, lane 3 unused) vs scalar.
  wds::chart_render::split_simd_force_for_tests("neon");
  wds::chart_render::shuriken::set_split_noise_exact_for_tests(0);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(1);
  const double an_neon = ns_per_call(8000, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      wds::chart_render::shuriken::eval_noise_velocity_an(st, 0.4f, xs[i], ys[i], zs[i], vx, vy, vz);
      s += vx + vy + vz;
    }
    sink = s;
  }) / static_cast<double>(kN);
  wds::chart_render::split_simd_force_for_tests("scalar");
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(0);
  const double an_scalar = ns_per_call(8000, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      float vx = 0.0f, vy = 0.0f, vz = 0.0f;
      wds::chart_render::shuriken::eval_noise_velocity_an(st, 0.4f, xs[i], ys[i], zs[i], vx, vy, vz);
      s += vx + vy + vz;
    }
    sink = s;
  }) / static_cast<double>(kN);
  wds::chart_render::split_simd_force_for_tests(nullptr);
  wds::chart_render::shuriken::set_split_noise_neon_for_tests(-1);

  // hop estimate only: table-free arithmetic-hash lattice gradient (not production).
  auto hash_grad = [](float x, float y, float z) {
    const float xf = x - std::floor(x);
    const float yf = y - std::floor(y);
    const float zf = z - std::floor(z);
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    auto h = [](int a, int b, int c) {
      uint32_t u = static_cast<uint32_t>(a) * 374761393u + static_cast<uint32_t>(b) * 668265263u +
                   static_cast<uint32_t>(c) * 1274126177u;
      u ^= u >> 16;
      u *= 0x7feb352du;
      u ^= u >> 15;
      return (u & 1u) ? 1.0f : -1.0f;
    };
    return h(ix, iy, iz) * xf + h(ix, iy + 1, iz) * yf + h(ix, iy, iz + 1) * zf;
  };
  const double hash_ns = ns_per_call(kIters, [&] {
    float s = 0.0f;
    for (int i = 0; i < kN; ++i) {
      s += hash_grad(xs[i], ys[i], zs[i]);
    }
    sink = s;
  }) / static_cast<double>(kN);

  std::printf(
      "noise-microbench sink=%.6g\n"
      "  perlin3 ns/call  ref=%.2f opt=%.2f\n"
      "  fbm(oct=1) ns/call ref=%.2f opt=%.2f\n"
      "  eval_noise_velocity ns/call fd_ref=%.2f fd_pair=%.2f fd_neon=%.2f "
      "analytic=%.2f analytic_neon=%.2f\n"
      "  eval_noise_velocity_an ns/call neon=%.2f scalar=%.2f neon/scalar=%.3f\n"
      "  hash_grad ns/call=%.2f  perlin3_opt/hash=%.2f\n",
      static_cast<double>(sink), pref, popt, fref, fopt, cref, copt, cneon, canal, canneon, an_neon,
      an_scalar, an_scalar > 1e-9 ? an_scalar / an_neon : 0.0, hash_ns,
      hash_ns > 1e-9 ? popt / hash_ns : 0.0);
}

// split_noise_hold_for must not key its per-thread cache on the address of p:
// task slots are reused for different rows. Reuse one address for many rows
// and require the same answer as the stable catalog pointer.
void test_hold_for_reused_slot() {
  using wds::chart_render::shuriken::split_noise_hold_for;
  const int32_t ids[] = {11621, 11711, 10380};
  SplitEffectParticle slot = catalog_noise_particle();
  int compared = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (const int32_t id : ids) {
      const auto* entry = official_split_effect(id);
      if (entry == nullptr) {
        continue;
      }
      uint16_t n = 0;
      const SplitEffectParticle* ps = official_split_effect_particles(*entry, &n);
      for (uint16_t i = 0; i < n; ++i) {
        const uint16_t k = pass == 0 ? i : static_cast<uint16_t>(n - 1 - i);
        const int expect = split_noise_hold_for(ps[k], id);
        slot = ps[k];  // same address, different content each iteration
        CHECK(split_noise_hold_for(slot, id) == expect);
        CHECK(split_noise_hold_for(slot, id) == expect);
        ++compared;
      }
    }
  }
  CHECK(compared > 0);
}

}  // namespace

int main(int argc, char** argv) {
  test_hold_for_reused_slot();
  test_perlin_and_fbm_random();
  test_perlin_lattice_and_pairs();
  test_eval_noise_velocity_catalog_and_random();
  test_bit_patterns();
  if (failures != 0) {
    std::fprintf(stderr, "%d noise bit-exact checks failed\n", failures);
    return 1;
  }
  std::printf("wds_shuriken_noise_tests: ok\n");
  if (argc > 1 && std::string(argv[1]) == "--bench") {
    run_microbench();
  }
  return 0;
}

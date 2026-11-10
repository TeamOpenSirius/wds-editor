// MSVC compiles this TU with /arch:AVX2. Include only the POD+decl header
// (integrate_mask0_x86.hpp → integrate_hot.hpp). Do not include internal.hpp
// or any header with inline functions / templates: their COMDAT would be
// AVX2-encoded and the linker might pick it for the whole program.
#include "integrate_mask0_x86.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

#if defined(__clang__) || defined(__GNUC__)
#define WDS_MASK0_AVX2_ATTR __attribute__((target("avx2,fma")))
#else
#define WDS_MASK0_AVX2_ATTR
#endif

namespace wds::chart_render::shuriken {
namespace {

WDS_MASK0_AVX2_ATTR
inline void mask0_commit_lane_avx2(IntegratorHot& h, IntegrationVelocityTrace& tr, float lx,
                                   float ly, float lz, float vx, float vy, float vz,
                                   int kmin) noexcept {
  h.lx = lx;
  h.ly = ly;
  h.lz = lz;
  h.vx = vx;
  h.vy = vy;
  h.vz = vz;
  h.next_step = kmin;
  tr.stored_x = vx;
  tr.stored_y = vy;
  tr.stored_z = vz;
  tr.vol_x = tr.vol_y = tr.vol_z = 0.0f;
  tr.noise_x = tr.noise_y = tr.noise_z = 0.0f;
  tr.inherited_x = tr.inherited_y = tr.inherited_z = 0.0f;
  tr.total_x = vx;
  tr.total_y = vy;
  tr.total_z = vz;
}

}  // namespace

// Whole load / n-step / store compiled as Avx2+FMA. Loop body has no calls.
// MSVC: this TU is built with /arch:AVX2 (see chart-render/CMakeLists.txt).
WDS_MASK0_AVX2_ATTR
void mask0_batch_loop_x4_avx2(IntegratorHot* h0, IntegratorHot* h1, IntegratorHot* h2,
                              IntegratorHot* h3, IntegrationVelocityTrace* t0,
                              IntegrationVelocityTrace* t1, IntegrationVelocityTrace* t2,
                              IntegrationVelocityTrace* t3, int kmin, float dt, float gy, float gz,
                              float sm) noexcept {
  const int i0 = h0->next_step;
  const int nsteps = kmin - i0;
  if (nsteps <= 0) {
    return;
  }
  __m128 lx = _mm_setr_ps(h0->lx, h1->lx, h2->lx, h3->lx);
  __m128 ly = _mm_setr_ps(h0->ly, h1->ly, h2->ly, h3->ly);
  __m128 lz = _mm_setr_ps(h0->lz, h1->lz, h2->lz, h3->lz);
  __m128 vx = _mm_setr_ps(h0->vx, h1->vx, h2->vx, h3->vx);
  __m128 vy = _mm_setr_ps(h0->vy, h1->vy, h2->vy, h3->vy);
  __m128 vz = _mm_setr_ps(h0->vz, h1->vz, h2->vz, h3->vz);
  const __m128 vdt = _mm_set1_ps(dt);
  const __m128 vgy = _mm_set1_ps(gy);
  const __m128 vgz = _mm_set1_ps(gz);
  const __m128 vsm = _mm_set1_ps(sm);
  for (int i = 0; i < nsteps; ++i) {
    vy = _mm_fmadd_ps(vgy, vdt, vy);
    vz = _mm_fmadd_ps(vgz, vdt, vz);
    lx = _mm_add_ps(lx, _mm_mul_ps(_mm_mul_ps(vx, vdt), vsm));
    ly = _mm_add_ps(ly, _mm_mul_ps(_mm_mul_ps(vy, vdt), vsm));
    lz = _mm_add_ps(lz, _mm_mul_ps(_mm_mul_ps(vz, vdt), vsm));
  }
  alignas(16) float lxa[4], lya[4], lza[4], vxa[4], vya[4], vza[4];
  _mm_store_ps(lxa, lx);
  _mm_store_ps(lya, ly);
  _mm_store_ps(lza, lz);
  _mm_store_ps(vxa, vx);
  _mm_store_ps(vya, vy);
  _mm_store_ps(vza, vz);
  mask0_commit_lane_avx2(*h0, *t0, lxa[0], lya[0], lza[0], vxa[0], vya[0], vza[0], kmin);
  mask0_commit_lane_avx2(*h1, *t1, lxa[1], lya[1], lza[1], vxa[1], vya[1], vza[1], kmin);
  mask0_commit_lane_avx2(*h2, *t2, lxa[2], lya[2], lza[2], vxa[2], vya[2], vza[2], kmin);
  mask0_commit_lane_avx2(*h3, *t3, lxa[3], lya[3], lza[3], vxa[3], vya[3], vza[3], kmin);
}

WDS_MASK0_AVX2_ATTR
void mask0_scalar_loop_avx2(IntegratorHot* h, IntegrationVelocityTrace* tr, int nsteps, float dt,
                            float gy, float gz, float sm) noexcept {
  if (h == nullptr || nsteps <= 0) {
    return;
  }
  const float vx = h->vx;
  // Same fused ops as 4-wide: gravity vfmadd, Δp mul×mul+add (explicit so
  // target("avx2,fma") cannot contract displacement).
  const __m128 vgy = _mm_set_ss(gy);
  const __m128 vgz = _mm_set_ss(gz);
  const __m128 vdt = _mm_set_ss(dt);
  const __m128 vsm = _mm_set_ss(sm);
  const __m128 vvx = _mm_set_ss(vx);
  __m128 x = _mm_set_ss(h->lx);
  __m128 ypos = _mm_set_ss(h->ly);
  __m128 zpos = _mm_set_ss(h->lz);
  __m128 y = _mm_set_ss(h->vy);
  __m128 z = _mm_set_ss(h->vz);
  for (int i = 0; i < nsteps; ++i) {
    y = _mm_fmadd_ss(vgy, vdt, y);
    z = _mm_fmadd_ss(vgz, vdt, z);
    x = _mm_add_ss(x, _mm_mul_ss(_mm_mul_ss(vvx, vdt), vsm));
    ypos = _mm_add_ss(ypos, _mm_mul_ss(_mm_mul_ss(y, vdt), vsm));
    zpos = _mm_add_ss(zpos, _mm_mul_ss(_mm_mul_ss(z, vdt), vsm));
  }
  const float lx = _mm_cvtss_f32(x);
  const float ly = _mm_cvtss_f32(ypos);
  const float lz = _mm_cvtss_f32(zpos);
  const float vy = _mm_cvtss_f32(y);
  const float vz = _mm_cvtss_f32(z);
  const int kmin = h->next_step + nsteps;
  if (tr != nullptr) {
    mask0_commit_lane_avx2(*h, *tr, lx, ly, lz, vx, vy, vz, kmin);
  } else {
    h->lx = lx;
    h->ly = ly;
    h->lz = lz;
    h->vx = vx;
    h->vy = vy;
    h->vz = vz;
    h->next_step = kmin;
  }
}

}  // namespace wds::chart_render::shuriken

#endif  // x86_64

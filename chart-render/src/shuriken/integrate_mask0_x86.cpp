#include "integrate_mask0_x86.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

namespace wds::chart_render::shuriken {
namespace {

inline void mask0_commit_lane(IntegratorHot& h, IntegrationVelocityTrace& tr, float lx, float ly,
                              float lz, float vx, float vy, float vz, int kmin) noexcept {
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

void mask0_batch_loop_x4_sse(IntegratorHot* h0, IntegratorHot* h1, IntegratorHot* h2,
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
    vy = _mm_add_ps(vy, _mm_mul_ps(vgy, vdt));
    vz = _mm_add_ps(vz, _mm_mul_ps(vgz, vdt));
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
  mask0_commit_lane(*h0, *t0, lxa[0], lya[0], lza[0], vxa[0], vya[0], vza[0], kmin);
  mask0_commit_lane(*h1, *t1, lxa[1], lya[1], lza[1], vxa[1], vya[1], vza[1], kmin);
  mask0_commit_lane(*h2, *t2, lxa[2], lya[2], lza[2], vxa[2], vya[2], vza[2], kmin);
  mask0_commit_lane(*h3, *t3, lxa[3], lya[3], lza[3], vxa[3], vya[3], vza[3], kmin);
}

}  // namespace wds::chart_render::shuriken

#endif  // x86_64

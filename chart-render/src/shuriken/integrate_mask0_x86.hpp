#pragma once

// x86 mask-0 whole-loop kernels. hop / perf, not 2022.3.
//
// ISA compile boundary, not a layout/i-cache tweak: Clang will not inline a
// target("avx2,fma") callee into a default-ISA caller, so the old per-step
// noinline helper paid a call + six __m128 refs every grid step. The loop
// (load / n steps / store) lives in one function compiled for that ISA;
// advance_mask0_batch / advance_to dispatch once outside the loop.
//
// This header is POD + declarations only (see integrate_hot.hpp). The Avx2
// TU must not pull internal.hpp: MSVC compiles it with /arch:AVX2, and any
// odr-used inline / template COMDAT from that ISA could be picked for the
// whole program (SIGILL on non-AVX2 CPUs).

#include "integrate_hot.hpp"

#if defined(__x86_64__) || defined(_M_X64)

namespace wds::chart_render::shuriken {

// Default x86 ISA (mul+add, byte-identical to x86 scalar). No FMA.
void mask0_batch_loop_x4_sse(IntegratorHot* h0, IntegratorHot* h1, IntegratorHot* h2,
                             IntegratorHot* h3, IntegrationVelocityTrace* t0,
                             IntegrationVelocityTrace* t1, IntegrationVelocityTrace* t2,
                             IntegrationVelocityTrace* t3, int kmin, float dt, float gy, float gz,
                             float sm) noexcept;

// Avx2: gravity vfmadd, Δp still mul×mul+add. Whole function is
// target("avx2,fma") (GNU/Clang) or a /arch:AVX2 TU (MSVC).
void mask0_batch_loop_x4_avx2(IntegratorHot* h0, IntegratorHot* h1, IntegratorHot* h2,
                              IntegratorHot* h3, IntegrationVelocityTrace* t0,
                              IntegrationVelocityTrace* t1, IntegrationVelocityTrace* t2,
                              IntegrationVelocityTrace* t3, int kmin, float dt, float gy, float gz,
                              float sm) noexcept;

// Avx2 scalar remainder / unbatched simple mask-0: same fused gravity as the
// 4-wide kernel, one dispatch per particle for n steps. No per-step call.
void mask0_scalar_loop_avx2(IntegratorHot* h, IntegrationVelocityTrace* tr, int nsteps, float dt,
                            float gy, float gz, float sm) noexcept;

}  // namespace wds::chart_render::shuriken

#endif  // x86_64

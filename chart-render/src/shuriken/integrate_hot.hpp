#pragma once

// POD pose + velocity-trace used by x86 mask-0 whole-loop TUs.
// Kept free of inline functions / templates so integrate_mask0_avx2.cpp
// (MSVC /arch:AVX2) cannot emit AVX2-encoded COMDAT that the linker might
// pick for the rest of the program. hop / perf, not 2022.3.

#include <cstdint>

namespace wds::chart_render::shuriken {

// Observation-only snapshot of the velocity terms already computed by
// integrate_pose. All vectors are in the native PS-local/LaneGroup basis.
// stored is Particle.velocity after Force/Gravity/Limit/Drag; vol and noise
// are the current-frame animated terms; inherited is Current-mode inherit
// (Initial is already in stored); total is the sum consumed by the position
// step. Orbital/radial post-step transforms are deliberately outside this
// snapshot.
struct IntegrationVelocityTrace {
  float stored_x = 0.0f;
  float stored_y = 0.0f;
  float stored_z = 0.0f;
  float vol_x = 0.0f;
  float vol_y = 0.0f;
  float vol_z = 0.0f;
  float noise_x = 0.0f;
  float noise_y = 0.0f;
  float noise_z = 0.0f;
  // Curl × positionAmount, before Strength. Hold keeps this; Strength is a
  // lifetime curve and is re-applied each step (2022.3 Noise Manual).
  float noise_unit_x = 0.0f;
  float noise_unit_y = 0.0f;
  float noise_unit_z = 0.0f;
  float inherited_x = 0.0f;
  float inherited_y = 0.0f;
  float inherited_z = 0.0f;
  float total_x = 0.0f;
  float total_y = 0.0f;
  float total_z = 0.0f;
};

// IntegratorHot is the only R+W state of run_step / advance_to. It lives in
// a contiguous array (persist SoA or a 128 B-aligned frame-arena batch).
// 32 B divides 64 so a record never straddles a 64 B line. trace is cold.
// alignas(32) is SIMD/hot-pose packing, not a false-sharing pad.
struct alignas(32) IntegratorHot {
  float lx = 0.0f;
  float ly = 0.0f;
  float lz = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  int next_step = 0;
  std::uint32_t pad = 0;
};
static_assert(sizeof(IntegratorHot) == 32, "hot pose is 32 B");
static_assert(64 % sizeof(IntegratorHot) == 0, "hot size divides 64");

}  // namespace wds::chart_render::shuriken

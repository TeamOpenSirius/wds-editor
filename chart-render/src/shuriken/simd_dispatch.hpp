#pragma once

namespace wds::chart_render {

// Portable SIMD for the analytic-noise hop. Not 2022.3.
// WDS_SPLIT_SIMD=auto|scalar|sse42|avx2|neon
//   auto: NEON on armv8.0-a; else AVX2+FMA3 if CPUID, else SSE4.2, else scalar.
// Forced names that the binary cannot run fall back (ARM has no AVX2; x86
// has no NEON). Adopted AN uses explicit IEEE fma (scalar std::fma /
// vfmaq_f32 / _mm_fmadd_ps). AVX2 requires FMA3. SSE4.2-only falls back
// to scalar fma (correct, outside the support matrix). hop, not 2022.3.
enum class SplitSimdPath : int {
  Scalar = 0,
  Neon = 1,
  Sse42 = 2,
  Avx2 = 3,
};

SplitSimdPath split_simd_path() noexcept;
const char* split_simd_name() noexcept;
bool split_simd_has(SplitSimdPath path) noexcept;
bool split_simd_has_fma() noexcept;

// nullptr / "" restores env. Used by benches and noise tests.
void split_simd_force_for_tests(const char* name) noexcept;

}  // namespace wds::chart_render

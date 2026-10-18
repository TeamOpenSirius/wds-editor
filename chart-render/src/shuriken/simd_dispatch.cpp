#include "simd_dispatch.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace wds::chart_render {
namespace {

enum class Forced : int {
  Unset = -1,
  Auto = 0,
  Scalar = 1,
  Neon = 2,
  Sse42 = 3,
  Avx2 = 4,
};

std::atomic<Forced> g_force{Forced::Unset};

// Resolved SplitSimdPath cached as int; -1 = unresolved. Avoids repeated CPUID
// (which traps under hypervisors) in per-particle hot loops.
constexpr int kPathUnresolved = -1;
std::atomic<int> g_cached_path{kPathUnresolved};

bool env_eq(const char* raw, const char* want) noexcept {
  return raw != nullptr && std::strcmp(raw, want) == 0;
}

Forced parse_name(const char* raw) noexcept {
  if (raw == nullptr || raw[0] == '\0' || env_eq(raw, "auto")) {
    return Forced::Auto;
  }
  if (env_eq(raw, "scalar")) {
    return Forced::Scalar;
  }
  if (env_eq(raw, "neon")) {
    return Forced::Neon;
  }
  if (env_eq(raw, "sse42") || env_eq(raw, "sse4.2") || env_eq(raw, "sse")) {
    return Forced::Sse42;
  }
  if (env_eq(raw, "avx2")) {
    return Forced::Avx2;
  }
  return Forced::Auto;
}

Forced env_force() noexcept {
  static const Forced v = parse_name(std::getenv("WDS_SPLIT_SIMD"));
  return v;
}

#if defined(__x86_64__) || defined(_M_X64)
void x86_cpuid(unsigned leaf, unsigned sub, unsigned* eax, unsigned* ebx, unsigned* ecx,
               unsigned* edx) noexcept {
#if defined(_MSC_VER)
  int regs[4] = {0, 0, 0, 0};
  __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(sub));
  *eax = static_cast<unsigned>(regs[0]);
  *ebx = static_cast<unsigned>(regs[1]);
  *ecx = static_cast<unsigned>(regs[2]);
  *edx = static_cast<unsigned>(regs[3]);
#else
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (__get_cpuid_count(leaf, sub, &a, &b, &c, &d) == 0) {
    *eax = *ebx = *ecx = *edx = 0;
    return;
  }
  *eax = a;
  *ebx = b;
  *ecx = c;
  *edx = d;
#endif
}

bool x86_has_sse42() noexcept {
  unsigned a = 0, b = 0, c = 0, d = 0;
  x86_cpuid(1, 0, &a, &b, &c, &d);
  return (c & (1u << 20)) != 0;
}

bool x86_has_fma3() noexcept {
  unsigned a = 0, b = 0, c = 0, d = 0;
  x86_cpuid(1, 0, &a, &b, &c, &d);
  return (c & (1u << 12)) != 0;
}

bool x86_has_avx2() noexcept {
  unsigned a = 0, b = 0, c = 0, d = 0;
  x86_cpuid(1, 0, &a, &b, &c, &d);
  const bool osxsave = (c & (1u << 27)) != 0;
  const bool avx = (c & (1u << 28)) != 0;
  if (!osxsave || !avx) {
    return false;
  }
  x86_cpuid(7, 0, &a, &b, &c, &d);
  return (b & (1u << 5)) != 0 && x86_has_fma3();
}
#endif

bool compile_has_neon() noexcept {
#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
  return true;
#else
  return false;
#endif
}

SplitSimdPath pick_auto() noexcept {
  if (compile_has_neon()) {
    return SplitSimdPath::Neon;
  }
#if defined(__x86_64__) || defined(_M_X64)
  if (x86_has_avx2()) {
    return SplitSimdPath::Avx2;
  }
  if (x86_has_sse42()) {
    return SplitSimdPath::Sse42;
  }
#endif
  return SplitSimdPath::Scalar;
}

SplitSimdPath apply_force(Forced f) noexcept {
  if (f == Forced::Scalar) {
    return SplitSimdPath::Scalar;
  }
  if (f == Forced::Neon) {
    return compile_has_neon() ? SplitSimdPath::Neon : pick_auto();
  }
  if (f == Forced::Sse42) {
#if defined(__x86_64__) || defined(_M_X64)
    return x86_has_sse42() ? SplitSimdPath::Sse42 : SplitSimdPath::Scalar;
#else
    return pick_auto();
#endif
  }
  if (f == Forced::Avx2) {
#if defined(__x86_64__) || defined(_M_X64)
    if (x86_has_avx2()) {
      return SplitSimdPath::Avx2;
    }
    return x86_has_sse42() ? SplitSimdPath::Sse42 : SplitSimdPath::Scalar;
#else
    return pick_auto();
#endif
  }
  return pick_auto();
}

}  // namespace

SplitSimdPath split_simd_path() noexcept {
  const int cached = g_cached_path.load(std::memory_order_acquire);
  if (cached != kPathUnresolved) {
    return static_cast<SplitSimdPath>(cached);
  }
  const Forced g = g_force.load(std::memory_order_acquire);
  const SplitSimdPath p = apply_force(g != Forced::Unset ? g : env_force());
  g_cached_path.store(static_cast<int>(p), std::memory_order_release);
  return p;
}

const char* split_simd_name() noexcept {
  switch (split_simd_path()) {
    case SplitSimdPath::Neon:
      return "neon";
    case SplitSimdPath::Sse42:
      return "sse42";
    case SplitSimdPath::Avx2:
      return "avx2";
    case SplitSimdPath::Scalar:
    default:
      return "scalar";
  }
}

bool split_simd_has(SplitSimdPath path) noexcept {
  switch (path) {
    case SplitSimdPath::Scalar:
      return true;
    case SplitSimdPath::Neon:
      return compile_has_neon();
    case SplitSimdPath::Sse42:
#if defined(__x86_64__) || defined(_M_X64)
      return x86_has_sse42();
#else
      return false;
#endif
    case SplitSimdPath::Avx2:
#if defined(__x86_64__) || defined(_M_X64)
      return x86_has_avx2();
#else
      return false;
#endif
  }
  return false;
}

bool split_simd_has_fma() noexcept {
#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
  return true;
#elif defined(__x86_64__) || defined(_M_X64)
  return x86_has_fma3();
#else
  return false;
#endif
}

void split_simd_force_for_tests(const char* name) noexcept {
  if (name == nullptr || name[0] == '\0') {
    g_force.store(Forced::Unset, std::memory_order_release);
  } else {
    g_force.store(parse_name(name), std::memory_order_release);
  }
  g_cached_path.store(kPathUnresolved, std::memory_order_release);
}

}  // namespace wds::chart_render

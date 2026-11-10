#pragma once

#include <cstddef>
#include <new>

namespace wds::common {

// Conservative false-sharing interval. Not a per-machine tune:
// Apple Silicon L1D is 128 B; x86 L1 is 64 B but adjacent-line prefetchers
// pull 128 B pairs (Intel 64/IA-32 optimization manual). C++17
// hardware_destructive_interference_size is 64 on MinGW libstdc++ and
// many libc++ builds — too small on Apple L1. Apple Clang 21 libc++ on
// this Mac reports 256; floor at 128 either way.
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kHardwareDestructiveInterference =
    std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kHardwareDestructiveInterference = 64;
#endif

inline constexpr std::size_t kFalseSharingAlign =
    kHardwareDestructiveInterference < 128 ? 128 : kHardwareDestructiveInterference;

static_assert(kFalseSharingAlign >= 128, "false-sharing pad spans two 64 B lines");

}  // namespace wds::common

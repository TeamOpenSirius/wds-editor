#include "wds/common/minflt_stage.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace wds::common {
namespace {

bool stages_on() noexcept {
  static const bool on = []() {
    const char* v = std::getenv("WDS_MINFLT_STAGES");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
  }();
  return on;
}

#ifdef _WIN32
long process_faults() noexcept {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return 0;
  }
  return static_cast<long>(pmc.PageFaultCount);
}

long process_rss_kb() noexcept {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return 0;
  }
  return static_cast<long>(pmc.WorkingSetSize / 1024);
}
#else
long process_faults() noexcept {
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return 0;
  }
  return ru.ru_minflt;
}

long process_rss_kb() noexcept {
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<long>(ru.ru_maxrss / 1024);
#else
  return ru.ru_maxrss;
#endif
}
#endif

}  // namespace

void minflt_stage(const char* name) noexcept {
  if (!stages_on() || name == nullptr) {
    return;
  }
  static long prev = 0;
  const long tot = process_faults();
  const long d = tot - prev;
  prev = tot;
  std::fprintf(stderr, "MINFLT %s d=%ld tot=%ld rss_kb=%ld\n", name, d, tot, process_rss_kb());
}

}  // namespace wds::common

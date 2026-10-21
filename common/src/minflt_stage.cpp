#include <wds/common/minflt_stage.hpp>

#if WDS_DIAG_ENABLED

#include "wds/common/minflt_stage.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#endif

#if defined(__linux__)
#include <unistd.h>
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

// Cumulative minor + major faults. Windows cannot split them cheaply, so it
// reports the total in minflt and leaves majflt at 0.
void process_faults(long& minflt, long& majflt) noexcept {
  minflt = 0;
  majflt = 0;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    minflt = static_cast<long>(pmc.PageFaultCount);
  }
#else
  rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    minflt = ru.ru_minflt;
    majflt = ru.ru_majflt;
  }
#endif
}

#if defined(_WIN32)
long process_faults() noexcept {
  long minflt = 0;
  long majflt = 0;
  process_faults(minflt, majflt);
  return minflt;
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

#if defined(__APPLE__)
long bytes_to_kb(mach_vm_size_t bytes) noexcept {
  return static_cast<long>(bytes / 1024u);
}

void sample_macos_task_info(ProcessMemSnap& out) noexcept {
  const mach_port_t task = mach_task_self();

  mach_task_basic_info_data_t basic{};
  mach_msg_type_number_t basic_n = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(task, MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic), &basic_n) ==
      KERN_SUCCESS) {
    out.rss_kb = bytes_to_kb(basic.resident_size);
    out.rss_peak_kb = bytes_to_kb(basic.resident_size_max);
  }

  task_vm_info_data_t vm{};
  mach_msg_type_number_t vm_n = TASK_VM_INFO_COUNT;
  const kern_return_t kr =
      task_info(task, TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &vm_n);
  // A kernel built without the rev we ask for returns fewer fields; only read
  // the ones it actually filled (REV1 is the first with phys_footprint).
  if (kr == KERN_SUCCESS && vm_n >= TASK_VM_INFO_REV1_COUNT) {
    out.footprint_kb = bytes_to_kb(vm.phys_footprint);
    out.compressed_kb = bytes_to_kb(vm.compressed);
    if (out.rss_kb == 0) {
      // MACH_TASK_BASIC_INFO failed but the VM info worked: use what we have.
      out.rss_kb = bytes_to_kb(vm.resident_size);
      out.rss_peak_kb = bytes_to_kb(vm.resident_size_peak);
    }
  }
}
#elif defined(__linux__)
void sample_linux_status(ProcessMemSnap& out) noexcept {
  // /proc/self/statm: size resident shared text lib data dt (pages).
  const long page_kb = ::sysconf(_SC_PAGESIZE) / 1024;
  if (page_kb > 0) {
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
      unsigned long size_pages = 0;
      unsigned long resident_pages = 0;
      if (std::fscanf(f, "%lu %lu", &size_pages, &resident_pages) == 2) {
        out.rss_kb = static_cast<long>(resident_pages) * page_kb;
      }
      std::fclose(f);
    }
  }
  if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      unsigned long kb = 0;
      if (std::sscanf(line, "VmHWM: %lu kB", &kb) == 1) {
        out.rss_peak_kb = static_cast<long>(kb);
      } else if (std::sscanf(line, "VmRSS: %lu kB", &kb) == 1) {
        if (out.rss_kb == 0) {
          out.rss_kb = static_cast<long>(kb);
        }
      }
    }
    std::fclose(f);
  }
}
#elif defined(_WIN32)
void sample_windows_mem(ProcessMemSnap& out) noexcept {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return;
  }
  out.rss_kb = static_cast<long>(pmc.WorkingSetSize / 1024);
  out.rss_peak_kb = static_cast<long>(pmc.PeakWorkingSetSize / 1024);
  // Commit charge: charged on commit, released on decommit. Unlike the working
  // set it includes pages that were never touched, which is exactly what a
  // std::vector::reserve() consumes on Windows.
  out.commit_kb = static_cast<long>(pmc.PagefileUsage / 1024);
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

void sample_process_mem(ProcessMemSnap& out) noexcept {
  out = ProcessMemSnap{};
  process_faults(out.minflt, out.majflt);
#if defined(__APPLE__)
  sample_macos_task_info(out);
  if (out.rss_kb == 0) {
    // Last resort: ru_maxrss is the peak, not the current RSS, but it keeps
    // the line monotone instead of reporting "0 KB" if task_info is blocked.
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
      out.rss_peak_kb = static_cast<long>(ru.ru_maxrss / 1024);
    }
  }
#elif defined(__linux__)
  sample_linux_status(out);
#elif defined(_WIN32)
  sample_windows_mem(out);
#endif
}

}  // namespace wds::common

#endif  // WDS_DIAG_ENABLED

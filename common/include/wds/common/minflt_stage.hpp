#pragma once

#ifndef WDS_DIAG_ENABLED
#define WDS_DIAG_ENABLED 0
#endif

namespace wds::common {

// WDS_MINFLT_STAGES=1 → stderr "MINFLT name d=… tot=… rss=…".
// Attribution only. No effect on particles or pixels.
#if WDS_DIAG_ENABLED
void minflt_stage(const char* name) noexcept;
#else
// Compile-time removal: hot paths call this unconditionally, so it must leave
// no call and no string in a diagnostics-off build.
inline constexpr void minflt_stage(const char*) noexcept {}
#endif

// One-shot process memory / fault counters, for the per-second diagnostics
// summary (ui/apps/wds_editor_qt.cpp FrameDiagLogger → MEMMEM line).
//
// This is a *sampler*: no env gate of its own, no allocation, no threads. The
// caller decides when it runs and must not call it per frame (it costs a few
// syscalls, including mach task_info on macOS). Deliberately independent of
// WDS_MINFLT_STAGES: that env only switches the ad-hoc stderr line above.
//
// Every field is "best effort": a platform (or a blocked syscall) that cannot
// answer leaves the field at 0 rather than failing the sample.
struct ProcessMemSnap {
  // Current resident set size, KiB.
  long rss_kb = 0;
  // Peak resident set size since process start, KiB.
  long rss_peak_kb = 0;
  // macOS only: task_vm_info.phys_footprint (dirty + compressed + GPU-wired
  // attribution, what the OS charges the process), KiB. 0 elsewhere.
  long footprint_kb = 0;
  // macOS only: task_vm_info.compressed — pages the compressor holds for this
  // task, i.e. what this process has swapped/compressed out, KiB. This is the
  // per-process "swapped bytes" figure; the vm.swapusage sysctl would report
  // the whole machine. 0 elsewhere.
  long compressed_kb = 0;
  // Windows only: commit charge (PROCESS_MEMORY_COUNTERS::PagefileUsage), KiB.
  // This is charged when a range is committed even if no page is ever touched,
  // so it moves when reserve() shrinks while rss_kb stays flat. 0 elsewhere.
  long commit_kb = 0;
  // Cumulative page faults since process start (not deltas).
  long minflt = 0;
  long majflt = 0;
};

// Never throws, never allocates, safe on any thread. macOS: TASK_VM_INFO +
// MACH_TASK_BASIC_INFO + getrusage. Linux: /proc/self/statm + /proc/self/status
// (VmHWM) + getrusage. Windows: GetProcessMemoryInfo (no major-fault split, so
// minflt carries the total page fault count and majflt stays 0).
#if WDS_DIAG_ENABLED
void sample_process_mem(ProcessMemSnap& out) noexcept;
#else
inline constexpr void sample_process_mem(ProcessMemSnap&) noexcept {}
#endif

}  // namespace wds::common

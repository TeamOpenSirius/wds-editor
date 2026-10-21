#pragma once

// WDS_DIAG_ENABLED is set by CMake (WDS_DIAGNOSTICS option). Everything in
// this header and the macros below must be usable with it at 0: the macros
// expand to nothing, so a Release build carries no stage lookups, no clock
// reads and no calls.
#ifndef WDS_DIAG_ENABLED
#define WDS_DIAG_ENABLED 0
#endif

#include <chrono>
#include <cstdint>
#include <string>

namespace wds::common::perf {

#if WDS_DIAG_ENABLED
bool enabled() noexcept;
#else
inline constexpr bool enabled() noexcept { return false; }
#endif
void set_enabled(bool on) noexcept;

int stage_id(const char* name);

void add_ns(int id, int64_t ns) noexcept;
void add_count(int id, int64_t n) noexcept;

class Scope {
 public:
  explicit Scope(int id) noexcept;
  ~Scope();

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  int id_ = -1;
  bool active_ = false;
  std::chrono::steady_clock::time_point start_{};
};

void end_frame() noexcept;
std::string window_report();
int window_frames() noexcept;

// Diagnostics only: OS thread ids of threads seen running a "split.collect.*"
// perf scope, i.e. the chart-render split collect pool workers. The current
// thread is recorded on its first such scope and never again (one store per
// thread for the process lifetime), so this is a tiny atomic read per collect
// call and nothing at all while scopes are off.
//
// Note: the GUI thread also participates in collects, so it can appear here
// too; the caller must classify its own tid first (the Qt shell does: main
// wins over pool). Returns 0 when perf is disabled, so the whole feature costs
// nothing unless WDS_FRAME_DIAG turns scopes on.
// Used by the Qt shell's FrameDiagLogger to split window CPU per thread class.
int pool_thread_ids(uint64_t* out, int cap) noexcept;

}  // namespace wds::common::perf

#if WDS_DIAG_ENABLED

#define WDS_PERF_CAT(a, b) WDS_PERF_CAT_IMPL(a, b)
#define WDS_PERF_CAT_IMPL(a, b) a##b

#define WDS_PERF_SCOPE(literal)                                                             \
  static const int WDS_PERF_CAT(wds_perf_id_, __LINE__) =                                   \
      ::wds::common::perf::stage_id(literal);                                                 \
  const bool WDS_PERF_CAT(wds_perf_on_, __LINE__) = ::wds::common::perf::enabled();          \
  ::wds::common::perf::Scope WDS_PERF_CAT(wds_perf_scope_, __LINE__)(                        \
      WDS_PERF_CAT(wds_perf_on_, __LINE__) ? WDS_PERF_CAT(wds_perf_id_, __LINE__) : -1)

#define WDS_PERF_COUNT(literal, n)                                                          \
  do {                                                                                      \
    static const int WDS_PERF_CAT(wds_perf_count_id_, __LINE__) =                             \
        ::wds::common::perf::stage_id(literal);                                               \
    ::wds::common::perf::add_count(WDS_PERF_CAT(wds_perf_count_id_, __LINE__), (n));          \
  } while (0)

#else  // !WDS_DIAG_ENABLED

// Compile-time removal: no id lookup, no clock, no call, and the argument is
// not evaluated.
#define WDS_PERF_SCOPE(literal) ((void)0)
#define WDS_PERF_COUNT(literal, n) ((void)0)

#endif  // WDS_DIAG_ENABLED

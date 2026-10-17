#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace wds::common::perf {

bool enabled() noexcept;
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

}  // namespace wds::common::perf

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

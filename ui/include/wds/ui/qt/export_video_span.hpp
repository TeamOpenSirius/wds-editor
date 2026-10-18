#pragma once

#include <algorithm>
#include <cstdint>

namespace wds::ui {

inline constexpr int64_t kExportVideoPadMs = 2000;

struct ExportVideoSpan {
  int64_t start_ms = -kExportVideoPadMs;
  int64_t end_ms = kExportVideoPadMs;

  int64_t duration_ms() const noexcept { return end_ms - start_ms; }
};

// Music t=0 vs earliest chart time, then pad 2s before. Latest of music end and
// latest note/split, then pad 2s after.
inline ExportVideoSpan compute_export_video_span(bool has_music, int64_t music_end_ms,
                                                int64_t chart_start_ms, int64_t chart_end_ms) {
  int64_t origin = chart_start_ms;
  int64_t last = chart_end_ms;
  if (has_music) {
    origin = std::min(origin, int64_t{0});
    last = std::max(last, music_end_ms);
  }
  ExportVideoSpan span;
  span.start_ms = origin - kExportVideoPadMs;
  span.end_ms = last + kExportVideoPadMs;
  if (span.end_ms <= span.start_ms) {
    span.end_ms = span.start_ms + 1000;
  }
  return span;
}

}  // namespace wds::ui

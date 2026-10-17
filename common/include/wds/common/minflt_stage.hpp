#pragma once

namespace wds::common {

// WDS_MINFLT_STAGES=1 → stderr "MINFLT name d=… tot=… rss=…".
// Attribution only. No effect on particles or pixels.
void minflt_stage(const char* name) noexcept;

}  // namespace wds::common

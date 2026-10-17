#pragma once

#include <cstdlib>

namespace wds::ui {

// Exact contract: only the C string "1" enables. Case / other values are off.
// Independent of compile-time WDS_ENABLE_LOGGING.
inline bool frame_diag_env_enabled(const char* value) noexcept {
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

struct FrameDiagContract {
  bool sample_timing = false;
  bool open_log_file = false;
  bool emit_output = false;
};

inline FrameDiagContract frame_diag_enable_contract(const char* env_value) noexcept {
  const bool on = frame_diag_env_enabled(env_value);
  return FrameDiagContract{on, on, on};
}

inline bool frame_diag_enabled_from_env() noexcept {
  return frame_diag_env_enabled(std::getenv("WDS_FRAME_DIAG"));
}

// Per-frame rows after seek/play. 0 / unset = off. Exact positive integer.
inline int frame_diag_frames_from_env() noexcept {
  const char* value = std::getenv("WDS_FRAME_DIAG_FRAMES");
  if (value == nullptr || value[0] == '\0') {
    return 0;
  }
  const int n = std::atoi(value);
  return n > 0 ? n : 0;
}

// Exact "0" disables WDS_PERF_SCOPE/COUNT. Unset / other values keep scopes on
// when WDS_FRAME_DIAG=1. Summary frame timing still follows FRAME_DIAG.
inline bool frame_diag_perf_env_enabled(const char* value) noexcept {
  return value == nullptr || !(value[0] == '0' && value[1] == '\0');
}

inline bool frame_diag_perf_enabled_from_env() noexcept {
  return frame_diag_enabled_from_env() &&
         frame_diag_perf_env_enabled(std::getenv("WDS_FRAME_DIAG_PERF"));
}

// A/B switches for preview CPU paths (exact "1" enables). Logged at startup.
inline bool preview_legacy_lanes_from_env() noexcept {
  return frame_diag_env_enabled(std::getenv("WDS_PREVIEW_LEGACY_LANES"));
}

inline bool preview_legacy_emit_from_env() noexcept {
  return frame_diag_env_enabled(std::getenv("WDS_PREVIEW_LEGACY_EMIT"));
}

inline bool preview_parallel_emit_from_env() noexcept {
  return frame_diag_env_enabled(std::getenv("WDS_PREVIEW_PARALLEL_EMIT"));
}

// Inject one render-thread sleep (milliseconds). 0 / unset = off.
inline int preview_stall_ms_from_env() noexcept {
  const char* value = std::getenv("WDS_PREVIEW_STALL_MS");
  if (value == nullptr || value[0] == '\0') {
    return 0;
  }
  const int ms = std::atoi(value);
  return ms > 0 ? ms : 0;
}

// Optional chart-time (seconds) at which WDS_PREVIEW_STALL_MS fires once.
// Unset / empty / negative = first playing render (play-start).
inline double preview_stall_at_sec_from_env() noexcept {
  const char* value = std::getenv("WDS_PREVIEW_STALL_AT_SEC");
  if (value == nullptr || value[0] == '\0') {
    return -1.0;
  }
  return std::atof(value);
}

// Exact "1" skips idle/background split UV/trail preload (baseline dumps).
inline bool preview_no_preload_from_env() noexcept {
  return frame_diag_env_enabled(std::getenv("WDS_PREVIEW_NO_PRELOAD"));
}

// Exact "0" keeps the old triangle-list trail/ribbon emit. Default on.
inline bool split_trail_indexed_from_env() noexcept {
  const char* value = std::getenv("WDS_SPLIT_TRAIL_INDEXED");
  if (value == nullptr || value[0] == '\0') {
    return true;
  }
  return !(value[0] == '0' && value[1] == '\0');
}

}  // namespace wds::ui

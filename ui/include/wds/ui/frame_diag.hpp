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

}  // namespace wds::ui

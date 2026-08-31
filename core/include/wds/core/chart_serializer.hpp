#pragma once

#include <wds/core/notation.hpp>

#include <string>

namespace wds::chart_editor {

enum class SerializeError {
  Ok = 0,
  IoError,
  ParseError,
  VersionMismatch,
  ReadOnly,
};

struct SerializeResult {
  SerializeError error = SerializeError::Ok;
  std::string message;
};

// Disk I/O boundary: ONLY these functions perform file read/write.
// All ChartDocument / ChartEditorEngine editing is in-memory until save is called.
//
// Native .wdschart v2 note records mirror official CSV columns (tick-based times):
//   N id start_tick end_tick type lane width gimmick_type scratch_length
// MusicTiming::offset_ms is NOT serialized here (see .wdsproject CHART_DELAY_MS).
class ChartSerializer {
 public:
  static constexpr int32_t kFormatVersion = 4;
  static constexpr int32_t kMinSupportedVersion = 1;

  // Native .wdschart format (always writes latest).
  // v3 adds TIMING points (BPM + meter). v4 adds has_bpm/has_meter flags on T rows.
  // v1/v2 load creates a default tick-0 point.
  // HoldEighth is never written; legacy HoldEighth rows are skipped on load.
  static SerializeResult save_to_file(const NotationChart& chart, const std::string& path);
  static SerializeResult load_from_file(const std::string& path, NotationChart& out_chart);

  // Auto-detect: .csv / official 7-column text → OfficialChartFormat; else .wdschart.
  // out_edit_mode is set when non-null.
  static SerializeResult load_auto(const std::string& path, NotationChart& out_chart,
                                   const std::string& music_config_path = {},
                                   ChartEditMode* out_edit_mode = nullptr);
};

}  // namespace wds::chart_editor

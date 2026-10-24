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
// Native .wdschart v2 note records mirror official CSV columns (tick-based times).
// v5 appends parent_hold_id on N rows; v6 adds SPEEDS / PROFILE records:
//   N id start_tick end_tick type lane width gimmick_type scratch_length parent_hold_id
// Concurrent (sync) lines are never persisted: load derives them from the note set.
// Legacy CONCURRENT / C sections are parsed and ignored.
// MusicTiming::offset_ms is NOT serialized here (see .wdsproject CHART_DELAY_MS).
class ChartSerializer {
 public:
  static constexpr int32_t kFormatVersion = 6;
  static constexpr int32_t kMinSupportedVersion = 1;

  // Native .wdschart format (always writes latest).
  // v3 adds TIMING points (BPM + meter). v4 adds has_bpm/has_meter flags on T rows.
  // v5 adds parent_hold_id on N rows; v6 adds scroll-speed points and compatibility profile.
  // Save never writes concurrent/sync lines; load derives them from the notes.
  // Load treats -1 / missing / non-hold parents as
  // unbound and infers binds (type + open span + exact lanes) on every version.
  // v1/v2 load creates a default tick-0 point.
  // HoldEighth is purely derived: save never writes HoldEighth N rows, X rows or a
  // SUPPRESSED_EIGHTHS section. Load parses and discards any legacy HoldEighth N
  // rows (authored / suppressed markers included) and legacy SUPPRESSED_EIGHTHS /
  // X rows, then recomputes every hold's eighths (tailed and nontail).
  static SerializeResult save_to_file(const NotationChart& chart, const std::string& path);
  static SerializeResult load_from_file(const std::string& path, NotationChart& out_chart);

  // Auto-detect: .csv / official 7-column text → OfficialChartFormat; else .wdschart.
  // out_edit_mode is set when non-null.
  static SerializeResult load_auto(const std::string& path, NotationChart& out_chart,
                                   const std::string& music_config_path = {},
                                   ChartEditMode* out_edit_mode = nullptr);
};

}  // namespace wds::chart_editor

#pragma once

#include <wds/core/chart_serializer.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace wds::chart_editor {

// Sliding Universal Score (SUS) v2.7 — text chart used by Ched / ChedPlus.
// WDS maps a practical subset: metadata, BPM, measure length, taps (#1),
// holds (#2), slides (#3/#4 ≈ hold), directionals (#5 → flick).
//
// 12-lane Ched charts commonly occupy SUS lanes 2..d; import auto-detects a
// lane offset so notes land in editor lanes 0..11. Export writes lanes as
// WDS+2 for Ched compatibility.

struct SusChartMetadata {
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string designer;
  std::string difficulty;
  std::string play_level;
  std::string song_id;
  std::string wave_path;       // relative to the .sus file when possible
  double wave_offset_sec = 0.0;
  std::string jacket_path;
  double base_bpm = 0.0;
  int32_t ticks_per_beat = 480;
};

struct SusChartLoadResult {
  NotationChart chart;
  SusChartMetadata meta;
};

struct SusChartSaveOptions {
  SusChartMetadata meta;
  // When true, export WDS lane L as SUS lane L+2 (Ched 12-key layout).
  bool ched_lane_padding = true;
};

class SusChartFormat {
 public:
  static SerializeResult parse(const std::string& text, SusChartLoadResult& out);
  static SerializeResult load_file(const std::string& path, SusChartLoadResult& out);

  static SerializeResult serialize(const NotationChart& chart, const SusChartSaveOptions& options,
                                   std::string& out_text);
  static SerializeResult save_file(const NotationChart& chart, const std::string& path,
                                   const SusChartSaveOptions& options);

  static bool looks_like_sus_path(const std::string& path);
  static bool looks_like_sus_text(const std::string& text);
};

}  // namespace wds::chart_editor

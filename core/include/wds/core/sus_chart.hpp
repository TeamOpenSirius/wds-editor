#pragma once

#include <wds/core/chart_serializer.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace wds::chart_editor {

// Sliding Universal Score (SUS) — Ched / ChedPlus authoring for World Dai Star (Sirius).
//
// Ched ground truth (see sonolus-sirius-engine chart_edit + sus2txt):
// - All Hold / ScratchHold ribbons are Slide #3 (Sirius does not parse #2).
// - Blue Hold vs purple ScratchHold is distinguished by a paired Flick+Air at the
//   slide end (#1 type 3 + #5). Air alone or Flick alone is illegal and ignored.
// - Critical gold head = Critical tap (#1 type 2) covering the slide start.
// - Damage (#1 type 4) at start → intentional headless; at end → Nontail (import
//   may degrade to a tailed Hold and record a warning).
// - Slide mid (#3 type 3) → Sound / ScratchSound by parent hold family.
// - Mid paired Flick+Air on a slide → JumpScratch split into multiple ScratchHolds.
// - #TIL01 = split-lane gimmick; #TIL00 HiSpeed is ignored (editor cannot author).
// - Legacy #2 Hold channels from older WDS exports import as blue Hold.
// - Ched 12-key pad: export L→L+2; import auto-detects offset 2 for the 2..d window.
// - HoldEighth is never written (would become Sound stars on re-import).

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
  // Human-readable lossy-import notes (Nontail→tailed, ignored HiSpeed, orphans…).
  std::vector<std::string> warnings;
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

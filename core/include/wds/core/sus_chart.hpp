#pragma once

#include <wds/core/chart_serializer.hpp>
#include <wds/core/notation.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace wds::chart_editor {

// Sliding Universal Score (SUS) — Ched / ChedPlus authoring for World Dai Star (Sirius).
//
// Import ground truth is sonolus-sirius-engine sus2txt (#5 Air geometry):
// - All Hold / ScratchHold ribbons are Slide #3 (Sirius does not parse #2).
// - Blue vs purple is decided by #5 at the slide end (or mid #5 cuts). #1 type 3
//   is Ched decoration and is never emitted as a note.
// - Start #5 on the body or an adjacent sus2txt span → addStart=false (no pink
//   head). Start #5 with no end/mid #5 → shouldUnscratch back to blue Hold + head.
// - Critical gold head = Critical tap (#1 type 2) covering the slide start.
// - Damage (#1 type 4) at start → intentional headless (addStart=false); at end
//   → Nontail (holdType+1000). Start Damage survives shouldUnscratch. Unused
//   leftover Damage is not a tap.
// - Slide mid (#3 type 3) → Sound / ScratchSound by parent hold family.
// - Mid #5 on the exact body span → purple split (SoundPurple). JumpScratch
//   only when the Air encodes a nonzero scratch span; sl=0 stays gimmick 0.
// - #TIL01 export must be `#TIL01: "` (colon-space) so sus2txt can parse it.
//   #TIL00 non-negative global HiSpeed is retained as scroll_speed_points.
//   Both use sus2txt's 1920ths-of-the-measure index (not raw ticks).
// - Legacy #2 Hold channels from older WDS exports import as blue Hold.
// - Ched pad is always +2 both ways (WDS L ↔ SUS L+2). Export clips each span
//   to SUS columns 0..f; columns outside that window are dropped. Import always
//   subtracts 2 (no layout heuristic). Overhang that remains auto-upgrades
//   Official → SiriusExtended unless #WDS_PROFILE Official.
// - HoldEighth is generated on export as slide type 5 (invisible mid). On import
//   slide type 5 never becomes Sound or HoldEighth; eighths are always recomputed
//   from the imported holds (tailed and nontail) on the tpq/2 grid.

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
  // Human-readable import notes (skipped incompatible SV points, orphans…).
  std::vector<std::string> warnings;
};

struct SusChartSaveOptions {
  SusChartMetadata meta;
  // Ignored: export always uses the Ched +2 pad and clips to SUS 0..f.
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

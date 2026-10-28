#pragma once

#include <wds/core/chart_serializer.hpp>
#include <wds/core/notation.hpp>

#include <optional>
#include <string>
#include <vector>

namespace wds::chart_editor {

// Official World Dai Star / Sirius decrypted chart CSV (5 or 7 columns, no
// header). The compact form omits gimmickType/scratchLength:
//   startTime, endTime, type, leftLane, laneLength[, gimmickType, scratchLength]
//
// Column meanings (see also sonolus-sirius-engine/levelData.cpp::txt2data):
//   startTime     — note/gimmick start in music seconds (beat≡second at BPM 60)
//   endTime       — hold/split end in seconds; -1 = instantaneous (no duration)
//   type          — AppConst.NoteType (10/20/80/100/900/…); 0 = gimmick-only (split);
//                   900 (HoldEighth) rows are kept on CSV import. Official CSV has no
//                   BPM, so the editor cannot rebuild the eighth grid; file times stay.
//                   Export of an editable chart still generates type 900 (gimmick 0 /
//                   scratch 0). Export rows are sorted by startTime then type;
//                   -1 = HiSpeed (endTime holds non-negative multiplier; stored in
//                   scroll_speed_points, never in NotationNote);
//                   31 = ScratchSound (purple mid-star); 40 = Scratch (kept as-is;
//                   looks like Flick; the editor cannot author it);
//                   split rows always export type 0, leftLane -1, laneLength 0
//   leftLane      — leftmost lane, official 1..12; Extended may overhang;
//                   split rows use -1
//   laneLength    — width in lanes; split rows use 0
//   gimmickType   — numeric GimmickType, or "JumpScratch" / "OneDirection"
//                   Official Flick: None = both arrows; OneDirection = one
//                   half-note arrow group; JumpScratch = one full-note group.
//                   Editor memory still uses None + 0/±width for Flick direction.
//   scratchLength — official GimmickValue: OneDirection 0/1 (left/right);
//                   JumpScratch signed lane span (may differ from Width);
//                   hold-chain JumpScratch requires nonzero scratchLength
//                   (sl=0 is written as gimmick 0);
//                   split Addressable SplitEffects/{id} (fadeIn follows
//                   LineHight rotation). Flick Width is laneLength, not this.
//                   Import maps official OneDirection 0/1 → None + ±width;
//                   old editor None + nonzero is kept as left/right.
//                   Export writes official None,0 for both arrows and
//                   JumpScratch ±laneLength for a one-sided multi-lane Flick
//                   (OneDirection would only fill half the note); a 1-lane
//                   one-sided Flick keeps OneDirection 0/1.
//                   → stored as NotationNote::scratch_length
//
// Times are converted to ticks (BPM 60 + TPQ 480 by default) for editor precision;
// wdschart never stores absolute seconds.
//
// music_config.csv (header + one data row):
//   CueSheetName,CueName,DelaySeconds,CueSheetDirectory
// DelaySeconds (seconds) → MusicTiming::offset_ms (chart delay); same role as
// .wdsproject CHART_DELAY_MS. Not written into .wdschart.
//
// Import: sibling music_config supplies DelaySeconds when present. Negative
// startTime (notes, split effects, HiSpeed) is always folded into offset_ms so
// the earliest start becomes tick 0 — the editor timeline cannot seek < 0.
// Resulting delay is DelaySeconds + earliest_start (may stay negative when the
// config delay cannot cover the early span); it is never forced to 0.
// Export: single-chart CSV may bake offset into seconds (bake_delay_into_times);
// project export keeps chart-local seconds and writes music_config separately.

struct OfficialMusicConfig {
  std::string cue_sheet_name;
  std::string cue_name;
  double delay_seconds = 0.0;
  std::string cue_sheet_directory;
};

struct OfficialChartLoadOptions {
  // Official times are absolute seconds. BPM 60 + TPQ 480 ⇒ 1s = 480 ticks
  // (matches Sonolus txt2data BpmChangeEntity bpm=60 convention).
  double bpm = 60.0;
  int32_t ticks_per_quarter = 480;

  // When set, overrides music_config / default delay.
  std::optional<double> delay_seconds;

  // Official lanes are 1-based (1..12); editor NotationNote uses 0-based (0..11).
  bool convert_lane_to_zero_based = true;

  // Skip HiSpeed (type == -1) rows for legacy callers. Leave false so
  // non-negative SV events are retained in scroll_speed_points.
  bool skip_hispeed = false;
  ChartCompatibilityProfile compatibility_profile = ChartCompatibilityProfile::Official;

  // Shift earliest startTime to 0 and add that (negative) delta into offset_ms.
  // Covers notes, split-lane effects, and HiSpeed rows. Pack import disables
  // per-file fold then applies one shared fold across difficulties.
  bool fold_negative_times_into_delay = true;
};

struct OfficialChartSaveOptions {
  // Editor 0-based → official 1-based when true.
  bool convert_lane_to_one_based = true;
  // Write JumpScratch / OneDirection as names (official) instead of 1 / 2.
  bool use_gimmick_names = true;
  // Unused: Flick export always writes official None,0 / OneDirection 0/1.
  bool expand_legacy_flick_direction = false;
  // When true, CSV seconds = chart-local time + offset_ms/1000 (single-chart
  // export). Project export leaves this false and writes music_config instead.
  bool bake_delay_into_times = false;
};

class OfficialChartFormat {
 public:
  static SerializeResult parse_music_config(const std::string& text, OfficialMusicConfig& out);
  static SerializeResult load_music_config_file(const std::string& path, OfficialMusicConfig& out);

  static SerializeResult parse_chart(const std::string& text, NotationChart& out_chart,
                                     const OfficialChartLoadOptions& options = {});
  static SerializeResult load_chart_file(const std::string& path, NotationChart& out_chart,
                                         const OfficialChartLoadOptions& options = {});

  // Load chart CSV and optionally apply DelaySeconds from music_config.csv.
  static SerializeResult load_chart_with_music_config(
      const std::string& chart_path, const std::string& music_config_path,
      NotationChart& out_chart, OfficialChartLoadOptions options = {});
  // Import UI convenience: retry as Sirius Extended only when strict lane
  // validation rejects an otherwise valid CSV.
  static SerializeResult load_chart_auto_profile(
      const std::string& chart_path, const std::string& music_config_path,
      NotationChart& out_chart, OfficialChartLoadOptions options = {});

  static SerializeResult serialize_chart(const NotationChart& chart, std::string& out_text,
                                         const OfficialChartSaveOptions& options = {});
  static SerializeResult save_chart_file(const NotationChart& chart, const std::string& path,
                                         const OfficialChartSaveOptions& options = {});

  static SerializeResult serialize_music_config(const OfficialMusicConfig& config,
                                                std::string& out_text);
  static SerializeResult save_music_config_file(const OfficialMusicConfig& config,
                                                const std::string& path);

  // Fold earliest start (notes / split effects / HiSpeed) to tick 0; add the
  // negative span into timing.offset_ms. No-op when min start >= 0.
  static void fold_negative_times_into_delay(NotationChart& chart);
  // Shared fold across pack difficulties (one shift, one offset delta).
  static void fold_negative_times_into_delay(std::vector<NotationChart>& charts);

  // True when path looks like official CSV (extension or first data line).
  static bool looks_like_official_chart_path(const std::string& path);
  static bool looks_like_official_chart_text(const std::string& text);
};

}  // namespace wds::chart_editor

#pragma once

#include <wds/core/chart_serializer.hpp>
#include <wds/core/notation.hpp>

#include <optional>
#include <string>

namespace wds::chart_editor {

// Official World Dai Star / Sirius decrypted chart CSV (7 columns, no header):
//   startTime, endTime, type, leftLane, laneLength, gimmickType, scratchLength
//
// Column meanings (see also sonolus-sirius-engine/levelData.cpp::txt2data):
//   startTime     — note/gimmick start in music seconds (beat≡second at BPM 60)
//   endTime       — hold/split end in seconds; -1 = instantaneous (no duration)
//   type          — AppConst.NoteType (10/20/80/100/900/…); 0 = gimmick-only (split);
//                   900 (HoldEighth) is ignored on import and generated on export;
//                   -1 = HiSpeed (endTime holds speed value; not stored in NotationNote);
//                   31 = ScratchSound (purple mid-star); 40 = SoundPurple (mid scratch,
//                   editor imports as JumpScratch split / orphan Flick — never stored as 40)
//   leftLane      — leftmost lane, official 1..12; split rows use -1
//   laneLength    — width in lanes; split rows use 0
//   gimmickType   — numeric GimmickType, or "JumpScratch" / "OneDirection"
//   scratchLength — flick/scratch signed span (0 / ±laneLength); JumpScratch
//                   target span; split Addressable SplitEffects/{id} (also
//                   fadeIn growth: LineHight z=180 = tip-anchored)
//                   → stored as NotationNote::scratch_length
//   Official CSV and old editor CSV share this 7-column text. Import never
//   rewrites ±1 (cannot tell them apart). Export of editor-authored Flick
//   may expand historic ±1 to ±width (see OfficialChartSaveOptions).
//
// Times are converted to ticks (BPM 60 + TPQ 480 by default) for editor precision;
// wdschart never stores absolute seconds.
//
// music_config.csv (header + one data row):
//   CueSheetName,CueName,DelaySeconds,CueSheetDirectory
// DelaySeconds (seconds) → MusicTiming::offset_ms (chart delay); same role as
// .wdsproject CHART_DELAY_MS. Not written into .wdschart.

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

  // Skip HiSpeed (type == -1) rows — no timescale channel in preview yet.
  bool skip_hispeed = true;
};

struct OfficialChartSaveOptions {
  // Editor 0-based → official 1-based when true.
  bool convert_lane_to_one_based = true;
  // Write JumpScratch / OneDirection as names (official) instead of 1 / 2.
  bool use_gimmick_names = true;
  // Editor historically stored Flick direction as ±1. Expand those to ±width
  // on export only. Import never does this — the file value is authoritative.
  bool expand_legacy_flick_direction = true;
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

  static SerializeResult serialize_chart(const NotationChart& chart, std::string& out_text,
                                         const OfficialChartSaveOptions& options = {});
  static SerializeResult save_chart_file(const NotationChart& chart, const std::string& path,
                                         const OfficialChartSaveOptions& options = {});

  static SerializeResult serialize_music_config(const OfficialMusicConfig& config,
                                                std::string& out_text);
  static SerializeResult save_music_config_file(const OfficialMusicConfig& config,
                                                const std::string& path);

  // True when path looks like official CSV (extension or first data line).
  static bool looks_like_official_chart_path(const std::string& path);
  static bool looks_like_official_chart_text(const std::string& text);
};

}  // namespace wds::chart_editor

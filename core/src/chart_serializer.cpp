#include <wds/core/chart_serializer.hpp>

#include <wds/core/file_io.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/sus_chart.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace wds::chart_editor {
namespace {

constexpr std::size_t kMaxSerializedCount = 1'000'000;
constexpr double kMaxTickRaw = static_cast<double>(INT32_MAX);
constexpr double kMinTickRaw = -kMaxTickRaw;

bool tick_field_in_range(double raw) noexcept {
  return std::isfinite(raw) && raw >= kMinTickRaw && raw <= kMaxTickRaw;
}

bool parse_tick_field(double raw, int32_t& out) noexcept {
  if (!tick_field_in_range(raw)) {
    return false;
  }
  out = static_cast<int32_t>(std::llround(raw));
  return true;
}

bool is_known_note_type(int32_t raw) noexcept {
  switch (static_cast<NoteType>(raw)) {
    case NoteType::HiSpeed:
    case NoteType::None:
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::Sound:
    case NoteType::ScratchSound:
    case NoteType::Scratch:
    case NoteType::Flick:
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
    case NoteType::Hold:
    case NoteType::CriticalHold:
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailHold:
    case NoteType::NontailCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::BlueTap:
    case NoteType::HoldEighth:
      return true;
  }
  return false;
}

bool is_known_gimmick(int32_t raw) noexcept {
  switch (static_cast<GimmickType>(raw)) {
    case GimmickType::None:
    case GimmickType::JumpScratch:
    case GimmickType::OneDirection:
    case GimmickType::Split1:
    case GimmickType::Split2:
    case GimmickType::Split3:
    case GimmickType::Split4:
    case GimmickType::Split5:
    case GimmickType::Split6:
    case GimmickType::FullSplit1:
    case GimmickType::FullSplit2:
    case GimmickType::FullSplit3:
    case GimmickType::FullSplit4:
    case GimmickType::FullSplit5:
    case GimmickType::FullSplit6:
    case GimmickType::LightSplit1:
    case GimmickType::LightSplit2:
    case GimmickType::LightSplit3:
    case GimmickType::LightSplit4:
    case GimmickType::LightSplit5:
    case GimmickType::LightSplit6:
    case GimmickType::IgnoreSplit1:
    case GimmickType::IgnoreSplit2:
    case GimmickType::IgnoreSplit3:
    case GimmickType::IgnoreSplit4:
    case GimmickType::IgnoreSplit5:
    case GimmickType::IgnoreSplit6:
      return true;
  }
  return false;
}

}  // namespace

SerializeResult ChartSerializer::save_to_file(const NotationChart& chart,
                                              const std::string& path) {
  if (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
      !required_chart_features(chart).empty()) {
    return {SerializeError::ParseError, "Sirius Extended profile required for SV, nontail holds or overhang"};
  }
  MusicTiming timing = chart.timing;
  normalize_timing_points(timing);

  std::ostringstream ss;
  ss << std::setprecision(std::numeric_limits<double>::max_digits10);
  // WDSCHART v5 — N rows include parent_hold_id (star ↔ hold bind).
  // Chart delay lives in .wdsproject (one song, many charts) — not in the chart file.
  ss << "WDSCHART " << kFormatVersion << '\n';
  ss << "BPM " << timing.bpm << '\n';
  ss << "TPQ " << timing.ticks_per_quarter << '\n';
  ss << "TIMING " << timing.points.size() << '\n';
  for (const auto& p : timing.points) {
    const int flags = (p.has_bpm ? 1 : 0) | (p.has_meter ? 2 : 0);
    ss << "T " << p.tick << ' ' << p.bpm << ' ' << p.numerator << ' ' << p.denominator << ' '
       << flags << '\n';
  }
  ss << "PROFILE "
     << (chart.compatibility_profile == ChartCompatibilityProfile::SiriusExtended
             ? "SiriusExtended"
             : "Official")
     << '\n';
  // HoldEighth is derived from its hold on load; it is never persisted.
  size_t persisted_notes = 0;
  for (const auto& note : chart.notes) {
    if (note.note_type == NoteType::HoldEighth) continue;
    ++persisted_notes;
    const bool split_placeholder =
        is_split_lane_gimmick(note.gimmick_type) ||
        (note.note_type == NoteType::None && note.lane == 0 && note.width == 0);
    const int64_t end_exclusive = static_cast<int64_t>(note.lane) + note.width;
    if (!split_placeholder &&
        (note.width < 1 ||
         (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
          (note.lane < 0 || note.width > 12 || end_exclusive > 12)))) {
      return {SerializeError::ParseError, "note lane/width out of range"};
    }
  }
  ss << "NOTES " << persisted_notes << '\n';

  for (const auto& note : chart.notes) {
    if (note.note_type == NoteType::HoldEighth) continue;
    ss << "N " << note.id << ' ' << note.start_tick << ' ' << note.end_tick << ' '
       << static_cast<int32_t>(note.note_type) << ' ' << note.lane << ' ' << note.width << ' '
       << static_cast<int32_t>(note.gimmick_type) << ' ' << note.scratch_length << ' '
       << note.parent_hold_id << '\n';
  }

  std::unordered_set<int32_t> speed_ticks;
  ss << "SPEEDS " << chart.scroll_speed_points.size() << '\n';
  for (const auto& point : chart.scroll_speed_points) {
    if (point.tick < 0 || !is_valid_scroll_multiplier(point.multiplier) ||
        !speed_ticks.insert(point.tick).second) {
      return {SerializeError::ParseError, "scroll speed point out of range"};
    }
    ss << "S " << point.tick << ' ' << point.multiplier << '\n';
  }
  ss << "CONCURRENT " << chart.concurrent_lines.size() << '\n';
  for (const auto& line : chart.concurrent_lines) {
    ss << "C " << line.milliseconds << ' ' << line.start_lane << ' ' << line.width << '\n';
  }

  ss << "END\n";
  return write_text_atomic(path, ss.str());
}

SerializeResult ChartSerializer::load_from_file(const std::string& path, NotationChart& out_chart) {
  SerializeResult io_status;
  const std::string bytes = read_text_file(path, io_status);
  if (io_status.error != SerializeError::Ok) return io_status;
  std::istringstream file(bytes);
  // Locale-independent decimals (avoid ',' decimal locales misparsing BPM/ticks).
  file.imbue(std::locale::classic());

  std::string magic;
  int32_t version = 0;
  file >> magic >> version;
  if (!file || magic != "WDSCHART" || version < kMinSupportedVersion ||
      version > kFormatVersion) {
    return {SerializeError::VersionMismatch, "unsupported chart file format"};
  }

  NotationChart chart;
  std::string key;
  size_t note_count = 0;
  size_t notes_seen = 0;
  size_t concurrent_count = 0;
  size_t speed_count = 0;
  // Legacy SUPPRESSED_EIGHTHS / X rows are validated and counted but otherwise ignored
  // (HoldEighth is derived from its hold, so tombstones carry no information).
  size_t suppressed_count = 0;
  size_t suppressed_rows_seen = 0;
  size_t timing_count = 0;
  bool saw_timing = false;
  bool saw_notes = false;
  bool saw_concurrent = false;
  bool saw_speeds = false;
  bool saw_suppressed = false;
  bool saw_profile = false;
  bool saw_end = false;
  std::unordered_set<int32_t> seen_ids;

  while (file >> key) {
    if (key == "BPM") {
      file >> chart.timing.bpm;
      if (!file) return {SerializeError::ParseError, "malformed BPM"};
      if (!std::isfinite(chart.timing.bpm) || !(chart.timing.bpm > 0.0)) {
        return {SerializeError::ParseError, "BPM out of range"};
      }
    } else if (key == "TPQ") {
      file >> chart.timing.ticks_per_quarter;
      if (!file) return {SerializeError::ParseError, "malformed TPQ"};
      if (!is_valid_ticks_per_quarter(chart.timing.ticks_per_quarter)) {
        return {SerializeError::ParseError, "TPQ out of range"};
      }
    } else if (key == "OFFSET_MS") {
      // Legacy charts may still contain OFFSET_MS; discard — offset is project-scoped.
      int64_t ignored = 0;
      file >> ignored;
      if (!file) return {SerializeError::ParseError, "malformed OFFSET_MS"};
    } else if (key == "TIMING") {
      file >> timing_count;
      if (!file) return {SerializeError::ParseError, "malformed TIMING count"};
      if (timing_count > kMaxSerializedCount) {
        return {SerializeError::ParseError, "TIMING count exceeds limit"};
      }
      saw_timing = true;
      chart.timing.points.clear();
      chart.timing.points.reserve(timing_count);
    } else if (key == "T") {
      TimingPoint point;
      file >> point.tick >> point.bpm >> point.numerator >> point.denominator;
      if (!file) {
        return {SerializeError::ParseError, "malformed timing point"};
      }
      // v4+: optional flags. v3 rows omit them → both BPM and meter authored.
      point.has_bpm = true;
      point.has_meter = true;
      if (version >= 4) {
        int flags = 3;
        file >> flags;
        if (!file) {
          return {SerializeError::ParseError, "malformed timing point flags"};
        }
        point.has_bpm = (flags & 1) != 0;
        point.has_meter = (flags & 2) != 0;
        if (!point.has_bpm && !point.has_meter) {
          point.has_bpm = true;
        }
      }
      if (point.tick < 0 || !std::isfinite(point.bpm) || !(point.bpm > 0.0) ||
          point.numerator <= 0 || point.denominator <= 0) {
        return {SerializeError::ParseError, "timing point out of range"};
      }
      chart.timing.points.push_back(point);
    } else if (key == "NOTES") {
      file >> note_count;
      if (!file) return {SerializeError::ParseError, "malformed NOTES count"};
      if (note_count > kMaxSerializedCount) {
        return {SerializeError::ParseError, "NOTES count exceeds limit"};
      }
      saw_notes = true;
      chart.notes.reserve(note_count);
    } else if (key == "N") {
      NotationNote note;
      int32_t note_type_raw = 0;
      int32_t gimmick_raw = 0;

      double start_tick_raw = 0.0;
      double end_tick_raw = 0.0;
      file >> note.id >> start_tick_raw >> end_tick_raw >> note_type_raw >> note.lane >>
          note.width >> gimmick_raw >> note.scratch_length;

      if (!file) {
        return {SerializeError::ParseError, "malformed note record"};
      }

      if (version >= 5) {
        file >> note.parent_hold_id;
        if (!file) {
          return {SerializeError::ParseError, "malformed note record"};
        }
      }

      // v1 appended four unused ignore_* collider flags.
      if (version == 1) {
        int ignore_li = 0;
        int ignore_ri = 0;
        int ignore_lo = 0;
        int ignore_ro = 0;
        file >> ignore_li >> ignore_ri >> ignore_lo >> ignore_ro;
        if (!file) {
          return {SerializeError::ParseError, "malformed v1 note record"};
        }
      }

      if (!is_known_note_type(note_type_raw)) {
        return {SerializeError::ParseError, "unknown note type"};
      }
      if (!is_known_gimmick(gimmick_raw)) {
        return {SerializeError::ParseError, "unknown gimmick type"};
      }
      const int64_t note_end = static_cast<int64_t>(note.lane) + note.width;
      const bool split_placeholder = is_split_lane_gimmick(static_cast<GimmickType>(gimmick_raw)) ||
                                     (note_type_raw == 0 && note.lane == 0 && note.width == 0);
      if (!split_placeholder && (note.width < 1 ||
          (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
           (note.lane < 0 || note.width > 12 || note_end > 12)))) {
        return {SerializeError::ParseError, "note lane/width out of range"};
      }
      if (!parse_tick_field(start_tick_raw, note.start_tick) ||
          !parse_tick_field(end_tick_raw, note.end_tick)) {
        return {SerializeError::ParseError, "note tick out of range"};
      }
      if (note.id == std::numeric_limits<int32_t>::max()) {
        return {SerializeError::ParseError, "note id out of range"};
      }
      if (note.id >= 0 && !seen_ids.insert(note.id).second) {
        return {SerializeError::ParseError, "duplicate note id"};
      }

      ++notes_seen;
      note.note_type = static_cast<NoteType>(note_type_raw);
      note.gimmick_type = static_cast<GimmickType>(gimmick_raw);
      if (note.note_type == NoteType::HoldEighth) {
        // Legacy file-provided eighth (derived, authored or suppressed marker): the
        // row was fully validated above, then dropped. Eighths are recomputed below.
        continue;
      }
      chart.notes.push_back(note);
    } else if (key == "SUPPRESSED_EIGHTHS") {
      file >> suppressed_count;
      if (!file) return {SerializeError::ParseError, "malformed SUPPRESSED_EIGHTHS count"};
      if (suppressed_count > kMaxSerializedCount) {
        return {SerializeError::ParseError, "SUPPRESSED_EIGHTHS count exceeds limit"};
      }
      saw_suppressed = true;
    } else if (key == "X") {
      // Legacy suppression tombstone: X parent_hold_id tick lane width.
      int32_t parent_hold_id = -1;
      int32_t tick = 0;
      int32_t lane = 0;
      int32_t width = 1;
      file >> parent_hold_id >> tick >> lane >> width;
      if (!file || parent_hold_id < 0 || tick < 0 || width < 1) {
        return {SerializeError::ParseError, "malformed suppressed HoldEighth"};
      }
      // Same lane/width bounds as normal note rows.
      const int64_t row_end = static_cast<int64_t>(lane) + width;
      if (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
          (lane < 0 || width > 12 || row_end > 12)) {
        return {SerializeError::ParseError, "suppressed HoldEighth lane/width out of range"};
      }
      if (suppressed_rows_seen >= kMaxSerializedCount) {
        return {SerializeError::ParseError, "SUPPRESSED_EIGHTHS count exceeds limit"};
      }
      ++suppressed_rows_seen;
    } else if (key == "CONCURRENT") {
      file >> concurrent_count;
      if (!file) return {SerializeError::ParseError, "malformed CONCURRENT count"};
      if (concurrent_count > kMaxSerializedCount) {
        return {SerializeError::ParseError, "CONCURRENT count exceeds limit"};
      }
      saw_concurrent = true;
      chart.concurrent_lines.reserve(concurrent_count);
    } else if (key == "SPEEDS") {
      file >> speed_count;
      if (!file) return {SerializeError::ParseError, "malformed SPEEDS count"};
      if (speed_count > kMaxSerializedCount) {
        return {SerializeError::ParseError, "SPEEDS count exceeds limit"};
      }
      saw_speeds = true;
      chart.scroll_speed_points.reserve(speed_count);
    } else if (key == "S") {
      ScrollSpeedPoint point;
      file >> point.tick >> point.multiplier;
      if (!file || point.tick < 0 || !is_valid_scroll_multiplier(point.multiplier)) {
        return {SerializeError::ParseError, "scroll speed point out of range"};
      }
      chart.scroll_speed_points.push_back(point);
    } else if (key == "PROFILE") {
      if (saw_profile) return {SerializeError::ParseError, "duplicate PROFILE"};
      saw_profile = true;
      std::string profile;
      file >> profile;
      if (!file) return {SerializeError::ParseError, "malformed PROFILE"};
      if (profile == "Official") {
        chart.compatibility_profile = ChartCompatibilityProfile::Official;
      } else if (profile == "SiriusExtended") {
        chart.compatibility_profile = ChartCompatibilityProfile::SiriusExtended;
      } else {
        return {SerializeError::ParseError, "unknown chart compatibility profile"};
      }
    } else if (key == "C") {
      ConcurrentLineNote line;
      file >> line.milliseconds >> line.start_lane >> line.width;
      if (!file) {
        return {SerializeError::ParseError, "malformed concurrent line record"};
      }
      if (line.start_lane < 0 || line.width < 0 || line.width > 12 ||
          static_cast<int64_t>(line.start_lane) + static_cast<int64_t>(line.width) > 12) {
        return {SerializeError::ParseError, "concurrent lane/width out of range"};
      }
      chart.concurrent_lines.push_back(line);
    } else if (key == "END") {
      saw_end = true;
      break;
    } else {
      return {SerializeError::ParseError, "unknown token: " + key};
    }
  }

  if (!saw_end) {
    return {SerializeError::ParseError, "chart file missing END"};
  }
  if (version >= 6 && (!saw_profile || !saw_speeds)) {
    return {SerializeError::ParseError, "chart file missing v6 profile or speeds"};
  }
  if (saw_timing && chart.timing.points.size() != timing_count) {
    return {SerializeError::ParseError, "TIMING count mismatch"};
  }
  if (saw_notes && notes_seen != note_count) {
    return {SerializeError::ParseError, "NOTES count mismatch"};
  }
  if (saw_concurrent && chart.concurrent_lines.size() != concurrent_count) {
    return {SerializeError::ParseError, "CONCURRENT count mismatch"};
  }
  if (saw_speeds && chart.scroll_speed_points.size() != speed_count) {
    return {SerializeError::ParseError, "SPEEDS count mismatch"};
  }
  if (saw_suppressed && suppressed_rows_seen != suppressed_count) {
    return {SerializeError::ParseError, "SUPPRESSED_EIGHTHS count mismatch"};
  }
  // A hold (tailed or nontail) whose tpq/2 grid would generate more than
  // kMaxAutoHoldEighths eighths is rejected here, before any consumer tries to
  // materialize them.
  if (!hold_eighths_within_limit(chart.notes, chart.timing.ticks_per_quarter)) {
    return {SerializeError::ParseError, "hold span exceeds auto HoldEighth limit"};
  }
  std::sort(chart.scroll_speed_points.begin(), chart.scroll_speed_points.end(),
            [](const auto& a, const auto& b) { return a.tick < b.tick; });
  for (size_t i = 1; i < chart.scroll_speed_points.size(); ++i) {
    if (chart.scroll_speed_points[i - 1].tick == chart.scroll_speed_points[i].tick) {
      return {SerializeError::ParseError, "duplicate scroll speed tick"};
    }
  }

  if (version >= 5) {
    for (auto& n : chart.notes) {
      if (n.note_type == NoteType::Sound || n.note_type == NoteType::ScratchSound) continue;
      n.parent_hold_id = kNoBoundHoldId;
    }
  }
  infer_legacy_star_hold_binds(chart.notes);
  // Eighths are derived from the holds (stars must be bound first: they occupy ticks).
  chart.notes = with_all_hold_eighths_recomputed(std::move(chart.notes),
                                                 chart.timing.ticks_per_quarter);

  if (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
      !required_chart_features(chart).empty()) {
    if (version >= 6) {
      return {SerializeError::ParseError, "Sirius Extended profile required for chart features"};
    }
    chart.compatibility_profile = ChartCompatibilityProfile::SiriusExtended;
  }

  normalize_timing_points(chart.timing);
  out_chart = std::move(chart);
  return {SerializeError::Ok, {}};
}

SerializeResult ChartSerializer::load_auto(const std::string& path, NotationChart& out_chart,
                                           const std::string& music_config_path,
                                           ChartEditMode* out_edit_mode) {
  auto set_mode = [&](ChartEditMode mode) {
    if (out_edit_mode != nullptr) {
      *out_edit_mode = mode;
    }
  };

  if (SusChartFormat::looks_like_sus_path(path)) {
    set_mode(ChartEditMode::OfficialPreviewOnly);
    SusChartLoadResult loaded;
    const auto result = SusChartFormat::load_file(path, loaded);
    if (result.error == SerializeError::Ok) out_chart = std::move(loaded.chart);
    return result;
  }

  if (OfficialChartFormat::looks_like_official_chart_path(path)) {
    set_mode(ChartEditMode::OfficialPreviewOnly);
    return OfficialChartFormat::load_chart_auto_profile(path, music_config_path, out_chart);
  }

  {
    SerializeResult peek_status;
    const std::string bytes = read_text_file(path, peek_status);
    if (peek_status.error == SerializeError::Ok) {
      const std::string sample = bytes.substr(0, std::min<std::size_t>(bytes.size(), 512));
      if (SusChartFormat::looks_like_sus_text(sample)) {
        set_mode(ChartEditMode::OfficialPreviewOnly);
        SusChartLoadResult loaded;
        const auto result = SusChartFormat::load_file(path, loaded);
        if (result.error == SerializeError::Ok) out_chart = std::move(loaded.chart);
        return result;
      }
      if (OfficialChartFormat::looks_like_official_chart_text(sample)) {
        set_mode(ChartEditMode::OfficialPreviewOnly);
        return OfficialChartFormat::load_chart_auto_profile(path, music_config_path, out_chart);
      }
    }
  }

  set_mode(ChartEditMode::Editable);
  return load_from_file(path, out_chart);
}

}  // namespace wds::chart_editor

#include <wds/core/chart_serializer.hpp>

#include <wds/core/file_io.hpp>
#include <wds/core/official_chart.hpp>
#include <wds/core/sus_chart.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <cstdint>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace wds::chart_editor {
namespace {

constexpr std::size_t kMaxSerializedCount = 1'000'000;
constexpr double kMaxTickRaw = static_cast<double>(INT32_MAX);

bool is_known_note_type(int32_t raw) noexcept {
  switch (static_cast<NoteType>(raw)) {
    case NoteType::HiSpeed:
    case NoteType::None:
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::Sound:
    case NoteType::ScratchSound:
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
  MusicTiming timing = chart.timing;
  normalize_timing_points(timing);

  std::ostringstream ss;
  // WDSCHART v4 — TIMING points with has_bpm / has_meter flags (bit0 / bit1).
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
  ss << "NOTES " << chart.notes.size() << '\n';

  for (const auto& note : chart.notes) {
    ss << "N " << note.id << ' ' << note.start_tick << ' ' << note.end_tick << ' '
       << static_cast<int32_t>(note.note_type) << ' ' << note.lane << ' ' << note.width << ' '
       << static_cast<int32_t>(note.gimmick_type) << ' ' << note.scratch_length << '\n';
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
  size_t concurrent_count = 0;
  size_t timing_count = 0;
  bool saw_timing = false;
  bool saw_notes = false;
  bool saw_concurrent = false;
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
      if (note.lane < 0 || note.width < 0 || note.width > 12 ||
          static_cast<int64_t>(note.lane) + static_cast<int64_t>(note.width) > 12) {
        return {SerializeError::ParseError, "note lane/width out of range"};
      }
      if (!std::isfinite(start_tick_raw) || !std::isfinite(end_tick_raw) || start_tick_raw < 0.0 ||
          end_tick_raw < 0.0 || start_tick_raw > kMaxTickRaw || end_tick_raw > kMaxTickRaw) {
        return {SerializeError::ParseError, "note tick out of range"};
      }
      note.start_tick = static_cast<int32_t>(std::llround(start_tick_raw));
      note.end_tick = static_cast<int32_t>(std::llround(end_tick_raw));
      if (note.id == std::numeric_limits<int32_t>::max()) {
        return {SerializeError::ParseError, "note id out of range"};
      }
      if (note.id >= 0 && !seen_ids.insert(note.id).second) {
        return {SerializeError::ParseError, "duplicate note id"};
      }

      note.note_type = static_cast<NoteType>(note_type_raw);
      note.gimmick_type = static_cast<GimmickType>(gimmick_raw);
      chart.notes.push_back(note);
    } else if (key == "CONCURRENT") {
      file >> concurrent_count;
      if (!file) return {SerializeError::ParseError, "malformed CONCURRENT count"};
      if (concurrent_count > kMaxSerializedCount) {
        return {SerializeError::ParseError, "CONCURRENT count exceeds limit"};
      }
      saw_concurrent = true;
      chart.concurrent_lines.reserve(concurrent_count);
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
  if (saw_timing && chart.timing.points.size() != timing_count) {
    return {SerializeError::ParseError, "TIMING count mismatch"};
  }
  if (saw_notes && chart.notes.size() != note_count) {
    return {SerializeError::ParseError, "NOTES count mismatch"};
  }
  if (saw_concurrent && chart.concurrent_lines.size() != concurrent_count) {
    return {SerializeError::ParseError, "CONCURRENT count mismatch"};
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
    return OfficialChartFormat::load_chart_with_music_config(path, music_config_path, out_chart);
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
        return OfficialChartFormat::load_chart_with_music_config(path, music_config_path, out_chart);
      }
    }
  }

  set_mode(ChartEditMode::Editable);
  return load_from_file(path, out_chart);
}

}  // namespace wds::chart_editor

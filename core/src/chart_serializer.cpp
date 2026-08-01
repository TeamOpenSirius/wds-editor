#include <wds/core/chart_serializer.hpp>

#include <wds/core/official_chart.hpp>
#include <wds/core/sus_chart.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace wds::chart_editor {

SerializeResult ChartSerializer::save_to_file(const NotationChart& chart,
                                              const std::string& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return {SerializeError::IoError, "failed to open file for writing: " + path};
  }

  MusicTiming timing = chart.timing;
  normalize_timing_points(timing);

  // WDSCHART v3 — official column layout + TIMING BPM/meter points.
  // Chart delay lives in .wdsproject (one song, many charts) — not in the chart file.
  file << "WDSCHART " << kFormatVersion << '\n';
  file << "BPM " << timing.bpm << '\n';
  file << "TPQ " << timing.ticks_per_quarter << '\n';
  file << "TIMING " << timing.points.size() << '\n';
  for (const auto& p : timing.points) {
    file << "T " << p.tick << ' ' << p.bpm << ' ' << p.numerator << ' ' << p.denominator << '\n';
  }
  file << "NOTES " << chart.notes.size() << '\n';

  for (const auto& note : chart.notes) {
    file << "N " << note.id << ' ' << note.start_tick << ' ' << note.end_tick << ' '
         << static_cast<int32_t>(note.note_type) << ' ' << note.lane << ' ' << note.width << ' '
         << static_cast<int32_t>(note.gimmick_type) << ' ' << note.scratch_length << '\n';
  }

  file << "CONCURRENT " << chart.concurrent_lines.size() << '\n';
  for (const auto& line : chart.concurrent_lines) {
    file << "C " << line.milliseconds << ' ' << line.start_lane << ' ' << line.width << '\n';
  }

  file << "END\n";
  if (!file) {
    return {SerializeError::IoError, "failed while writing: " + path};
  }

  return {SerializeError::Ok, {}};
}

SerializeResult ChartSerializer::load_from_file(const std::string& path, NotationChart& out_chart) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {SerializeError::IoError, "failed to open file for reading: " + path};
  }

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

  while (file >> key) {
    if (key == "BPM") {
      file >> chart.timing.bpm;
    } else if (key == "TPQ") {
      file >> chart.timing.ticks_per_quarter;
    } else if (key == "OFFSET_MS") {
      // Legacy charts may still contain OFFSET_MS; discard — offset is project-scoped.
      int64_t ignored = 0;
      file >> ignored;
    } else if (key == "TIMING") {
      file >> timing_count;
      chart.timing.points.clear();
      chart.timing.points.reserve(timing_count);
    } else if (key == "T") {
      TimingPoint point;
      file >> point.tick >> point.bpm >> point.numerator >> point.denominator;
      if (!file) {
        return {SerializeError::ParseError, "malformed timing point"};
      }
      chart.timing.points.push_back(point);
    } else if (key == "NOTES") {
      file >> note_count;
      chart.notes.reserve(note_count);
    } else if (key == "N") {
      NotationNote note;
      int32_t note_type_raw = 0;
      int32_t gimmick_raw = 0;

      file >> note.id >> note.start_tick >> note.end_tick >> note_type_raw >> note.lane >>
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

      note.note_type = static_cast<NoteType>(note_type_raw);
      note.gimmick_type = static_cast<GimmickType>(gimmick_raw);
      chart.notes.push_back(note);
    } else if (key == "CONCURRENT") {
      file >> concurrent_count;
      chart.concurrent_lines.reserve(concurrent_count);
    } else if (key == "C") {
      ConcurrentLineNote line;
      file >> line.milliseconds >> line.start_lane >> line.width;
      if (!file) {
        return {SerializeError::ParseError, "malformed concurrent line record"};
      }
      chart.concurrent_lines.push_back(line);
    } else if (key == "END") {
      break;
    } else {
      return {SerializeError::ParseError, "unknown token: " + key};
    }
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

  std::ifstream peek(path, std::ios::binary);
  if (peek) {
    std::string sample;
    sample.resize(512);
    peek.read(sample.data(), static_cast<std::streamsize>(sample.size()));
    sample.resize(static_cast<size_t>(std::max<std::streamsize>(0, peek.gcount())));
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

  set_mode(ChartEditMode::Editable);
  return load_from_file(path, out_chart);
}

}  // namespace wds::chart_editor

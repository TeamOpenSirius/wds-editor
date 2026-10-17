#include <wds/core/official_chart.hpp>

#include <wds/core/file_io.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace wds::chart_editor {

namespace {

constexpr int32_t kHiSpeedType = -1;

std::string trim_copy(std::string s) {
  auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r'; };
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : line) {
    if (c == ',') {
      parts.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  return parts;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  if (!cur.empty() || (!text.empty() && (text.back() == '\n' || text.back() == '\r'))) {
    lines.push_back(cur);
  }
  return lines;
}

bool parse_double(const std::string& text, double& out) {
  try {
    size_t idx = 0;
    out = std::stod(text, &idx);
    return idx > 0;
  } catch (...) {
    return false;
  }
}

bool parse_int(const std::string& text, int32_t& out) {
  try {
    size_t idx = 0;
    out = static_cast<int32_t>(std::stol(text, &idx));
    return idx > 0;
  } catch (...) {
    return false;
  }
}

int32_t seconds_to_tick(double seconds, const MusicTiming& timing) {
  // Official CSV seconds are chart-local (DelaySeconds lives in music_config).
  MusicTiming local = timing;
  local.offset_ms = 0;
  return milliseconds_to_tick(static_cast<int64_t>(std::llround(seconds * 1000.0)), local);
}

double tick_to_seconds(int32_t tick, const MusicTiming& timing) {
  // Integrate BPM changes; exclude project-level chart delay (music_config DelaySeconds).
  MusicTiming local = timing;
  local.offset_ms = 0;
  return static_cast<double>(tick_to_milliseconds(tick, local)) / 1000.0;
}

bool parse_gimmick_token(const std::string& token, GimmickType& out) {
  const std::string t = trim_copy(token);
  if (t.empty() || t == "0" || t == "None") {
    out = GimmickType::None;
    return true;
  }
  if (t == "JumpScratch") {
    out = GimmickType::JumpScratch;
    return true;
  }
  if (t == "OneDirection") {
    out = GimmickType::OneDirection;
    return true;
  }
  int32_t raw = 0;
  if (!parse_int(t, raw)) {
    return false;
  }
  out = static_cast<GimmickType>(raw);
  return true;
}

std::string format_gimmick(GimmickType gimmick, bool use_names) {
  if (use_names) {
    if (gimmick == GimmickType::JumpScratch) {
      return "JumpScratch";
    }
    if (gimmick == GimmickType::OneDirection) {
      return "OneDirection";
    }
  }
  return std::to_string(static_cast<int32_t>(gimmick));
}

std::string read_file_text(const std::string& path, SerializeResult& status) {
  return read_text_file(path, status);
}

SerializeResult write_file_text(const std::string& path, const std::string& text) {
  return write_text_atomic(path, text);
}

bool path_ends_with_ci(const std::string& path, const std::string& suffix) {
  if (path.size() < suffix.size()) {
    return false;
  }
  for (size_t i = 0; i < suffix.size(); ++i) {
    const char a = path[path.size() - suffix.size() + i];
    const char b = suffix[i];
    const char al = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
    const char bl = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
    if (al != bl) {
      return false;
    }
  }
  return true;
}

}  // namespace

SerializeResult OfficialChartFormat::parse_music_config(const std::string& text,
                                                        OfficialMusicConfig& out) {
  const auto lines = split_lines(text);
  const std::string* data_line = nullptr;
  for (const auto& line : lines) {
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed.find("CueSheetName") != std::string::npos &&
        trimmed.find("DelaySeconds") != std::string::npos) {
      continue;  // header
    }
    data_line = &line;
    break;
  }
  if (data_line == nullptr) {
    return {SerializeError::ParseError, "music_config: no data row"};
  }

  const auto cols = split_csv_line(*data_line);
  if (cols.size() < 3) {
    return {SerializeError::ParseError, "music_config: expected >= 3 columns"};
  }

  OfficialMusicConfig config;
  config.cue_sheet_name = trim_copy(cols[0]);
  config.cue_name = trim_copy(cols[1]);
  if (!parse_double(trim_copy(cols[2]), config.delay_seconds)) {
    return {SerializeError::ParseError, "music_config: invalid DelaySeconds"};
  }
  if (cols.size() >= 4) {
    config.cue_sheet_directory = trim_copy(cols[3]);
  }
  out = std::move(config);
  return {SerializeError::Ok, {}};
}

SerializeResult OfficialChartFormat::load_music_config_file(const std::string& path,
                                                            OfficialMusicConfig& out) {
  SerializeResult status;
  const std::string text = read_file_text(path, status);
  if (status.error != SerializeError::Ok) {
    return status;
  }
  return parse_music_config(text, out);
}

SerializeResult OfficialChartFormat::parse_chart(const std::string& text, NotationChart& out_chart,
                                                 const OfficialChartLoadOptions& options) {
  NotationChart chart;
  chart.timing.bpm = options.bpm;
  chart.timing.ticks_per_quarter = options.ticks_per_quarter;
  chart.compatibility_profile = options.compatibility_profile;
  if (options.delay_seconds.has_value()) {
    chart.timing.offset_ms =
        static_cast<int64_t>(std::llround(*options.delay_seconds * 1000.0));
  }

  const auto lines = split_lines(text);
  chart.notes.reserve(lines.size());

  int32_t next_id = 0;
  size_t line_no = 0;
  for (const auto& raw_line : lines) {
    ++line_no;
    const std::string line = trim_copy(raw_line);
    if (line.empty()) {
      continue;
    }

    const auto cols = split_csv_line(line);
    // StandardNotation.Parse in OpenWDS accepts the compact 5-column form
    // (start, end, type, lane, width) as well as the full 7-column form
    // (the latter carries gimmick type/value). Keep the same shape contract
    // so a hand-authored preview chart can be loaded by both runtimes.
    if (cols.size() != 5 && cols.size() != 7) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) +
                  " must have 5 or 7 columns"};
    }

    double start_sec = 0.0;
    double end_sec = -1.0;
    int32_t type_raw = 0;
    int32_t left_lane = 0;
    int32_t lane_length = 0;
    int32_t scratch_length = 0;

    if (!parse_double(trim_copy(cols[0]), start_sec) ||
        !parse_double(trim_copy(cols[1]), end_sec) || !parse_int(trim_copy(cols[2]), type_raw) ||
        !parse_int(trim_copy(cols[3]), left_lane) ||
        !parse_int(trim_copy(cols[4]), lane_length)) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has invalid numeric fields"};
    }
    if (cols.size() == 7 &&
        (!parse_int(trim_copy(cols[6]), scratch_length))) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has invalid numeric fields"};
    }
    // OpenWDS StandardNotation.Parse has no lower bound on start/end seconds
    // (negative start is valid). HiSpeed stores the multiplier in endTime, so
    // do not apply the ordinary end_sec upper bound to those rows.
    const bool is_hispeed = type_raw == kHiSpeedType;
    if (!std::isfinite(start_sec) || !std::isfinite(end_sec) || start_sec > 1.0e7 ||
        (!is_hispeed && end_sec > 1.0e7)) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has out-of-range time"};
    }
    if (lane_length < 0 ||
        (lane_length > 12 &&
         options.compatibility_profile == ChartCompatibilityProfile::Official)) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has out-of-range lane/width"};
    }
    // Official leftLane is often 1-based; validate the eventual 0-based span.
    const bool zero_width_placeholder =
        lane_length == 0 && left_lane <= 0 && (type_raw == 0 || is_hispeed);
    if (!zero_width_placeholder) {
      const int64_t zero_based =
          (options.convert_lane_to_zero_based)
              ? static_cast<int64_t>(left_lane) - 1
              : static_cast<int64_t>(left_lane);
      const int64_t end_exclusive = zero_based + static_cast<int64_t>(lane_length);
      if (options.compatibility_profile == ChartCompatibilityProfile::Official
              ? (zero_based < 0 || end_exclusive > 12)
              : (lane_length < 1)) {
        return {SerializeError::ParseError,
                "official chart: line " + std::to_string(line_no) + " has out-of-range lane/width"};
      }
    }

    GimmickType gimmick = GimmickType::None;
    if (cols.size() == 7 && !parse_gimmick_token(cols[5], gimmick)) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has invalid gimmickType"};
    }

    if (is_hispeed) {
      if (options.skip_hispeed) {
        continue;
      }
      if (!is_valid_scroll_multiplier(end_sec)) {
        return {SerializeError::ParseError,
                "official chart: HiSpeed multiplier must be non-negative (line " +
                    std::to_string(line_no) + ")"};
      }
      chart.scroll_speed_points.push_back(
          ScrollSpeedPoint{seconds_to_tick(start_sec, chart.timing), end_sec});
      continue;
    }
    NotationNote note;
    note.id = next_id++;
    note.start_tick = seconds_to_tick(start_sec, chart.timing);
    if (end_sec < 0.0) {
      // Official -1 = no duration. Mirror editor convention: end_tick == start_tick
      // (not 0), so HoldStart/taps never get end_ms < start_ms after offset.
      note.end_tick = note.start_tick;
    } else {
      note.end_tick = seconds_to_tick(end_sec, chart.timing);
    }
    note.width = std::max(0, lane_length);
    note.gimmick_type = gimmick;
    note.scratch_length = scratch_length;

    // Official split rows use type 0 and dummy leftLane=-1 / 0, laneLength=0.
    // Any split gimmick is a lane event, never a tap/hold, even if type is 20.
    const bool split_row = is_split_lane_gimmick(gimmick);
    const bool dummy_split_lane =
        left_lane <= 0 && (split_row || type_raw == 0 || (left_lane == 0 && lane_length == 0));
    if (dummy_split_lane) {
      note.lane = 0;
      note.width = 0;
    } else if (options.convert_lane_to_zero_based) {
      note.lane = left_lane - 1;
    } else {
      note.lane = left_lane;
    }

    note.note_type = split_row ? NoteType::None : static_cast<NoteType>(type_raw);
    if (is_flick_like(note.note_type) && !is_jump_scratch(note.gimmick_type) &&
        is_one_direction(note.gimmick_type)) {
      // Official OneDirection 0/1 → editor-internal None + ±width so place /
      // convert / mirror / arrows stay on the ±width path. None + nonzero is
      // an old editor chart and must keep its signed direction.
      const int32_t dir = note.scratch_length <= 0 ? -1 : 1;
      note.scratch_length = encode_flick_scratch_length(dir, note.width);
      note.gimmick_type = GimmickType::None;
    }
    chart.notes.push_back(note);
  }

  std::sort(chart.scroll_speed_points.begin(), chart.scroll_speed_points.end(),
            [](const auto& a, const auto& b) { return a.tick < b.tick; });
  for (size_t i = 1; i < chart.scroll_speed_points.size(); ++i) {
    if (chart.scroll_speed_points[i - 1].tick == chart.scroll_speed_points[i].tick) {
      return {SerializeError::ParseError, "official chart: duplicate HiSpeed time"};
    }
  }
  if (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
      !required_chart_features(chart).empty()) {
    return {SerializeError::ParseError, "official chart: Sirius Extended profile required"};
  }

  normalize_timing_points(chart.timing);
  // Official CSV has no BPM. Type 900 rows are the eighth positions; a tpq/2
  // grid from the default BPM-60 clock would not match the file. Editable
  // export still generates eighths in serialize_chart.
  chart.keep_file_eighths = true;
  // Sync lines are derived the same way as editor charts.
  chart.concurrent_lines = build_concurrent_lines(chart.notes, chart.timing);
  out_chart = std::move(chart);
  return {SerializeError::Ok, {}};
}

SerializeResult OfficialChartFormat::load_chart_file(const std::string& path,
                                                     NotationChart& out_chart,
                                                     const OfficialChartLoadOptions& options) {
  SerializeResult status;
  const std::string text = read_file_text(path, status);
  if (status.error != SerializeError::Ok) {
    return status;
  }
  return parse_chart(text, out_chart, options);
}

SerializeResult OfficialChartFormat::load_chart_with_music_config(
    const std::string& chart_path, const std::string& music_config_path, NotationChart& out_chart,
    OfficialChartLoadOptions options) {
  if (!music_config_path.empty()) {
    OfficialMusicConfig music;
    const auto music_result = load_music_config_file(music_config_path, music);
    if (music_result.error != SerializeError::Ok) {
      return music_result;
    }
    if (!options.delay_seconds.has_value()) {
      options.delay_seconds = music.delay_seconds;
    }
  }
  return load_chart_file(chart_path, out_chart, options);
}

SerializeResult OfficialChartFormat::load_chart_auto_profile(
    const std::string& chart_path, const std::string& music_config_path,
    NotationChart& out_chart) {
  auto result = load_chart_with_music_config(chart_path, music_config_path, out_chart);
  if (result.error == SerializeError::Ok ||
      (result.message.find("out-of-range lane/width") == std::string::npos &&
       result.message.find("Sirius Extended profile required") == std::string::npos)) {
    return result;
  }
  OfficialChartLoadOptions extended;
  extended.compatibility_profile = ChartCompatibilityProfile::SiriusExtended;
  return load_chart_with_music_config(chart_path, music_config_path, out_chart, extended);
}

SerializeResult OfficialChartFormat::serialize_chart(const NotationChart& chart,
                                                     std::string& out_text,
                                                     const OfficialChartSaveOptions& options) {
  if (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
      !required_chart_features(chart).empty()) {
    return {SerializeError::ParseError, "Sirius Extended profile required for SV, nontail holds or overhang"};
  }
  std::ostringstream ss;
  ss.setf(std::ios::fixed);
  ss.precision(4);

  std::vector<NotationNote> notes = with_all_hold_eighths_recomputed(
      chart.notes, chart.timing.ticks_per_quarter);
  // txt2data sorts by startTime then type. Appending 900s unsorted makes time
  // jump backward; a sequential judge pointer dies from that row onward.
  std::sort(notes.begin(), notes.end(), [](const NotationNote& a, const NotationNote& b) {
    if (a.start_tick != b.start_tick) return a.start_tick < b.start_tick;
    if (a.note_type != b.note_type) {
      return static_cast<int32_t>(a.note_type) < static_cast<int32_t>(b.note_type);
    }
    if (a.lane != b.lane) return a.lane < b.lane;
    return a.id < b.id;
  });

  struct CsvRow {
    int32_t tick = 0;
    bool speed = false;
    const NotationNote* note = nullptr;
    double multiplier = 1.0;
  };
  std::vector<CsvRow> rows;
  rows.reserve(notes.size() + chart.scroll_speed_points.size());
  for (const auto& note : notes) {
    rows.push_back(CsvRow{note.start_tick, false, &note, 1.0});
  }
  for (const auto& point : chart.scroll_speed_points) {
    if (point.tick < 0 || !is_valid_scroll_multiplier(point.multiplier)) {
      return {SerializeError::ParseError, "official chart: invalid HiSpeed multiplier"};
    }
    rows.push_back(CsvRow{point.tick, true, nullptr, point.multiplier});
  }
  std::vector<int32_t> speed_ticks;
  speed_ticks.reserve(chart.scroll_speed_points.size());
  for (const auto& point : chart.scroll_speed_points) speed_ticks.push_back(point.tick);
  std::sort(speed_ticks.begin(), speed_ticks.end());
  if (std::adjacent_find(speed_ticks.begin(), speed_ticks.end()) != speed_ticks.end()) {
    return {SerializeError::ParseError, "official chart: duplicate HiSpeed time"};
  }
  std::stable_sort(rows.begin(), rows.end(), [](const CsvRow& a, const CsvRow& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    return a.speed && !b.speed;
  });

  for (const auto& row : rows) {
    if (row.speed) {
      ss << tick_to_seconds(row.tick, chart.timing) << ',' << std::defaultfloat
         << std::setprecision(std::numeric_limits<double>::max_digits10) << row.multiplier
         << ',' << kHiSpeedType << ",0,0,0,0\n";
      ss << std::fixed << std::setprecision(4);
      continue;
    }
    const auto& note = *row.note;
    const bool split_placeholder =
        is_split_lane_gimmick(note.gimmick_type) ||
        (note.note_type == NoteType::None && note.lane == 0 && note.width == 0);
    const int64_t end_exclusive = static_cast<int64_t>(note.lane) + note.width;
    if (!split_placeholder &&
        (note.width < 1 ||
         (chart.compatibility_profile == ChartCompatibilityProfile::Official &&
          (note.lane < 0 || note.width > 12 || end_exclusive > 12)))) {
      return {SerializeError::ParseError, "official chart: note lane/width out of range"};
    }
    const double start_sec = tick_to_seconds(note.start_tick, chart.timing);

    // Official: hold body / split write endTime; taps / heads / mid-stars / HoldEighth → -1.
    const bool writes_end =
        (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) ||
        is_split_lane_gimmick(note.gimmick_type) || note.note_type == NoteType::None;
    const double end_sec =
        writes_end ? tick_to_seconds(note.end_tick, chart.timing) : -1.0;

    if (is_split_lane_gimmick(note.gimmick_type)) {
      ss << start_sec << ',';
      if (end_sec < 0.0) {
        ss << "-1.0";
      } else {
        ss << end_sec;
      }
      // Official sus2txt placeholder: type 0, leftLane=-1, laneLength=0.
      ss << ",0,-1,0," << format_gimmick(note.gimmick_type, options.use_gimmick_names) << ','
         << note.scratch_length << '\n';
      continue;
    }

    const bool split_row = note.note_type == NoteType::None;
    int32_t lane = note.lane;
    int32_t width = note.width;
    if (split_row && note.lane == 0 && note.width == 0) {
      lane = -1;
      width = 0;
    } else if (options.convert_lane_to_one_based) {
      lane = note.lane + 1;
    }

    GimmickType gimmick = note.gimmick_type;
    int32_t scratch_length = note.scratch_length;
    if (note.note_type == NoteType::HoldEighth) {
      gimmick = GimmickType::None;
      scratch_length = 0;
    } else if (is_flick_like(note.note_type)) {
      if (is_jump_scratch(gimmick)) {
        scratch_length = note.scratch_length;
      } else if (is_one_direction(gimmick)) {
        scratch_length = note.scratch_length <= 0 ? 0 : 1;
      } else if (note.scratch_length == 0) {
        gimmick = GimmickType::None;
        scratch_length = 0;
      } else {
        // Editor-internal / old-chart ±1 or ±width → official OneDirection 0/1.
        gimmick = GimmickType::OneDirection;
        scratch_length = note.scratch_length < 0 ? 0 : 1;
      }
    } else if (is_hold_chain_body(note.note_type)) {
      if (scratch_length == 0 && is_jump_scratch(gimmick)) {
        // JumpScratch requires a nonzero span. sl=0 is a plain scratch tail.
        gimmick = GimmickType::None;
      } else if (scratch_length != 0 && gimmick == GimmickType::None) {
        // Official: hold-chain body with non-zero GimmickValue uses JumpScratch.
        gimmick = GimmickType::JumpScratch;
      }
    }

    ss << start_sec << ',';
    if (end_sec < 0.0) {
      ss << "-1.0";
    } else {
      ss << end_sec;
    }
    ss << ',' << static_cast<int32_t>(note.note_type) << ',' << lane << ',' << width << ','
       << format_gimmick(gimmick, options.use_gimmick_names) << ',' << scratch_length
       << '\n';
  }

  out_text = ss.str();
  return {SerializeError::Ok, {}};
}

SerializeResult OfficialChartFormat::save_chart_file(const NotationChart& chart,
                                                     const std::string& path,
                                                     const OfficialChartSaveOptions& options) {
  std::string text;
  const auto result = serialize_chart(chart, text, options);
  if (result.error != SerializeError::Ok) {
    return result;
  }
  return write_file_text(path, text);
}

SerializeResult OfficialChartFormat::serialize_music_config(const OfficialMusicConfig& config,
                                                            std::string& out_text) {
  std::ostringstream ss;
  ss.setf(std::ios::fixed);
  ss.precision(3);
  ss << "CueSheetName,CueName,DelaySeconds,CueSheetDirectory\n";
  ss << config.cue_sheet_name << ',' << config.cue_name << ',' << config.delay_seconds << ','
     << config.cue_sheet_directory << '\n';
  out_text = ss.str();
  return {SerializeError::Ok, {}};
}

SerializeResult OfficialChartFormat::save_music_config_file(const OfficialMusicConfig& config,
                                                            const std::string& path) {
  std::string text;
  const auto result = serialize_music_config(config, text);
  if (result.error != SerializeError::Ok) {
    return result;
  }
  return write_file_text(path, text);
}

bool OfficialChartFormat::looks_like_official_chart_path(const std::string& path) {
  return path_ends_with_ci(path, ".csv");
}

bool OfficialChartFormat::looks_like_official_chart_text(const std::string& text) {
  for (const auto& raw : split_lines(text)) {
    const std::string line = trim_copy(raw);
    if (line.empty()) {
      continue;
    }
    if (line.rfind("WDSCHART", 0) == 0) {
      return false;
    }
    const auto cols = split_csv_line(line);
    if (cols.size() == 5 || cols.size() == 7) {
      double start = 0.0;
      int32_t type = 0;
      return parse_double(trim_copy(cols[0]), start) && parse_int(trim_copy(cols[2]), type);
    }
    return false;
  }
  return false;
}

}  // namespace wds::chart_editor

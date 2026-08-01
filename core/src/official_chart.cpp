#include <wds/core/official_chart.hpp>

#include <wds/core/gimmick.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
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

float seconds_to_tick(double seconds, const MusicTiming& timing) {
  if (timing.bpm <= 0.0 || timing.ticks_per_quarter <= 0) {
    return 0.0f;
  }
  const double ticks =
      seconds * (timing.bpm / 60.0) * static_cast<double>(timing.ticks_per_quarter);
  return static_cast<float>(ticks);
}

double tick_to_seconds(float tick, const MusicTiming& timing) {
  if (timing.bpm <= 0.0 || timing.ticks_per_quarter <= 0) {
    return 0.0;
  }
  return static_cast<double>(tick) * 60.0 /
         (timing.bpm * static_cast<double>(timing.ticks_per_quarter));
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
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    status = {SerializeError::IoError, "failed to open file for reading: " + path};
    return {};
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  if (!file && !file.eof()) {
    status = {SerializeError::IoError, "failed while reading: " + path};
    return {};
  }
  status = {SerializeError::Ok, {}};
  return ss.str();
}

SerializeResult write_file_text(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return {SerializeError::IoError, "failed to open file for writing: " + path};
  }
  file << text;
  if (!file) {
    return {SerializeError::IoError, "failed while writing: " + path};
  }
  return {SerializeError::Ok, {}};
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
    if (cols.size() < 7) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has < 7 columns"};
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
        !parse_int(trim_copy(cols[4]), lane_length) ||
        !parse_int(trim_copy(cols[6]), scratch_length)) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has invalid numeric fields"};
    }

    GimmickType gimmick = GimmickType::None;
    if (!parse_gimmick_token(cols[5], gimmick)) {
      return {SerializeError::ParseError,
              "official chart: line " + std::to_string(line_no) + " has invalid gimmickType"};
    }

    if (type_raw == kHiSpeedType) {
      if (options.skip_hispeed) {
        continue;
      }
      return {SerializeError::ParseError,
              "official chart: HiSpeed rows are not supported (line " +
                  std::to_string(line_no) + ")"};
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
    note.note_type = static_cast<NoteType>(type_raw);
    note.width = std::max(0, lane_length);
    note.gimmick_type = gimmick;
    note.scratch_length = scratch_length;

    if (options.convert_lane_to_zero_based && left_lane > 0) {
      note.lane = left_lane - 1;
    } else {
      note.lane = left_lane;
    }

    chart.notes.push_back(note);
  }

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

SerializeResult OfficialChartFormat::serialize_chart(const NotationChart& chart,
                                                     std::string& out_text,
                                                     const OfficialChartSaveOptions& options) {
  std::ostringstream ss;
  ss.setf(std::ios::fixed);
  ss.precision(4);

  for (const auto& note : chart.notes) {
    const double start_sec = tick_to_seconds(note.start_tick, chart.timing);

    // Official rule: hold body / split / type None write endTime (may equal start);
    // tap / hold-start / mid-star / flick write -1.
    const bool writes_end = is_hold_body(note.note_type) ||
                            is_split_lane_gimmick(note.gimmick_type) ||
                            note.note_type == NoteType::None;
    const double end_sec =
        writes_end ? tick_to_seconds(note.end_tick, chart.timing) : -1.0;

    // Pure gimmick placeholders stay at official lane 0; playable notes are 1-based.
    const bool placeholder_lane =
        note.lane == 0 && note.width == 0 &&
        (is_split_lane_gimmick(note.gimmick_type) || note.note_type == NoteType::None);
    int32_t lane = note.lane;
    if (options.convert_lane_to_one_based && !placeholder_lane) {
      lane = note.lane + 1;
    }

    ss << start_sec << ',';
    if (end_sec < 0.0) {
      ss << "-1.0";
    } else {
      ss << end_sec;
    }
    ss << ',' << static_cast<int32_t>(note.note_type) << ',' << lane << ',' << note.width << ','
       << format_gimmick(note.gimmick_type, options.use_gimmick_names) << ',' << note.scratch_length
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
    if (cols.size() >= 7) {
      double start = 0.0;
      int32_t type = 0;
      return parse_double(trim_copy(cols[0]), start) && parse_int(trim_copy(cols[2]), type);
    }
    return false;
  }
  return false;
}

}  // namespace wds::chart_editor

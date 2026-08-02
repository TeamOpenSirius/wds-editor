#include <wds/core/project.hpp>

#include <wds/core/file_io.hpp>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace wds::chart_editor {
namespace {

namespace fs = std::filesystem;

std::string trim_copy(std::string s) {
  const auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

std::string read_rest_of_line(std::istream& in) {
  std::string line;
  std::getline(in, line);
  return trim_copy(std::move(line));
}

fs::path path_from_utf8(const std::string& utf8) {
#if defined(_WIN32)
  return fs::u8path(utf8);
#else
  return fs::path(utf8);
#endif
}

std::string path_to_utf8(const fs::path& path) {
#if defined(_WIN32)
  const auto u8 = path.generic_u8string();
  return std::string(u8.begin(), u8.end());
#else
  return path.generic_string();
#endif
}

}  // namespace

const std::string& WdsProject::chart_path() const noexcept {
  static const std::string empty;
  if (chart_paths.empty()) return empty;
  const int32_t index = (active_chart_index >= 0 &&
                         active_chart_index < static_cast<int32_t>(chart_paths.size()))
                            ? active_chart_index
                            : 0;
  return chart_paths[static_cast<size_t>(index)];
}

std::string& WdsProject::chart_path() {
  if (chart_paths.empty()) chart_paths.emplace_back();
  if (active_chart_index < 0 || active_chart_index >= static_cast<int32_t>(chart_paths.size()))
    active_chart_index = 0;
  return chart_paths[static_cast<size_t>(active_chart_index)];
}

std::string ProjectSerializer::project_directory(const std::string& project_file_path) {
  const fs::path parent = path_from_utf8(project_file_path).parent_path();
  if (parent.empty()) {
    return {};
  }
  return path_to_utf8(parent);
}

std::string ProjectSerializer::resolve_path(const std::string& project_file_path,
                                            const std::string& stored_path) {
  if (stored_path.empty()) {
    return {};
  }
  const fs::path stored = path_from_utf8(stored_path);
  if (stored.is_absolute()) {
    return path_to_utf8(stored);
  }
  const fs::path base = path_from_utf8(project_file_path).parent_path();
  if (base.empty()) {
    return path_to_utf8(stored);
  }
  return path_to_utf8((base / stored).lexically_normal());
}

std::string ProjectSerializer::make_relative_path(const std::string& project_file_path,
                                                  const std::string& path) {
  if (path.empty()) {
    return {};
  }

  // Lexical only — avoid fs::relative/absolute which on MinGW+Windows may route
  // through the narrow ACP and corrupt/fail non-ASCII music paths.
  const fs::path base = path_from_utf8(project_file_path).parent_path();
  const fs::path target = path_from_utf8(path);

  if (!target.is_absolute()) {
    return path_to_utf8(target.lexically_normal());
  }
  if (base.empty() || !base.is_absolute()) {
    return path_to_utf8(target);
  }

  const fs::path relative = target.lexically_relative(base);
  // Empty / absolute result means roots differ or relativization is impossible.
  if (relative.empty() || relative.is_absolute()) {
    return path_to_utf8(target);
  }
  return path_to_utf8(relative.lexically_normal());
}

SerializeResult ProjectSerializer::save_to_file(const WdsProject& project,
                                                const std::string& path) {
  std::ostringstream ss;
  ss << "WDSPROJECT " << kFormatVersion << '\n';
  ss << "MUSIC " << project.music_path << '\n';
  ss << "CHART_DELAY_MS " << project.offset_ms << '\n';
  const int32_t active = project.chart_paths.empty()
                             ? 0
                             : std::clamp(project.active_chart_index, 0,
                                          static_cast<int32_t>(project.chart_paths.size()) - 1);
  ss << "ACTIVE_CHART " << active << '\n';
  for (const auto& chart_path : project.chart_paths) ss << "CHART " << chart_path << '\n';
  ss << "END\n";
  return write_text_atomic(path, ss.str());
}

SerializeResult ProjectSerializer::save_relativized(WdsProject project,
                                                    const std::string& project_file_path) {
  project.music_path = make_relative_path(project_file_path, project.music_path);
  for (auto& chart_path : project.chart_paths)
    chart_path = make_relative_path(project_file_path, chart_path);
  return save_to_file(project, project_file_path);
}

SerializeResult ProjectSerializer::load_from_file(const std::string& path,
                                                  WdsProject& out_project) {
  SerializeResult io_status;
  const std::string bytes = read_text_file(path, io_status);
  if (io_status.error != SerializeError::Ok) return io_status;
  std::istringstream file(bytes);

  std::string magic;
  int32_t version = 0;
  file >> magic >> version;
  if (!file || magic != "WDSPROJECT" || version < kMinSupportedVersion ||
      version > kFormatVersion) {
    return {SerializeError::VersionMismatch, "unsupported project file format"};
  }

  // Consume remainder of the magic line.
  read_rest_of_line(file);

  WdsProject project;
  std::string key;
  bool saw_end = false;

  while (file >> key) {
    if (key == "MUSIC") {
      project.music_path = read_rest_of_line(file);
    } else if (key == "CHART_DELAY_MS" || key == "OFFSET_MS") {
      // OFFSET_MS is a legacy alias for CHART_DELAY_MS (chart delay, not BGM delay).
      file >> project.offset_ms;
      if (!file) {
        return {SerializeError::ParseError, "malformed CHART_DELAY_MS"};
      }
      read_rest_of_line(file);
    } else if (key == "ACTIVE_CHART") {
      file >> project.active_chart_index;
      if (!file) return {SerializeError::ParseError, "malformed ACTIVE_CHART"};
      read_rest_of_line(file);
    } else if (key == "CHART") {
      project.chart_paths.push_back(read_rest_of_line(file));
    } else if (key == "END") {
      saw_end = true;
      break;
    } else {
      return {SerializeError::ParseError, "unknown project token: " + key};
    }
  }

  if (!saw_end) {
    return {SerializeError::ParseError, "project file missing END"};
  }
  if (project.chart_paths.empty() ||
      std::any_of(project.chart_paths.begin(), project.chart_paths.end(),
                  [](const std::string& chart_path) { return chart_path.empty(); })) {
    return {SerializeError::ParseError, "project CHART path is required"};
  }
  if (project.active_chart_index < 0 ||
      project.active_chart_index >= static_cast<int32_t>(project.chart_paths.size())) {
    return {SerializeError::ParseError, "ACTIVE_CHART is out of range"};
  }

  out_project = std::move(project);
  return {SerializeError::Ok, {}};
}

}  // namespace wds::chart_editor

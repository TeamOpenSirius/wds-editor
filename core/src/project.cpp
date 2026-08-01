#include <wds/core/project.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
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
  const fs::path parent = fs::path(project_file_path).parent_path();
  if (parent.empty()) {
    return {};
  }
  return parent.generic_string();
}

std::string ProjectSerializer::resolve_path(const std::string& project_file_path,
                                            const std::string& stored_path) {
  if (stored_path.empty()) {
    return {};
  }
  const fs::path stored(stored_path);
  if (stored.is_absolute()) {
    return stored.generic_string();
  }
  const fs::path base = fs::path(project_file_path).parent_path();
  if (base.empty()) {
    return stored.generic_string();
  }
  return (base / stored).lexically_normal().generic_string();
}

std::string ProjectSerializer::make_relative_path(const std::string& project_file_path,
                                                  const std::string& path) {
  if (path.empty()) {
    return {};
  }

  std::error_code ec;
  const fs::path base = fs::path(project_file_path).parent_path();
  fs::path target(path);

  if (!target.is_absolute()) {
    // Already relative — normalize separators only.
    return target.lexically_normal().generic_string();
  }

  if (base.empty()) {
    return target.generic_string();
  }

  fs::path abs_base = base;
  if (!abs_base.is_absolute()) {
    abs_base = fs::absolute(abs_base, ec);
    if (ec) {
      return target.generic_string();
    }
  }

  const fs::path relative = fs::relative(target, abs_base, ec);
  if (ec || relative.empty()) {
    return target.generic_string();
  }
  return relative.generic_string();
}

SerializeResult ProjectSerializer::save_to_file(const WdsProject& project,
                                                const std::string& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return {SerializeError::IoError, "failed to open project for writing: " + path};
  }

  file << "WDSPROJECT " << kFormatVersion << '\n';
  file << "MUSIC " << project.music_path << '\n';
  file << "CHART_DELAY_MS " << project.offset_ms << '\n';
  const int32_t active = project.chart_paths.empty()
                             ? 0
                             : std::clamp(project.active_chart_index, 0,
                                          static_cast<int32_t>(project.chart_paths.size()) - 1);
  file << "ACTIVE_CHART " << active << '\n';
  for (const auto& chart_path : project.chart_paths) file << "CHART " << chart_path << '\n';
  file << "END\n";

  if (!file) {
    return {SerializeError::IoError, "failed while writing project: " + path};
  }
  return {SerializeError::Ok, {}};
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
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {SerializeError::IoError, "failed to open project for reading: " + path};
  }

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

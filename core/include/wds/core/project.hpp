#pragma once

#include <wds/core/chart_serializer.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace wds::chart_editor {

// Native .wdsproject — project entry file that references chart + BGM assets.
// One project = one song (MUSIC + CHART_DELAY_MS) and one or more charts (CHART
// is the active chart path; additional charts may sit beside it and be swapped).
//
// Layout (v2), paths are relative to the .wdsproject directory when possible:
//   WDSPROJECT 2
//   MUSIC music.ogg
//   CHART_DELAY_MS 3019
//   ACTIVE_CHART 0
//   CHART chart-a.wdschart
//   CHART chart-b.wdschart
//   END
//
// MUSIC / CHART values are the remainder of the line (spaces allowed).
// Empty MUSIC means no background music assigned yet.
// CHART_DELAY_MS is song-level chart delay (ms) — never stored in .wdschart.
// Legacy key OFFSET_MS is accepted on load and treated as CHART_DELAY_MS.
struct WdsProject {
  std::string music_path;
  int64_t offset_ms = 0;  // chart delay ms (CHART_DELAY_MS)
  std::vector<std::string> chart_paths;
  int32_t active_chart_index = 0;

  // v1-compatible active chart accessor.
  const std::string& chart_path() const noexcept;
  std::string& chart_path();
};

class ProjectSerializer {
 public:
  static constexpr int32_t kFormatVersion = 2;
  static constexpr int32_t kMinSupportedVersion = 1;

  static SerializeResult save_to_file(const WdsProject& project, const std::string& path);
  static SerializeResult load_from_file(const std::string& path, WdsProject& out_project);

  // Directory containing the .wdsproject file (empty if path has no parent).
  static std::string project_directory(const std::string& project_file_path);

  // Resolve a stored path against the project file location.
  // Absolute paths are returned unchanged; relative paths join project_directory.
  static std::string resolve_path(const std::string& project_file_path,
                                  const std::string& stored_path);

  // Prefer a path relative to the project directory (portable projects).
  // Falls back to the input path when relativization is not possible.
  static std::string make_relative_path(const std::string& project_file_path,
                                        const std::string& path);

  // Rewrite music_path / chart_paths to be relative to `project_file_path`, then save.
  static SerializeResult save_relativized(WdsProject project, const std::string& project_file_path);
};

}  // namespace wds::chart_editor

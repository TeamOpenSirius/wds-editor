// Runtime resolver for the split-effect particle data directory (`resources/`).
// No Qt; builds throwaway directory trees under the system temp dir.

#include <wds/chart_render/split_effect_particles.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using wds::chart_render::find_split_effects_resource_root;
using wds::chart_render::set_split_effects_resource_root;
using wds::chart_render::split_effects_resource_root;

int failures = 0;

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

const char* kMarkerRel =
    "game_splitlaneelement_assets_spliteffectelements/"
    "spliteffectelements10050.bundle.objects.tsv";

// Unique temp tree, removed on destruction.
struct TempTree {
  fs::path root;
  explicit TempTree(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() /
           (std::string("wds_split_root_") + tag + "_" + std::to_string(stamp));
    fs::create_directories(root);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;
};

// Creates dir/<marker> (and parents).
void make_valid_resources(const fs::path& dir) {
  const fs::path marker = dir / kMarkerRel;
  fs::create_directories(marker.parent_path());
  std::ofstream(marker) << "1\tT\tdeadbeef\n";
}

std::string norm(const fs::path& p) { return p.lexically_normal().generic_string(); }

std::string found(const fs::path& exe_dir, const fs::path& cwd) {
  return fs::path(find_split_effects_resource_root(exe_dir, cwd)).generic_string();
}

void test_cwd_resources() {
  TempTree t("cwd");
  const fs::path cwd = t.root / "work";
  const fs::path exe = t.root / "elsewhere" / "bin";
  fs::create_directories(exe);
  make_valid_resources(cwd / "resources");
  CHECK(found(exe, cwd) == norm(cwd / "resources"));
}

void test_exe_dir_resources() {
  TempTree t("exe");
  const fs::path cwd = t.root / "work";
  const fs::path exe = t.root / "app";
  fs::create_directories(cwd);
  make_valid_resources(exe / "resources");
  CHECK(found(exe, cwd) == norm(exe / "resources"));
}

void test_macos_bundle_layout() {
  TempTree t("mac");
  const fs::path exe = t.root / "WDS Editor.app" / "Contents" / "MacOS";
  const fs::path cwd = t.root / "work";
  fs::create_directories(exe);
  fs::create_directories(cwd);
  make_valid_resources(t.root / "WDS Editor.app" / "Contents" / "Resources" / "resources");
  const std::string got = found(exe, cwd);
  CHECK(got == norm(t.root / "WDS Editor.app" / "Contents" / "Resources" / "resources"));
  CHECK(got.find("..") == std::string::npos);
}

void test_build_tree_walk_up() {
  TempTree t("walk");
  const fs::path repo = t.root / "repo";
  const fs::path exe = repo / "build-x" / "ui";
  const fs::path cwd = t.root / "unrelated";
  fs::create_directories(exe);
  fs::create_directories(cwd);
  make_valid_resources(repo / "resources");
  CHECK(found(exe, cwd) == norm(repo / "resources"));

  // Walk-up from cwd works too (ctest: cwd == exe dir inside the build tree).
  const fs::path elsewhere = t.root / "nowhere";
  fs::create_directories(elsewhere);
  CHECK(found(elsewhere, exe) == norm(repo / "resources"));
}

void test_walk_up_limit() {
  TempTree t("limit");
  const fs::path exe = t.root / "a" / "b" / "c" / "d" / "e" / "f" / "g";  // 7 levels below root
  fs::create_directories(exe);
  make_valid_resources(t.root / "resources");
  // exe's 6th parent is a/, the 7th is root: out of reach.
  CHECK(found(exe, exe).empty());
}

void test_marker_required() {
  TempTree t("nomarker");
  const fs::path cwd = t.root / "work";
  const fs::path exe = t.root / "repo" / "build" / "ui";
  fs::create_directories(cwd / "resources");
  fs::create_directories(t.root / "repo" / "resources" /
                         "game_splitlaneelement_assets_spliteffectelements");
  fs::create_directories(exe);
  CHECK(find_split_effects_resource_root(exe, cwd).empty());
}

void test_override() {
  set_split_effects_resource_root("/x/y");
  CHECK(split_effects_resource_root() == "/x/y");
  set_split_effects_resource_root("");
  CHECK(split_effects_resource_root() != "/x/y");
  CHECK(!split_effects_resource_root().empty());  // auto-detected root or "resources"
}

}  // namespace

int main() {
  test_cwd_resources();
  test_exe_dir_resources();
  test_macos_bundle_layout();
  test_build_tree_walk_up();
  test_walk_up_limit();
  test_marker_required();
  test_override();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("split_effect_resource_root_tests: ok");
  return 0;
}

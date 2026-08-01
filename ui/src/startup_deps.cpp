#include "wds/ui/startup_deps.hpp"

#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/resource_paths.hpp"

#include <wds/audio/audio_engine.hpp>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace wds::ui {
namespace fs = std::filesystem;

namespace {

fs::path exe_dir_from_argv0(const char* argv0) {
  std::error_code ec;
  if (argv0 == nullptr || argv0[0] == '\0') {
    return {};
  }
  fs::path exe = fs::path(argv0);
  if (!exe.is_absolute()) {
    exe = fs::current_path(ec) / exe;
  }
  if (ec) {
    return {};
  }
  return exe.lexically_normal().parent_path();
}

bool is_regular(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec) && !ec;
}

bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec) && !ec;
}

bool any_exists(std::initializer_list<fs::path> paths) {
  for (const auto& p : paths) {
    if (is_regular(p)) {
      return true;
    }
  }
  return false;
}

bool looks_like_macos_app(const fs::path& exe_dir) {
  // .../Something.app/Contents/MacOS
  const auto macos = exe_dir.filename();
  const auto contents = exe_dir.parent_path().filename();
  return macos == "MacOS" && contents == "Contents";
}

void require_resource_dir(StartupDependencyReport& report, const std::string& resolved,
                          const char* label, bool (*looks_ok)(const std::string&)) {
  if (resolved.empty() || !looks_ok(resolved)) {
    report.missing.push_back(std::string(label) + "（未找到可用目录）");
  }
}

void check_bass_runtime(StartupDependencyReport& report, const fs::path& exe_dir) {
  // If we reached main(), the dynamic linker already mapped BASS. Still verify
  // the API responds, and that the companion file exists for portable layouts.
  const uint32_t ver = wds::audio::linked_bass_version();
  if (ver == 0) {
    report.missing.push_back("BASS 音频库（linked_bass_version 失败）");
    return;
  }
#if defined(_WIN32)
  if (!any_exists({exe_dir / "bass.dll", exe_dir / "lib" / "bass.dll"})) {
    report.missing.push_back("bass.dll（应位于程序目录）");
  }
#elif defined(__APPLE__)
  if (!any_exists({exe_dir / "libbass.dylib", exe_dir / "lib" / "libbass.dylib"})) {
    report.missing.push_back("libbass.dylib（应位于程序目录或 lib/）");
  }
#else
  if (!any_exists({exe_dir / "libbass.so", exe_dir / "lib" / "libbass.so"})) {
    report.missing.push_back("libbass.so（应位于程序目录或 lib/）");
  }
#endif
}

fs::path bundled_moltenvk_icd(const fs::path& exe_dir) {
  const fs::path standard =
      (exe_dir / ".." / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json").lexically_normal();
  if (is_regular(standard)) {
    return standard;
  }
  const fs::path legacy =
      (exe_dir / ".." / "Resources" / "share" / "vulkan" / "icd" / "MoltenVK_icd.json")
          .lexically_normal();
  if (is_regular(legacy)) {
    return legacy;
  }
  return {};
}

void check_vulkan_runtime_files(StartupDependencyReport& report, const fs::path& exe_dir) {
#if defined(__APPLE__)
  if (looks_like_macos_app(exe_dir)) {
    const fs::path molten = exe_dir / "lib" / "libMoltenVK.dylib";
    const fs::path icd = bundled_moltenvk_icd(exe_dir);
    if (!is_regular(molten)) {
      report.missing.push_back("libMoltenVK.dylib（.app 内 Contents/MacOS/lib/）");
    }
    if (icd.empty()) {
      report.missing.push_back(
          "MoltenVK_icd.json（.app 内 Contents/Resources/vulkan/icd.d/）");
    }
  } else {
    // Dev / zip layout: optional MoltenVK beside the binary; loader may use brew ICD.
    (void)exe_dir;
  }
#elif defined(_WIN32)
  // Packaged builds ship vulkan-1.dll next to the exe. System installs may omit it.
  const fs::path local_vk = exe_dir / "vulkan-1.dll";
  if (is_regular(local_vk)) {
    return;  // bundled loader present
  }
  if (::GetModuleHandleW(L"vulkan-1.dll") == nullptr) {
    // Not loaded yet (before glfwInit). Probe LoadLibrary without keeping it.
    HMODULE mod = ::LoadLibraryW(L"vulkan-1.dll");
    if (mod == nullptr) {
      report.missing.push_back(
          "vulkan-1.dll（请安装 Vulkan Runtime，或将 DLL 放到程序目录）");
    } else {
      ::FreeLibrary(mod);
    }
  }
#else
  // Linux: ICD comes from the GPU driver; only ensure the loader is findable.
  void* vk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
  if (vk == nullptr) {
    vk = dlopen("libvulkan.so.1", RTLD_LAZY);
  }
  if (vk == nullptr) {
    report.missing.push_back("libvulkan.so.1（请安装 Vulkan 驱动 / mesa-vulkan-drivers）");
  } else {
    dlclose(vk);
  }
  (void)exe_dir;
#endif
}

}  // namespace

std::string StartupDependencyReport::format_message() const {
  std::string body = "启动失败：缺少必要的外部依赖或资源。\n\n";
  for (const auto& line : missing) {
    body += "• ";
    body += line;
    body += '\n';
  }
  body += "\n请重新安装完整程序包，或检查显卡驱动 / Vulkan 运行时。";
  return body;
}

void prepare_macos_vulkan_environment(const char* argv0) {
#if defined(__APPLE__)
  const fs::path exe_dir = exe_dir_from_argv0(argv0);
  if (!looks_like_macos_app(exe_dir)) {
    return;
  }
  const fs::path icd = bundled_moltenvk_icd(exe_dir);
  if (icd.empty()) {
    return;
  }
  // Always prefer the bundled ICD inside a shipped .app. Stale host
  // VK_ICD_FILENAMES values otherwise override Resources/vulkan/icd.d discovery
  // and vkCreateInstance fails with VK_ERROR_INCOMPATIBLE_DRIVER (-9).
  const std::string icd_utf8 = icd.string();
  ::setenv("VK_ICD_FILENAMES", icd_utf8.c_str(), 1);
  ::setenv("VK_DRIVER_FILES", icd_utf8.c_str(), 1);
#else
  (void)argv0;
#endif
}

StartupDependencyReport check_startup_dependencies(const char* argv0) {
  StartupDependencyReport report;
  const fs::path exe_dir = exe_dir_from_argv0(argv0);

  check_bass_runtime(report, exe_dir);
  check_vulkan_runtime_files(report, exe_dir);

  require_resource_dir(report, resolve_skins_dir(argv0), "skins 皮肤资源", looks_like_skins_dir);
  require_resource_dir(report, resolve_effects_dir(argv0), "effects 音效资源",
                       looks_like_effects_dir);

  const std::string icons = resolve_icons_dir(argv0);
  if (icons.empty() || !is_dir(icons) ||
      !(is_regular(fs::path(icons) / "open.svg") || is_regular(fs::path(icons) / "open.png"))) {
    report.missing.push_back("icons 工具栏图标（open.svg / open.png）");
  }

  if (resolve_ui_font_path(argv0).empty()) {
    report.missing.push_back("UI 字体（fonts/NotoSansSC-Regular.ttf）");
  }

  return report;
}

void add_vulkan_unavailable(StartupDependencyReport& report) {
  report.missing.push_back(
      "Vulkan 不可用（glfwVulkanSupported=false：请检查 GPU 驱动 / MoltenVK / ICD）");
}

int fail_startup_dependencies(const StartupDependencyReport& report) {
  const std::string title = "WDS Editor — 依赖缺失";
  const std::string message = report.format_message();
  std::fprintf(stderr, "%s\n%s\n", title.c_str(), message.c_str());
  native_file_dialog::alert_error(title, message);
  return 1;
}

}  // namespace wds::ui

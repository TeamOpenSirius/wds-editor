#pragma once

#include <string>
#include <vector>

namespace wds::ui {

// Lightweight startup checks for bundled runtimes + required assets.
// Safe to call before the Qt application creates its Vulkan surface.
struct StartupDependencyReport {
  std::vector<std::string> missing;

  bool ok() const noexcept { return missing.empty(); }
  std::string format_message() const;
};

// On macOS .app bundles: point VK_ICD_FILENAMES at the bundled MoltenVK ICD
// before the Qt Vulkan instance is created. Overrides stale host SDK paths that would otherwise win.
void prepare_macos_vulkan_environment(const char* argv0);

// Probe companion libs (BASS / MoltenVK / vulkan-1) and skins/fonts/icons/effects.
StartupDependencyReport check_startup_dependencies(const char* argv0);

// Append a Vulkan availability failure after QVulkanInstance::create() fails.
void add_vulkan_unavailable(StartupDependencyReport& report);

// Show a blocking native error dialog and return a process exit code.
int fail_startup_dependencies(const StartupDependencyReport& report);

}  // namespace wds::ui

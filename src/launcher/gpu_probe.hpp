#pragma once

#include <string>

namespace bb::launcher {

// Opens the Vulkan loader at run time and asks it for physical devices. False when the loader is
// missing, an instance can't be made or no device is listed.
bool vulkan_gpu_usable();
// One line per device: name, Vulkan version, driver name and info. For the problem report.
std::string vulkan_gpu_summary();

}  // namespace bb::launcher

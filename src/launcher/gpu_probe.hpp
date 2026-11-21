#pragma once

#include <string>
#include <vector>

namespace bb::launcher {

// Opens the Vulkan loader at run time and asks it for physical devices. False when the loader is
// missing, an instance can't be made or no device is listed.
bool vulkan_gpu_usable();

// The names of the Vulkan devices the game can run on, in the loader's order: every device but
// the CPU ones (llvmpipe), which run out of memory with the game. Empty when there is none or no
// loader.
std::vector<std::string> vulkan_gpu_names();

}  // namespace bb::launcher

#pragma once

namespace bb::launcher {

// Opens the Vulkan loader at run time and asks it for physical devices. False when the loader is
// missing, an instance can't be made or no device is listed.
bool vulkan_gpu_usable();

}  // namespace bb::launcher

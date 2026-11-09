// SPDX-License-Identifier: GPL-2.0-or-later
// The Vulkan device the bbvk tests use: lavapipe unless BB_TEST_VK_DEVICE names another (a part
// of its deviceName, as Device::create takes it; for example "radv"). The loader is limited to
// lavapipe's driver only when the variable is unset; with it set, VK_LOADER_DRIVERS_SELECT is
// the caller's.
#pragma once

#include <cstdlib>
#include <string>

namespace bb::test {

inline std::string vk_test_device() {
    const char* name = std::getenv("BB_TEST_VK_DEVICE");
    if (name == nullptr || *name == '\0') {
        ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
        return "llvmpipe";
    }
    return name;
}

}  // namespace bb::test

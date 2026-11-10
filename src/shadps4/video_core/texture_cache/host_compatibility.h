// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/texture_cache/host_compatibility.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2015-2023 The Khronos Group Inc.
// Copyright © 2015-2023 Valve Corporation
// Copyright © 2015-2023 LunarG, Inc.

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

/// Returns true if the two formats are compatible according to Vulkan's format compatibility rules
bool IsVulkanFormatCompatible(vk::Format base, vk::Format view);

} // namespace VideoCore

// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/amdgpu/regs_texture.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <bit>
#include "common/types.h"

namespace AmdGpu {

struct BorderColorBuffer {
    u64 base_addr : 40;

    template <typename T = VAddr>
    const T Address() const {
        return std::bit_cast<T>(base_addr << 8);
    }
};

} // namespace AmdGpu

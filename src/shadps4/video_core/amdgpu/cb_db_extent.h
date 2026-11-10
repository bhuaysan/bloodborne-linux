// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/amdgpu/cb_db_extent.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "common/types.h"

namespace AmdGpu {

union CbDbExtent {
    struct {
        u16 width;
        u16 height;
    };
    u32 raw;

    bool Valid() const {
        return raw != 0;
    }
};

} // namespace AmdGpu

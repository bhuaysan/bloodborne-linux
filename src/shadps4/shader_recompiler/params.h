// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/params.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <span>
#include "common/types.h"

namespace Shader {

/**
 * Compilation parameters used to identify and locate a guest shader program.
 */
struct ShaderParams {
    static constexpr u32 NumShaderUserData = 16;

    std::span<const u32, NumShaderUserData> user_data;
    std::span<const u32> code;
    u64 hash;

    VAddr Base() const noexcept {
        return reinterpret_cast<VAddr>(code.data());
    }
};

} // namespace Shader

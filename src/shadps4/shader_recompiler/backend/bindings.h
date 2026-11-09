// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/backend/bindings.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "common/types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 buffer{};

    auto operator<=>(const Bindings&) const = default;
};

} // namespace Shader::Backend

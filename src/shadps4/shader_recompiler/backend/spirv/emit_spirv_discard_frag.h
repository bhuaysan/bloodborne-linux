// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/backend/spirv/emit_spirv_discard_frag.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <vector>
#include "common/types.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

[[nodiscard]] std::vector<u32> EmitDiscardFragmentShader(std::array<Shader::OutputMap, 3> outputs);

} // namespace Shader::Backend::SPIRV

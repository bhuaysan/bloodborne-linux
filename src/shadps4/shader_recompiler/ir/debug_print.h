// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/ir/debug_print.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#include "common/bit_field.h"
#include "shader_recompiler/ir/opcodes.h"
#include "common/types.h"

#pragma once

namespace Shader::IR {

constexpr size_t DEBUGPRINT_NUM_FORMAT_ARGS = NumArgsOf(IR::Opcode::DebugPrint) - 1;

union DebugPrintFlags {
    u32 raw;
    // For now, only flag is the number of variadic format args actually used
    // So bitfield not really needed
    BitField<0, 32, u32> num_args;
};

} // namespace Shader::IR
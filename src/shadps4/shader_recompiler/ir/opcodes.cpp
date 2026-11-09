// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/ir/opcodes.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#include "shader_recompiler/ir/opcodes.h"

namespace Shader::IR {

std::string_view NameOf(Opcode op) {
    return Detail::META_TABLE[static_cast<size_t>(op)].name;
}

} // namespace Shader::IR

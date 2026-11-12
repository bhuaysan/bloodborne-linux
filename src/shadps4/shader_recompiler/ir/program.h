// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/ir/program.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "shader_recompiler/frontend/instruction.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/abstract_syntax_list.h"
#include "shader_recompiler/ir/basic_block.h"

namespace Shader::IR {

struct Program {
    explicit Program(Info& info_) : info{info_} {}

    AbstractSyntaxList syntax_list;
    BlockList blocks;
    BlockList post_order_blocks;
    std::vector<Gcn::GcnInst> ins_list;
    u32 next_reg_index{};
    Info& info;
};

} // namespace Shader::IR

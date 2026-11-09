// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/ir/dominance.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "shader_recompiler/ir/basic_block.h"

namespace Shader::IR {

void ComputeDominators(const IR::BlockList& post_order);

void ComputeDominanceFrontiers(const IR::BlockList& post_order);

inline bool Dominates(IR::Block* a, IR::Block* b) {
    IR::Block* current = b;
    while (current) {
        if (current == a) {
            return true;
        }
        if (current->immediate_dominator == current) {
            break;
        }
        current = current->immediate_dominator;
    }
    return false;
}

} // namespace Shader::IR
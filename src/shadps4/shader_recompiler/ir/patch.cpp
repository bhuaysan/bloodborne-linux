// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/ir/patch.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#include "shader_recompiler/ir/patch.h"

namespace Shader::IR {

std::string NameOf(Patch patch) {
    switch (patch) {
    case Patch::TessellationLodLeft:
        return "TessellationLodLeft";
    case Patch::TessellationLodTop:
        return "TessellationLodTop";
    case Patch::TessellationLodRight:
        return "TessellationLodRight";
    case Patch::TessellationLodBottom:
        return "TessellationLodBottom";
    case Patch::TessellationLodInteriorU:
        return "TessellationLodInteriorU";
    case Patch::TessellationLodInteriorV:
        return "TessellationLodInteriorV";
    default:
        const u32 index = u32(patch) - u32(Patch::Component0);
        return fmt::format("Component{}", index);
    }
}

} // namespace Shader::IR

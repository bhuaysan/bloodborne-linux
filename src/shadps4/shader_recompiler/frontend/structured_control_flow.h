// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/shader_recompiler/frontend/structured_control_flow.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/ir/abstract_syntax_list.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/value.h"

namespace Shader {
struct Info;
struct Profile;
struct RuntimeInfo;
struct Pools;
} // namespace Shader

namespace Shader::Gcn {

[[nodiscard]] IR::AbstractSyntaxList BuildASL(Pools& pools, CFG& cfg, Info& info);

} // namespace Shader::Gcn

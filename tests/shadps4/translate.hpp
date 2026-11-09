// Runs the translator derived from shadPS4 from a program's code to SPIR-V, for the tests.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "shader_recompiler/params.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace bb::test::shadps4 {

// The fragment stage's runtime information with one RGBA8 color target that takes uncompressed exports.
Shader::RuntimeInfo fragment_runtime_info();

// The fragment stage's runtime information from the register block of a program's entry header
// (docs/formats/shader-binary.md): the export formats of all targets and the pixel inputs.
Shader::RuntimeInfo fragment_runtime_info(const std::array<std::uint32_t, 16>& block);

// The user data registers a program starts with.
using UserData = std::array<std::uint32_t, Shader::ShaderParams::NumShaderUserData>;

// TranslateProgram and EmitSPIRV with the device profile of the reference GPU. All user data is 0
// unless given. Throws bb::shader::TranslationFailure when an assertion fails.
std::vector<std::uint32_t> translate(std::span<const std::uint32_t> code,
                                     Shader::RuntimeInfo runtime_info, Shader::Pools& pools,
                                     const UserData& user_data = {});

// The same with the given user data and pools of its own.
std::vector<std::uint32_t> translate(std::span<const std::uint32_t> code,
                                     Shader::RuntimeInfo runtime_info, const UserData& user_data);

// The same with pools of its own.
std::vector<std::uint32_t> translate(std::span<const std::uint32_t> code,
                                     Shader::RuntimeInfo runtime_info);

}  // namespace bb::test::shadps4

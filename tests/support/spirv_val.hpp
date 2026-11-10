// Validates SPIR-V modules with spirv-val from SPIRV-Tools, for the tests that build them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bb::test::gcn {

// Whether CMake found spirv-val. Tests that validate skip without it.
bool have_spirv_val();

// What `spirv-val --target-env vulkan1.3` says about the module: empty when it accepts it,
// otherwise its messages (or the exit status when it printed none).
std::string validate(const std::vector<std::uint32_t>& module);

}  // namespace bb::test::gcn

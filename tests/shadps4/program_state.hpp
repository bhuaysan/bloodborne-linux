// The GPU state a program of the shader corpus is translated with, built without guest memory
// (docs/design/shader-corpus.md).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "support/corpus.hpp"
#include "shader_recompiler/runtime_info.h"
#include "shadps4/translate.hpp"

namespace bb::test::shadps4 {

// The stage's runtime information: registers from the program's register block when it has one,
// plausible fixed values for the state that only exists at draw time. `copy_shader` is the copy
// shader of a geometry program.
Shader::RuntimeInfo runtime_info_of(const gcn::Program& program,
                                    std::span<const std::uint32_t> copy_shader);

// How a tessellation program reaches its V# of tessellation constants. The files don't say, and
// no one layout of the user data serves both: a program that reads the wrong one faults.
enum class TessConstantsForm { inline_v_sharp, through_pointer };

// User data whose pointers lead to what the translator dereferences while translating: the empty
// fetch shader for a vertex stage, the tessellation constants for a hull or domain shader, where
// every register holds that address. In all other cases the registers are 0.
UserData user_data_of(const gcn::Program& program, TessConstantsForm form);

// Starts the line a corpus program prints when it fails with a reason.
inline constexpr std::string_view failed_marker = "RESULT failed ";

// Ends the process after printing the failed-program line the corpus test reads.
[[noreturn]] void report_failure(std::string_view message);

// Installs a SIGSEGV handler that passes faults to the translator's SRT walker handler. In the
// translator the walker's loads through bad pointers read as 0 instead (corpus programs only).
void install_walker_fault_handler();

}  // namespace bb::test::shadps4

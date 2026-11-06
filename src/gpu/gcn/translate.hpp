// GCN to SPIR-V translation (docs/design/graphics.md, "Execution model" and "Interface").
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "gpu/gcn/descriptors.hpp"

namespace bb::gcn {

enum class Stage { vertex, pixel, compute };

// The draw block that the push constant points to (docs/design/graphics.md, "Per-draw data"):
// user_data[16], then the 64-bit address_bias and code_address.
inline constexpr std::uint32_t draw_user_data_offset = 0;
inline constexpr std::uint32_t draw_address_bias_offset = 64;
inline constexpr std::uint32_t draw_code_address_offset = 72;
inline constexpr std::uint32_t draw_block_size = 80;

// Everything the generated SPIR-V depends on besides the code (docs/design/graphics.md, "Stage
// interfaces and ShaderKey"). So far the compute stage's register values.
struct ShaderKey {
    Stage stage = Stage::compute;
    // COMPUTE_PGM_RSRC1. Only the float mode matters: FLOAT_MODE (bits 12-19: the rounding mode
    // and denormal handling of f32 in bits 12-13 and 16-17), DX10_CLAMP (bit 21) and IEEE_MODE
    // (bit 23). A shader that uses floats gets the execution modes of the f32 fields (round to
    // nearest even or to zero, flush or keep denormals); the rounding modes toward the
    // infinities and the half-flushing denormal modes are rejected. The rest (register counts,
    // priority, debug bits) is ignored.
    std::uint32_t pgm_rsrc1 = 0;
    // COMPUTE_PGM_RSRC2. USER_SGPR (at most 16) gives the user data in s0 and up; v0 holds the
    // thread ID in X. The fields that add initial registers (SCRATCH_EN, TGID_*_EN, TG_SIZE_EN,
    // TIDIG_COMP_CNT) are rejected so far. The others (TRAP_PRESENT, EXCP_EN_MSB, LDS_SIZE,
    // EXCP_EN) don't change the initial registers and are ignored.
    std::uint32_t pgm_rsrc2 = 0;
    // NUM_THREAD_FULL of COMPUTE_NUM_THREAD_X, _Y and _Z: the workgroup size.
    std::array<std::uint32_t, 3> num_threads = {64, 1, 1};
};

// What the run time must know about a translated shader.
struct ShaderInterface {
    // The sources of the buffer descriptors the program uses (see ResourceSource).
    std::vector<ResourceSource> buffers;
    bool wave_size_dependent = false;  // the result can differ in lane-local mode
    // VCC or an SGPR mask pair is written as a value and read back as a value (flow-insensitive).
    // The value survives only in 64-lane subgroups: smaller ones lose the other lanes' bits.
    bool mask_round_trip = false;
};

struct Translation {
    std::vector<std::uint32_t> spirv;
    ShaderInterface interface;
};

// Translates the program at the start of `code` into a SPIR-V 1.6 module with the entry point
// "main". `fetch` is the vertex stages' fetch subroutine (unused in compute). Throws GcnError for
// an instruction or key that isn't supported, naming it.
Translation translate(std::span<const std::uint32_t> code, std::span<const std::uint32_t> fetch,
                      const ShaderKey& key);

}  // namespace bb::gcn

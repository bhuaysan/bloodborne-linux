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

// address_bias holds the device address of the bias table: bias_entries u64 values, and device
// address = guest address + bias[(guest address >> bias_shift) & (bias_entries - 1)], one entry
// per 2 GiB buffer of the GPU memory mirror (docs/design/graphics.md, "GPU memory mirror").
inline constexpr std::uint32_t bias_entries = 16;
inline constexpr std::uint32_t bias_shift = 31;
inline constexpr std::uint32_t bias_table_size = 8 * bias_entries;

// Where a stage reads its draw block's address in the push constants (ShaderKey::push_offset):
// the VS and the PS of a pipeline each read their own block from one 16-byte range.
inline constexpr std::uint32_t vertex_push_offset = 0;
inline constexpr std::uint32_t pixel_push_offset = 8;
inline constexpr std::uint32_t graphics_push_size = 16;

// The image type class of a T# (docs/design/graphics.md, "Image sampling"): what the SPIR-V
// image type of its binding depends on. The run time reads it from each T# at draw time with
// image_class() and translates again when it differs from the key.
struct ImageClass {
    enum class Number : std::uint8_t { float_, sint, uint };
    // The T#'s type field (dword 3 bits 28-31): 9 2D, 11 cube, 13 2D array; the translator
    // rejects the others so far (1D, 3D and MSAA).
    std::uint8_t type = 9;
    Number number = Number::float_;  // the sampled type, from the number format

    bool operator==(const ImageClass&) const = default;
};

// The class of a T# from its first four dwords (the type in dword 3, the number format in bits
// 26-29 of dword 1: UINT and SINT sample integers, every other format floats).
ImageClass image_class(std::span<const std::uint32_t, 4> tsharp);

// Everything the generated SPIR-V depends on besides the code (docs/design/graphics.md, "Stage
// interfaces and ShaderKey"): the stage's register values, raw, as the PM4 side copies them.
struct ShaderKey {
    Stage stage = Stage::compute;
    // COMPUTE_PGM_RSRC1 or SPI_SHADER_PGM_RSRC1_VS/_PS, which share the float mode bits. For every
    // stage only the float mode matters: FLOAT_MODE (bits 12-19: the rounding mode and denormal
    // handling of f32 in bits 12-13 and 16-17), DX10_CLAMP (bit 21) and IEEE_MODE (bit 23). A
    // shader that uses floats gets the execution modes of the f32 fields (round to nearest even
    // or to zero, flush or keep denormals); the rounding modes toward the infinities and the
    // half-flushing denormal modes are rejected. The VS also reads VGPR_COMP_CNT (bits 24-25):
    // the VGPRs after the vertex index in v0 (1: v1, the instance index divided by step rate 0;
    // 2: also v2, divided by step rate 1; 3: also v3, the instance index). The rest (register
    // counts, priority, debug bits) is ignored.
    std::uint32_t pgm_rsrc1 = 0;
    // COMPUTE_PGM_RSRC2 or SPI_SHADER_PGM_RSRC2_VS. USER_SGPR (bits 1-5, at most 16) gives the
    // user data in s0 and up, and compute TGID_X_EN (bit 7) the workgroup's X in the next SGPR.
    // The other fields that add initial registers are rejected so far: compute SCRATCH_EN,
    // TGID_Y_EN, TGID_Z_EN, TG_SIZE_EN and TIDIG_COMP_CNT (v0 holds the thread ID in X); VS
    // SCRATCH_EN, OC_LDS_EN and the stream-out fields SO_BASE0_EN to SO_EN. Compute LDS_SIZE sizes
    // the LDS (a DS instruction with none is an error). The others don't change the initial
    // registers and are ignored.
    // SPI_SHADER_PGM_RSRC2_PS has USER_SGPR in the same bits, and its SCRATCH_EN is rejected too.
    std::uint32_t pgm_rsrc2 = 0;
    // NUM_THREAD_FULL of COMPUTE_NUM_THREAD_X, _Y and _Z: the workgroup size.
    std::array<std::uint32_t, 3> num_threads = {64, 1, 1};

    // VGT_INSTANCE_STEP_RATE_0 and _1 (VS). A rate of 0 gives 0.
    std::array<std::uint32_t, 2> instance_step_rate = {1, 1};

    // PS (docs/design/graphics.md, "Pixel key"): SPI_PS_INPUT_ADDR places the input VGPRs and
    // SPI_PS_INPUT_ENA says which are loaded. SPI_PS_INPUT_CNTL_<n> gives attribute n its location,
    // a default value or flat shading. Without SPI_BARYC_CNTL, POS_*_FLOAT are the pixel center
    // ("Pixel: not in the key yet").
    std::uint32_t ps_input_ena = 0;
    std::uint32_t ps_input_addr = 0;
    std::array<std::uint32_t, 32> ps_input_cntl = {};
    std::uint32_t col_format = 0;  // SPI_SHADER_COL_FORMAT, 4 bits per render target
    // mrtz (docs/design/graphics.md, "Depth"): SPI_SHADER_Z_FORMAT (bits 0-3, the values of
    // SPI_SHADER_COL_FORMAT) and DB_SHADER_CONTROL, whose Z_EXPORT_ENABLE (bit 0) makes the
    // export's first channel FragDepth. Its stencil, sample mask and alpha to mask export enables
    // (bits 1-2, 8 and 7) are rejected at an mrtz export whose format carries their channel; its
    // other fields are ignored.
    std::uint32_t z_format = 0;
    std::uint32_t db_shader_control = 0;
    // The device has VK_KHR_shader_quad_control with shaderQuadControl enabled: pixel shaders
    // declare QuadDerivativesKHR and RequireFullQuadsKHR (docs/design/graphics.md, "Quad control").
    bool quad_control = false;

    // The class of each image, by its position in ShaderInterface::images. An image without an
    // entry is a 2D float image.
    std::vector<ImageClass> images{};

    // The byte offset of the draw block's address in the push constants, a multiple of 8 below
    // 128: vertex_push_offset or pixel_push_offset in a graphics pipeline, 0 in compute.
    std::uint32_t push_offset = 0;

    bool operator==(const ShaderKey&) const = default;
};

// Where a stage's images and samplers are bound: one descriptor set per hardware stage, 0 for VS
// and CS, 1 for PS (ES/LS 2 and GS/HS 3 later; docs/design/graphics.md, "Image sampling"). Image i
// of ShaderInterface::images is binding i (a sampled image, arrayed for a cube), sampler j of
// ShaderInterface::samplers is binding images.size() + j.
constexpr std::uint32_t descriptor_set(Stage stage) { return stage == Stage::pixel ? 1 : 0; }

// What the run time must know about a translated shader.
struct ShaderInterface {
    // The sources of the buffer descriptors the program uses (see ResourceSource).
    std::vector<ResourceSource> buffers;
    // The T# and S# sources of the MIMG instructions, in order of first use (bindings above).
    std::vector<ResourceSource> images{}, samplers{};
    bool wave_size_dependent = false;  // the result can differ in lane-local mode (also: LDS)
    // VCC or an SGPR mask pair is written as a value and read back as a value (flow-insensitive).
    // The value survives only in 64-lane subgroups: smaller ones lose the other lanes' bits.
    bool mask_round_trip = false;
    // The vertex stage's program calls the fetch subroutine (s_swappc_b64), which was inlined.
    bool calls_fetch = false;
};

struct Translation {
    std::vector<std::uint32_t> spirv;
    ShaderInterface interface;
};

// Translates the program at the start of `code` into a SPIR-V 1.6 module with the entry point
// "main". `fetch` is the code of the vertex stage's fetch subroutine, from the address in user-data
// dwords 0-1, which may extend past its s_setpc_b64; it is inlined at the program's s_swappc_b64
// and ignored in the other stages and in a program without the call. Throws GcnError for an
// instruction or key that isn't supported, naming it.
Translation translate(std::span<const std::uint32_t> code, std::span<const std::uint32_t> fetch,
                      const ShaderKey& key);

}  // namespace bb::gcn

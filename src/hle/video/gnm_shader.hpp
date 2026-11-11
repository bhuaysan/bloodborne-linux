// The stage register imports of libSceGnmDriver (docs/design/graphics.md, "Stage register
// imports"; docs/formats/gnm.md, "Shader objects and the stage writers"). Each one writes the
// registers of a shader stage from the register block of a shader object as SET_SH_REG and
// SET_CONTEXT_REG packets at `cmd`, then fills the rest of its fixed size with one NOP whose
// payload is zero. The game's writers (0x10753c0..0x1075a70, 0x108a870) reserve the fixed size,
// pass it as `dwords` and advance the cursor by it only when the result is 0.
//
// A register block starts with the program address shifted right by 8, low dword then high
// dword (`SPI_SHADER_PGM_LO/HI`), then `RSRC1` and `RSRC2`. The high dword must be 0. The other
// dwords are listed with each import.
//
// An import fails with gnm_shader_error and writes nothing when `cmd` or `regs` is null (except
// for the PS imports), when `dwords` is below its fixed size, when the high address dword is not
// 0 or when the modifier has a bit outside rsrc1_modifier_bits.
//
// The "unchanged" variants (sceGnmUpdate*Shader) are the same functions: the VS and PS binders
// (0x108fbb0, 0x108fdd0) call them only when the context register dwords of the new block equal
// those of the bound one, so writing those registers again sets the same state.
//
#pragma once

#include <cstdint>

namespace bb::hle::video {

inline constexpr std::int32_t gnm_shader_error = -1;

// The RSRC1 bits a modifier may set: SGPRS (bits 6-9) and VGPR_COMP_CNT (bits 24-25).
inline constexpr std::uint32_t rsrc1_modifier_bits = ~0xfcfffc3fu;

inline constexpr std::uint32_t vs_shader_dwords = 29;
inline constexpr std::uint32_t ps_shader_dwords = 40;
inline constexpr std::uint32_t cs_shader_dwords = 25;
inline constexpr std::uint32_t es_shader_dwords = 20;
inline constexpr std::uint32_t gs_shader_dwords = 29;
inline constexpr std::uint32_t hs_shader_dwords = 30;
inline constexpr std::uint32_t ls_shader_dwords = 23;

// NIDs gAhCn6UiU4Y and V31V01UiScY. regs: PGM_LO, PGM_HI, RSRC1, RSRC2, SPI_VS_OUT_CONFIG,
// SPI_SHADER_POS_FORMAT, PA_CL_VS_OUT_CNTL. A non-zero modifier replaces the rsrc1_modifier_bits
// of RSRC1.
std::int32_t sceGnmSetVsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t modifier);

// NIDs bQVd5YzCal0 and 4MgRw-bVNQU. regs: PGM_LO, PGM_HI, RSRC1, RSRC2, SPI_SHADER_Z_FORMAT,
// SPI_SHADER_COL_FORMAT, SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, SPI_PS_IN_CONTROL, SPI_BARYC_CNTL,
// DB_SHADER_CONTROL, CB_SHADER_MASK. A null `regs` unbinds the pixel shader: the program address
// and DB_SHADER_CONTROL become 0.
std::int32_t sceGnmSetPsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs);

// NID KXltnCwEJHQ. regs: COMPUTE_PGM_LO, COMPUTE_PGM_HI, RSRC1, RSRC2, COMPUTE_NUM_THREAD_X, Y,
// Z. Called for the graphics ring (0x1090020) and for compute command buffers (0x1090ec0).
std::int32_t sceGnmSetCsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs);

// NID FUHG8sQ3R58. regs: PGM_LO, PGM_HI, RSRC1, RSRC2. The modifier works as for the VS.
std::int32_t sceGnmSetEsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t modifier);

// NIDs UJwNuMBcUAk and nLM2i2+65hA. regs: PGM_LO, PGM_HI, RSRC1, RSRC2, VGT_STRMOUT_CONFIG,
// VGT_GS_OUT_PRIM_TYPE, VGT_GS_INSTANCE_CNT.
std::int32_t sceGnmSetGsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs);

// NIDs VJNjFtqiF5w and GNlx+y7xPdE. regs: PGM_LO, PGM_HI, RSRC1, RSRC2, VGT_TF_PARAM,
// VGT_HOS_MAX_TESS_LEVEL, VGT_HOS_MIN_TESS_LEVEL. `ls_hs_config` is the value of VGT_LS_HS_CONFIG.
std::int32_t sceGnmSetHsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t ls_hs_config);

// NID vckdzbQ46SI. regs: PGM_LO, PGM_HI, RSRC1, RSRC2. A modifier that sets only SGPRS replaces
// that field of RSRC1; one that sets VGPR_COMP_CNT replaces both. The packets write RSRC2 once
// (shadPS4 writes it twice, the second time together with RSRC1).
std::int32_t sceGnmSetLsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t modifier);

}  // namespace bb::hle::video

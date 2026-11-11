// The stage register imports of libSceGnmDriver (gnm_shader.hpp). The register block layouts and
// the modifier masks follow shadPS4's sceGnmSet*Shader (docs/references.md); the packets are our
// own and were checked against the game's shader files (tests/hle/gnm_shader_test.cpp).
#include "hle/video/gnm_shader.hpp"

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "si_ci_vi_merged_offset.h"
#include "si_ci_vi_merged_pm4_it_opcodes.h"

namespace bb::hle::video {
namespace {

using namespace Pal::Gfx6::Chip;

constexpr std::string_view channel = "hle.libSceGnmDriver";

constexpr std::uint32_t sh_base = 0x2c00;
constexpr std::uint32_t context_base = 0xa000;

// Writes type-3 packets from `begin` up to a fixed size.
class Packets {
public:
    explicit Packets(std::uint32_t* begin) : cursor_(begin), begin_(begin) {}

    void sh(std::uint32_t reg, std::initializer_list<std::uint32_t> values) {
        put(IT_SET_SH_REG, reg - sh_base, values);
    }
    void context(std::uint32_t reg, std::initializer_list<std::uint32_t> values) {
        put(IT_SET_CONTEXT_REG, reg - context_base, values);
    }

    // The program address (its high dword is 0) and RSRC1/RSRC2, which follow PGM_LO by two in
    // the graphics stages.
    void program(std::uint32_t pgm_lo, std::uint32_t rsrc1_reg, const std::uint32_t* regs,
                 std::uint32_t rsrc1) {
        sh(pgm_lo, {regs[0], 0});
        sh(rsrc1_reg, {rsrc1, regs[3]});
    }

    // Fills up to `size` dwords with one NOP and a zero payload. Every layout leaves at least 2.
    std::int32_t pad(std::uint32_t size) {
        const auto rest = static_cast<std::uint32_t>(begin_ + size - cursor_);
        *cursor_ = header(IT_NOP, rest - 1);
        std::fill_n(cursor_ + 1, rest - 1, 0u);
        return 0;
    }

private:
    static std::uint32_t header(std::uint32_t opcode, std::uint32_t body) {
        return 0xc0000000u | (body - 1) << 16 | opcode << 8;
    }

    void put(std::uint32_t opcode, std::uint32_t offset,
             std::initializer_list<std::uint32_t> values) {
        *cursor_++ = header(opcode, static_cast<std::uint32_t>(values.size()) + 1);
        *cursor_++ = offset;
        cursor_ = std::copy(values.begin(), values.end(), cursor_);
    }

    std::uint32_t* cursor_;
    std::uint32_t* begin_;
};

// The checks every import makes. Logs why it fails.
bool valid(std::string_view stage, const std::uint32_t* cmd, std::uint32_t dwords,
           std::uint32_t size, const std::uint32_t* regs, std::uint32_t modifier = 0) {
    std::string_view why;
    if (cmd == nullptr || dwords < size) {
        why = "no room";
    } else if (regs == nullptr) {
        why = "no register block";
    } else if (regs[1] != 0) {
        why = "program address above 40 bits";
    } else if ((modifier & ~rsrc1_modifier_bits) != 0) {
        why = "modifier outside SGPRS and VGPR_COMP_CNT";
    } else {
        return true;
    }
    common::log(common::LogLevel::warn, channel, "{} shader: {} ({} of {} dwords, modifier {:#x})",
                stage, why, dwords, size, modifier);
    return false;
}

// RSRC1 with the modifier's fields: those in `fields` replaced, unless the modifier is 0.
std::uint32_t modified(std::uint32_t rsrc1, std::uint32_t modifier, std::uint32_t fields) {
    return modifier == 0 ? rsrc1 : (rsrc1 & ~fields) | modifier;
}

}  // namespace

std::int32_t sceGnmSetVsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t modifier) {
    if (!valid("VS", cmd, dwords, vs_shader_dwords, regs, modifier)) {
        return gnm_shader_error;
    }
    Packets p(cmd);
    p.program(mmSPI_SHADER_PGM_LO_VS, mmSPI_SHADER_PGM_RSRC1_VS, regs,
              modified(regs[2], modifier, rsrc1_modifier_bits));
    p.context(mmSPI_VS_OUT_CONFIG, {regs[4]});
    p.context(mmSPI_SHADER_POS_FORMAT, {regs[5]});
    p.context(mmPA_CL_VS_OUT_CNTL, {regs[6]});
    return p.pad(vs_shader_dwords);
}

std::int32_t sceGnmSetPsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs) {
    static constexpr std::uint32_t unbound[2] = {0, 0};  // a null block passes the checks
    if (!valid("PS", cmd, dwords, ps_shader_dwords, regs == nullptr ? unbound : regs)) {
        return gnm_shader_error;
    }
    Packets p(cmd);
    if (regs == nullptr) {
        p.sh(mmSPI_SHADER_PGM_LO_PS, {0, 0});
        p.context(mmDB_SHADER_CONTROL, {0});
        return p.pad(ps_shader_dwords);
    }
    p.program(mmSPI_SHADER_PGM_LO_PS, mmSPI_SHADER_PGM_RSRC1_PS, regs, regs[2]);
    p.context(mmSPI_SHADER_Z_FORMAT, {regs[4], regs[5]});  // and SPI_SHADER_COL_FORMAT
    p.context(mmSPI_PS_INPUT_ENA, {regs[6], regs[7]});     // and SPI_PS_INPUT_ADDR
    p.context(mmSPI_PS_IN_CONTROL, {regs[8]});
    p.context(mmSPI_BARYC_CNTL, {regs[9]});
    p.context(mmDB_SHADER_CONTROL, {regs[10]});
    p.context(mmCB_SHADER_MASK, {regs[11]});
    return p.pad(ps_shader_dwords);
}

std::int32_t sceGnmSetCsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs) {
    if (!valid("CS", cmd, dwords, cs_shader_dwords, regs)) {
        return gnm_shader_error;
    }
    Packets p(cmd);
    p.program(mmCOMPUTE_PGM_LO, mmCOMPUTE_PGM_RSRC1, regs, regs[2]);
    p.sh(mmCOMPUTE_NUM_THREAD_X, {regs[4], regs[5], regs[6]});  // and Y, Z
    return p.pad(cs_shader_dwords);
}

std::int32_t sceGnmSetEsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t modifier) {
    if (!valid("ES", cmd, dwords, es_shader_dwords, regs, modifier)) {
        return gnm_shader_error;
    }
    Packets p(cmd);
    p.program(mmSPI_SHADER_PGM_LO_ES, mmSPI_SHADER_PGM_RSRC1_ES, regs,
              modified(regs[2], modifier, rsrc1_modifier_bits));
    return p.pad(es_shader_dwords);
}

std::int32_t sceGnmSetGsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs) {
    if (!valid("GS", cmd, dwords, gs_shader_dwords, regs)) {
        return gnm_shader_error;
    }
    Packets p(cmd);
    p.program(mmSPI_SHADER_PGM_LO_GS, mmSPI_SHADER_PGM_RSRC1_GS, regs, regs[2]);
    p.context(mmVGT_STRMOUT_CONFIG, {regs[4]});
    p.context(mmVGT_GS_OUT_PRIM_TYPE, {regs[5]});
    p.context(mmVGT_GS_INSTANCE_CNT, {regs[6]});
    return p.pad(gs_shader_dwords);
}

std::int32_t sceGnmSetHsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t ls_hs_config) {
    if (!valid("HS", cmd, dwords, hs_shader_dwords, regs)) {
        return gnm_shader_error;
    }
    Packets p(cmd);
    p.program(mmSPI_SHADER_PGM_LO_HS, mmSPI_SHADER_PGM_RSRC1_HS, regs, regs[2]);
    p.context(mmVGT_HOS_MAX_TESS_LEVEL, {regs[5], regs[6]});  // and VGT_HOS_MIN_TESS_LEVEL
    p.context(mmVGT_TF_PARAM, {regs[4]});
    p.context(mmVGT_LS_HS_CONFIG, {ls_hs_config});
    return p.pad(hs_shader_dwords);
}

std::int32_t sceGnmSetLsShader(std::uint32_t* cmd, std::uint32_t dwords,
                               const std::uint32_t* regs, std::uint32_t modifier) {
    // A modifier that sets only SGPRS replaces only that field; one that sets anything else
    // (VGPR_COMP_CNT) replaces both.
    const std::uint32_t fields =
        (modifier & 0xfffffc3fu) == 0 ? 0x3c0u : rsrc1_modifier_bits;
    if (!valid("LS", cmd, dwords, ls_shader_dwords, regs, modifier)) {
        return gnm_shader_error;
    }
    Packets p(cmd);
    p.program(mmSPI_SHADER_PGM_LO_LS, mmSPI_SHADER_PGM_RSRC1_LS, regs,
              modified(regs[2], modifier, fields));
    return p.pad(ls_shader_dwords);
}

BB_HLE_FUNCTION("libSceGnmDriver", "gAhCn6UiU4Y", sceGnmSetVsShader);
BB_HLE_FUNCTION("libSceGnmDriver", "V31V01UiScY", sceGnmSetVsShader);  // sceGnmUpdateVsShader
BB_HLE_FUNCTION("libSceGnmDriver", "bQVd5YzCal0", sceGnmSetPsShader);
BB_HLE_FUNCTION("libSceGnmDriver", "4MgRw-bVNQU", sceGnmSetPsShader);  // sceGnmUpdatePsShader
BB_HLE_FUNCTION("libSceGnmDriver", "KXltnCwEJHQ", sceGnmSetCsShader);
BB_HLE_FUNCTION("libSceGnmDriver", "FUHG8sQ3R58", sceGnmSetEsShader);
BB_HLE_FUNCTION("libSceGnmDriver", "UJwNuMBcUAk", sceGnmSetGsShader);
BB_HLE_FUNCTION("libSceGnmDriver", "nLM2i2+65hA", sceGnmSetGsShader);  // sceGnmUpdateGsShader
BB_HLE_FUNCTION("libSceGnmDriver", "VJNjFtqiF5w", sceGnmSetHsShader);
BB_HLE_FUNCTION("libSceGnmDriver", "GNlx+y7xPdE", sceGnmSetHsShader);  // sceGnmUpdateHsShader
BB_HLE_FUNCTION("libSceGnmDriver", "vckdzbQ46SI", sceGnmSetLsShader);

}  // namespace bb::hle::video

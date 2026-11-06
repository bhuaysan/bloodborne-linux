#include "gpu/pm4/registers.hpp"

#include <algorithm>
#include <array>

#include "si_ci_vi_merged_offset.h"

namespace bb::pm4 {

namespace {

namespace amd = Pal::Gfx6::Chip;

constexpr std::array spaces = {RegSpace::config, RegSpace::sh, RegSpace::context,
                               RegSpace::uconfig};

constexpr std::uint32_t total_dwords() {
    std::uint32_t total = 0;
    for (RegSpace space : spaces) {
        total += space_range(space).size;
    }
    return total;
}

struct StageRegs {
    std::uint32_t pgm_lo, pgm_hi, rsrc1, rsrc2, user_data;
};

constexpr StageRegs stage_regs(Stage stage) {
    switch (stage) {
    case Stage::ps:
        return {amd::mmSPI_SHADER_PGM_LO_PS, amd::mmSPI_SHADER_PGM_HI_PS,
                amd::mmSPI_SHADER_PGM_RSRC1_PS, amd::mmSPI_SHADER_PGM_RSRC2_PS,
                amd::mmSPI_SHADER_USER_DATA_PS_0};
    case Stage::vs:
        return {amd::mmSPI_SHADER_PGM_LO_VS, amd::mmSPI_SHADER_PGM_HI_VS,
                amd::mmSPI_SHADER_PGM_RSRC1_VS, amd::mmSPI_SHADER_PGM_RSRC2_VS,
                amd::mmSPI_SHADER_USER_DATA_VS_0};
    case Stage::gs:
        return {amd::mmSPI_SHADER_PGM_LO_GS, amd::mmSPI_SHADER_PGM_HI_GS,
                amd::mmSPI_SHADER_PGM_RSRC1_GS, amd::mmSPI_SHADER_PGM_RSRC2_GS,
                amd::mmSPI_SHADER_USER_DATA_GS_0};
    case Stage::es:
        return {amd::mmSPI_SHADER_PGM_LO_ES, amd::mmSPI_SHADER_PGM_HI_ES,
                amd::mmSPI_SHADER_PGM_RSRC1_ES, amd::mmSPI_SHADER_PGM_RSRC2_ES,
                amd::mmSPI_SHADER_USER_DATA_ES_0};
    case Stage::hs:
        return {amd::mmSPI_SHADER_PGM_LO_HS, amd::mmSPI_SHADER_PGM_HI_HS,
                amd::mmSPI_SHADER_PGM_RSRC1_HS, amd::mmSPI_SHADER_PGM_RSRC2_HS,
                amd::mmSPI_SHADER_USER_DATA_HS_0};
    case Stage::ls:
        return {amd::mmSPI_SHADER_PGM_LO_LS, amd::mmSPI_SHADER_PGM_HI_LS,
                amd::mmSPI_SHADER_PGM_RSRC1_LS, amd::mmSPI_SHADER_PGM_RSRC2_LS,
                amd::mmSPI_SHADER_USER_DATA_LS_0};
    case Stage::cs:
        break;
    }
    return {amd::mmCOMPUTE_PGM_LO, amd::mmCOMPUTE_PGM_HI, amd::mmCOMPUTE_PGM_RSRC1,
            amd::mmCOMPUTE_PGM_RSRC2, amd::mmCOMPUTE_USER_DATA_0};
}

constexpr std::uint32_t sh_base = space_range(RegSpace::sh).base;

}  // namespace

RegisterMirror::RegisterMirror()
    : regs_(total_dwords(), 0), context_defaults_(space_range(RegSpace::context).size, 0) {}

std::uint32_t RegisterMirror::index(RegSpace space) const {
    std::uint32_t index = 0;
    for (RegSpace s : spaces) {
        if (s == space) {
            break;
        }
        index += space_range(s).size;
    }
    return index;
}

std::size_t RegisterMirror::write(RegSpace space, std::uint32_t offset,
                                  std::span<const std::uint32_t> values) {
    const std::uint32_t size = space_range(space).size;
    if (offset >= size) {
        return 0;
    }
    const std::size_t count = std::min<std::size_t>(values.size(), size - offset);
    std::copy_n(values.begin(), count, regs_.begin() + index(space) + offset);
    return count;
}

std::uint32_t RegisterMirror::get(std::uint32_t reg) const {
    for (RegSpace space : spaces) {
        const SpaceRange range = space_range(space);
        if (reg >= range.base && reg - range.base < range.size) {
            return regs_[index(space) + reg - range.base];
        }
    }
    return 0;
}

std::span<const std::uint32_t> RegisterMirror::space(RegSpace space) const {
    return std::span(regs_).subspan(index(space), space_range(space).size);
}

void RegisterMirror::clear_context() {
    std::ranges::copy(context_defaults_, regs_.begin() + index(RegSpace::context));
}

void RegisterMirror::set_context_defaults(std::span<const std::uint32_t> defaults) {
    std::ranges::fill(context_defaults_, 0);
    std::copy_n(defaults.begin(), std::min(defaults.size(), context_defaults_.size()),
                context_defaults_.begin());
}

std::uint64_t RegisterMirror::program_address(Stage stage) const {
    const StageRegs regs = stage_regs(stage);
    // LO holds address bits 8-39, the low byte of HI bits 40-47.
    return std::uint64_t{get(regs.pgm_lo)} << 8 | std::uint64_t{get(regs.pgm_hi) & 0xFF} << 40;
}

std::uint32_t RegisterMirror::rsrc1(Stage stage) const { return get(stage_regs(stage).rsrc1); }

std::uint32_t RegisterMirror::rsrc2(Stage stage) const { return get(stage_regs(stage).rsrc2); }

std::span<const std::uint32_t, user_data_dwords> RegisterMirror::user_data(Stage stage) const {
    const std::uint32_t at = index(RegSpace::sh) + stage_regs(stage).user_data - sh_base;
    return std::span(regs_).subspan(at).first<user_data_dwords>();
}

std::uint32_t RegisterMirror::num_threads(unsigned dimension) const {
    const std::uint32_t regs[] = {amd::mmCOMPUTE_NUM_THREAD_X, amd::mmCOMPUTE_NUM_THREAD_Y,
                                  amd::mmCOMPUTE_NUM_THREAD_Z};
    return dimension < 3 ? get(regs[dimension]) & 0xFFFF : 0;
}

}  // namespace bb::pm4

#include "gpu/pm4/targets.hpp"

#include <bit>

#include "si_ci_vi_merged_offset.h"

namespace bb::pm4 {

namespace {

namespace amd = Pal::Gfx6::Chip;

constexpr std::uint32_t color_stride = amd::mmCB_COLOR1_BASE - amd::mmCB_COLOR0_BASE;
constexpr std::uint32_t hint_header = 0xC0001000;  // a type-3 NOP with one body dword

constexpr std::uint32_t field(std::uint32_t value, unsigned first, unsigned bits) {
    return value >> first & ((1u << bits) - 1);
}

}  // namespace

void ExtentHints::observe(const Packet& packet) {
    if (packet.opcode() == IT_SET_CONTEXT_REG && !packet.body.empty()) {
        const std::uint32_t reg = space_range(RegSpace::context).base + (packet.body[0] & 0xFFFF);
        const std::uint32_t slot = (reg - amd::mmCB_COLOR0_BASE) / color_stride;
        if (reg == amd::mmDB_Z_INFO) {
            pending_ = depth_slot;
        } else if (reg >= amd::mmCB_COLOR0_BASE && slot < color_targets &&
                   (reg - amd::mmCB_COLOR0_BASE) % color_stride == 0) {
            pending_ = slot;
        } else {
            return;  // other context registers keep the pending target
        }
        hints_[pending_] = 0;
        return;
    }
    if (pending_ != none && packet.header == hint_header && !nop_tag(packet)) {
        hints_[pending_] = packet.body[0];
    }
    pending_ = none;
}

Extent ExtentHints::color(unsigned slot) const {
    return slot < color_targets ? extent(hints_[slot]) : Extent{};
}

Extent ExtentHints::extent(std::uint32_t hint) {
    const Extent e{hint & 0xFFFF, hint >> 16};
    return e.width != 0 && e.height != 0 ? e : Extent{};
}

std::optional<ColorTarget> color_target(const RegisterMirror& regs, const ExtentHints& hints,
                                        unsigned slot) {
    if (slot >= color_targets) {
        return std::nullopt;
    }
    const auto reg = [&](std::uint32_t slot0) { return regs.get(slot0 + slot * color_stride); };
    const std::uint32_t info = reg(amd::mmCB_COLOR0_INFO);
    ColorTarget t;
    t.address = std::uint64_t{reg(amd::mmCB_COLOR0_BASE)} << 8;
    t.format = field(info, 2, 5);
    if (t.address == 0 || t.format == 0) {
        return std::nullopt;
    }
    t.pitch = (field(reg(amd::mmCB_COLOR0_PITCH), 0, 11) + 1) * 8;
    t.height = (field(reg(amd::mmCB_COLOR0_SLICE), 0, 22) + 1) * 64 / t.pitch;
    const Extent hint = hints.color(slot);
    t.extent = hint.width != 0 ? hint : Extent{t.pitch, t.height};
    const std::uint32_t view = reg(amd::mmCB_COLOR0_VIEW);
    t.base_slice = field(view, 0, 11);
    t.slices = field(view, 13, 11) + 1;
    t.number_type = field(info, 8, 3);
    t.comp_swap = field(info, 11, 2);
    t.linear_general = field(info, 7, 1) != 0;
    t.fast_clear = field(info, 13, 1) != 0;
    t.compression = field(info, 14, 1) != 0;
    const std::uint32_t attrib = reg(amd::mmCB_COLOR0_ATTRIB);
    t.tile_mode = field(attrib, 0, 5);
    t.samples = 1u << field(attrib, 15, 2);
    t.cmask_address = std::uint64_t{reg(amd::mmCB_COLOR0_CMASK)} << 8;
    t.clear_word = std::uint64_t{reg(amd::mmCB_COLOR0_CLEAR_WORD1)} << 32 |
                   reg(amd::mmCB_COLOR0_CLEAR_WORD0);
    return t;
}

std::optional<DepthTarget> depth_target(const RegisterMirror& regs, const ExtentHints& hints) {
    const std::uint32_t z_info = regs.get(amd::mmDB_Z_INFO);
    DepthTarget t;
    t.z_address = std::uint64_t{regs.get(amd::mmDB_Z_READ_BASE)} << 8;
    t.z_write_address = std::uint64_t{regs.get(amd::mmDB_Z_WRITE_BASE)} << 8;
    t.stencil_address = std::uint64_t{regs.get(amd::mmDB_STENCIL_READ_BASE)} << 8;
    t.stencil_write_address = std::uint64_t{regs.get(amd::mmDB_STENCIL_WRITE_BASE)} << 8;
    t.z_format = t.z_address != 0 ? field(z_info, 0, 2) : 0;
    t.stencil = t.stencil_address != 0 && field(regs.get(amd::mmDB_STENCIL_INFO), 0, 1) != 0;
    if (t.z_format == 0 && !t.stencil) {
        return std::nullopt;
    }
    const std::uint32_t size = regs.get(amd::mmDB_DEPTH_SIZE);
    t.pitch = (field(size, 0, 11) + 1) * 8;
    t.height = (field(size, 11, 11) + 1) * 8;
    const Extent hint = hints.depth();
    t.extent = hint.width != 0 ? hint : Extent{t.pitch, t.height};
    const std::uint32_t view = regs.get(amd::mmDB_DEPTH_VIEW);
    t.base_slice = field(view, 0, 11);
    t.slices = field(view, 13, 11) + 1;
    t.tile_mode = field(z_info, 20, 3);
    t.samples = 1u << field(z_info, 2, 2);
    if (field(z_info, 29, 1) != 0) {
        t.htile_address = std::uint64_t{regs.get(amd::mmDB_HTILE_DATA_BASE)} << 8;
    }
    const std::uint32_t control = regs.get(amd::mmDB_RENDER_CONTROL);
    t.clear_depth = field(control, 0, 1) != 0;
    t.clear_stencil = field(control, 1, 1) != 0;
    t.depth_clear = std::bit_cast<float>(regs.get(amd::mmDB_DEPTH_CLEAR));
    t.stencil_clear = static_cast<std::uint8_t>(field(regs.get(amd::mmDB_STENCIL_CLEAR), 0, 8));
    return t;
}

}  // namespace bb::pm4

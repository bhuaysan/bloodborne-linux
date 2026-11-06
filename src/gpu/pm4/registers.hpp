// The GPU register mirror: the four register spaces that PM4 packets write, as arrays indexed by
// dword offset, with typed accessors for the registers the backend reads (docs/design/graphics.md,
// "Register mirror"). Offsets are AMD's dword offsets (third_party/amd-gcn/si_ci_vi_merged_offset.h).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bb::pm4 {

// The register spaces of the SET_*_REG packets. A packet's register offset counts from the
// space's base.
enum class RegSpace : std::uint8_t { config, sh, context, uconfig };

struct SpaceRange {
    std::uint32_t base;  // first dword offset
    std::uint32_t size;  // in dwords
};

constexpr SpaceRange space_range(RegSpace space) {
    switch (space) {
    case RegSpace::config:
        return {0x2000, 0xC00};
    case RegSpace::sh:
        return {0x2C00, 0x400};
    case RegSpace::context:
        return {0xA000, 0x400};
    case RegSpace::uconfig:
        return {0xC000, 0x4000};
    }
    return {0, 0};
}

// The shader stages with an SH register block of their own.
enum class Stage : std::uint8_t { ps, vs, gs, es, hs, ls, cs };

inline constexpr std::uint32_t user_data_dwords = 16;

class RegisterMirror {
public:
    // Every register 0, and the context defaults too.
    RegisterMirror();

    // Writes `values` to the registers from `offset` (counted from the space's base). Values past
    // the end of the space are dropped. Returns how many were written.
    std::size_t write(RegSpace space, std::uint32_t offset, std::span<const std::uint32_t> values);

    // The register at an absolute dword offset; 0 outside the four spaces.
    std::uint32_t get(std::uint32_t reg) const;
    std::span<const std::uint32_t> space(RegSpace space) const;

    // CLEAR_STATE: every context register back to its default.
    void clear_context();
    // The values clear_context writes, one per context register (missing ones are 0). The default
    // hardware state comes with the GPU thread (item 18).
    void set_context_defaults(std::span<const std::uint32_t> defaults);

    // SPI_SHADER_PGM_{LO,HI}_<stage> or COMPUTE_PGM_{LO,HI}: the program's byte address.
    std::uint64_t program_address(Stage stage) const;
    // SPI_SHADER_PGM_RSRC{1,2}_<stage> or COMPUTE_PGM_RSRC{1,2}.
    std::uint32_t rsrc1(Stage stage) const;
    std::uint32_t rsrc2(Stage stage) const;
    // SPI_SHADER_USER_DATA_<stage>_0..15 or COMPUTE_USER_DATA_0..15.
    std::span<const std::uint32_t, user_data_dwords> user_data(Stage stage) const;
    // COMPUTE_NUM_THREAD_{X,Y,Z}: the full thread group size (NUM_THREAD_FULL, bits 0-15).
    std::uint32_t num_threads(unsigned dimension) const;

private:
    std::uint32_t index(RegSpace space) const;

    std::vector<std::uint32_t> regs_;  // config, sh, context, uconfig, one after the other
    std::vector<std::uint32_t> context_defaults_;
};

}  // namespace bb::pm4

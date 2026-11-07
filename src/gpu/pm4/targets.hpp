// Colour and depth targets as the CB_COLOR* and DB_* registers of the mirror describe them, and
// the extent hints the game writes after those registers (docs/design/graphics.md, "Render
// targets and depth"). Register offsets come from AMD's offset header; the field positions are
// listed with the source in docs/references.md.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "gpu/pm4/packet.hpp"
#include "gpu/pm4/registers.hpp"

namespace bb::pm4 {

inline constexpr unsigned color_targets = 8;

struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const Extent&) const = default;
};

// The game's GNM code ends each colour target setup (SET_CONTEXT_REG from CB_COLOR<n>_BASE) and
// each depth target setup (SET_CONTEXT_REG from DB_Z_INFO, then four single-register writes)
// with a one-dword NOP, header 0xc0001000, whose body holds the target's width (bits 0-15) and
// height (bits 16-31). The registers only give the size rounded up to whole tiles.
class ExtentHints {
public:
    // Every packet that ran, in stream order.
    void observe(const Packet& packet);

    // The hint for colour target `slot` or the depth target; 0 x 0 when the last setup of the
    // target had none.
    Extent color(unsigned slot) const;
    Extent depth() const { return extent(hints_[depth_slot]); }

private:
    static Extent extent(std::uint32_t hint);
    static constexpr unsigned depth_slot = color_targets;
    static constexpr unsigned none = color_targets + 1;
    std::array<std::uint32_t, color_targets + 1> hints_{};
    unsigned pending_ = none;  // the target whose setup is waiting for its hint
};

struct ColorTarget {
    std::uint64_t address = 0;  // CB_COLOR<n>_BASE << 8
    std::uint32_t pitch = 0;    // in pixels, (PITCH.TILE_MAX + 1) * 8
    std::uint32_t height = 0;   // in pixels, (SLICE.TILE_MAX + 1) * 64 / pitch
    Extent extent;              // the hint, or pitch x height without one
    std::uint32_t base_slice = 0;  // VIEW.SLICE_START
    std::uint32_t slices = 1;      // VIEW.SLICE_MAX + 1
    std::uint32_t format = 0;      // INFO.FORMAT (COLOR_*)
    std::uint32_t number_type = 0;  // INFO.NUMBER_TYPE (NUMBER_*; 6 is SRGB)
    std::uint32_t comp_swap = 0;    // INFO.COMP_SWAP
    bool linear_general = false;    // INFO.LINEAR_GENERAL
    bool fast_clear = false;        // INFO.FAST_CLEAR: CMASK can mark tiles cleared
    bool compression = false;       // INFO.COMPRESSION: FMASK in use
    std::uint32_t tile_mode = 0;    // ATTRIB.TILE_MODE_INDEX
    std::uint32_t samples = 1;      // 1 << ATTRIB.NUM_FRAGMENTS
    std::uint64_t cmask_address = 0;  // CB_COLOR<n>_CMASK << 8
    std::uint64_t clear_word = 0;     // CLEAR_WORD1:CLEAR_WORD0, one pixel as stored in memory
};

// Colour target `slot` (0-7), or nothing when its base address or format is 0.
std::optional<ColorTarget> color_target(const RegisterMirror& regs, const ExtentHints& hints,
                                        unsigned slot);

struct DepthTarget {
    std::uint32_t z_format = 0;  // DB_Z_INFO.FORMAT: 1 Z16, 3 Z32_FLOAT; 0 without depth
    bool stencil = false;        // DB_STENCIL_INFO.FORMAT: an 8-bit stencil
    std::uint64_t z_address = 0;  // DB_Z_READ_BASE << 8, and the write and stencil bases
    std::uint64_t z_write_address = 0;
    std::uint64_t stencil_address = 0;
    std::uint64_t stencil_write_address = 0;
    std::uint32_t pitch = 0;   // in pixels, (DB_DEPTH_SIZE.PITCH_TILE_MAX + 1) * 8
    std::uint32_t height = 0;  // in pixels, (DB_DEPTH_SIZE.HEIGHT_TILE_MAX + 1) * 8
    Extent extent;             // the hint, or pitch x height without one
    std::uint32_t base_slice = 0;  // DB_DEPTH_VIEW.SLICE_START
    std::uint32_t slices = 1;      // DB_DEPTH_VIEW.SLICE_MAX + 1
    std::uint32_t tile_mode = 0;   // DB_Z_INFO.TILE_MODE_INDEX
    std::uint32_t samples = 1;     // 1 << DB_Z_INFO.NUM_SAMPLES
    std::uint64_t htile_address = 0;  // DB_HTILE_DATA_BASE << 8 with TILE_SURFACE_ENABLE, else 0
    bool clear_depth = false;    // DB_RENDER_CONTROL.DEPTH_CLEAR_ENABLE
    bool clear_stencil = false;  // DB_RENDER_CONTROL.STENCIL_CLEAR_ENABLE
    float depth_clear = 0;       // DB_DEPTH_CLEAR
    std::uint8_t stencil_clear = 0;  // DB_STENCIL_CLEAR.CLEAR
};

// The depth target, or nothing when it has neither depth (a format and a read base) nor stencil.
// A part without its format or read base is left out (z_format 0, stencil false).
std::optional<DepthTarget> depth_target(const RegisterMirror& regs, const ExtentHints& hints);

}  // namespace bb::pm4

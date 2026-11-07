// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/amdgpu/tiling.cpp @ff1b7a2e87eef8f301377e0cabbbeeeb73cc3b49
// Source: shadPS4 src/video_core/texture_cache/tile.h @ff1b7a2e87eef8f301377e0cabbbeeeb73cc3b49
// Source: shadPS4 src/video_core/host_shaders/tiling.comp @ff1b7a2e87eef8f301377e0cabbbeeeb73cc3b49
// Changed for bloodborne-linux, 2026-11-07: only one fragment, thin non-PRT modes and the
// P8_32x32_16x16 pipe configuration, on the CPU.
#include "gpu/tile/tiling.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace bb::tile {

namespace {

constexpr std::uint32_t pipes = 8;      // P8_32x32_16x16
constexpr std::uint32_t pipe_bits = 3;  // log2(pipes)
constexpr std::uint32_t interleave_bits = 8;  // 256-byte pipe interleave

constexpr std::uint32_t bit(std::uint32_t value, unsigned n) {
    return value >> n & 1;
}

constexpr std::uint32_t align(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// The element's index in its 8x8 micro tile: bit i of the index is bit order[i] of
// (x & 7) | (y & 7) << 3. Display tiling depends on the element size, thin and depth don't.
std::uint32_t pixel_index(MicroMode micro, std::uint32_t bytes, std::uint32_t x,
                          std::uint32_t y) {
    static constexpr std::uint8_t orders[5][6] = {
        {0, 1, 2, 4, 3, 5},  // display, 1 byte: x0 x1 x2 y1 y0 y2
        {0, 1, 2, 3, 4, 5},  // display, 2 bytes: x0 x1 x2 y0 y1 y2
        {0, 1, 3, 2, 4, 5},  // display, 4 bytes: x0 x1 y0 x2 y1 y2
        {0, 3, 1, 2, 4, 5},  // display, 8 bytes: x0 y0 x1 x2 y1 y2
        {0, 3, 1, 4, 2, 5},  // display, 16 bytes; thin and depth: x0 y0 x1 y1 x2 y2
    };
    const auto& order = orders[micro == MicroMode::display ? std::countr_zero(bytes) : 4];
    const std::uint32_t xy = (x & 7) | (y & 7) << 3;
    std::uint32_t index = 0;
    for (unsigned i = 0; i < 6; ++i) {
        index |= bit(xy, order[i]) << i;
    }
    return index;
}

// Macro tile modes 0-4 (1x4_16, 1x2_16, 1x1_16, 1x1_16_Dup, 1x1_8): bank height, banks and
// aspect. A thin tile of one fragment holds at most 1024 bytes, so 5-7 are never chosen.
struct MacroMode {
    std::uint32_t bank_height, banks, aspect;
};
constexpr MacroMode macro_modes[] = {{4, 16, 4}, {2, 16, 2}, {1, 16, 2}, {1, 16, 2}, {1, 8, 1}};

}  // namespace

std::optional<ModeInfo> mode_info(std::uint32_t tile_mode) {
    using enum TileMode;
    using A = ArrayMode;
    using M = MicroMode;
    switch (static_cast<TileMode>(tile_mode)) {
    case depth_2d_thin_64: return ModeInfo{A::tiled_2d_thin, M::depth, 64};
    case depth_2d_thin_128: return ModeInfo{A::tiled_2d_thin, M::depth, 128};
    case depth_2d_thin_256: return ModeInfo{A::tiled_2d_thin, M::depth, 256};
    case depth_2d_thin_512: return ModeInfo{A::tiled_2d_thin, M::depth, 512};
    case depth_2d_thin_1k: return ModeInfo{A::tiled_2d_thin, M::depth, 1024};
    case depth_1d_thin: return ModeInfo{A::tiled_1d_thin, M::depth, 64};
    case display_linear_aligned: return ModeInfo{A::linear_aligned, M::display, 0};
    case display_1d_thin: return ModeInfo{A::tiled_1d_thin, M::display, 0};
    case display_2d_thin: return ModeInfo{A::tiled_2d_thin, M::display, 0};
    case thin_1d_thin: return ModeInfo{A::tiled_1d_thin, M::thin, 0};
    case thin_2d_thin: return ModeInfo{A::tiled_2d_thin, M::thin, 0};
    case display_linear_general: return ModeInfo{A::linear_general, M::display, 0};
    }
    return std::nullopt;
}

std::optional<Layout> layout(const Surface& s) {
    const auto mode = mode_info(s.tile_mode);
    if (!mode || !std::has_single_bit(s.bits) || s.bits < 8 || s.bits > 128 || s.pitch == 0 ||
        s.height == 0 || s.layers == 0 || s.levels == 0 || s.levels > 16) {
        return std::nullopt;
    }
    Layout l;
    l.mode = *mode;
    l.bytes = s.bits / 8;
    l.layers = s.layers;
    l.bank_swizzle = s.bank_swizzle;
    if (mode->array == ArrayMode::tiled_2d_thin) {
        // The tile split: the depth modes' own, for colour one that never splits a thin tile of
        // one fragment. The macro tile mode follows from the bytes of one tile.
        const std::uint32_t tile_bytes = 64 * l.bytes;
        l.split = mode->micro == MicroMode::depth ? mode->depth_split : 1024;
        const MacroMode& m = macro_modes[std::countr_zero(std::min(l.split, tile_bytes) / 64)];
        l.banks = m.banks;
        l.bank_height = m.bank_height;
        l.aspect = m.aspect;
        l.macro_width = 8 * pipes * m.aspect;
        l.macro_height = 8 * m.bank_height * m.banks / m.aspect;
    }
    for (std::uint32_t i = 0; i < s.levels; ++i) {
        std::uint32_t w = std::max(s.pitch >> i, 1u);
        std::uint32_t h = std::max(s.height >> i, 1u);
        if (s.blocks) {
            w = (w + 3) / 4;
            h = (h + 3) / 4;
        }
        Level level{w, h, l.size, 0, false};
        if (mode->array == ArrayMode::linear_aligned) {
            const std::uint32_t pitch_align = std::max(8u, 64 / l.bytes);
            const std::uint32_t layer_align = std::max(64u, 256 / l.bytes);
            level.pitch = align(w, pitch_align);
            while (std::uint64_t{level.pitch} * h % layer_align != 0) {
                level.pitch += pitch_align;
            }
        } else if (mode->array == ArrayMode::tiled_2d_thin &&
                   (i == 0 || (w >= l.macro_width && h >= l.macro_height))) {
            level.pitch = align(w, l.macro_width);
            level.height = align(h, l.macro_height);
            level.macro = true;
        } else if (mode->array != ArrayMode::linear_general) {
            level.pitch = align(w, 8);
            level.height = align(h, 8);
            while (std::uint64_t{level.pitch} * level.height * l.bytes % 256 != 0) {
                level.pitch += 8;
            }
        }
        level.layer_size = std::uint64_t{level.pitch} * level.height * l.bytes;
        l.size += level.layer_size * s.layers;
        l.levels.push_back(level);
    }
    if (l.size >= std::uint64_t{1} << 32) {
        return std::nullopt;
    }
    return l;
}

std::uint64_t tiled_offset(const Layout& l, std::uint32_t level, std::uint32_t x,
                           std::uint32_t y, std::uint32_t layer) {
    const Level& lv = l.levels.at(level);
    const std::uint64_t base = lv.offset + layer * lv.layer_size;
    if (l.mode.array == ArrayMode::linear_general || l.mode.array == ArrayMode::linear_aligned) {
        return base + (std::uint64_t{y} * lv.pitch + x) * l.bytes;
    }
    std::uint32_t element = pixel_index(l.mode.micro, l.bytes, x, y) * l.bytes;
    std::uint32_t tile_bytes = 64 * l.bytes;
    if (!lv.macro) {
        return base + (std::uint64_t{y / 8} * (lv.pitch / 8) + x / 8) * tile_bytes + element;
    }
    // ComputeSurfaceAddrFromCoordMacroTiled of tiling.comp, one fragment, bank width 1.
    std::uint32_t splits = 1;
    std::uint32_t split = 0;
    if (tile_bytes > l.split) {
        splits = tile_bytes / l.split;
        split = element / l.split;
        element %= l.split;
        tile_bytes = l.split;
    }
    const std::uint64_t macro_bytes = std::uint64_t{tile_bytes} * l.bank_height;
    const std::uint64_t per_row = lv.pitch / l.macro_width;
    const std::uint64_t slice_bytes = per_row * (lv.height / l.macro_height) * macro_bytes;
    const std::uint64_t total =
        slice_bytes * (split + splits * layer) +
        ((y / l.macro_height) * per_row + x / l.macro_width) * macro_bytes + element +
        (y / 8 % l.bank_height) * tile_bytes;

    const std::uint32_t tx = x / 8;
    const std::uint32_t ty = y / 8;
    const std::uint32_t pipe = (bit(tx, 0) ^ bit(ty, 0) ^ bit(tx, 1)) |
                               (bit(tx, 1) ^ bit(ty, 1)) << 1 | (bit(tx, 2) ^ bit(ty, 2)) << 2;
    const std::uint32_t bx = x / 8 / pipes;
    const std::uint32_t by = y / 8 / l.bank_height;
    std::uint32_t bank;
    if (l.banks == 16) {
        bank = (bit(bx, 0) ^ bit(by, 3)) | (bit(bx, 1) ^ bit(by, 2) ^ bit(by, 3)) << 1 |
               (bit(bx, 2) ^ bit(by, 1)) << 2 | (bit(bx, 3) ^ bit(by, 0)) << 3;
    } else {
        bank = (bit(bx, 0) ^ bit(by, 2)) | (bit(bx, 1) ^ bit(by, 1) ^ bit(by, 2)) << 1 |
               (bit(bx, 2) ^ bit(by, 0)) << 2;
    }
    bank ^= l.bank_swizzle + (l.banks / 2 - 1) * layer;
    bank ^= (l.banks / 2 + 1) * split;
    bank &= l.banks - 1;
    const std::uint32_t bank_bits = std::countr_zero(l.banks);
    return lv.offset + (total & 255) + (std::uint64_t{pipe} << interleave_bits) +
           (std::uint64_t{bank} << (interleave_bits + pipe_bits)) +
           (total >> interleave_bits << (interleave_bits + pipe_bits + bank_bits));
}

namespace {

template <bool to_linear, typename From, typename To>
void copy(const Layout& l, From from, To to) {
    if (from.size() < l.size || to.size() < l.size) {
        throw std::invalid_argument("tile: buffer smaller than the layout");
    }
    for (std::uint32_t i = 0; i < l.levels.size(); ++i) {
        const Level& lv = l.levels[i];
        std::uint64_t linear = lv.offset;
        for (std::uint32_t layer = 0; layer < l.layers; ++layer) {
            for (std::uint32_t y = 0; y < lv.height; ++y) {
                for (std::uint32_t x = 0; x < lv.pitch; ++x, linear += l.bytes) {
                    const std::uint64_t tiled = tiled_offset(l, i, x, y, layer);
                    std::memcpy(&to[to_linear ? linear : tiled], &from[to_linear ? tiled : linear],
                                l.bytes);
                }
            }
        }
    }
}

}  // namespace

void detile(const Layout& l, std::span<const std::byte> tiled, std::span<std::byte> linear) {
    copy<true>(l, tiled, linear);
}

void tile(const Layout& l, std::span<const std::byte> linear, std::span<std::byte> tiled) {
    copy<false>(l, linear, tiled);
}

}  // namespace bb::tile

// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/amdgpu/tiling.h @ff1b7a2e87eef8f301377e0cabbbeeeb73cc3b49
// Changed for bloodborne-linux, 2026-11-07
//
// Tile modes and the tiled layout of surfaces in guest memory (docs/design/graphics.md,
// "Tiling (item 23, as built)"): the tile mode index of CB_COLOR<n>_ATTRIB, DB_Z_INFO and a T#,
// level layouts, the tiled byte offset of an element, and detiling on the CPU.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bb::tile {

// Tile mode indices (the PS4's GB_TILE_MODE<n> table, as in shadPS4's TileMode).
enum class TileMode : std::uint32_t {
    depth_2d_thin_64 = 0,
    depth_2d_thin_128 = 1,
    depth_2d_thin_256 = 2,
    depth_2d_thin_512 = 3,
    depth_2d_thin_1k = 4,
    depth_1d_thin = 5,
    display_linear_aligned = 8,
    display_1d_thin = 9,
    display_2d_thin = 10,
    thin_1d_thin = 13,
    thin_2d_thin = 14,
    display_linear_general = 31,
};

// AMD's array modes, values as in the hardware; only the ones the supported tile modes use.
enum class ArrayMode : std::uint32_t {
    linear_general = 0,
    linear_aligned = 1,
    tiled_1d_thin = 2,
    tiled_2d_thin = 4,
};

enum class MicroMode : std::uint32_t { display = 0, thin = 1, depth = 2 };

struct ModeInfo {
    ArrayMode array;
    MicroMode micro;
    std::uint32_t depth_split;  // the depth modes' tile split in bytes, otherwise 0
};

// The supported tile modes are the enumerators above: every non-PRT thin mode with eight pipes
// (P8_32x32_16x16) or a linear one. PRT, thick and 3D modes give nothing.
std::optional<ModeInfo> mode_info(std::uint32_t tile_mode);

struct Surface {
    std::uint32_t tile_mode = 0;
    std::uint32_t bits = 32;  // per element: 8, 16, 32, 64 or 128 (a BC block is one element)
    // Level 0 in pixels: a T#'s pitch or a target's pitch, and the height. Level i is
    // max(pitch >> i, 1) by max(height >> i, 1) before padding.
    std::uint32_t pitch = 1;
    std::uint32_t height = 1;
    std::uint32_t layers = 1;
    std::uint32_t levels = 1;
    bool blocks = false;  // a BC format: one element is a 4x4 block of pixels
    std::uint32_t bank_swizzle = 0;  // a T#'s BANK_SWIZZLE
};

struct Level {
    std::uint32_t pitch;   // padded, in elements
    std::uint32_t height;  // padded, in elements
    std::uint64_t offset;  // from the surface's base, the same tiled and linear
    std::uint64_t layer_size;  // bytes per layer: pitch * height * bytes per element
    bool macro;  // 2D-tiled; a 2D mode's small levels fall back to 1D tiling
};

// A surface's layout. Only one fragment per pixel is supported.
struct Layout {
    ModeInfo mode{};
    std::uint32_t bytes = 0;  // per element
    std::uint32_t layers = 0;
    std::uint32_t bank_swizzle = 0;
    // The macro tile of a 2D mode (bank width is always 1 with one fragment): its banks, bank
    // height, aspect, tile split in bytes and extent in elements.
    std::uint32_t banks = 0, bank_height = 0, aspect = 0, split = 0;
    std::uint32_t macro_width = 0, macro_height = 0;
    std::vector<Level> levels;
    std::uint64_t size = 0;  // bytes, levels one after another, each layer after layer
};

// Nothing for an unsupported tile mode or element size, an empty surface or one of 4 GiB.
std::optional<Layout> layout(const Surface& surface);

// The byte offset of element (x, y) of `layer` in `level` (x below the level's pitch, y below
// its height), from the surface's base. The linear copy keeps each level's padded rows:
// level.offset + ((layer * height + y) * pitch + x) * bytes.
std::uint64_t tiled_offset(const Layout& layout, std::uint32_t level, std::uint32_t x,
                           std::uint32_t y, std::uint32_t layer);

// Copies between tiled guest memory and the linear copy, each layout.size bytes.
void detile(const Layout& layout, std::span<const std::byte> tiled, std::span<std::byte> linear);
void tile(const Layout& layout, std::span<const std::byte> linear, std::span<std::byte> tiled);

}  // namespace bb::tile

// The HUD's world markers at a frame buffer other than 1920x1080 (#713). The lock-on reticle, the
// enemy health bars and the other markers the HUD pins to a point in the world are computed in
// pixels of the frame buffer (frame_buffer_width / frame_buffer_height), but the HUD places them
// as coordinates of its 1920x1080 stage, which the game scales to the screen. Above 1080p they
// land off the target: at 2560x1440 a target at x = 1273 showed its reticle at 1273 x 1440/1080.
// The game's own 16:9 projection is what the stage needs, also at wide sizes: a point lies at
// stage x = 960 + 960 x ndc of the 16:9 camera, and the stage is centred on the wider screen.
// So the loads of the frame buffer size in these functions read the stage's 1920 and 1080 instead.
#pragma once

#include <array>
#include <cstdint>

#include "game/gfx/frame_buffer.hpp"
#include "loader/image.hpp"

namespace bb::game {

// The size of the HUD's stage, in its own coordinates.
inline constexpr std::uint32_t hud_stage_width = 1920;
inline constexpr std::uint32_t hud_stage_height = 1080;

// One load of a frame buffer global in the HUD code, 9 bytes of whole instructions:
//   lea r, [rip + global]   (48 8d 05/0d rel32)
//   mov r32, [r]            (8b 00 or 8b 09)
// with r = rax or rcx. The stage load in its place is
//   mov r32, imm32          (b8/b9 imm32)
//   nop dword [rax + 0]     (0f 1f 40 00)
// which leaves the register as the game's load does (zero-extended to 64 bits) and touches no
// flags. No jump of the game lands inside the 9 bytes.
struct HudSizeLoad {
    std::uint64_t link;    // the lea
    std::uint64_t global;  // frame_buffer_width or frame_buffer_height
    bool rcx;              // the register is rcx, not rax
};

// The functions:
// - 0x016439b0 projects the world markers at [0x05562878]+0x900 (a list of 0x90-byte entries).
// - 0x016448f0 projects the lock-on point and stores the reticle at [0x05562878]+0xcf0 and
//   0x0555bda0.
// - 0x01644cf0 projects the eight enemy bar slots at [0x05562878]+0x10f8 (0x98 bytes each; the
//   position at +0x78, its depth at +0x80, shown at +0x94).
// The three are called in a row by 0x0193ac10. Each scales the 16:9 camera's ndc with 0.5 and
// 0.5 to 0..1, then multiplies by the size. Readers of their results that check a marker against
// the size, 0 <= x < width and 0 <= y < height, read the stage's size too:
// - 0x01654d80, an enemy bar's update ("F20_open_eneny_HP"), hides the bar of a slot outside the
//   size, and centres the bar on half the size when its +0x18c is set.
// - 0x016b3320 counts the lock-on reticle as on screen.
// - 0x01bfb2a0 (the HUD's update) counts an enemy bar slot as on screen.
inline constexpr std::array<HudSizeLoad, 14> hud_size_loads = {{
    {0x01644357, frame_buffer_width, false},
    {0x01644365, frame_buffer_height, false},
    {0x01644c55, frame_buffer_width, false},
    {0x01644c63, frame_buffer_height, false},
    {0x016452c7, frame_buffer_width, false},
    {0x016452d5, frame_buffer_height, false},
    {0x01654df6, frame_buffer_width, false},
    {0x01654e1c, frame_buffer_height, false},
    {0x01655611, frame_buffer_height, false},
    {0x01655628, frame_buffer_width, false},
    {0x016b3372, frame_buffer_width, true},
    {0x016b3398, frame_buffer_height, false},
    {0x01bfd491, frame_buffer_width, false},
    {0x01bfd4d1, frame_buffer_height, false},
}};

inline constexpr std::size_t hud_size_load_bytes = 9;

// The game's bytes of `load`.
std::array<std::uint8_t, hud_size_load_bytes> game_size_load(const HudSizeLoad& load);
// The bytes that load the stage's width or height in its place.
std::array<std::uint8_t, hud_size_load_bytes> stage_size_load(const HudSizeLoad& load);

// Makes the HUD loads read the stage's size when the frame buffer is not 1920x1080, and gives
// them the game's bytes back at 1920x1080 (where both read the same). Called after the eboot is
// mapped and before its entry point runs, like the hooks. Checks every load first and throws
// std::runtime_error, writing nothing, when one holds neither form. Pages of a finalized image
// are made writable for the write and get their segment's protection back.
void set_hud_stage_loads(loader::Image& eboot, std::uint32_t width, std::uint32_t height);

}  // namespace bb::game

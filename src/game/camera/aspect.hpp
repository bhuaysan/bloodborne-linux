// The aspect ratio of the game's projection at a frame buffer wider than 16:9 (#667;
// docs/design/image-quality.md, "Camera and aspect ratio"). Every game camera holds the constant
// 16:9 at +0x54, and the copy into the render camera (0x01d16690) passes it on to the projection
// (0x021aa290), which keeps the vertical angle and widens the horizontal one with the aspect
// (Hor+). With the frame buffer's own width / height there, a wide frame shows more at the sides
// instead of the 16:9 picture stretched.
#pragma once

#include <array>
#include <cstdint>

namespace bb::game {

// SprjPersCam's copy of a game camera into a render camera, its vtable slot +0x28 (also in the
// chase camera's vtable): (game camera, render camera). A leaf that only moves fields.
inline constexpr std::uint64_t render_cam_copy = 0x01d16690;
// Its first three instructions, vmovaps xmm0..xmm2 from [rdi+0x10..0x30].
inline constexpr std::array<std::uint8_t, 15> render_cam_copy_prologue = {
    0xc5, 0xf8, 0x28, 0x47, 0x10, 0xc5, 0xf8, 0x28, 0x4f, 0x20, 0xc5, 0xf8, 0x28, 0x57, 0x30};

// The game camera's aspect, and the render camera's that the copy fills from it (0 there means
// the render camera's width / height, which the copy sets to 1920 x 1080).
inline constexpr std::uint32_t game_cam_aspect = 0x54;
inline constexpr std::uint32_t render_cam_aspect = 0x64;

// The constant every camera constructor stores at +0x54: 16 / 9 as a float (0x3fe38e39).
inline constexpr float game_aspect = 16.0f / 9.0f;

// Sets the aspect of the frame buffer. A 16:9 size (width x 9 == height x 16) leaves the game's
// projection alone. Called before the game runs; not thread-safe.
void set_screen_aspect(std::uint32_t width, std::uint32_t height);
// The aspect set_screen_aspect gave, or game_aspect at 16:9.
float screen_aspect();

// The render camera's aspect for a game camera's `aspect`: `screen` in place of the game's 16:9,
// any other aspect unchanged (the character editor's square preview, the point light shadow
// cubes' 0).
float wide_aspect(float aspect, float screen);

}  // namespace bb::game

// A black backdrop for the loading page at a frame buffer wider than 16:9 (#711;
// docs/design/image-quality.md, "Camera and aspect ratio"). The engine gives each Scaleform movie a
// viewport of the whole frame buffer, and the movies fit their 16:9 stage in its centre. At 16:9
// the loading page covers the world the engine keeps rendering under it while it loads; at a wider
// frame buffer that world shows beside the page, grey while it is still empty. A movie with an
// opaque background colour clears its whole viewport first, so the loading page gets black.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace bb::game {

// The engine's Scaleform movie holder, constructed by 0x01f5fae0 for each movie it loads:
// (holder, owner, movie definition, kind, name, start). It stores the movie at +0x18 and the
// name, a UTF-16 string, at +0x28, sets the movie's viewport to the frame buffer (vcall +0x68)
// and its background colour to 0, transparent (vcall +0x108).
inline constexpr std::uint64_t movie_holder_construct = 0x01f58360;
// Its first six instructions: push rbp; mov rbp, rsp; push r15; push r14; push r12; push rbx;
// sub rsp, 0x70.
inline constexpr std::array<std::uint8_t, 15> movie_holder_construct_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x70};
inline constexpr std::uint32_t movie_holder_movie = 0x18;

// The movie's background colour setter, vtable slot +0x108: (movie, const colour*), the colour
// as Scaleform's 0xAARRGGBB. It stores the colour at +0x2208 and in the movie's render root.
inline constexpr std::uint32_t movie_set_background_slot = 0x108;
inline constexpr std::uint32_t opaque_black = 0xff000000;

// The loading page's movie (the item text over the book while a map loads).
inline constexpr std::u16string_view loading_page_movie = u"NowLoading2";

// Whether the movie called `name` gets an opaque black background at a frame buffer of
// `aspect` (screen_aspect()): only the loading page, and only at an aspect other than 16:9. The
// other movies (the HUD, the in-game menus) show the world under them on purpose, and at 16:9 the
// page covers the frame.
bool wants_black_backdrop(std::u16string_view name, float aspect);

}  // namespace bb::game

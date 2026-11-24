// bb-run's switches for the game's post-processing effects (#665): motion blur and chromatic
// aberration off or weaker (docs/design/image-quality.md).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bb::game {

// The post effect parameter block that pfx_apply hands to YEBIS, as dword indices (byte offset =
// 4 * index; docs/design/image-quality.md). The game builds the block every frame from the map's
// draw parameters; pfx_apply reads these fields of it at the addresses below.
inline constexpr std::size_t pfx_chromatic_aberration_enable = 0x2b;  // cmpl $0, 0xac(%rbx) at 0x021d7178
inline constexpr std::size_t pfx_chromatic_aberration_first = 0x2d;   // lateral and uniform dispersion,
inline constexpr std::size_t pfx_chromatic_aberration_last = 0x30;    // floats, 0xb4 to 0xc0 at 0x021d7193
inline constexpr std::size_t pfx_motion_blur_enable = 0x77;           // cmpl $0, 0x1dc(%rbx) at 0x021d7d2e
inline constexpr std::size_t pfx_motion_blur_time_ratio = 0x79;       // float, 0x1e4(%r15) at 0x021d7f5f

// 0x055406e0: the live global block (0x370 bytes) that draw_params_update writes on the game
// thread. pfx_apply has only been seen with the frame's copies of it (#726).
inline constexpr std::uint64_t pfx_live_block_link = 0x055406e0;

// 0x021d5f80: pfx_apply(self, context, block), on the render thread once a frame.
inline constexpr std::uint64_t pfx_apply_link = 0x021d5f80;
// Its first instructions: push rbp; mov rbp, rsp; push r15 to rbx; and rsp, -0x20.
inline constexpr std::array<std::uint8_t, 20> pfx_apply_prologue = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff};

// What bb-run changes. The defaults leave every field as the game sets it.
struct PostEffects {
    bool motion_blur = true;
    float motion_blur_strength = 1.0f;  // scales the blur time ratio, 0 to 1
    bool chromatic_aberration = true;
    float chromatic_aberration_strength = 1.0f;  // scales the four dispersions, 0 to 1

    bool operator==(const PostEffects&) const = default;
};

// The strengths' range.
inline constexpr float max_post_effect_strength = 1.0f;

// Sets the switches (a strength outside 0 to 1 or NaN throws std::invalid_argument). Called before
// the game runs; not thread-safe.
void set_post_effects(const PostEffects& effects);
const PostEffects& post_effects();

// The fields of a block that `effects` changes, with their values before the change.
struct PostEffectEdit {
    static constexpr std::size_t max_fields = 6;
    std::array<std::size_t, max_fields> index{};
    std::array<std::uint32_t, max_fields> saved{};
    std::array<std::uint32_t, max_fields> written{};
    std::size_t count = 0;
};

// Writes `effects` into `block`: an effect switched off gets its enable field 0, a strength below
// 1 scales its float fields. Returns what it changed, for restore_post_effects. A default `effects`
// changes nothing.
PostEffectEdit edit_post_effects(const PostEffects& effects, std::uint32_t* block);

// Writes the saved values of `edit` back into `block`, but only into fields that still hold the
// value the edit wrote: a value another thread wrote in between stays.
void restore_post_effects(const PostEffectEdit& edit, std::uint32_t* block);

// The wrap of pfx_apply with its original as a parameter: edits the block with post_effects(),
// calls `original` and restores the block, so the game's block holds its own values again after
// the call and a strength is never applied twice to the same value. A `block` equal to
// `live_block` is logged once (#726): the frames seen so far never passed it.
using PfxApply = void(void* self, void* context, std::uint32_t* block);
void pfx_apply_edited(PfxApply* original, void* self, void* context, std::uint32_t* block,
                      const std::uint32_t* live_block = nullptr);

// How many calls of the wrap got `live_block` (#726).
std::uint64_t pfx_live_block_calls();

// 0x021d5f80, wrapped: pfx_apply_edited with the game's function.
void pfx_apply(void* self, void* context, std::uint32_t* block);

}  // namespace bb::game

// The graphics parameter blends that the game eases per frame (docs/design/frame-rate.md,
// "The parameter blend (#598)").
#pragma once

#include <cstdint>

namespace bb::game {

// One entry of a parameter list: an id and a 32-bit value, an int or the bits of a float. The
// list's unlink (0x2926360) and the virtual call through +0 (0x2926470) show the first two fields.
struct ParamEntry {
    const void* vtable;
    ParamEntry* prev;
    ParamEntry* next;
    std::int32_t id;
    std::int32_t value;
};
static_assert(sizeof(ParamEntry) == 0x20);

// Looks up `id` in the list at `self + 0x30` and writes the int `current` eased towards the
// entry's value by `t` to `*out`: `*current` for t <= 0, the entry's value for t >= 1, else
// trunc(value * t + current * (1 - t)) in single precision. Returns false, writing nothing, when
// no entry has the id. At a target frame rate other than the game's 30 an entry equal to
// `*current` gives `*current`: the step's t of 1/30 rounds 3 * t + 3 * (1 - t) below 3.
bool param_blend_int(const void* self, std::int32_t id, float t, const std::int32_t* current,
                     std::int32_t* out);

}  // namespace bb::game

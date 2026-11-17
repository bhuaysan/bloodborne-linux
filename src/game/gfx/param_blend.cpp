// The int parameter blend at the target frame rate (docs/design/frame-rate.md, "The parameter
// blend (#598)").
#include "game/gfx/param_blend.hpp"

#include <immintrin.h>

#include <cstring>

#include "game/frame/frame.hpp"
#include "hook/hook.hpp"

namespace bb::game {

// 0x029262f0
// The float steps go through intrinsics so that the compiler keeps the game's two products and
// sum (no fused multiply-add) and its truncation (cvttss2si: INT_MIN for NaN and out of range).
bool param_blend_int(const void* self, std::int32_t id, float t, const std::int32_t* current,
                     std::int32_t* out) {
    const ParamEntry* entry;
    std::memcpy(&entry, static_cast<const std::uint8_t*>(self) + 0x30, sizeof(entry));
    while (entry != nullptr && entry->id != id) {
        entry = entry->next;
    }
    if (entry == nullptr) {
        return false;
    }
    std::int32_t value;
    if (frame_rate() != default_frame_rate && entry->value == *current) {
        value = *current;
    } else if (t <= 0.0f) {  // ucomiss: NaN is not <= 0 and not >= 1, so it takes the fraction
        value = *current;
    } else if (t >= 1.0f) {
        value = entry->value;
    } else {
        const __m128 vt = _mm_set_ss(t);
        const __m128 towards = _mm_mul_ss(_mm_set_ss(static_cast<float>(entry->value)), vt);
        const __m128 from =
            _mm_mul_ss(_mm_sub_ss(_mm_set_ss(1.0f), vt), _mm_set_ss(static_cast<float>(*current)));
        value = _mm_cvttss_si32(_mm_add_ss(towards, from));
    }
    *out = value;
    return true;
}

}  // namespace bb::game

BB_REPLACE(0x029262f0, bb::game::param_blend_int);

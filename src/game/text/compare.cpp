#include "game/text/compare.hpp"

#include "hook/hook.hpp"

namespace bb::game {

// 0x0043e190
// TODO(name): `cursor` is the first member of an object that callers advance through parsed
// text; what the object is, is not known yet.
int text_starts_with(const char* const* cursor, const char* prefix) {
    for (const char* t = *cursor; *prefix != '\0'; ++t, ++prefix) {
        if (*t != *prefix) {
            return 0;  // includes the text ending before the prefix
        }
    }
    return 1;
}

// 0x0040e9e0
// TODO(verify): only reached indirectly, so `self` (the unused first argument) is assumed to be
// the object of a method, and the int result to be read as a whole like 0x0043e190's.
int utf16_range_equals(const void* /*self*/, const char16_t* begin, const char16_t* end,
                       const char* narrow) {
    for (; *narrow != '\0'; ++narrow, ++begin) {
        if (begin == end || *begin != static_cast<unsigned char>(*narrow)) {
            return 0;
        }
    }
    return begin == end ? 1 : 0;
}

}  // namespace bb::game

BB_REPLACE(0x0043e190, bb::game::text_starts_with);
BB_REPLACE(0x0040e9e0, bb::game::utf16_range_equals);

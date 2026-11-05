// String comparisons that the game does in small leaf functions.
#pragma once

namespace bb::game {

// 1 when the NUL-terminated string that `*cursor` points to begins with the NUL-terminated
// `prefix`, else 0. An empty prefix matches every text. Callers test the full 32-bit result.
int text_starts_with(const char* const* cursor, const char* prefix);

// 1 when the UTF-16 range [begin, end) holds exactly the characters of the NUL-terminated
// `narrow`, each byte compared as an unsigned value against one 16-bit unit, else 0. `self` is
// not read. `end` must be reachable from `begin` in whole units.
int utf16_range_equals(const void* self, const char16_t* begin, const char16_t* end,
                       const char* narrow);

}  // namespace bb::game

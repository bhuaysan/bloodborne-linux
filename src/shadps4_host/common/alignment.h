// The alignment helper of shadPS4's common/alignment.h that the translator uses. Written for this
// project: the upstream header is CC0-1.0, which is not on the allow-list of docs/source-policy.md.
#pragma once

#include <concepts>
#include <cstddef>

namespace Common {

// The next multiple of `size` at or above `value`.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T AlignUp(T value, std::size_t size) {
    const T over = static_cast<T>(value % size);
    return over == 0 ? value : static_cast<T>(value + (size - over));
}

}  // namespace Common

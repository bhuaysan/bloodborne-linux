// Bounds-checked reads from untrusted file data.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace bb::loader {

// Any problem with the input files: malformed, truncated, unsupported or not the pinned build.
struct LoadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// True when [offset, offset + size) lies inside `total` bytes. Never overflows.
constexpr bool in_bounds(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

// Throws LoadError naming `what` unless `size` bytes at `offset` lie inside `data`.
inline void need_bytes(std::span<const std::byte> data, std::uint64_t offset, std::uint64_t size,
                       std::string_view what) {
    if (!in_bounds(offset, size, data.size())) {
        throw LoadError(std::format("truncated data: {} at {:#x} + {:#x} exceeds {:#x} bytes", what,
                                    offset, size, data.size()));
    }
}

// The little-endian integer at `offset`. PS4 files and the host are both little-endian.
template <class T>
T read_le(std::span<const std::byte> data, std::uint64_t offset, std::string_view what) {
    static_assert(std::is_integral_v<T> && std::endian::native == std::endian::little);
    need_bytes(data, offset, sizeof(T), what);
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

}  // namespace bb::loader

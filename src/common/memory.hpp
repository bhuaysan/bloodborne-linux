// Pages and checked mmap/mprotect.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bb::common {

// The size of a page of this process, a power of two.
std::size_t page_size();

inline std::uint64_t page_down(std::uint64_t value) {
    return value & ~(std::uint64_t{page_size()} - 1);
}

inline std::uint64_t page_up(std::uint64_t value) {
    return page_down(value + page_size() - 1);
}

// mmap of `size` bytes of private anonymous memory, with `flags` added, at the hint `addr`.
// Throws std::system_error with `what` when it fails. A hint that is not honoured is no failure.
void* map_anonymous(std::size_t size, int prot, std::string_view what, void* addr = nullptr,
                    int flags = 0);

// mprotect of `size` bytes at `addr`. Throws std::system_error with the message
// "<context>mprotect of <start>..<end>" when it fails.
void protect(void* addr, std::size_t size, int prot, std::string_view context = {});

}  // namespace bb::common

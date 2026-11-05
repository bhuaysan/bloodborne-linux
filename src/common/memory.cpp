#include "common/memory.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <format>
#include <string>
#include <system_error>

namespace bb::common {

std::size_t page_size() {
    static const auto size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

// errno is saved right after the failing call, before formatting the message can change it.
void* map_anonymous(std::size_t size, int prot, std::string_view what, void* addr, int flags) {
    void* p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
    if (p == MAP_FAILED) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), std::string(what));
    }
    return p;
}

void protect(void* addr, std::size_t size, int prot, std::string_view context) {
    if (mprotect(addr, size, prot) != 0) {
        const int err = errno;
        const auto start = reinterpret_cast<std::uintptr_t>(addr);
        throw std::system_error(
            err, std::generic_category(),
            std::format("{}mprotect of {:#x}..{:#x}", context, start, start + size));
    }
}

}  // namespace bb::common

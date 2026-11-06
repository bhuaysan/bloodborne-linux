#include "hle/kernel/dmem.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/vmem.hpp"

namespace bb::hle::kernel::dmem {
namespace {

using common::LogLevel;
using vmem::page_size;

constexpr std::string_view channel = "hle.libkernel";

constexpr int guest_einval = 22;
constexpr int guest_enomem = 12;  // ours: every caller tests for non-zero only

struct Allocation {
    std::uint64_t end = 0;  // exclusive
    std::int32_t type = 0;
};

// The allocations, keyed by start offset. They never overlap. Adjacent ones stay apart, so that
// a type query (BC+OG5m9+bw, still in vmem.cpp) can report the bounds of each.
std::mutex table_mutex;
std::map<std::uint64_t, Allocation> table;

constexpr bool power_of_two(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

// `value` rounded up to `alignment`, a power of two; both at most total_size.
constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// The alignment argument of the allocate and map calls: 0 means page_size, smaller powers of two
// are raised to it. Nothing for other values.
std::optional<std::uint64_t> checked_alignment(std::uint64_t alignment) {
    if (alignment == 0) {
        return page_size;
    }
    if (!power_of_two(alignment) || alignment > total_size) {
        return std::nullopt;
    }
    return alignment < page_size ? page_size : alignment;
}

// The lowest aligned start of `length` free bytes in [low, high). Needs table_mutex.
std::optional<std::uint64_t> find_free(std::uint64_t low, std::uint64_t high, std::uint64_t length,
                                       std::uint64_t alignment) {
    std::uint64_t at = align_up(low, alignment);
    for (const auto& [start, allocation] : table) {
        if (at >= high) {
            break;
        }
        if (allocation.end <= at) {
            continue;
        }
        if (start >= at && start - at >= length) {
            break;
        }
        at = align_up(allocation.end, alignment);
    }
    if (at >= high || high - at < length) {
        return std::nullopt;
    }
    return at;
}

// Whether allocations cover [start, end) without a hole. Needs table_mutex.
bool covered(std::uint64_t start, std::uint64_t end) {
    auto it = table.upper_bound(start);
    if (it == table.begin()) {
        return false;
    }
    std::uint64_t at = start;
    for (--it; it != table.end() && it->first <= at; ++it) {
        at = std::max(at, it->second.end);
        if (at >= end) {
            return true;
        }
    }
    return false;
}

// Splits the allocation that holds `at` so that one starts there. Needs table_mutex.
void split_at(std::uint64_t at) {
    auto it = table.upper_bound(at);
    if (it == table.begin()) {
        return;
    }
    --it;
    if (it->first == at || it->second.end <= at) {
        return;
    }
    table.emplace(at, Allocation{it->second.end, it->second.type});
    it->second.end = at;
}

// Whether [offset, offset + length) is a non-empty range of whole guest pages in the memfd.
bool valid_range(std::uint64_t offset, std::uint64_t length) {
    return length != 0 && length % page_size == 0 && offset % page_size == 0 &&
           offset <= total_size && length <= total_size - offset;
}

}  // namespace

int fd() {
    static const int memfd = [] {
        const int created = ::memfd_create("bb-direct-memory", MFD_CLOEXEC);
        if (created < 0 || ::ftruncate(created, static_cast<off_t>(total_size)) != 0) {
            const int error = errno;
            common::log(LogLevel::error, channel, "direct memory: no memfd of {:#x} bytes: {}",
                        total_size, std::strerror(error));
            if (created >= 0) {
                ::close(created);
            }
            errno = error;
            return -1;
        }
        return created;
    }();
    if (memfd < 0) {
        errno = ENOMEM;
    }
    return memfd;
}

}  // namespace bb::hle::kernel::dmem

namespace bb::hle::kernel {

using namespace dmem;

std::uint64_t direct_memory_size() {
    return total_size;
}

std::int32_t allocate_direct(std::uint64_t search_start, std::uint64_t search_end,
                             std::uint64_t length, std::uint64_t alignment, std::int32_t type,
                             std::uint64_t* offset) {
    const std::optional<std::uint64_t> align = checked_alignment(alignment);
    search_end = std::min(search_end, total_size);
    if (offset == nullptr || !align || length == 0 || length % page_size != 0 ||
        search_start >= search_end) {
        return sce_error(guest_einval);
    }
    if (fd() < 0) {
        return sce_fail(errno);
    }
    const std::scoped_lock lock(table_mutex);
    const std::optional<std::uint64_t> at = find_free(search_start, search_end, length, *align);
    if (!at) {
        common::log(LogLevel::warn, channel,
                    "direct memory: no {:#x} bytes aligned to {:#x} free in {:#x}-{:#x}", length,
                    *align, search_start, search_end);
        return sce_error(guest_enomem);
    }
    table.emplace(*at, Allocation{*at + length, type});
    *offset = *at;
    common::log(LogLevel::debug, channel, "direct memory: allocate {:#x}-{:#x} type {}", *at,
                *at + length, type);
    return 0;
}

std::int32_t map_direct(void** address, std::uint64_t length, int prot, int flags,
                        std::uint64_t offset, std::uint64_t alignment) {
    if ((flags & ~map_flag_0x80) != 0) {
        common::log(LogLevel::warn, channel, "direct memory: map flags {:#x} refused", flags);
        return sce_error(guest_einval);
    }
    const std::optional<std::uint64_t> align = checked_alignment(alignment);
    if (address == nullptr || !align || !valid_range(offset, length)) {
        return sce_error(guest_einval);
    }
    const std::scoped_lock lock(table_mutex);
    if (!covered(offset, offset + length)) {
        return sce_error(guest_einval);
    }
    vmem::Mapping m;
    m.address = reinterpret_cast<std::uintptr_t>(*address);
    m.length = length;
    m.prot = prot;
    m.host_flags = MAP_SHARED;
    m.fd = fd();
    m.fd_offset = static_cast<off_t>(offset);
    m.flags = vmem::region_direct;
    m.offset = offset;
    m.alignment = *align;
    std::uintptr_t out = 0;
    if (const int error = vmem::map(m, out)) {
        return sce_fail(error);
    }
    *address = reinterpret_cast<void*>(out);
    return 0;
}

// Mappings of the range stay; the hole punched into the memfd makes them read zeros, as does
// a later allocation of the range. The eboot unmaps before it releases (0x20826fd), except on
// one clean-up path that drops the mapping's address (0x26d3f70).
std::int32_t release_direct(std::uint64_t offset, std::uint64_t length) {
    length = align_up(std::min(length, total_size), page_size);
    if (!valid_range(offset, length)) {
        return sce_error(guest_einval);
    }
    const std::scoped_lock lock(table_mutex);
    const std::uint64_t end = offset + length;
    if (!covered(offset, end)) {
        return sce_error(guest_einval);
    }
    split_at(offset);
    split_at(end);
    table.erase(table.lower_bound(offset), table.lower_bound(end));
    if (::fallocate(fd(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                    static_cast<off_t>(length)) != 0) {
        common::log(LogLevel::warn, channel, "direct memory: no hole punched at {:#x}-{:#x}: {}",
                    offset, end, std::strerror(errno));
    }
    common::log(LogLevel::debug, channel, "direct memory: release {:#x}-{:#x}", offset, end);
    return 0;
}

BB_HLE_FUNCTION("libkernel", "pO96TwzOm5E", direct_memory_size);  // name unknown
BB_HLE_FUNCTION("libkernel", "rTXw65xmLIA", allocate_direct);     // name unknown
BB_HLE_FUNCTION("libkernel", "L-Q3LEjIbgA", map_direct);          // name unknown
BB_HLE_FUNCTION("libkernel", "MBuItvba6z8", release_direct);      // name unknown

}  // namespace bb::hle::kernel

#include "hle/kernel/dmem.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/vmem.hpp"

namespace bb::hle::kernel::dmem {
namespace {

using common::LogLevel;
using vmem::page_size;

constexpr std::string_view channel = "hle.libkernel";

constexpr int guest_enoent = 2;   // ours: the type query's only caller tests for non-zero
constexpr int guest_einval = 22;
constexpr int guest_enomem = 12;  // ours: every caller tests for non-zero only

struct Allocation {
    std::uint64_t end = 0;  // exclusive
    std::int32_t type = 0;
};

// The allocations, keyed by start offset. They never overlap. Adjacent ones stay apart, so that
// the type query (BC+OG5m9+bw) can report the bounds of each. Leaked, never destroyed: game
// threads may call in during exit.
constinit std::mutex table_mutex;
std::map<std::uint64_t, Allocation>& table = *new std::map<std::uint64_t, Allocation>;

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
    // Allocations don't overlap, so every one before the last that starts at or below `at` ends
    // at or below it: the scan starts at that last one.
    auto first = table.upper_bound(at);
    if (first != table.begin()) {
        --first;
    }
    for (const auto& [start, allocation] : std::ranges::subrange(first, table.end())) {
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

std::vector<AllocatedRange> allocations() {
    const std::scoped_lock lock(table_mutex);
    std::vector<AllocatedRange> out;
    for (const auto& [start, a] : table) {
        out.push_back({start, a.end, a.type});
    }
    return out;
}

int restore_allocations(const std::vector<AllocatedRange>& baseline) {
    const std::scoped_lock lock(table_mutex);
    int error = 0;
    auto punch = [&](std::uint64_t start, std::uint64_t end) {
        if (::fallocate(fd(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                        static_cast<off_t>(start), static_cast<off_t>(end - start)) != 0) {
            error = errno;
        }
    };
    for (const auto& [start, a] : table) {
        // The parts of [start, a.end) between the baseline's allocations, which are in order.
        std::uint64_t at = start;
        for (const AllocatedRange& b : baseline) {
            if (b.end <= at) {
                continue;
            }
            if (b.start >= a.end) {
                break;
            }
            if (b.start > at) {
                punch(at, b.start);
            }
            at = b.end;
        }
        if (at < a.end) {
            punch(at, a.end);
        }
    }
    table.clear();
    for (const AllocatedRange& b : baseline) {
        table.emplace(b.start, Allocation{b.end, b.type});
    }
    return error;
}

namespace {

// A shared view of the memfd at `offset`, as a direct region of the region table.
vmem::Mapping direct_mapping(std::uintptr_t address, std::uint64_t length, int prot,
                             std::uint64_t offset) {
    vmem::Mapping m;
    m.address = address;
    m.length = length;
    m.prot = prot;
    m.host_flags = MAP_SHARED;
    m.fd = fd();
    m.fd_offset = static_cast<off_t>(offset);
    m.flags = vmem::region_direct;
    m.offset = offset;
    return m;
}

// One entry of the batch map: 0 or a host errno.
int run_batch_entry(const BatchEntry& e) {
    if (e.operation == batch_unmap) {
        return vmem::restore_reservation(e.address, e.length);
    }
    if (e.operation != batch_map) {
        common::log(LogLevel::warn, channel, "direct memory: batch operation {} refused",
                    e.operation);
        return EINVAL;
    }
    if (!valid_range(e.offset, e.length)) {
        return EINVAL;
    }
    const std::scoped_lock lock(table_mutex);
    if (!covered(e.offset, e.offset + e.length)) {
        return EINVAL;
    }
    vmem::Mapping m = direct_mapping(e.address, e.length, e.prot, e.offset);
    m.fixed = true;
    m.into_reservation = true;
    std::uintptr_t out = 0;
    return vmem::map(m, out);
}

}  // namespace
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
    vmem::Mapping m =
        direct_mapping(reinterpret_cast<std::uintptr_t>(*address), length, prot, offset);
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

// The only caller (0x26bae74) passes the offset that the mapping query reports and tests the type
// for 3.
std::int32_t direct_memory_type(std::uint64_t offset, std::int32_t* type, std::uint64_t* start,
                                std::uint64_t* end) {
    if (type == nullptr || start == nullptr || end == nullptr) {
        return sce_error(guest_einval);
    }
    const std::scoped_lock lock(table_mutex);
    auto it = table.upper_bound(offset);
    if (it == table.begin() || std::prev(it)->second.end <= offset) {
        return sce_error(guest_enoent);
    }
    --it;
    *type = it->second.type;
    *start = it->first;
    *end = it->second.end;
    return 0;
}

// The only caller (0x2901fbd) passes flags 0x80 and alignment 0, and releases the range with
// sceKernelMunmap.
std::int32_t reserve_range(void** address, std::uint64_t length, int flags,
                           std::uint64_t alignment) {
    if ((flags & ~map_flag_0x80) != 0) {
        common::log(LogLevel::warn, channel, "direct memory: reserve flags {:#x} refused", flags);
        return sce_error(guest_einval);
    }
    const std::optional<std::uint64_t> align = checked_alignment(alignment);
    if (address == nullptr || !align) {
        return sce_error(guest_einval);
    }
    std::uintptr_t out = 0;
    if (const int error =
            vmem::reserve(reinterpret_cast<std::uintptr_t>(*address), length, *align, out)) {
        return sce_fail(error);
    }
    *address = reinterpret_cast<void*>(out);
    return 0;
}

// 0x2901eb0 maps blocks of direct memory into a reservation with batch_map and passes `done`; on
// failure it undoes the first *done entries with batch_unmap and a null `done` (0x2902101), then
// unmaps the whole reservation (0x2902115).
std::int32_t map_direct_batch(const BatchEntry* entries, std::int32_t count, std::int32_t* done) {
    if (done != nullptr) {
        *done = 0;
    }
    if (entries == nullptr || count < 0) {
        return sce_error(guest_einval);
    }
    for (std::int32_t i = 0; i < count; ++i) {
        if (const int error = run_batch_entry(entries[i])) {
            common::log(LogLevel::warn, channel, "direct memory: batch entry {} of {} failed: {}",
                        i, count, std::strerror(error));
            return sce_fail(error);
        }
        if (done != nullptr) {
            *done = i + 1;
        }
    }
    return 0;
}

BB_HLE_FUNCTION("libkernel", "pO96TwzOm5E", direct_memory_size);  // name unknown
BB_HLE_FUNCTION("libkernel", "rTXw65xmLIA", allocate_direct);     // name unknown
BB_HLE_FUNCTION("libkernel", "L-Q3LEjIbgA", map_direct);          // name unknown
BB_HLE_FUNCTION("libkernel", "MBuItvba6z8", release_direct);      // name unknown
BB_HLE_FUNCTION("libkernel", "BC+OG5m9+bw", direct_memory_type);  // name unknown
BB_HLE_FUNCTION("libkernel", "7oxv3PPCumo", reserve_range);       // name unknown
BB_HLE_FUNCTION("libkernel", "2SKEx6bSq-4", map_direct_batch);    // name unknown

}  // namespace bb::hle::kernel

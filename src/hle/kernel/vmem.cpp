#include "hle/kernel/vmem.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/file.hpp"

namespace bb::hle::kernel::vmem {
namespace {

using common::LogLevel;

constexpr std::string_view channel = "hle.libkernel";

template <class... Args>
void log(LogLevel level, std::format_string<Args...> format, Args&&... args) {
    common::log(level, channel, format, std::forward<Args>(args)...);
}

// Larger lengths can't be mapped on x86-64 and would overflow the alignment arithmetic.
constexpr std::uint64_t max_length = std::uint64_t{1} << 47;

// The host flags of a reservation: address space only, no memory committed.
constexpr int reservation_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

constexpr std::uint64_t round_up(std::uint64_t value) {
    return (value + page_size - 1) & ~(page_size - 1);
}

// `value` rounded up to `alignment`, a power of two.
constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void* ptr(std::uintptr_t address) {
    return reinterpret_cast<void*>(address);
}

// Every change of the table that the observers see (map, unmap, reserve again, restore) holds
// change_mutex from start to end, and table_mutex only while it reads or changes the table and
// the host mappings: the observers run between, without table_mutex (MappingObserver). Lock
// order: dmem's allocation lock, change_mutex, table_mutex.
constinit std::mutex change_mutex;

// The table, keyed by start. Regions never overlap. The maps are leaked, never destroyed: game
// threads may call in during exit.
constinit std::mutex table_mutex;
std::map<std::uintptr_t, Region>& table = *new std::map<std::uintptr_t, Region>;

constinit std::array<std::atomic<MappingObserver*>, max_mapping_observers> observers{};

// A part of a region that the memfd backs.
struct Backed {
    std::uintptr_t address;
    std::uint64_t length;
    std::uint64_t backing;
    bool flexible;  // its backing goes back to dmem's flexible area when it is unmapped
};

// The backed parts of the regions in [start, end). Needs table_mutex.
std::vector<Backed> backed_parts(std::uintptr_t start, std::uintptr_t end) {
    std::vector<Backed> out;
    auto it = table.upper_bound(start);
    if (it != table.begin()) {
        --it;
    }
    for (; it != table.end() && it->first < end; ++it) {
        const Region& r = it->second;
        const std::uintptr_t low = std::max(start, r.start), high = std::min(end, r.end);
        if (r.backing != no_backing && low < high) {
            out.push_back({low, high - low, r.backing + (low - r.start),
                           (r.flags & region_direct) == 0});
        }
    }
    return out;
}

// Calls `call` on every observer.
template <typename Call>
void each_observer(Call call) {
    for (std::atomic<MappingObserver*>& slot : observers) {
        if (MappingObserver* const o = slot.load(std::memory_order_acquire)) {
            call(*o);
        }
    }
}

// Tells the observers that `parts` are about to go. Needs change_mutex, not table_mutex.
void notify_unmapped(const std::vector<Backed>& parts) {
    for (const Backed& p : parts) {
        each_observer([&](MappingObserver& o) { o.unmapped(p.address, p.length, p.backing); });
    }
}

// Gives the flexible memory of `parts` back, once no host mapping views it. Needs change_mutex.
void free_flexible(const std::vector<Backed>& parts) {
    for (const Backed& p : parts) {
        if (p.flexible) {
            dmem::free_flexible(p.backing, p.length);
        }
    }
}

// The ranges of the window that reserve_again dropped, start to end. The host may still hold the
// guest mapping there or may have reused the range, so placement in the window skips them for
// good. They are no regions, so unmap, protect and the queries refuse them. Needs table_mutex.
std::map<std::uintptr_t, std::uintptr_t>& dropped = *new std::map<std::uintptr_t, std::uintptr_t>;

// Splits the region that holds `at` so that a region starts there. Needs table_mutex.
void split_at(std::uintptr_t at) {
    auto it = table.upper_bound(at);
    if (it == table.begin()) {
        return;
    }
    Region& r = std::prev(it)->second;
    if (r.start == at || r.end <= at) {
        return;
    }
    Region tail = r;
    tail.start = at;
    if ((tail.flags & region_direct) != 0) {
        tail.offset += at - r.start;
    }
    if (tail.backing != no_backing) {
        tail.backing += at - r.start;
    }
    r.end = at;
    table.emplace(at, std::move(tail));
}

// Splits at both ends and removes the regions in [start, end). Needs table_mutex.
void erase_range(std::uintptr_t start, std::uintptr_t end) {
    split_at(start);
    split_at(end);
    table.erase(table.lower_bound(start), table.lower_bound(end));
}

// Whether guest mappings cover [start, end) without a hole and every region satisfies `accept`.
// Needs table_mutex.
template <typename Accept>
bool covered_by(std::uintptr_t start, std::uintptr_t end, Accept accept) {
    auto it = table.upper_bound(start);
    if (it == table.begin()) {
        return false;
    }
    --it;
    std::uintptr_t at = start;
    for (; it != table.end() && it->second.start <= at; ++it) {
        if (it->second.end > at) {
            if (!accept(it->second)) {
                return false;
            }
            at = it->second.end;
        }
        if (at >= end) {
            return true;
        }
    }
    return false;
}

// As covered_by; with `reserved`, only regions whose `reserved` equals it.
bool covered(std::uintptr_t start, std::uintptr_t end, std::optional<bool> reserved = {}) {
    return covered_by(start, end,
                      [&](const Region& r) { return !reserved || r.reserved == *reserved; });
}

// Reserves [start, end) again over the guest mappings there. A failed MAP_FIXED may already have
// unmapped the range (ENOMEM at the host's map count limit, for one), so on failure the table
// drops it and its part in the window goes to `dropped`: the host may reuse it, and no later
// guest map or unmap may touch that memory. A guest mapping that survived on the host is only
// leaked. Returns 0 or errno. Needs table_mutex.
int reserve_again(std::uintptr_t start, std::uintptr_t end) {
    if (::mmap(ptr(start), end - start, PROT_NONE, reservation_flags | MAP_FIXED, -1, 0) !=
        MAP_FAILED) {
        return 0;
    }
    const int error = errno;
    log(LogLevel::warn, "reserve {:#x}-{:#x} again: failed, the range is dropped", start, end);
    erase_range(start, end);
    const std::uintptr_t low = std::max(start, window_start);
    const std::uintptr_t high = std::min(end, window_end);
    if (low < high) {
        dropped.emplace(low, high);
    }
    return error;
}

// Gives guest memory back: unmaps the parts outside the window and reserves the part in it again
// (reserve_again's rules), so the window keeps its whole range. The table drops each part outside
// the window as soon as it is unmapped, so a failure on a later part leaves no region over memory
// the host may reuse. Returns 0 or errno. The observers must have been told. Needs table_mutex.
int release(std::uintptr_t start, std::uintptr_t end) {
    const std::uintptr_t low = std::clamp(start, window_start, window_end);
    const std::uintptr_t high = std::clamp(end, window_start, window_end);
    const auto unmap_outside = [](std::uintptr_t from, std::uintptr_t to) {
        if (::munmap(ptr(from), to - from) != 0) {
            return errno;
        }
        erase_range(from, to);
        return 0;
    };
    if (start < low) {
        if (const int error = unmap_outside(start, std::min(end, window_start))) {
            return error;
        }
    }
    if (high < end) {
        if (const int error = unmap_outside(std::max(start, window_end), end)) {
            return error;
        }
    }
    return low < high ? reserve_again(low, high) : 0;
}

// Reserves the guest window on the first call. Returns 0 or errno, also on later calls after a
// failure. Needs table_mutex.
int reserve_window() {
    static int error = -1;
    if (error >= 0) {
        return error;
    }
    constexpr std::uint64_t size = window_end - window_start;
    void* const at = ::mmap(ptr(window_start), size, PROT_NONE,
                            reservation_flags | MAP_FIXED_NOREPLACE, -1, 0);
    error = at == ptr(window_start) ? 0 : at == MAP_FAILED ? errno : EEXIST;
    if (at != MAP_FAILED && at != ptr(window_start)) {  // a kernel before 4.17 took it as a hint
        ::munmap(at, size);
    }
    if (error != 0) {
        // AddressSanitizer keeps the range as its PROT_NONE shadow gap without the option.
        log(LogLevel::error, "guest window {:#x}-{:#x} not reserved: {} (AddressSanitizer needs "
            "protect_shadow_gap=0)",
            window_start, window_end, std::strerror(error));
    }
    return error;
}

// The end of a region or dropped range that overlaps [start, end), which lies past start, or
// nothing. Needs table_mutex.
std::optional<std::uintptr_t> taken_until(std::uintptr_t start, std::uintptr_t end) {
    std::optional<std::uintptr_t> out;
    const auto check = [&](const auto& ranges, const auto& end_of) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && end_of(std::prev(it)->second) > start) {
            --it;
        }
        if (it != ranges.end() && it->first < end) {
            out = std::max(out.value_or(0), end_of(it->second));
        }
    };
    check(table, [](const Region& r) { return r.end; });
    check(dropped, [](std::uintptr_t e) { return e; });
    return out;
}

// Whether neither a region nor a dropped range overlaps [start, end). Needs table_mutex.
bool free_range(std::uintptr_t start, std::uintptr_t end) {
    return !taken_until(start, end);
}

// The lowest start of `length` free bytes (free_range) in the window at or above `low` that is
// `residue` modulo `alignment`. Needs table_mutex.
std::optional<std::uintptr_t> find_free(std::uintptr_t low, std::uint64_t length,
                                        std::uint64_t alignment, std::uint64_t residue = 0) {
    const auto next = [&](std::uintptr_t from) {
        return align_up(std::max(from, window_start) - residue, alignment) + residue;
    };
    std::uintptr_t at = next(low);
    while (at < window_end && window_end - at >= length) {
        const std::optional<std::uintptr_t> taken = taken_until(at, at + length);
        if (!taken) {
            return at;
        }
        at = next(*taken);
    }
    return std::nullopt;
}

// Maps at an `alignment`-aligned address the host picks: reserves enough to find one, maps over
// it, then returns the rest. For MAP_32BIT, which the window can't serve.
void* map_host_placed(const Mapping& m, int prot) {
    const auto host_page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    const std::uint64_t reserved = m.length + m.alignment - host_page;
    void* const base = ::mmap(ptr(m.address), reserved, PROT_NONE, reservation_flags | MAP_32BIT,
                              -1, 0);
    if (base == MAP_FAILED) {
        return MAP_FAILED;
    }
    const auto b = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t aligned = align_up(b, m.alignment);
    void* const at =
        ::mmap(ptr(aligned), m.length, prot, m.host_flags | MAP_FIXED, m.fd, m.fd_offset);
    const int error = errno;
    if (at == MAP_FAILED) {
        ::munmap(base, reserved);
        errno = error;
        return MAP_FAILED;
    }
    if (aligned > b) {
        ::munmap(base, aligned - b);
    }
    if (b + reserved > aligned + m.length) {
        ::munmap(ptr(aligned + m.length), b + reserved - aligned - m.length);
    }
    return at;
}

// Maps at the lowest free aligned address in the window, at or above the hint if it lies there.
// Returns the address, or MAP_FAILED with errno set. Needs table_mutex.
void* map_in_window(const Mapping& m, int prot) {
    // An alignment whose multiples the offset is not one of rules the congruence out; map_locked
    // logs that.
    std::uint64_t alignment = m.alignment, residue = 0;
    if (m.congruent > alignment && m.offset % alignment == 0) {
        residue = m.offset % m.congruent;
        alignment = m.congruent;
    }
    std::optional<std::uintptr_t> at;
    if (in_window(m.address, m.address + 1)) {
        at = find_free(m.address, m.length, alignment, residue);
    }
    if (!at) {
        at = find_free(window_start, m.length, alignment, residue);
    }
    if (!at) {
        log(LogLevel::warn, "guest window: no {:#x} bytes aligned to {:#x} free", m.length,
            m.alignment);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    // MAP_FIXED replaces the window's reservation in one call.
    void* const out = ::mmap(ptr(*at), m.length, prot, m.host_flags | MAP_FIXED, m.fd, m.fd_offset);
    if (out == MAP_FAILED) {
        const int error = errno;
        reserve_again(*at, *at + m.length);  // the failed MAP_FIXED may have unmapped the range
        errno = error;
    }
    return out;
}

// The host protection for a guest protection, or nothing for unknown bits.
std::optional<int> host_protection(int guest) {
    const Flags host = host_prot(guest & ~prot_extra);
    if (host.unknown != 0) {
        log(LogLevel::warn, "unknown protection bits {:#x}", host.unknown);
        return std::nullopt;
    }
    static std::atomic<bool> extra_logged{false};
    if ((guest & prot_extra) != 0 && !extra_logged.exchange(true)) {
        log(LogLevel::info, "protection bits {:#x} accepted and not applied", guest & prot_extra);
    }
    return host.value;
}

// Checks a range for unmap() and protect(); returns 0 or EINVAL. Needs table_mutex.
int check_range(std::uintptr_t start, std::uint64_t length, std::uintptr_t& end) {
    if (start % page_size != 0 || length == 0 || length > max_length) {
        return EINVAL;
    }
    end = start + round_up(length);
    return end > start && covered(start, end) ? 0 : EINVAL;
}

// The part of map_region under the lock, with the checked host protection and, for flexible
// memory, the memfd at `backing` in place of the mapping's own. Needs table_mutex.
int map_locked(const Mapping& m, int prot, bool reserved, std::uint64_t backing,
               std::uintptr_t& out) {
    if (const int error = reserve_window()) {
        return error;
    }
    void* const hint = ptr(m.address);
    const std::uintptr_t end = m.address + m.length;
    void* at = MAP_FAILED;
    if (m.fixed && end < m.address) {
        return EINVAL;
    }
    if (m.fixed && m.into_reservation) {
        if (!covered(m.address, end, true)) {
            log(LogLevel::warn, "map {:#x}-{:#x}: not all reserved", m.address, end);
            return EINVAL;
        }
        // MAP_FIXED replaces the reservation in one call, so no other mapping can come between.
        at = ::mmap(hint, m.length, prot, m.host_flags | MAP_FIXED, m.fd, m.fd_offset);
        if (at == MAP_FAILED) {
            const int error = errno;
            reserve_again(m.address, end);  // the failed MAP_FIXED may have unmapped the range
            return error;
        }
        erase_range(m.address, end);
    } else if (m.fixed && in_window(m.address, end)) {
        // Over the window's reservation, where no region is.
        if (m.address < window_start || end > window_end || !free_range(m.address, end)) {
            return EEXIST;
        }
        at = ::mmap(hint, m.length, prot, m.host_flags | MAP_FIXED, m.fd, m.fd_offset);
        if (at == MAP_FAILED) {
            const int error = errno;
            reserve_again(m.address, end);  // as above
            return error;
        }
    } else if (m.fixed) {
        at = ::mmap(hint, m.length, prot, m.host_flags | MAP_FIXED_NOREPLACE, m.fd, m.fd_offset);
        if (at != MAP_FAILED && at != hint) {  // a kernel before 4.17 took it as a hint
            ::munmap(at, m.length);
            return EEXIST;
        }
    } else if ((m.host_flags & MAP_32BIT) != 0) {
        at = map_host_placed(m, prot);
    } else {
        at = map_in_window(m, prot);
    }
    if (at == MAP_FAILED) {
        return errno;
    }
    out = reinterpret_cast<std::uintptr_t>(at);
    table.insert_or_assign(out, Region{out, out + m.length, m.prot, m.flags, m.offset, m.name,
                                       reserved, backing});
    if (m.congruent != 0 && (out - m.offset) % m.congruent != 0) {
        log(LogLevel::warn, "map {:#x}-{:#x} of offset {:#x}{}: not congruent mod {:#x}", out,
            out + m.length, m.offset, m.fixed ? " (fixed)" : "", m.congruent);
    }
    return 0;
}

// map() and reserve(): `reserved` marks the region as a reservation.
int map_region(const Mapping& m, bool reserved, std::uintptr_t& out) {
    if (m.length == 0 || m.length % page_size != 0 || m.length > max_length ||
        m.alignment < page_size || m.alignment > max_length ||
        (m.alignment & (m.alignment - 1)) != 0 || (m.fixed && m.address % page_size != 0)) {
        return EINVAL;
    }
    const std::optional<int> prot = host_protection(m.prot);
    if (!prot) {
        return EINVAL;
    }
    const std::scoped_lock change(change_mutex);
    std::uint64_t backing = (m.flags & region_direct) != 0 ? m.offset : no_backing;
    Mapping host = m;
    if (m.flexible) {
        const std::optional<std::uint64_t> taken = dmem::allocate_flexible(m.length);
        if (!taken) {
            return ENOMEM;
        }
        backing = *taken;
        host.fd = dmem::fd();
        host.fd_offset = static_cast<off_t>(backing);
        host.host_flags = (m.host_flags & ~(MAP_ANONYMOUS | MAP_PRIVATE)) | MAP_SHARED;
    }
    int error = 0;
    {
        const std::scoped_lock lock(table_mutex);
        error = map_locked(host, *prot, reserved, backing, out);
    }
    if (error != 0) {
        if (m.flexible) {
            dmem::free_flexible(backing, m.length);
        }
        return error;
    }
    // After the lock: threads may log out of table order (accepted, debug lines only).
    log(LogLevel::debug, "map {:#x}-{:#x} prot {:#x} flags {:#x} '{}'", out, out + m.length,
        m.prot, m.flags, m.name);
    if (backing != no_backing) {
        each_observer([&](MappingObserver& o) { o.mapped(out, m.length, backing); });
    }
    return 0;
}

}  // namespace

bool add_mapping_observer(MappingObserver* o) {
    for (std::atomic<MappingObserver*>& slot : observers) {
        MappingObserver* expected = nullptr;
        if (slot.compare_exchange_strong(expected, o, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

void remove_mapping_observer(MappingObserver* o) {
    for (std::atomic<MappingObserver*>& slot : observers) {
        MappingObserver* expected = o;
        slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
}

void notify_released(std::uint64_t offset, std::uint64_t length) {
    each_observer([&](MappingObserver& o) { o.released(offset, length); });
}

int map(const Mapping& m, std::uintptr_t& out) {
    return map_region(m, false, out);
}

int reserve(std::uintptr_t hint, std::uint64_t length, std::uint64_t alignment,
            std::uintptr_t& out) {
    Mapping m;
    m.address = hint;
    m.length = length;
    m.host_flags = reservation_flags;
    m.flags = 0;
    m.alignment = alignment;
    m.name = "reserved";
    return map_region(m, true, out);
}

// A failed host call may leave the backed parts mapped, so their flexible memory is only given
// back on success (unmap and restore as well).
int restore_reservation(std::uintptr_t start, std::uint64_t length) {
    const std::scoped_lock change(change_mutex);
    std::uintptr_t end = 0;
    std::vector<Backed> parts;
    {
        const std::scoped_lock lock(table_mutex);
        if (const int error = check_range(start, length, end)) {
            return error;
        }
        parts = backed_parts(start, end);
    }
    notify_unmapped(parts);
    {
        const std::scoped_lock lock(table_mutex);
        if (const int error = reserve_again(start, end)) {
            return error;
        }
        erase_range(start, end);
        table.emplace(start, Region{start, end, 0, 0, 0, "reserved", true});
    }
    free_flexible(parts);
    log(LogLevel::debug, "reserve again {:#x}-{:#x}", start, end);  // after the lock, as in map
    return 0;
}

int unmap(std::uintptr_t start, std::uint64_t length) {
    const std::scoped_lock change(change_mutex);
    std::uintptr_t end = 0;
    std::vector<Backed> parts;
    {
        const std::scoped_lock lock(table_mutex);
        if (const int error = check_range(start, length, end)) {
            return error;
        }
        parts = backed_parts(start, end);
    }
    notify_unmapped(parts);
    {
        const std::scoped_lock lock(table_mutex);
        if (const int error = release(start, end)) {
            return error;
        }
        erase_range(start, end);
    }
    free_flexible(parts);
    log(LogLevel::debug, "unmap {:#x}-{:#x}", start, end);  // after the lock, as in map
    return 0;
}

int protect(std::uintptr_t start, std::uint64_t length, int prot) {
    const std::optional<int> host = host_protection(prot);
    if (!host) {
        return EINVAL;
    }
    const std::scoped_lock lock(table_mutex);
    std::uintptr_t end = 0;
    if (const int error = check_range(start, length, end)) {
        return error;
    }
    if (!covered(start, end, false)) {  // a reservation in the range
        return EINVAL;
    }
    if (::mprotect(reinterpret_cast<void*>(start), end - start, *host) != 0) {
        return errno;
    }
    split_at(start);
    split_at(end);
    for (auto it = table.lower_bound(start); it != table.lower_bound(end); ++it) {
        it->second.prot = prot;
    }
    return 0;
}

bool mapped(std::uintptr_t start, std::uintptr_t end) {
    const std::scoped_lock lock(table_mutex);
    return start < end && covered(start, end);
}

bool readable(std::uintptr_t start, std::uintptr_t end) {
    const std::scoped_lock lock(table_mutex);
    return start < end && covered_by(start, end, [](const Region& r) {
        return !r.reserved && (r.prot & PROT_READ) != 0;
    });
}

std::optional<Region> find(std::uintptr_t address) {
    const std::scoped_lock lock(table_mutex);
    auto it = table.upper_bound(address);
    if (it == table.begin() || std::prev(it)->second.end <= address) {
        return std::nullopt;
    }
    return std::prev(it)->second;
}

std::vector<Region> regions() {
    const std::scoped_lock lock(table_mutex);
    std::vector<Region> out;
    out.reserve(table.size());
    for (const auto& [start, region] : table) {
        out.push_back(region);
    }
    return out;
}

int restore(const std::vector<Region>& baseline) {
    const std::scoped_lock change(change_mutex);
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> added;
    std::vector<Backed> parts;
    {
        const std::scoped_lock lock(table_mutex);
        for (const Region& r : baseline) {
            const auto it = table.find(r.start);
            if (it == table.end() || it->second != r) {
                log(LogLevel::warn, "restore: the baseline's region {:#x}-{:#x} '{}' changed",
                    r.start, r.end, r.name);
                return EINVAL;
            }
        }
        // Regions don't overlap, so every other region was mapped after the baseline.
        for (const auto& [start, r] : table) {
            if (!std::ranges::binary_search(baseline, start, {}, &Region::start)) {
                added.emplace_back(start, r.end);
                const std::vector<Backed> more = backed_parts(start, r.end);
                parts.insert(parts.end(), more.begin(), more.end());
            }
        }
    }
    notify_unmapped(parts);
    {
        const std::scoped_lock lock(table_mutex);
        for (const auto& [start, end] : added) {
            if (const int error = release(start, end)) {
                return error;
            }
            erase_range(start, end);
            log(LogLevel::debug, "restore: unmap {:#x}-{:#x}", start, end);
        }
    }
    free_flexible(parts);
    return 0;
}

}  // namespace bb::hle::kernel::vmem

namespace bb::hle::kernel {
namespace {

using common::LogLevel;
using vmem::log;

constexpr int guest_map_shared = 0x1;
constexpr int guest_map_private = 0x2;
constexpr int guest_map_anon = 0x1000;

void* const map_failed = MAP_FAILED;

// The anonymous map calls: *address is a hint in and the mapping out. Every call site passes
// flags 0 (libc.prx 0x23eb8, 0x23ff4, 0x24926; eboot 0x2081db9).
std::int32_t map_anonymous_named(void** address, std::uint64_t length, int prot, int flags,
                                 std::string name) {
    if (address == nullptr || flags != 0) {
        if (flags != 0) {
            log(LogLevel::warn, "anonymous map: flags {:#x} refused", flags);
        }
        return sce_error(22);
    }
    vmem::Mapping m;
    m.address = reinterpret_cast<std::uintptr_t>(*address);
    m.length = length;
    m.prot = prot;
    m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    m.flexible = true;
    m.name = std::move(name);
    std::uintptr_t out = 0;
    if (const int error = vmem::map(m, out)) {
        return sce_fail(error);
    }
    // The GPU memory mirror holds direct memory only. Which of these mappings the GPU reads is
    // open until a run shows it.
    if ((prot & vmem::prot_extra) == vmem::prot_extra) {
        log(LogLevel::info, "flexible memory {:#x}-{:#x} '{}' mapped with protection {:#x}", out,
            out + length, m.name, prot);
    }
    *address = reinterpret_cast<void*>(out);
    return 0;
}

}  // namespace

// A file mapping (no MAP_ANON) maps the host descriptor behind guest descriptor `fd` at `offset`,
// which must be a multiple of the guest page size. An anonymous mapping is flexible memory, as
// in the map calls below, and its offset is ignored. Without MAP_SHARED or MAP_PRIVATE a mapping
// is private, so it never writes to its file.
void* guest_mmap(void* address, std::uint64_t length, int prot, int flags, int fd,
                 std::int64_t offset) {
    const Flags host = host_map_flags(flags);
    const int sharing = flags & (guest_map_shared | guest_map_private);
    if (host.unknown != 0) {
        log(LogLevel::warn, "mmap: unknown flags {:#x}", host.unknown);
        posix_fail(EINVAL);
        return map_failed;
    }
    if (length == 0 || length > (std::uint64_t{1} << 47) ||
        sharing == (guest_map_shared | guest_map_private)) {
        posix_fail(EINVAL);
        return map_failed;
    }
    vmem::Mapping m;
    m.address = reinterpret_cast<std::uintptr_t>(address);
    m.length = (length + vmem::page_size - 1) & ~(vmem::page_size - 1);
    m.prot = prot;
    m.host_flags = (host.value & ~MAP_FIXED) | (sharing == 0 ? MAP_PRIVATE : 0);
    m.fixed = (host.value & MAP_FIXED) != 0;
    // Holds the host descriptor open until the host mapping has its own reference to the file.
    std::shared_ptr<OpenFile> file;
    if ((flags & guest_map_anon) == 0) {
        file = fd_table().get(fd);
        if (!file) {
            log(LogLevel::warn, "mmap: guest descriptor {} is not open", fd);
            posix_fail(EBADF);
            return map_failed;
        }
        if (offset < 0 || static_cast<std::uint64_t>(offset) % vmem::page_size != 0) {
            log(LogLevel::warn, "mmap: file offset {:#x} refused", offset);
            posix_fail(EINVAL);
            return map_failed;
        }
        m.fd = file->host_fd;
        m.fd_offset = static_cast<off_t>(offset);
        m.flags = 0;
        m.name = std::format("fd {}", fd);
    } else {
        m.flexible = true;
    }
    std::uintptr_t out = 0;
    if (const int error = vmem::map(m, out)) {
        posix_fail(error);
        return map_failed;
    }
    return reinterpret_cast<void*>(out);
}

int guest_munmap(void* address, std::uint64_t length) {
    const int error = vmem::unmap(reinterpret_cast<std::uintptr_t>(address), length);
    return error == 0 ? 0 : posix_fail(error);
}

// libc.prx's only call (0x541a5) probes whether a 4 KiB page is mapped with MS_ASYNC, so it goes
// to the host for any address, not only guest mappings. In the guest window, where the host
// always has a mapping, it fails with ENOMEM as on an unmapped host page where no guest mapping
// is.
int guest_msync(void* address, std::uint64_t length, int flags) {
    const Flags host = host_msync_flags(flags);
    if (host.unknown != 0) {
        log(LogLevel::warn, "msync: unknown or refused flags {:#x}", host.unknown);
        return posix_fail(EINVAL);
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (length != 0 && vmem::in_window(start, start + length) &&
        !vmem::mapped(start & ~std::uintptr_t{0xfff}, start + length)) {
        return posix_fail(ENOMEM);
    }
    return ::msync(address, length, host.value) == 0 ? 0 : posix_fail(errno);
}

int guest_getpagesize() {
    return static_cast<int>(vmem::page_size);
}

std::int32_t sceKernelMunmap(void* address, std::uint64_t length) {
    return sce_fail(vmem::unmap(reinterpret_cast<std::uintptr_t>(address), length));
}

std::int32_t sceKernelMprotect(const void* address, std::uint64_t length, int prot) {
    return sce_fail(vmem::protect(reinterpret_cast<std::uintptr_t>(address), length, prot));
}

// Checked, not applied: the host's RLIMIT_MEMLOCK would make libc's arena growth fail, which
// unmaps the arena when this call fails (libc.prx 0x23ed9).
std::int32_t sceKernelMlock(const void* address, std::uint64_t length) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (length == 0) {
        return 0;
    }
    return vmem::mapped(start, start + length) ? 0 : sce_error(12);
}

std::int32_t map_named_anonymous(void** address, std::uint64_t length, int prot, int flags,
                                 const char* name) {
    std::string copy;
    if (name != nullptr) {
        copy.assign(name, ::strnlen(name, 255));
    }
    return map_anonymous_named(address, length, prot, flags, std::move(copy));
}

std::int32_t map_anonymous(void** address, std::uint64_t length, int prot, int flags) {
    return map_anonymous_named(address, length, prot, flags, {});
}

std::int32_t release_anonymous(void* address, std::uint64_t length) {
    return sceKernelMunmap(address, length);
}

// Every call site passes flags 0 and size 0x48.
std::int32_t query_mapping(const void* address, int flags, void* record, std::uint64_t size) {
    if (flags != 0 || record == nullptr || size < sizeof(QueryRecord)) {
        return sce_error(22);
    }
    const auto region = vmem::find(reinterpret_cast<std::uintptr_t>(address));
    if (!region) {
        return sce_error(13);  // ours: the callers test for non-zero
    }
    QueryRecord r{};
    r.start = region->start;
    r.end = region->end;
    r.offset = region->offset;
    r.flags = static_cast<std::uint8_t>(region->flags);
    std::memcpy(record, &r, sizeof r);
    return 0;
}

std::int32_t mapping_protection(const void* address, std::uint64_t* start, std::uint64_t* end,
                                std::uint32_t* prot) {
    const auto region = vmem::find(reinterpret_cast<std::uintptr_t>(address));
    if (!region) {
        return sce_error(13);  // ours: the callers test for non-zero
    }
    if (start != nullptr) {
        *start = region->start;
    }
    if (end != nullptr) {
        *end = region->end;
    }
    if (prot != nullptr) {
        *prot = static_cast<std::uint32_t>(region->prot);
    }
    return 0;
}

BB_HLE_FUNCTION("libkernel", "BPE9s9vQQXo", guest_mmap);
BB_HLE_FUNCTION("libkernel", "UqDGjXA5yUM", guest_munmap);
BB_HLE_FUNCTION("libkernel", "tZY4+SZNFhA", guest_msync);
BB_HLE_FUNCTION("libkernel", "k+AXqu2-eBc", guest_getpagesize);
BB_HLE_FUNCTION("libScePosix", "BPE9s9vQQXo", guest_mmap);
BB_HLE_FUNCTION("libScePosix", "UqDGjXA5yUM", guest_munmap);
BB_HLE_FUNCTION("libScePosix", "tZY4+SZNFhA", guest_msync);
BB_HLE_FUNCTION("libScePosix", "k+AXqu2-eBc", guest_getpagesize);
BB_HLE_FUNCTION("libkernel", "cQke9UuBQOk", sceKernelMunmap);
BB_HLE_FUNCTION("libkernel", "vSMAm3cxYTY", sceKernelMprotect);
BB_HLE_FUNCTION("libkernel", "3k6kx-zOOSQ", sceKernelMlock);
// sceKernelMapNamedFlexibleMemory
BB_HLE_FUNCTION("libkernel", "mL8NDH86iQI", map_named_anonymous);
BB_HLE_FUNCTION("libkernel", "IWIBBdTHit4", map_anonymous);        // sceKernelMapFlexibleMemory
BB_HLE_FUNCTION("libkernel", "teiItL2boFw", release_anonymous);    // sceKernelReleaseFlexibleMemory
BB_HLE_FUNCTION("libkernel", "rVjRvHJ0X6c", query_mapping);        // sceKernelVirtualQuery
BB_HLE_FUNCTION("libkernel", "WFcfL2lzido", mapping_protection);   // sceKernelQueryMemoryProtection

}  // namespace bb::hle::kernel

#include "hle/kernel/vmem.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/file.hpp"

namespace bb::hle::kernel::vmem {
namespace {

// Until the logger of m3-core-os.md ("Logging") exists.
template <class... Args>
void log(std::format_string<Args...> format, Args&&... args) {
    const std::string line =
        std::format("hle.kernel: {}\n", std::format(format, std::forward<Args>(args)...));
    std::fputs(line.c_str(), stderr);
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

// The table, keyed by start. Regions never overlap.
std::mutex table_mutex;
std::map<std::uintptr_t, Region> table;

// The ranges of the window that reserve_again dropped, start to end. The host may still hold the
// guest mapping there or may have reused the range, so placement in the window skips them for
// good. They are no regions, so unmap, protect and the queries refuse them. Needs table_mutex.
std::map<std::uintptr_t, std::uintptr_t> dropped;

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
    r.end = at;
    table.emplace(at, std::move(tail));
}

// Splits at both ends and removes the regions in [start, end). Needs table_mutex.
void erase_range(std::uintptr_t start, std::uintptr_t end) {
    split_at(start);
    split_at(end);
    table.erase(table.lower_bound(start), table.lower_bound(end));
}

// Whether guest mappings cover [start, end) without a hole; with `reserved`, only regions whose
// `reserved` equals it. Needs table_mutex.
bool covered(std::uintptr_t start, std::uintptr_t end, std::optional<bool> reserved = {}) {
    auto it = table.upper_bound(start);
    if (it == table.begin()) {
        return false;
    }
    --it;
    std::uintptr_t at = start;
    for (; it != table.end() && it->second.start <= at; ++it) {
        if (it->second.end > at) {
            if (reserved && it->second.reserved != *reserved) {
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
    log("reserve {:#x}-{:#x} again: failed, the range is dropped", start, end);
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
// the host may reuse. Returns 0 or errno. Needs table_mutex.
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
        log("guest window {:#x}-{:#x} not reserved: {} (AddressSanitizer needs "
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

// The lowest `alignment`-aligned start of `length` free bytes (free_range) in the window at or
// above `low`. Needs table_mutex.
std::optional<std::uintptr_t> find_free(std::uintptr_t low, std::uint64_t length,
                                        std::uint64_t alignment) {
    std::uintptr_t at = align_up(std::max(low, window_start), alignment);
    while (at < window_end && window_end - at >= length) {
        const std::optional<std::uintptr_t> taken = taken_until(at, at + length);
        if (!taken) {
            return at;
        }
        at = align_up(*taken, alignment);
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
    std::optional<std::uintptr_t> at;
    if (in_window(m.address, m.address + 1)) {
        at = find_free(m.address, m.length, m.alignment);
    }
    if (!at) {
        at = find_free(window_start, m.length, m.alignment);
    }
    if (!at) {
        log("guest window: no {:#x} bytes aligned to {:#x} free", m.length, m.alignment);
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
        log("unknown protection bits {:#x}", host.unknown);
        return std::nullopt;
    }
    static std::atomic<bool> extra_logged{false};
    if ((guest & prot_extra) != 0 && !extra_logged.exchange(true)) {
        log("protection bits {:#x} accepted and not applied", guest & prot_extra);
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
    const std::scoped_lock lock(table_mutex);
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
            log("map {:#x}-{:#x}: not all reserved", m.address, end);
            return EINVAL;
        }
        // MAP_FIXED replaces the reservation in one call, so no other mapping can come between.
        at = ::mmap(hint, m.length, *prot, m.host_flags | MAP_FIXED, m.fd, m.fd_offset);
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
        at = ::mmap(hint, m.length, *prot, m.host_flags | MAP_FIXED, m.fd, m.fd_offset);
        if (at == MAP_FAILED) {
            const int error = errno;
            reserve_again(m.address, end);  // as above
            return error;
        }
    } else if (m.fixed) {
        at = ::mmap(hint, m.length, *prot, m.host_flags | MAP_FIXED_NOREPLACE, m.fd, m.fd_offset);
        if (at != MAP_FAILED && at != hint) {  // a kernel before 4.17 took it as a hint
            ::munmap(at, m.length);
            return EEXIST;
        }
    } else if ((m.host_flags & MAP_32BIT) != 0) {
        at = map_host_placed(m, *prot);
    } else {
        at = map_in_window(m, *prot);
    }
    if (at == MAP_FAILED) {
        return errno;
    }
    out = reinterpret_cast<std::uintptr_t>(at);
    table.insert_or_assign(out,
                           Region{out, out + m.length, m.prot, m.flags, m.offset, m.name, reserved});
    log("map {:#x}-{:#x} prot {:#x} flags {:#x} '{}'", out, out + m.length, m.prot, m.flags,
        m.name);
    return 0;
}

}  // namespace

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

int restore_reservation(std::uintptr_t start, std::uint64_t length) {
    const std::scoped_lock lock(table_mutex);
    std::uintptr_t end = 0;
    if (const int error = check_range(start, length, end)) {
        return error;
    }
    if (const int error = reserve_again(start, end)) {
        return error;
    }
    erase_range(start, end);
    table.emplace(start, Region{start, end, 0, 0, 0, "reserved", true});
    log("reserve again {:#x}-{:#x}", start, end);
    return 0;
}

int unmap(std::uintptr_t start, std::uint64_t length) {
    const std::scoped_lock lock(table_mutex);
    std::uintptr_t end = 0;
    if (const int error = check_range(start, length, end)) {
        return error;
    }
    if (const int error = release(start, end)) {
        return error;
    }
    erase_range(start, end);
    log("unmap {:#x}-{:#x}", start, end);
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

}  // namespace bb::hle::kernel::vmem

namespace bb::hle::kernel {
namespace {

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
            log("anonymous map: flags {:#x} refused", flags);
        }
        return sce_error(22);
    }
    vmem::Mapping m;
    m.address = reinterpret_cast<std::uintptr_t>(*address);
    m.length = length;
    m.prot = prot;
    m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    m.name = std::move(name);
    std::uintptr_t out = 0;
    if (const int error = vmem::map(m, out)) {
        return sce_fail(error);
    }
    *address = reinterpret_cast<void*>(out);
    return 0;
}

}  // namespace

// A file mapping (no MAP_ANON) maps the host descriptor behind guest descriptor `fd` at `offset`,
// which must be a multiple of the guest page size. The offset of an anonymous mapping is ignored.
// Without MAP_SHARED or MAP_PRIVATE a mapping is private, so it never writes to its file.
void* guest_mmap(void* address, std::uint64_t length, int prot, int flags, int fd,
                 std::int64_t offset) {
    const Flags host = host_map_flags(flags);
    const int sharing = flags & (guest_map_shared | guest_map_private);
    if (host.unknown != 0) {
        log("mmap: unknown flags {:#x}", host.unknown);
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
            log("mmap: guest descriptor {} is not open", fd);
            posix_fail(EBADF);
            return map_failed;
        }
        if (offset < 0 || static_cast<std::uint64_t>(offset) % vmem::page_size != 0) {
            log("mmap: file offset {:#x} refused", offset);
            posix_fail(EINVAL);
            return map_failed;
        }
        m.fd = file->host_fd;
        m.fd_offset = static_cast<off_t>(offset);
        m.flags = 0;
        m.name = std::format("fd {}", fd);
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
        log("msync: unknown or refused flags {:#x}", host.unknown);
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
BB_HLE_FUNCTION("libkernel", "mL8NDH86iQI", map_named_anonymous);  // name unknown
BB_HLE_FUNCTION("libkernel", "IWIBBdTHit4", map_anonymous);        // name unknown
BB_HLE_FUNCTION("libkernel", "teiItL2boFw", release_anonymous);    // name unknown
BB_HLE_FUNCTION("libkernel", "rVjRvHJ0X6c", query_mapping);        // name unknown
BB_HLE_FUNCTION("libkernel", "WFcfL2lzido", mapping_protection);   // name unknown

}  // namespace bb::hle::kernel

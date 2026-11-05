#include "hle/kernel/vmem.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"

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

constexpr std::uint64_t round_up(std::uint64_t value) {
    return (value + page_size - 1) & ~(page_size - 1);
}

// The table, keyed by start. Regions never overlap.
std::mutex table_mutex;
std::map<std::uintptr_t, Region> table;

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

// Whether guest mappings cover [start, end) without a hole. Needs table_mutex.
bool covered(std::uintptr_t start, std::uintptr_t end) {
    auto it = table.upper_bound(start);
    if (it == table.begin()) {
        return false;
    }
    --it;
    std::uintptr_t at = start;
    for (; it != table.end() && it->second.start <= at; ++it) {
        if (it->second.end > at) {
            at = it->second.end;
        }
        if (at >= end) {
            return true;
        }
    }
    return false;
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
    return covered(start, end) ? 0 : EINVAL;
}

}  // namespace

int map(const Mapping& m, std::uintptr_t& out) {
    if (m.length == 0 || m.length % page_size != 0 || m.length > max_length ||
        (m.fixed && m.address % page_size != 0)) {
        return EINVAL;
    }
    const std::optional<int> prot = host_protection(m.prot);
    if (!prot) {
        return EINVAL;
    }
    const std::scoped_lock lock(table_mutex);
    void* const hint = reinterpret_cast<void*>(m.address);
    void* at = MAP_FAILED;
    if (m.fixed) {
        at = ::mmap(hint, m.length, *prot, m.host_flags | MAP_FIXED_NOREPLACE, m.fd, m.fd_offset);
        if (at != MAP_FAILED && at != hint) {  // a kernel before 4.17 took it as a hint
            ::munmap(at, m.length);
            return EEXIST;
        }
    } else {
        // Reserve enough to find an aligned start, map over it, then return the rest.
        const auto host_page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
        const std::uint64_t reserved = m.length + page_size - host_page;
        void* const base =
            ::mmap(hint, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (base == MAP_FAILED) {
            return errno;
        }
        const auto b = reinterpret_cast<std::uintptr_t>(base);
        const std::uintptr_t aligned = round_up(b);
        at = ::mmap(reinterpret_cast<void*>(aligned), m.length, *prot, m.host_flags | MAP_FIXED,
                    m.fd, m.fd_offset);
        const int error = errno;
        if (at == MAP_FAILED) {
            ::munmap(base, reserved);
            return error;
        }
        if (aligned > b) {
            ::munmap(base, aligned - b);
        }
        if (b + reserved > aligned + m.length) {
            ::munmap(reinterpret_cast<void*>(aligned + m.length), b + reserved - aligned - m.length);
        }
    }
    if (at == MAP_FAILED) {
        return errno;
    }
    out = reinterpret_cast<std::uintptr_t>(at);
    table.insert_or_assign(out, Region{out, out + m.length, m.prot, m.flags, m.offset, m.name});
    log("map {:#x}-{:#x} prot {:#x} flags {:#x} '{}'", out, out + m.length, m.prot, m.flags,
        m.name);
    return 0;
}

int unmap(std::uintptr_t start, std::uint64_t length) {
    const std::scoped_lock lock(table_mutex);
    std::uintptr_t end = 0;
    if (const int error = check_range(start, length, end)) {
        return error;
    }
    if (::munmap(reinterpret_cast<void*>(start), end - start) != 0) {
        return errno;
    }
    split_at(start);
    split_at(end);
    table.erase(table.lower_bound(start), table.lower_bound(end));
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

std::optional<Region> find(std::uintptr_t address) {
    const std::scoped_lock lock(table_mutex);
    auto it = table.upper_bound(address);
    if (it == table.begin() || std::prev(it)->second.end <= address) {
        return std::nullopt;
    }
    return std::prev(it)->second;
}

}  // namespace bb::hle::kernel::vmem

namespace bb::hle::kernel {

std::int32_t sceKernelMunmap(void* address, std::uint64_t length) {
    return sce_fail(vmem::unmap(reinterpret_cast<std::uintptr_t>(address), length));
}

std::int32_t sceKernelMprotect(const void* address, std::uint64_t length, int prot) {
    return sce_fail(vmem::protect(reinterpret_cast<std::uintptr_t>(address), length, prot));
}

BB_HLE_FUNCTION("libkernel", "cQke9UuBQOk", sceKernelMunmap);
BB_HLE_FUNCTION("libkernel", "vSMAm3cxYTY", sceKernelMprotect);

}  // namespace bb::hle::kernel

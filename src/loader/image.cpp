#include "loader/image.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "loader/bytes.hpp"
#include "loader/elf.hpp"

namespace bb::loader {
namespace {

constexpr std::uint32_t pf_x = 1, pf_w = 2, pf_r = 4;

std::uint64_t page_size() {
    return static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
}

std::uint64_t page_down(std::uint64_t value) {
    return value & ~(page_size() - 1);
}

std::uint64_t page_up(std::uint64_t value) {
    return page_down(value + page_size() - 1);
}

int prot_from_flags(std::uint32_t flags) {
    return ((flags & pf_r) ? PROT_READ : 0) | ((flags & pf_w) ? PROT_WRITE : 0) |
           ((flags & pf_x) ? PROT_EXEC : 0);
}

// The PT_LOAD segments, checked: in the file, not overlapping, not sharing a page. Sets
// `max_align` to the largest p_align, at least the page size.
std::vector<Segment> read_segments(std::span<const std::byte> elf, std::span<const Phdr> phdrs,
                                   std::uint64_t& max_align) {
    std::vector<Segment> segments;
    max_align = page_size();
    for (const Phdr& ph : phdrs) {
        if (ph.type != pt_load) {
            continue;
        }
        if (ph.filesz > ph.memsz) {
            throw LoadError(std::format("PT_LOAD at {:#x}: p_filesz {:#x} > p_memsz {:#x}",
                                        ph.vaddr, ph.filesz, ph.memsz));
        }
        // Far beyond any PS4 image, and keeps the page arithmetic below from overflowing.
        if (!in_bounds(ph.vaddr, ph.memsz, std::uint64_t{1} << 40)) {
            throw LoadError(std::format("PT_LOAD at {:#x} + {:#x} is too large", ph.vaddr,
                                        ph.memsz));
        }
        if (ph.align & (ph.align - 1)) {
            throw LoadError(std::format("PT_LOAD at {:#x}: p_align {:#x} is not a power of two",
                                        ph.vaddr, ph.align));
        }
        need_bytes(elf, ph.offset, ph.filesz, "PT_LOAD file data");
        max_align = std::max(max_align, ph.align);
        segments.push_back(Segment{ph.vaddr, ph.memsz, ph.filesz, prot_from_flags(ph.flags)});
    }
    if (segments.empty()) {
        throw LoadError("no PT_LOAD segment");
    }
    // Protections are set per page, so two segments must not share one.
    std::vector<Segment> sorted = segments;
    std::sort(sorted.begin(), sorted.end(),
              [](const Segment& a, const Segment& b) { return a.link_addr < b.link_addr; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        const Segment& prev = sorted[i - 1];
        if (page_up(prev.link_addr + prev.mem_size) > page_down(sorted[i].link_addr)) {
            throw LoadError(std::format("PT_LOAD segments at {:#x} and {:#x} share a page",
                                        prev.link_addr, sorted[i].link_addr));
        }
    }
    return segments;
}

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

Reservation::Reservation(Reservation&& other) noexcept
    : addr_(other.addr_), size_(std::exchange(other.size_, 0)) {}

Reservation& Reservation::operator=(Reservation&& other) noexcept {
    Reservation taken(std::move(other));
    std::swap(addr_, taken.addr_);
    std::swap(size_, taken.size_);
    return *this;  // `taken` unmaps the old range
}

Reservation::~Reservation() {
    if (size_ != 0) {
        munmap(reinterpret_cast<void*>(addr_), size_);
    }
}

std::uintptr_t Image::runtime(std::uint64_t link) const {
    for (const Segment& s : segments) {
        if (link >= s.link_addr && link - s.link_addr < s.mem_size) {
            return base + link;
        }
    }
    throw std::out_of_range(std::format("link address {:#x} lies outside the image", link));
}

std::uintptr_t Image::code(std::uint64_t link) const {
    for (const Segment& s : segments) {
        if (link >= s.link_addr && link - s.link_addr < s.mem_size && (s.prot & PROT_EXEC)) {
            return base + link;
        }
    }
    throw std::out_of_range(
        std::format("link address {:#x} lies outside the executable segments", link));
}

Image map_elf(std::span<const std::byte> elf, std::uintptr_t base) {
    ElfHeader header = read_elf_header(elf);
    if (header.machine != em_x86_64 ||
        (header.type != et_sce_dynexec && header.type != et_sce_dynamic)) {
        throw LoadError(std::format("not a PS4 x86-64 executable or module (e_type {:#x})",
                                    header.type));
    }
    std::vector<Phdr> phdrs = read_phdrs(elf, header);
    std::uint64_t align = 0;
    std::vector<Segment> segments = read_segments(elf, phdrs, align);
    if (base % align != 0) {
        throw LoadError(std::format("base {:#x} is not aligned to {:#x}", base, align));
    }
    std::uint64_t span = 0;
    for (const Segment& s : segments) {
        span = std::max(span, page_up(s.link_addr + s.mem_size));
    }
    if (base + span < base) {
        throw LoadError(std::format("base {:#x} + {:#x} overflows", base, span));
    }

    // Kernels before 4.17 treat MAP_FIXED_NOREPLACE as a hint, hence the address check.
    auto range = std::format("{:#x}..{:#x}", base, base + span);
    void* want = reinterpret_cast<void*>(base);
    void* got = mmap(want, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                     -1, 0);
    if (got == MAP_FAILED) {
        throw_errno(std::format("cannot reserve {} for the image", range));
    }
    if (got != want) {
        munmap(got, span);
        throw std::system_error(std::make_error_code(std::errc::address_in_use),
                                std::format("cannot reserve {} for the image", range));
    }
    Image image{base, std::move(segments), header.entry, {}, Reservation(base, span)};

    // Anonymous pages start out zero, so the memory after the file data needs no clearing.
    for (const Segment& s : image.segments) {
        std::uint64_t start = page_down(s.link_addr), end = page_up(s.link_addr + s.mem_size);
        if (mprotect(reinterpret_cast<void*>(base + start), end - start,
                     PROT_READ | PROT_WRITE) != 0) {
            throw_errno(std::format("mprotect of {:#x}..{:#x}", base + start, base + end));
        }
    }
    for (const Phdr& ph : phdrs) {
        if (ph.type == pt_load && ph.filesz != 0) {
            std::memcpy(reinterpret_cast<void*>(base + ph.vaddr), elf.data() + ph.offset,
                        ph.filesz);
        }
    }
    image.dyn = parse_dynamic(elf, phdrs);
    return image;
}

}  // namespace bb::loader

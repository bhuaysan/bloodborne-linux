#include "loader/image.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "common/file.hpp"
#include "common/memory.hpp"
#include "common/sha256.hpp"
#include "loader/bytes.hpp"
#include "loader/elf.hpp"
#include "loader/self.hpp"

namespace bb::loader {
namespace {

constexpr std::uint32_t pf_w = 2;

using common::page_down, common::page_up;

int prot_from_flags(std::uint32_t flags) {
    return ((flags & pf_r) ? PROT_READ : 0) | ((flags & pf_w) ? PROT_WRITE : 0) |
           ((flags & pf_x) ? PROT_EXEC : 0);
}

// The PT_LOAD segments, checked: in the file, not overlapping, not sharing a page. Sets
// `max_align` to the largest p_align, at least the page size.
std::vector<Segment> read_segments(std::span<const std::byte> elf, std::span<const Phdr> phdrs,
                                   std::uint64_t& max_align) {
    std::vector<Segment> segments;
    max_align = common::page_size();
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

// Throws LoadError unless [link, link + size) lies inside the memory of one PT_LOAD.
void need_in_load(std::span<const Phdr> phdrs, std::uint64_t link, std::uint64_t size,
                  std::string_view what) {
    if (!find_load(phdrs, link, size)) {
        throw LoadError(std::format("{} at {:#x} + {:#x} lies outside the PT_LOAD segments",
                                    what, link, size));
    }
}

// mprotect over the pages of segment `s` of the image at `base`.
void protect(std::uintptr_t base, const Segment& s, int prot) {
    std::uint64_t start = page_down(s.link_addr), end = page_up(s.link_addr + s.mem_size);
    common::protect(reinterpret_cast<void*>(base + start), end - start, prot);
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

const Segment* Image::segment_at(std::uint64_t link, std::uint64_t size, int prot) const {
    for (const Segment& s : segments) {
        if ((s.prot & prot) == prot && link >= s.link_addr &&
            in_bounds(link - s.link_addr, size, s.mem_size)) {
            return &s;
        }
    }
    return nullptr;
}

std::uintptr_t Image::runtime(std::uint64_t link) const {
    if (segment_at(link)) {
        return base + link;
    }
    throw std::out_of_range(std::format("link address {:#x} lies outside the image", link));
}

std::uintptr_t Image::code(std::uint64_t link) const {
    if (segment_at(link, 1, PROT_EXEC)) {
        return base + link;
    }
    throw std::out_of_range(
        std::format("link address {:#x} lies outside the executable segments", link));
}

std::optional<std::uint64_t> Image::function_size(std::uint64_t link) const {
    if (!unwind) {
        return std::nullopt;
    }
    // read_unwind_table checked that this file data lies inside a readable segment.
    auto* data = reinterpret_cast<const std::byte*>(base + unwind->segment_link);
    return fde_range(*unwind, {data, unwind->segment_file_size}, link);
}

void Image::finalize_protection() {
    for (const Segment& s : segments) {
        protect(base, s, s.prot);
    }
    finalized_ = true;
}

std::optional<TlsTemplate> read_tls(std::span<const Phdr> phdrs) {
    const Phdr* ph = single_phdr(phdrs, pt_tls, "PT_TLS");
    if (!ph) {
        return std::nullopt;
    }
    if (ph->filesz > ph->memsz) {
        throw LoadError(std::format("PT_TLS: p_filesz {:#x} > p_memsz {:#x}", ph->filesz,
                                    ph->memsz));
    }
    // As for any ELF segment, p_align 0 and 1 both mean no alignment constraint.
    std::uint64_t align = ph->align == 0 ? 1 : ph->align;
    if (align & (align - 1)) {
        throw LoadError(std::format("PT_TLS: p_align {:#x} is not a power of two", align));
    }
    need_in_load(phdrs, ph->vaddr, ph->filesz, "PT_TLS initialization image");
    return TlsTemplate{ph->vaddr, ph->filesz, ph->memsz, align};
}

std::optional<std::uint64_t> read_proc_param(std::span<const Phdr> phdrs) {
    const Phdr* ph = single_phdr(phdrs, pt_sce_procparam, "PT_SCE_PROCPARAM");
    if (!ph) {
        return std::nullopt;
    }
    need_in_load(phdrs, ph->vaddr, ph->memsz, "PT_SCE_PROCPARAM");
    return ph->vaddr;
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
    std::optional<TlsTemplate> tls = read_tls(phdrs);
    std::optional<std::uint64_t> proc_param = read_proc_param(phdrs);
    std::optional<UnwindTable> unwind = read_unwind_table(elf, phdrs);
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
    void* got = common::map_anonymous(span, PROT_NONE,
                                      std::format("cannot reserve {} for the image", range), want,
                                      MAP_FIXED_NOREPLACE);
    if (got != want) {
        munmap(got, span);
        throw std::system_error(std::make_error_code(std::errc::address_in_use),
                                std::format("cannot reserve {} for the image", range));
    }
    Image image(base, Reservation(base, span));
    image.segments = std::move(segments);
    image.entry = header.entry;
    image.tls = tls;
    image.proc_param = proc_param;
    image.unwind = unwind;

    // Anonymous pages start out zero, so the memory after the file data needs no clearing.
    for (const Segment& s : image.segments) {
        protect(base, s, PROT_READ | PROT_WRITE);
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

Image map_eboot(const std::filesystem::path& game_dir, std::uintptr_t base) {
    std::vector<std::byte> self = common::read_file(game_dir / "eboot.bin");
    std::string digest = common::to_hex(common::sha256(self));
    if (digest != eboot_bin_sha256) {
        throw LoadError(std::format(
            "{} has SHA-256 {}, expected {} (CUSA03173 patch 1.09, see docs/clean-room.md)",
            (game_dir / "eboot.bin").string(), digest, eboot_bin_sha256));
    }
    return map_elf(self_to_elf(self), base);
}

}  // namespace bb::loader

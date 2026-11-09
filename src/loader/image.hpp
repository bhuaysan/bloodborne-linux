// A PS4 ELF mapped into this process (docs/design/runtime.md, "Loaded image").
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "loader/dynamic.hpp"
#include "loader/elf.hpp"
#include "loader/unwind.hpp"

namespace bb::loader {

// Below AddressSanitizer's shadow memory, which starts at 0x7fff8000 on x86-64 Linux.
inline constexpr std::uintptr_t default_base = 0x10000000;

// A PT_LOAD segment.
struct Segment {
    std::uint64_t link_addr, mem_size, file_size;
    int prot;  // final PROT_* from p_flags
};

// The PT_TLS segment: the initialization image at link_addr (file_size bytes, the rest of the
// block is zero), and the size and alignment of each thread's TLS block.
struct TlsTemplate {
    std::uint64_t link_addr, file_size, mem_size;
    std::uint64_t align;  // a power of two, p_align 0 read as 1
};

// An address range this process reserved with mmap. Unmaps it on destruction. Move-only.
class Reservation {
public:
    Reservation(std::uintptr_t addr, std::size_t size) : addr_(addr), size_(size) {}
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    ~Reservation();

private:
    std::uintptr_t addr_;
    std::size_t size_;  // 0 when nothing is owned
};

// The mapped image. It owns its mapping and unmaps it on destruction. Move-only: addresses taken
// from the image stay valid when it is moved, and become invalid when its owner is destroyed.
struct Image {
    std::uintptr_t base;
    std::vector<Segment> segments;  // PT_LOAD only, in program header order
    std::uint64_t entry;            // link address
    std::optional<TlsTemplate> tls;
    std::optional<std::uint64_t> proc_param;  // link address of PT_SCE_PROCPARAM, eboot only
    DynamicInfo dyn;
    std::optional<UnwindTable> unwind;  // PT_GNU_EH_FRAME, checked at map time

    // The first segment with all of `prot` whose memory holds [link, link + size), or nullptr.
    const Segment* segment_at(std::uint64_t link, std::uint64_t size = 1, int prot = 0) const;

    // base + link. Throws std::out_of_range unless `link` lies inside a segment's memory.
    std::uintptr_t runtime(std::uint64_t link) const;

    // The game function at `link`. Throws std::out_of_range unless `link` lies inside an
    // executable segment.
    template <class F>
    F* fn(std::uint64_t link) const {
        static_assert(std::is_function_v<F>);
        return reinterpret_cast<F*>(code(link));
    }

    // The size of the function at `link` from its FDE: the pc_range of the FDE that the
    // .eh_frame_hdr search table lists as starting exactly at `link`. Nothing when no listed FDE
    // starts there or the image has no PT_GNU_EH_FRAME. Throws LoadError when the listed FDE or
    // its CIE is malformed or uses an unsupported encoding (see fde_range).
    std::optional<std::uint64_t> function_size(std::uint64_t link) const;

    // Whether finalize_protection() has run.
    bool finalized() const { return finalized_; }

    // Sets each segment's pages to its final protection from p_flags (code r-x, data rw-). Until
    // then every segment is read-write. Throws std::system_error when mprotect fails.
    void finalize_protection();

    Reservation mapping;

private:
    friend Image map_elf(std::span<const std::byte> elf, std::uintptr_t base);
    Image(std::uintptr_t base_addr, Reservation reserved)
        : base(base_addr), entry(0), mapping(std::move(reserved)) {}

    std::uintptr_t code(std::uint64_t link) const;

    bool finalized_ = false;
};

// The PT_TLS program header, if any. Throws LoadError when there are several, when p_filesz
// exceeds p_memsz, when p_align is not a power of two (0 counts as 1) or when the initialization
// image [p_vaddr, p_vaddr + p_filesz) lies outside every PT_LOAD.
std::optional<TlsTemplate> read_tls(std::span<const Phdr> phdrs);

// The link address of the PT_SCE_PROCPARAM program header, if any. Throws LoadError when there
// are several or when [p_vaddr, p_vaddr + p_memsz) lies outside every PT_LOAD.
std::optional<std::uint64_t> read_proc_param(std::span<const Phdr> phdrs);

// Maps the PS4 ELF `elf` with link address 0 at `base` and parses its dynamic data. Every
// PT_LOAD is read-write until its final protection is set. `base` must be aligned to the largest
// PT_LOAD p_align. The whole span is reserved in one piece and never moved: throws
// std::system_error naming the range when it is taken, LoadError when the ELF is malformed.
// Reads PT_TLS, PT_SCE_PROCPARAM and PT_GNU_EH_FRAME. Relocations are not applied.
Image map_elf(std::span<const std::byte> elf, std::uintptr_t base);

// Reads eboot.bin from `game_dir`, checks that it is the pinned build (eboot_bin_sha256, see
// docs/source-policy.md), rebuilds its ELF and maps it at `base` with map_elf. Throws
// std::system_error when the file can't be read, LoadError when it is not the pinned build.
Image map_eboot(const std::filesystem::path& game_dir, std::uintptr_t base = default_base);

}  // namespace bb::loader

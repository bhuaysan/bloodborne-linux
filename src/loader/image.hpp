// A PS4 ELF mapped into this process (docs/design/runtime.md, "Loaded image").
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "loader/dynamic.hpp"

namespace bb::loader {

// Below AddressSanitizer's shadow memory, which starts at 0x7fff8000 on x86-64 Linux.
inline constexpr std::uintptr_t default_base = 0x10000000;

// A PT_LOAD segment.
struct Segment {
    std::uint64_t link_addr, mem_size, file_size;
    int prot;  // final PROT_* from p_flags
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
    DynamicInfo dyn;

    // base + link. Throws std::out_of_range unless `link` lies inside a segment's memory.
    std::uintptr_t runtime(std::uint64_t link) const;

    // The game function at `link`. Throws std::out_of_range unless `link` lies inside an
    // executable segment.
    template <class F>
    F* fn(std::uint64_t link) const {
        static_assert(std::is_function_v<F>);
        return reinterpret_cast<F*>(code(link));
    }

    Reservation mapping;

private:
    std::uintptr_t code(std::uint64_t link) const;
};

// Maps the PS4 ELF `elf` with link address 0 at `base` and parses its dynamic data. Every
// PT_LOAD is read-write until its final protection is set. `base` must be aligned to the largest
// PT_LOAD p_align. The whole span is reserved in one piece and never moved: throws
// std::system_error naming the range when it is taken, LoadError when the ELF is malformed.
// Relocations are not applied.
Image map_elf(std::span<const std::byte> elf, std::uintptr_t base);

}  // namespace bb::loader

// ELF header and program headers of PS4 ELFs (docs/formats/self.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bb::loader {

inline constexpr std::size_t elf_header_size = 0x40;
inline constexpr std::size_t phdr_size = 0x38;

inline constexpr std::uint16_t et_sce_dynexec = 0xFE10;  // eboot.bin
inline constexpr std::uint16_t et_sce_dynamic = 0xFE18;  // .prx
inline constexpr std::uint16_t em_x86_64 = 62;

inline constexpr std::uint32_t pt_load = 1;
inline constexpr std::uint32_t pt_dynamic = 2;
inline constexpr std::uint32_t pt_tls = 7;
inline constexpr std::uint32_t pt_gnu_eh_frame = 0x6474E550;
inline constexpr std::uint32_t pt_sce_dynlibdata = 0x61000000;
inline constexpr std::uint32_t pt_sce_procparam = 0x61000001;
inline constexpr std::uint32_t pt_sce_libversion = 0x6FFFFF01;

inline constexpr std::uint32_t pf_x = 1;  // Phdr::flags: executable
inline constexpr std::uint32_t pf_r = 4;  // Phdr::flags: readable

struct ElfHeader {
    std::uint16_t type, machine;
    std::uint64_t entry, phoff, shoff;
    std::uint16_t ehsize, phentsize, phnum, shnum;
};

struct Phdr {
    std::uint32_t type, flags;
    std::uint64_t offset, vaddr, filesz, memsz, align;
};

// The header of the ELF at the start of `data`. Throws LoadError unless it is a 64-bit
// little-endian ELF. The other fields are left to the caller to check.
ElfHeader read_elf_header(std::span<const std::byte> data);

// The program headers described by `header`. Throws LoadError when the entry size is not
// phdr_size or the table doesn't fit `data`.
std::vector<Phdr> read_phdrs(std::span<const std::byte> data, const ElfHeader& header);

// The program header of `type`, or nullptr when there is none. Throws LoadError naming `what`
// when there are several.
const Phdr* single_phdr(std::span<const Phdr> phdrs, std::uint32_t type, std::string_view what);

// The first PT_LOAD with all of `flags` whose memory holds the link addresses [addr, addr + size),
// or nullptr. An empty range may start at the end of a segment.
const Phdr* find_load(std::span<const Phdr> phdrs, std::uint64_t addr, std::uint64_t size,
                      std::uint32_t flags = 0);

}  // namespace bb::loader

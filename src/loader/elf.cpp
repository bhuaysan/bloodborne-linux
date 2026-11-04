#include "loader/elf.hpp"

#include "loader/bytes.hpp"

namespace bb::loader {

ElfHeader read_elf_header(std::span<const std::byte> data) {
    need_bytes(data, 0, elf_header_size, "ELF header");
    auto ident = [&](std::size_t i) { return std::to_integer<unsigned>(data[i]); };
    if (ident(0) != 0x7f || ident(1) != 'E' || ident(2) != 'L' || ident(3) != 'F') {
        throw LoadError("not an ELF file");
    }
    if (ident(4) != 2 || ident(5) != 1) {
        throw LoadError("not a 64-bit little-endian ELF");
    }
    auto u16 = [&](std::size_t off) { return read_le<std::uint16_t>(data, off, "ELF header"); };
    auto u64 = [&](std::size_t off) { return read_le<std::uint64_t>(data, off, "ELF header"); };
    return ElfHeader{u16(0x10), u16(0x12), u64(0x18), u64(0x20), u64(0x28),
                     u16(0x34), u16(0x36), u16(0x38), u16(0x3C)};
}

std::vector<Phdr> read_phdrs(std::span<const std::byte> data, const ElfHeader& header) {
    if (header.phentsize != phdr_size) {
        throw LoadError(std::format("unexpected program header size {:#x}", header.phentsize));
    }
    need_bytes(data, header.phoff, std::uint64_t{header.phnum} * phdr_size, "program headers");
    std::vector<Phdr> out;
    for (std::uint64_t i = 0; i < header.phnum; ++i) {
        std::uint64_t at = header.phoff + i * phdr_size;
        auto u32 = [&](std::uint64_t off) {
            return read_le<std::uint32_t>(data, at + off, "program header");
        };
        auto u64 = [&](std::uint64_t off) {
            return read_le<std::uint64_t>(data, at + off, "program header");
        };
        // p_paddr at 0x18 is unused.
        out.push_back(Phdr{u32(0x00), u32(0x04), u64(0x08), u64(0x10), u64(0x20), u64(0x28),
                           u64(0x30)});
    }
    return out;
}

}  // namespace bb::loader

// The dynamic linking data of PS4 ELFs: the tables in PT_SCE_DYNLIBDATA
// (docs/formats/elf-dynamic.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "loader/elf.hpp"

namespace bb::loader {

inline constexpr std::uint32_t r_x86_64_64 = 1;
inline constexpr std::uint32_t r_x86_64_glob_dat = 6;
inline constexpr std::uint32_t r_x86_64_jump_slot = 7;
inline constexpr std::uint32_t r_x86_64_relative = 8;

// An Elf64_Sym with its name read from the string table.
struct Symbol {
    std::string name;  // the name as stored, e.g. NID#library#module
    std::uint8_t type, bind;
    std::uint16_t shndx;  // 0 for undefined symbols
    std::uint64_t value;  // link address for defined symbols
};

struct Rela {
    std::uint64_t offset;  // link address of the target, checked when it is applied
    std::uint32_t type;
    std::uint32_t sym;  // index into DynamicInfo::symbols, checked
    std::int64_t addend;
};

struct DynamicInfo {
    std::vector<Symbol> symbols;   // in table order, so relocations index it
    std::vector<Rela> plt_relocs;  // DT_SCE_JMPREL
    std::vector<Rela> relocs;      // DT_SCE_RELA
};

// The dynamic data of the PS4 ELF `elf` with program headers `phdrs`. Every offset, size, count
// and string is checked. Throws LoadError naming the first problem.
DynamicInfo parse_dynamic(std::span<const std::byte> elf, std::span<const Phdr> phdrs);

}  // namespace bb::loader

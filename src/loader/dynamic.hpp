// The dynamic linking data of PS4 ELFs: the tables and records in PT_SCE_DYNLIBDATA
// (docs/formats/elf-dynamic.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "loader/elf.hpp"

namespace bb::loader {

inline constexpr std::uint32_t r_x86_64_64 = 1;
inline constexpr std::uint32_t r_x86_64_glob_dat = 6;
inline constexpr std::uint32_t r_x86_64_jump_slot = 7;
inline constexpr std::uint32_t r_x86_64_relative = 8;

// A loadable file (the eboot, a .prx) as a module record names it.
struct Module {
    std::uint16_t id;       // 0 for the file itself, 1..n for the needed modules
    std::string name;       // without .prx, can differ from the file name
    std::uint16_t version;  // two bytes, read as major.minor
    std::string file;       // the matching DT_NEEDED entry, empty for the file itself
};

// A named set of exports inside a module, as a library record names it.
struct Library {
    std::uint16_t id;  // numbered separately for imported and exported libraries
    std::string name;
    std::uint16_t version;
    bool exported;
    std::optional<std::uint32_t> attr;  // from the attribute record, meaning unknown
};

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
    Module self;                        // DT_SCE_MODULE_INFO
    std::optional<std::string> soname;  // PRX only
    std::vector<Module> needed;         // ids 1..n, in DT_NEEDED order
    std::vector<Library> libraries;     // imported and exported, in table order
    std::vector<Symbol> symbols;        // in table order, so relocations index it
    std::vector<Rela> plt_relocs;       // DT_SCE_JMPREL
    std::vector<Rela> relocs;           // DT_SCE_RELA
    std::uint64_t pltgot = 0;           // DT_SCE_PLTGOT, link address
    bool textrel = false;               // DT_TEXTREL
    // DT_INIT, DT_FINI and the init and fini arrays follow in the next part of issue #43.
};

// The dynamic data of the PS4 ELF `elf` with program headers `phdrs`. Every offset, size, count,
// string and record is checked, the records as tools/elfimports checks them.
// Throws LoadError naming the first problem.
DynamicInfo parse_dynamic(std::span<const std::byte> elf, std::span<const Phdr> phdrs);

}  // namespace bb::loader

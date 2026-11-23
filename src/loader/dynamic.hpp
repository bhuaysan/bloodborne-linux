// The dynamic linking data of PS4 ELFs: the tables and records in PT_SCE_DYNLIBDATA
// (docs/formats/elf-dynamic.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "loader/elf.hpp"

namespace bb::loader {

inline constexpr std::uint8_t stt_notype = 0;
inline constexpr std::uint8_t stt_object = 1;
inline constexpr std::uint8_t stt_func = 2;
inline constexpr std::uint8_t stt_section = 3;
inline constexpr std::uint8_t stt_tls = 6;
inline constexpr std::uint8_t stb_weak = 2;

inline constexpr std::uint32_t r_x86_64_64 = 1;
inline constexpr std::uint32_t r_x86_64_glob_dat = 6;
inline constexpr std::uint32_t r_x86_64_jump_slot = 7;
inline constexpr std::uint32_t r_x86_64_relative = 8;
inline constexpr std::uint32_t r_x86_64_dtpmod64 = 16;

// A dynamic tag with its name for error messages. It converts to its value; pass `.value` where
// a template would deduce the type (put, std::pair{...}).
struct DynamicTag {
    std::uint64_t value;
    std::string_view name;
    constexpr operator std::uint64_t() const { return value; }
};

inline constexpr DynamicTag dt_needed{1, "DT_NEEDED"};
inline constexpr DynamicTag dt_rela{7, "DT_RELA"};
inline constexpr DynamicTag dt_init{0x0C, "DT_INIT"};
inline constexpr DynamicTag dt_fini{0x0D, "DT_FINI"};
inline constexpr DynamicTag dt_soname{0x0E, "DT_SONAME"};
inline constexpr DynamicTag dt_textrel{0x16, "DT_TEXTREL"};
inline constexpr DynamicTag dt_init_array{0x19, "DT_INIT_ARRAY"};
inline constexpr DynamicTag dt_fini_array{0x1A, "DT_FINI_ARRAY"};
inline constexpr DynamicTag dt_init_arraysz{0x1B, "DT_INIT_ARRAYSZ"};
inline constexpr DynamicTag dt_fini_arraysz{0x1C, "DT_FINI_ARRAYSZ"};
inline constexpr DynamicTag dt_preinit_array{0x20, "DT_PREINIT_ARRAY"};
inline constexpr DynamicTag dt_preinit_arraysz{0x21, "DT_PREINIT_ARRAYSZ"};

// Sony tags, our names (docs/formats/elf-dynamic.md).
inline constexpr DynamicTag dt_sce_module_info{0x6100000D, "DT_SCE_MODULE_INFO"};
inline constexpr DynamicTag dt_sce_needed_module{0x6100000F, "DT_SCE_NEEDED_MODULE"};
inline constexpr DynamicTag dt_sce_export_lib{0x61000013, "DT_SCE_EXPORT_LIB"};
inline constexpr DynamicTag dt_sce_import_lib{0x61000015, "DT_SCE_IMPORT_LIB"};
inline constexpr DynamicTag dt_sce_export_lib_attr{0x61000017, "DT_SCE_EXPORT_LIB_ATTR"};
inline constexpr DynamicTag dt_sce_import_lib_attr{0x61000019, "DT_SCE_IMPORT_LIB_ATTR"};
inline constexpr DynamicTag dt_sce_pltgot{0x61000027, "DT_SCE_PLTGOT"};
inline constexpr DynamicTag dt_sce_jmprel{0x61000029, "DT_SCE_JMPREL"};
inline constexpr DynamicTag dt_sce_pltrel{0x6100002B, "DT_SCE_PLTREL"};
inline constexpr DynamicTag dt_sce_pltrelsz{0x6100002D, "DT_SCE_PLTRELSZ"};
inline constexpr DynamicTag dt_sce_rela{0x6100002F, "DT_SCE_RELA"};
inline constexpr DynamicTag dt_sce_relasz{0x61000031, "DT_SCE_RELASZ"};
inline constexpr DynamicTag dt_sce_relaent{0x61000033, "DT_SCE_RELAENT"};
inline constexpr DynamicTag dt_sce_strtab{0x61000035, "DT_SCE_STRTAB"};
inline constexpr DynamicTag dt_sce_strsz{0x61000037, "DT_SCE_STRSZ"};
inline constexpr DynamicTag dt_sce_symtab{0x61000039, "DT_SCE_SYMTAB"};
inline constexpr DynamicTag dt_sce_syment{0x6100003B, "DT_SCE_SYMENT"};
inline constexpr DynamicTag dt_sce_symtabsz{0x6100003F, "DT_SCE_SYMTABSZ"};

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
    std::optional<std::uint32_t> attr;    // from the attribute record, meaning unknown
    std::optional<std::uint16_t> module;  // module id of the symbols that use it, if any
};

// A NID#library#module symbol name decoded through the records. Imports go to the resolver.
struct Import {
    std::string nid;              // 11 characters
    std::string library, module;  // names from the library and module records
    std::uint8_t type;            // STT_FUNC or STT_OBJECT for imports
};

// An Elf64_Sym with its name read from the string table.
struct Symbol {
    enum class Kind : std::uint8_t {
        none,        // index 0 and the unnamed section symbol
        import,      // undefined, of an imported library
        exported,    // defined, of an exported library
        plain_weak,  // module_start or module_stop: weak, untyped, undefined, never an import
    };
    std::string name;  // the name as stored, e.g. NID#library#module
    std::uint8_t type, bind;
    std::uint16_t shndx;  // 0 for undefined symbols
    std::uint64_t value;  // link address for defined symbols
    Kind kind;
    Import decoded;  // for imports and exports
};

struct Rela {
    std::uint64_t offset;  // link address of the target, checked when it is applied
    std::uint32_t type;
    std::uint32_t sym;  // index into DynamicInfo::symbols, checked
    std::int64_t addend;
};

// An array of function pointers, from an address tag and its size tag.
struct FuncArray {
    std::uint64_t addr;  // link address, addr..addr+size lies in one PT_LOAD's memory
    std::uint64_t size;  // in bytes, a multiple of 8, often 0
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
    // Link addresses inside an executable PT_LOAD. 0 is a valid DT_INIT in PRXs.
    std::optional<std::uint64_t> init, fini;  // DT_INIT, DT_FINI
    std::optional<FuncArray> preinit_array, init_array, fini_array;
};

// The dynamic data of the PS4 ELF `elf` with program headers `phdrs`. Every offset, size, count,
// string, record and symbol name is checked, the records as tools/elfimports checks them.
// Throws LoadError naming the first problem.
DynamicInfo parse_dynamic(std::span<const std::byte> elf, std::span<const Phdr> phdrs);

}  // namespace bb::loader

// What the game tests expect of eboot.bin of CUSA03173 patch 1.09 and of the PRXs it loads with,
// written once for all of them (#71). Link addresses are relative to the image's base. The
// counts are those that tools/elfimports and tools/common/ps4elf.py report for eboot.elf.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bb::test::eboot_facts {

inline constexpr std::uint64_t entry = 0xA0;

struct Segment {
    std::uint64_t link_addr, mem_size, file_size;
};
inline constexpr Segment code{0, 0x50D96DC, 0x50D96DC};    // r-x
inline constexpr Segment data{0x50DC000, 0x5F70F4, 0x309B38};  // rw-, .bss after its file data
// The end of the mapped image: the data segment's memory rounded up to the page.
inline constexpr std::uint64_t image_end = 0x56D4000;

struct Tls {
    std::uint64_t link_addr, file_size, mem_size, align;
};
inline constexpr Tls tls{0x50DC040, 0, 0x750, 0x10};
inline constexpr std::uint64_t proc_param = 0x50DC000;  // PT_SCE_PROCPARAM

// Relocations by type. DT_JMPREL holds the JUMP_SLOT ones, DT_RELA the others.
inline constexpr std::size_t relocs_64 = 2838;
inline constexpr std::size_t relocs_glob_dat = 23;
inline constexpr std::size_t relocs_jump_slot = 661;
inline constexpr std::size_t relocs_relative = 231478;

// The dynamic symbols: 701 imports (681 functions, 20 objects) and 2 others. 686 imports are
// referenced by a relocation, all 20 objects among them; 14 of the others are libc imports.
inline constexpr std::size_t symbols = 703;
inline constexpr std::size_t imports = 701;
inline constexpr std::size_t function_imports = 681;
inline constexpr std::size_t object_imports = 20;
inline constexpr std::size_t referenced_imports = 686;
inline constexpr std::size_t referenced_function_imports = referenced_imports - object_imports;
inline constexpr std::size_t unreferenced_libc_imports = 14;

// The imports by library, in the order of the summary of tools/elfimports.
struct LibraryImports {
    std::string_view library;
    int functions, objects;
};
inline constexpr std::array<LibraryImports, 39> library_imports = {{
    {"libc", 197, 19},
    {"libkernel", 79, 1},
    {"libSceGnmDriver", 50, 0},
    {"libSceNet", 35, 0},
    {"libScePosix", 32, 0},
    {"libSceNpMatching2", 27, 0},
    {"libSceHttp", 24, 0},
    {"libSceNpManager", 20, 0},
    {"libSceFios2", 17, 0},
    {"libSceNpWebApi", 17, 0},
    {"libSceAvPlayer", 16, 0},
    {"libSceNpScore", 14, 0},
    {"libSceAjm", 12, 0},
    {"libSceSaveData", 11, 0},
    {"libSceVoice", 11, 0},
    {"libSceRtc", 10, 0},
    {"libScePad", 9, 0},
    {"libSceVideoOut", 8, 0},
    {"libSceNpSignaling", 7, 0},
    {"libSceNpUtility", 7, 0},
    {"libSceAudioOut", 6, 0},
    {"libSceNetCtl", 6, 0},
    {"libSceNpTrophy", 6, 0},
    {"libSceUserService", 6, 0},
    {"libSceImeDialog", 5, 0},
    {"libSceNpProfileDialog", 5, 0},
    {"libScePlayGo", 5, 0},
    {"libSceSystemService", 5, 0},
    {"libSceMouse", 4, 0},
    {"libSceMsgDialog", 4, 0},
    {"libSceNpAuth", 4, 0},
    {"libSceNpCommerce", 4, 0},
    {"libSceSaveDataDialog", 4, 0},
    {"libSceAppContent", 3, 0},
    {"libSceAudioIn", 3, 0},
    {"libSceSysmodule", 3, 0},
    {"libSceNpCommon", 2, 0},
    {"libSceSsl", 2, 0},
    {"libSceCommonDialog", 1, 0},
}};

// The entry of `library` in library_imports; {} for a library the eboot doesn't import from.
constexpr LibraryImports imports_of(std::string_view library) {
    for (const LibraryImports& entry : library_imports) {
        if (entry.library == library) {
            return entry;
        }
    }
    return {};
}

// Functions from the unwind tables (tools/noreturn's fde_ranges over eboot.elf).
struct Function {
    std::uint64_t link_addr, size;
};
// Long enough for a hook; starts with c7 47 08 01 (mov dword [rdi+8], 1).
inline constexpr Function hookable_function{0xF0, 0x77};
// Shorter than a hook patch: mov al, 1; ret.
inline constexpr Function short_function{0x34F0, 3};

// The load set (loader::map_load_set): libSceFios2.prx right after the eboot, then libc.prx.
inline constexpr std::uint64_t fios2_offset = image_end;  // from the eboot's base
inline constexpr std::uint64_t libc_offset = fios2_offset + 0x178000;
inline constexpr std::size_t fios2_dtpmod_relocs = 3;  // R_X86_64_DTPMOD64
inline constexpr std::size_t libc_dtpmod_relocs = 14;

}  // namespace bb::test::eboot_facts

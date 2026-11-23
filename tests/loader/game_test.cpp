// The loader's tests against the eboot, in eboot_game_test with those of the HLE and the hook
// (tests/CMakeLists.txt): they read eboot.bin once per process (support/eboot_file.hpp) and map
// its ELF with map_elf, where map_eboot would read and rebuild it again.

#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/sha256.hpp"
#include "loader/dynamic.hpp"
#include "loader/elf.hpp"
#include "loader/image.hpp"
#include "loader/load_set.hpp"
#include "loader/relocate.hpp"
#include "loader/self.hpp"
#include "support/eboot_facts.hpp"
#include "support/eboot_file.hpp"
#include "support/elf_builder.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::loader;
using bb::test::eboot_file;
using bb::test::protection;
namespace facts = bb::test::eboot_facts;

std::uint64_t slot(const Image& image, std::uint64_t link) {
    std::uint64_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(image.runtime(link)), sizeof value);
    return value;
}

TEST(SelfToElfGame, RebuildsThePinnedEboot) {
    BB_REQUIRE_GAME(game);
    const auto& self = eboot_file(game).self;
    ASSERT_EQ(bb::common::to_hex(bb::common::sha256(self)), bb::loader::eboot_bin_sha256);
    // eboot.elf in PINNED_ELFS of tools/common/ps4elf.py: the output of tools/self2elf. eboot_file
    // rebuilt it with self_to_elf.
    EXPECT_EQ(bb::common::to_hex(bb::common::sha256(eboot_file(game).elf)),
              "941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7");
}

TEST(MapEbootGame, FindsFunctionSizes) {
    BB_REQUIRE_GAME(game);
    Image image = map_elf(eboot_file(game).elf, default_base);
    ASSERT_TRUE(image.unwind);
    EXPECT_EQ(image.unwind->link_addr, 0x4F41510U);
    EXPECT_EQ(image.unwind->fde_count, 162959U);
    // Sizes from tools/noreturn's fde_ranges over eboot.elf: the first and last table entries,
    // two in between and two functions shorter than a hook patch (mov al, 1; ret and ret).
    const std::pair<std::uint64_t, std::uint64_t> sizes[] = {
        {0xA0, 0x4F},
        {facts::hookable_function.link_addr, facts::hookable_function.size},
        {0x190A950, 0xC3},
        {0x2BBE370, 0x135},
        {facts::short_function.link_addr, facts::short_function.size},
        {0x6600, 1}};
    for (auto [link, size] : sizes) {
        EXPECT_EQ(image.function_size(link), size) << link;
    }
    for (std::uint64_t miss : {0xA1UL, 0x2BBE371UL, 0x4F41510UL, 0x50DC000UL}) {
        EXPECT_FALSE(image.function_size(miss)) << miss;
    }
}

TEST(MapEbootGame, MapsTheEbootAtTheDefaultBase) {
    BB_REQUIRE_GAME(game);
    Image image = map_elf(eboot_file(game).elf, default_base);
    EXPECT_EQ(image.base, default_base);
    EXPECT_EQ(image.entry, facts::entry);
    ASSERT_EQ(image.segments.size(), 2U);
    const Segment& code = image.segments[0];
    EXPECT_EQ(code.link_addr, facts::code.link_addr);
    EXPECT_EQ(code.mem_size, facts::code.mem_size);
    EXPECT_EQ(code.file_size, facts::code.file_size);
    EXPECT_EQ(code.prot, PROT_READ | PROT_EXEC);
    const Segment& data = image.segments[1];
    EXPECT_EQ(data.link_addr, facts::data.link_addr);
    EXPECT_EQ(data.mem_size, facts::data.mem_size);
    EXPECT_EQ(data.file_size, facts::data.file_size);
    EXPECT_EQ(data.prot, PROT_READ | PROT_WRITE);
    ASSERT_TRUE(image.tls);
    EXPECT_EQ(image.tls->link_addr, facts::tls.link_addr);
    EXPECT_EQ(image.tls->file_size, facts::tls.file_size);
    EXPECT_EQ(image.tls->mem_size, facts::tls.mem_size);
    EXPECT_EQ(image.tls->align, facts::tls.align);
    EXPECT_EQ(image.proc_param, facts::proc_param);

    image.finalize_protection();
    EXPECT_EQ(protection(default_base), "r-xp");
    EXPECT_EQ(protection(default_base + facts::code.mem_size - 1), "r-xp");
    EXPECT_EQ(protection(default_base + 0x50DB000), "---p");  // gap before the data segment
    EXPECT_EQ(protection(default_base + facts::data.link_addr), "rw-p");
    EXPECT_EQ(protection(default_base + facts::data.link_addr + facts::data.mem_size - 1), "rw-p");
    EXPECT_EQ(protection(default_base + facts::image_end), "unmapped");
}

TEST(ParseDynamicGame, ReadsTheEboot) {
    BB_REQUIRE_GAME(game);
    const auto& elf = eboot_file(game).elf;
    auto phdrs = read_phdrs(elf, read_elf_header(elf));
    DynamicInfo info = parse_dynamic(elf, phdrs);

    EXPECT_EQ(info.symbols.size(), facts::symbols);
    auto undefined = [&](std::uint8_t type) {
        return static_cast<std::size_t>(
            std::count_if(info.symbols.begin(), info.symbols.end(), [&](const Symbol& s) {
                return s.shndx == 0 && s.type == type && s.name.find('#') != std::string::npos;
            }));
    };
    EXPECT_EQ(undefined(2), facts::function_imports);  // STT_FUNC imports
    EXPECT_EQ(undefined(1), facts::object_imports);    // STT_OBJECT imports
    auto count = [](const std::vector<Rela>& relas, std::uint32_t type) {
        return static_cast<std::size_t>(std::count_if(
            relas.begin(), relas.end(), [&](const Rela& r) { return r.type == type; }));
    };
    EXPECT_EQ(info.plt_relocs.size(), facts::relocs_jump_slot);
    EXPECT_EQ(count(info.plt_relocs, r_x86_64_jump_slot), facts::relocs_jump_slot);
    EXPECT_EQ(info.relocs.size(),
              facts::relocs_relative + facts::relocs_64 + facts::relocs_glob_dat);
    EXPECT_EQ(count(info.relocs, r_x86_64_relative), facts::relocs_relative);
    EXPECT_EQ(count(info.relocs, r_x86_64_64), facts::relocs_64);
    EXPECT_EQ(count(info.relocs, r_x86_64_glob_dat), facts::relocs_glob_dat);

    EXPECT_EQ(info.self.id, 0);
    EXPECT_FALSE(info.soname);
    EXPECT_TRUE(info.textrel);
    EXPECT_EQ(info.init, 0x20U);
    EXPECT_EQ(info.fini, 0x2ba8220U);
    for (const auto& array : {info.preinit_array, info.init_array, info.fini_array}) {
        ASSERT_TRUE(array);
        EXPECT_EQ(array->addr, 0x53e5b38U);  // all three empty, at the same address
        EXPECT_EQ(array->size, 0U);
    }
    EXPECT_EQ(info.needed.size(), 42U);
    auto exported = [](const Library& lib) { return lib.exported; };
    EXPECT_EQ(std::count_if(info.libraries.begin(), info.libraries.end(), exported), 0);
    EXPECT_EQ(info.libraries.size(), 43U);
    auto unused = [](const Library& lib) { return !lib.module; };
    EXPECT_EQ(std::count_if(info.libraries.begin(), info.libraries.end(), unused),
              43 - static_cast<int>(facts::library_imports.size()));
    auto imports = [&](std::string_view library, std::uint8_t type) {
        return std::count_if(info.symbols.begin(), info.symbols.end(), [&](const Symbol& s) {
            return s.kind == Symbol::Kind::import && s.decoded.library == library &&
                   s.decoded.type == type;
        });
    };
    auto kind = [&](Symbol::Kind k) {
        return static_cast<std::size_t>(
            std::count_if(info.symbols.begin(), info.symbols.end(),
                          [&](const Symbol& s) { return s.kind == k; }));
    };
    EXPECT_EQ(kind(Symbol::Kind::import), facts::imports);
    EXPECT_EQ(kind(Symbol::Kind::none), facts::symbols - facts::imports);
    EXPECT_EQ(imports("libc", stt_func), facts::imports_of("libc").functions);
    EXPECT_EQ(imports("libc", stt_object), facts::imports_of("libc").objects);
    EXPECT_EQ(imports("libkernel", stt_func), facts::imports_of("libkernel").functions);
    EXPECT_EQ(imports("libkernel", stt_object), facts::imports_of("libkernel").objects);
    EXPECT_EQ(imports("libScePosix", stt_func), facts::imports_of("libScePosix").functions);
    EXPECT_EQ(imports("libSceAppContent", stt_func),
              facts::imports_of("libSceAppContent").functions);
    // Library names and the module and file names they come from.
    auto module_of = [&](std::string_view library) -> const Module* {
        auto lib = std::find_if(info.libraries.begin(), info.libraries.end(),
                                [&](const Library& l) { return l.name == library; });
        if (lib == info.libraries.end() || !lib->module || *lib->module == 0 ||
            *lib->module > info.needed.size()) {
            return nullptr;
        }
        return &info.needed[*lib->module - 1];
    };
    const Module* posix = module_of("libScePosix");
    const Module* app_content = module_of("libSceAppContent");
    ASSERT_TRUE(posix);
    ASSERT_TRUE(app_content);
    EXPECT_EQ(posix->name, "libkernel");
    EXPECT_EQ(app_content->name, "libSceAppContentUtil");
    EXPECT_EQ(app_content->file, "libSceAppContent.prx");
}

TEST(RelocateGame, RelocatesTheEboot) {
    BB_REQUIRE_GAME(game);
    Image image = map_elf(eboot_file(game).elf, default_base);
    std::map<std::string, std::uintptr_t> fakes;  // a distinct address per NID#library
    relocate(image, [&](const Import& imp) {
        std::uintptr_t next = 0x7e00'0000'0000 + 0x20 * (fakes.size() + 1);
        return fakes.try_emplace(imp.nid + '#' + imp.library, next).first->second;
    }, eboot_module_id);
    EXPECT_EQ(fakes.size(), facts::referenced_imports);

    for (const Rela& r : image.dyn.plt_relocs) {
        const Import& imp = image.dyn.symbols[r.sym].decoded;
        ASSERT_EQ(slot(image, r.offset), fakes.at(imp.nid + '#' + imp.library)) << r.offset;
    }
    EXPECT_EQ(slot(image, 0x513b330), default_base + 0x5540528);  // the first R_X86_64_RELATIVE
    // No target in the code segment despite DT_TEXTREL, and imported data is used at offsets
    // 0 and 0x10 only (docs/design/runtime.md, "Data imports").
    for (const Rela& r : image.dyn.relocs) {
        ASSERT_GE(r.offset, image.segments[1].link_addr);
        if (r.type == r_x86_64_64 && image.dyn.symbols[r.sym].type == stt_object) {
            ASSERT_TRUE(r.addend == 0 || r.addend == 0x10) << r.offset;
        }
    }
    image.finalize_protection();
}

// Maps eboot, libSceFios2 and libc one after the other, with module ids 1 to 3, and relocates
// them with fake imports. libc.prx also exports 4 STT_TLS symbols, which exports() skips.
TEST(LoadSetGame, MapsAndRelocatesTheLoadSet) {
    BB_REQUIRE_GAME(game);
    LoadSet set = map_load_set(game, eboot_file(game).elf);
    ASSERT_EQ(set.modules.size(), 3U);
    const LoadedModule& eboot = set.modules[0];
    const LoadedModule& fios2 = set.modules[1];
    const LoadedModule& libc = set.modules[2];
    EXPECT_EQ(&set.eboot(), &eboot);
    EXPECT_EQ(eboot.file, "eboot.bin");
    EXPECT_EQ(eboot.id, eboot_module_id);
    EXPECT_EQ(eboot.image.base, default_base);
    EXPECT_EQ(fios2.file, "libSceFios2.prx");
    EXPECT_EQ(fios2.id, 2U);
    EXPECT_EQ(fios2.image.base, default_base + facts::fios2_offset);  // after the eboot's .bss
    EXPECT_EQ(fios2.image.dyn.self.name, "libSceFios2");
    EXPECT_EQ(libc.file, "libc.prx");
    EXPECT_EQ(libc.id, 3U);
    EXPECT_EQ(libc.image.base, default_base + facts::libc_offset);
    EXPECT_EQ(libc.image.dyn.self.name, "libc");
    EXPECT_TRUE(exports(eboot.image).empty());
    EXPECT_EQ(exports(fios2.image).size(), 165U);  // as tools/elfimports counts them
    EXPECT_EQ(exports(libc.image).size(), 1597U + 539U);

    for (LoadedModule& m : set.modules) {
        relocate(m.image, [](const Import&) { return std::uintptr_t{0x7e00'0000'0000}; }, m.id);
        m.image.finalize_protection();
    }
    std::size_t dtpmod = 0;  // each holding the module id
    for (const LoadedModule* m : {&fios2, &libc}) {
        for (const Rela& r : m->image.dyn.relocs) {
            if (r.type == r_x86_64_dtpmod64) {
                ++dtpmod;
                EXPECT_EQ(slot(m->image, r.offset), m->id) << m->file;
            }
        }
    }
    EXPECT_EQ(dtpmod, facts::fios2_dtpmod_relocs + facts::libc_dtpmod_relocs);
}

}  // namespace

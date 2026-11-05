#include "loader/dynamic.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/file.hpp"
#include "loader/bytes.hpp"
#include "loader/elf.hpp"
#include "loader/image.hpp"
#include "loader/relocate.hpp"
#include "loader/self.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::loader;
using Bytes = std::vector<std::byte>;
using Dyn = std::pair<std::uint64_t, std::uint64_t>;

constexpr std::uint64_t dt_sce_pltrel = 0x6100002B, dt_sce_pltrelsz = 0x6100002D,
                        dt_sce_relaent = 0x61000033, dt_sce_strtab = 0x61000035,
                        dt_sce_strsz = 0x61000037, dt_sce_symtab = 0x61000039,
                        dt_sce_syment = 0x6100003B, dt_sce_symtabsz = 0x6100003F;
constexpr std::uint64_t dt_init = 0x0C, dt_fini = 0x0D, dt_init_array = 0x19,
                        dt_init_arraysz = 0x1B, dt_preinit_array = 0x20,
                        dt_preinit_arraysz = 0x21;
constexpr std::uint64_t dt_needed = 1, dt_soname = 0x0E, dt_sce_module_info = 0x6100000D,
                        dt_sce_needed_module = 0x6100000F, dt_sce_export_lib = 0x61000013,
                        dt_sce_import_lib = 0x61000015, dt_sce_export_lib_attr = 0x61000017,
                        dt_sce_import_lib_attr = 0x61000019;
constexpr std::size_t dynlib_offset = 0x3000;  // file offset of PT_SCE_DYNLIBDATA

// A module or library record.
constexpr std::uint64_t record(std::uint64_t id, std::uint64_t version, std::uint32_t low) {
    return id << 48 | version << 32 | low;
}

template <class T>
void put(Bytes& out, std::size_t offset, T value) {
    if (out.size() < offset + sizeof value) {
        out.resize(offset + sizeof value);
    }
    std::memcpy(out.data() + offset, &value, sizeof value);
}

struct Elf {
    Bytes bytes;
    std::vector<Phdr> phdrs;
};

// A minimal PS4 ELF: an executable and a writable PT_LOAD and a PT_SCE_DYNLIBDATA segment at the
// end of the file with the tables and the dynamic table. It needs modules libFoo (id 1) and
// libBar (id 2), imports library libFoo (id 0) from libFoo, declares library libBar (id 1)
// without importing from it, and exports library libSelf (id 0). It has DT_INIT and the preinit
// and init arrays, but no DT_FINI and no fini array. The empty preinit array sits at the end of
// the writable PT_LOAD.
struct Synthetic {
    Bytes strtab{std::byte{0}};
    Bytes symtab, jmprel, rela;
    std::vector<Dyn> records;

    Synthetic() {
        add_symbol(0, 0, 0, 0);
        add_symbol(str("AAAAAAAAAAA#A#B"), 0x12, 0, 0);  // global function, undefined
        add_symbol(str("module_start"), 0x20, 0, 0);     // weak, untyped
        add_symbol(0, 0x03, 1, 0);                       // unnamed local section symbol
        add_symbol(str("BBBBBBBBBBB#A#A"), 0x11, 1, 0x4100);  // global object, defined
        add_symbol(str("CCCCCCCCC+-#A#B"), 0x11, 0, 0);       // global object, undefined
        add_rela(jmprel, 0x4008, r_x86_64_jump_slot, 1, 0);
        add_rela(rela, 0x4010, r_x86_64_relative, 0, 0x40);
        add_rela(rela, 0x4018, r_x86_64_64, 1, -8);
        records = {{dt_sce_module_info, record(0, 0x0101, str("self"))},
                   {dt_needed, str("libFoo.prx")},
                   {dt_needed, str("libBar_x.prx")},
                   {dt_sce_needed_module, record(2, 0x0009, str("libBar"))},
                   {dt_sce_needed_module, record(1, 0x0101, str("libFoo"))},
                   {dt_sce_import_lib, record(0, 1, str("libFoo"))},
                   {dt_sce_import_lib, record(1, 1, str("libBar"))},
                   {dt_sce_export_lib, record(0, 1, str("libSelf"))},
                   {dt_sce_import_lib_attr, record(1, 0, 9)},
                   {dt_sce_export_lib_attr, record(0, 0, 1)},
                   {dt_soname, str("self.prx")},
                   {0x16, 0},  // DT_TEXTREL
                   {dt_init, 0x200},
                   {dt_init_array, 0x4300},
                   {dt_init_arraysz, 0x10},
                   {dt_preinit_array, 0x6000},
                   {dt_preinit_arraysz, 0}};
    }

    std::uint32_t str(std::string_view s) {
        auto offset = static_cast<std::uint32_t>(strtab.size());
        for (char c : s) {
            strtab.push_back(static_cast<std::byte>(c));
        }
        strtab.push_back(std::byte{0});
        return offset;
    }

    void add_symbol(std::uint32_t name, std::uint8_t info, std::uint16_t shndx,
                    std::uint64_t value) {
        std::size_t at = symtab.size();
        put(symtab, at, name);
        put(symtab, at + 4, info);
        put(symtab, at + 5, std::uint8_t{0});
        put(symtab, at + 6, shndx);
        put(symtab, at + 8, value);
        put(symtab, at + 16, std::uint64_t{0});
    }

    static void add_rela(Bytes& table, std::uint64_t offset, std::uint32_t type, std::uint32_t sym,
                         std::int64_t addend) {
        std::size_t at = table.size();
        put(table, at, offset);
        put(table, at + 8, std::uint64_t{sym} << 32 | type);
        put(table, at + 16, addend);
    }

    // `edit` may change the dynamic entries (all but DT_NULL) before they are written.
    Elf build(const std::function<void(std::vector<Dyn>&)>& edit = {}) const {
        Bytes blob = strtab;
        auto append = [&](const Bytes& table) {
            blob.resize((blob.size() + 7) & ~std::size_t{7});
            std::uint64_t offset = blob.size();
            blob.insert(blob.end(), table.begin(), table.end());
            return offset;
        };
        std::vector<Dyn> dyn{{0x61000007, 0},  // unknown, ignored
                             {dt_sce_strtab, 0},
                             {dt_sce_strsz, strtab.size()},
                             {dt_sce_symtab, append(symtab)},
                             {dt_sce_symtabsz, symtab.size()},
                             {dt_sce_syment, 24},
                             {0x61000029, append(jmprel)},  // DT_SCE_JMPREL
                             {dt_sce_pltrelsz, jmprel.size()},
                             {dt_sce_pltrel, 7},
                             {0x6100002F, append(rela)},  // DT_SCE_RELA
                             {0x61000031, rela.size()},   // DT_SCE_RELASZ
                             {dt_sce_relaent, 24},
                             {0x61000027, 0x4000}};  // DT_SCE_PLTGOT
        dyn.insert(dyn.begin(), records.begin(), records.end());
        if (edit) {
            edit(dyn);
        }
        dyn.emplace_back(0, 0);
        std::uint64_t dyn_offset = append({});
        for (auto [tag, value] : dyn) {
            put(blob, blob.size(), tag);
            put(blob, blob.size(), value);
        }
        std::uint64_t dyn_size = dyn.size() * 16;

        Elf elf;
        elf.bytes.resize(dynlib_offset);
        elf.bytes.insert(elf.bytes.end(), blob.begin(), blob.end());
        elf.phdrs = {
            {pt_load, 5, 0, 0, 0x1000, 0x1000, 0x4000},
            {pt_load, 6, 0x1000, 0x4000, 0x1000, 0x2000, 0x4000},
            {pt_dynamic, 6, dynlib_offset + dyn_offset, 0, dyn_size, dyn_size, 8},
            {pt_sce_dynlibdata, 4, dynlib_offset, 0, blob.size(), 0, 0x10},
        };
        return elf;
    }
};

// Parses `elf` and checks that it fails with a LoadError whose message contains `message`.
void expect_load_error(const Elf& elf, std::string_view message) {
    try {
        parse_dynamic(elf.bytes, elf.phdrs);
        ADD_FAILURE() << "no error, expected: " << message;
    } catch (const LoadError& e) {
        EXPECT_NE(std::string_view(e.what()).find(message), std::string_view::npos)
            << "got: " << e.what() << "\nexpected: " << message;
    }
}

TEST(ParseDynamic, ReadsTheTables) {
    Elf elf = Synthetic{}.build();
    DynamicInfo info = parse_dynamic(elf.bytes, elf.phdrs);

    EXPECT_EQ(info.self.id, 0);
    EXPECT_EQ(info.self.name, "self");
    EXPECT_EQ(info.self.version, 0x0101);
    EXPECT_EQ(info.soname, "self.prx");
    ASSERT_EQ(info.needed.size(), 2U);  // sorted into DT_NEEDED order
    EXPECT_EQ(info.needed[0].id, 1);
    EXPECT_EQ(info.needed[0].name, "libFoo");
    EXPECT_EQ(info.needed[0].file, "libFoo.prx");
    EXPECT_EQ(info.needed[1].name, "libBar");
    EXPECT_EQ(info.needed[1].version, 0x0009);
    EXPECT_EQ(info.needed[1].file, "libBar_x.prx");

    ASSERT_EQ(info.libraries.size(), 3U);
    const Library& foo = info.libraries[0];
    EXPECT_EQ(foo.name, "libFoo");
    EXPECT_FALSE(foo.exported);
    EXPECT_EQ(foo.module, 1);
    EXPECT_FALSE(foo.attr);
    const Library& bar = info.libraries[1];
    EXPECT_EQ(bar.id, 1);
    EXPECT_FALSE(bar.module);  // declared, nothing imported from it
    EXPECT_EQ(bar.attr, 9U);
    const Library& own = info.libraries[2];
    EXPECT_EQ(own.name, "libSelf");
    EXPECT_TRUE(own.exported);
    EXPECT_EQ(own.module, 0);
    EXPECT_EQ(own.attr, 1U);

    ASSERT_EQ(info.symbols.size(), 6U);
    EXPECT_EQ(info.symbols[0].kind, Symbol::Kind::none);
    const Symbol& import = info.symbols[1];
    EXPECT_EQ(import.name, "AAAAAAAAAAA#A#B");
    EXPECT_EQ(import.type, 2);
    EXPECT_EQ(import.bind, 1);
    EXPECT_EQ(import.shndx, 0);
    EXPECT_EQ(import.kind, Symbol::Kind::import);
    EXPECT_EQ(import.decoded.nid, "AAAAAAAAAAA");
    EXPECT_EQ(import.decoded.library, "libFoo");
    EXPECT_EQ(import.decoded.module, "libFoo");
    EXPECT_EQ(import.decoded.type, stt_func);
    EXPECT_EQ(info.symbols[2].name, "module_start");
    EXPECT_EQ(info.symbols[2].bind, 2);
    EXPECT_EQ(info.symbols[2].kind, Symbol::Kind::plain_weak);
    EXPECT_EQ(info.symbols[3].kind, Symbol::Kind::none);
    const Symbol& exported = info.symbols[4];
    EXPECT_EQ(exported.kind, Symbol::Kind::exported);
    EXPECT_EQ(exported.value, 0x4100U);
    EXPECT_EQ(exported.decoded.library, "libSelf");
    EXPECT_EQ(exported.decoded.module, "self");
    EXPECT_EQ(info.symbols[5].kind, Symbol::Kind::import);
    EXPECT_EQ(info.symbols[5].decoded.nid, "CCCCCCCCC+-");
    EXPECT_EQ(info.symbols[5].decoded.type, stt_object);
    EXPECT_EQ(info.pltgot, 0x4000U);
    EXPECT_TRUE(info.textrel);
    EXPECT_EQ(info.init, 0x200U);
    EXPECT_FALSE(info.fini);
    ASSERT_TRUE(info.init_array);
    EXPECT_EQ(info.init_array->addr, 0x4300U);
    EXPECT_EQ(info.init_array->size, 0x10U);
    ASSERT_TRUE(info.preinit_array);
    EXPECT_EQ(info.preinit_array->addr, 0x6000U);
    EXPECT_EQ(info.preinit_array->size, 0U);
    EXPECT_FALSE(info.fini_array);

    ASSERT_EQ(info.plt_relocs.size(), 1U);
    EXPECT_EQ(info.plt_relocs[0].offset, 0x4008U);
    EXPECT_EQ(info.plt_relocs[0].type, r_x86_64_jump_slot);
    EXPECT_EQ(info.plt_relocs[0].sym, 1U);
    ASSERT_EQ(info.relocs.size(), 2U);
    EXPECT_EQ(info.relocs[0].type, r_x86_64_relative);
    EXPECT_EQ(info.relocs[0].addend, 0x40);
    EXPECT_EQ(info.relocs[1].type, r_x86_64_64);
    EXPECT_EQ(info.relocs[1].sym, 1U);
    EXPECT_EQ(info.relocs[1].addend, -8);
}

TEST(ParseDynamic, RejectsBadTags) {
    const std::uint64_t cut_last_nul = Synthetic{}.strtab.size() - 1;
    struct Case {
        std::string_view message;
        std::uint64_t tag;
        std::optional<std::uint64_t> value;  // nothing erases the tag
    };
    const Case cases[] = {
        {"missing DT_SCE_STRTAB", dt_sce_strtab, std::nullopt},
        {"DT_SCE_PLTREL is 0x11", dt_sce_pltrel, 17},
        {"DT_SCE_SYMENT is 0x10", dt_sce_syment, 16},
        {"DT_SCE_RELAENT is 0x10", dt_sce_relaent, 16},
        {"symbol table at 0xffffffffffffff00", dt_sce_symtab, ~std::uint64_t{0xff}},
        {"string table at 0x0 + 0x10000", dt_sce_strsz, 0x10000},
        {"symbol table size 0x47", dt_sce_symtabsz, 71},
        {"PLT relocations size 0x17", dt_sce_pltrelsz, 23},
        {"DT_SONAME: unterminated string", dt_sce_strsz, cut_last_nul},
        {"DT_INIT_ARRAYSZ 0xc is not a multiple of 8", dt_init_arraysz, 12},
        {"DT_PREINIT_ARRAY without DT_PREINIT_ARRAYSZ", dt_preinit_arraysz, std::nullopt},
        {"DT_INIT_ARRAYSZ without DT_INIT_ARRAY", dt_init_array, std::nullopt},
        {"DT_INIT 0x4200 outside the executable PT_LOADs", dt_init, 0x4200},
        {"DT_INIT 0x1000 outside the executable PT_LOADs", dt_init, 0x1000},
        {"DT_INIT_ARRAY 0x5ff8 + 0x10 outside the PT_LOADs", dt_init_array, 0x5ff8},
        {"DT_INIT_ARRAY 0x4300 + 0xfffffffffffffff8 outside", dt_init_arraysz, ~std::uint64_t{7}},
        {"DT_PREINIT_ARRAY 0x6008 + 0x0 outside the PT_LOADs", dt_preinit_array, 0x6008},
    };
    for (const Case& c : cases) {
        expect_load_error(Synthetic{}.build([&](std::vector<Dyn>& dyn) {
            auto it = std::find_if(dyn.begin(), dyn.end(),
                                   [&](const Dyn& d) { return d.first == c.tag; });
            if (c.value) {
                it->second = *c.value;
            } else {
                dyn.erase(it);
            }
        }),
                          c.message);
    }
    expect_load_error(Synthetic{}.build([](auto& dyn) { dyn.emplace_back(dt_sce_strtab, 0); }),
                      "duplicate DT_SCE_STRTAB");
    expect_load_error(Synthetic{}.build([](auto& dyn) { dyn.emplace_back(dt_init, 0); }),
                      "duplicate DT_INIT");
    expect_load_error(Synthetic{}.build([](auto& dyn) { dyn.emplace_back(dt_fini, 0x2000); }),
                      "DT_FINI 0x2000 outside the executable PT_LOADs");
}

TEST(ParseDynamic, RejectsBadEntries) {
    Synthetic far_name;
    far_name.add_symbol(0x1000, 0x12, 0, 0);
    expect_load_error(far_name.build(), "string offset 0x1000 outside the string table");
    Synthetic non_ascii;
    non_ascii.add_symbol(non_ascii.str("\xff"), 0x12, 0, 0);
    expect_load_error(non_ascii.build(), "non-ASCII string");
    Synthetic bad_index;
    Synthetic::add_rela(bad_index.jmprel, 0x4020, r_x86_64_jump_slot, 6, 0);
    expect_load_error(bad_index.build(), "PLT relocations entry 1: symbol index 6 but only 6");
}

TEST(ParseDynamic, RejectsBadRecords) {
    struct Case {
        std::string_view message;
        std::function<void(std::vector<Dyn>&)> edit;  // of Synthetic::records
    };
    const Case cases[] = {
        {"missing DT_SCE_MODULE_INFO", [](auto& r) { r.erase(r.begin()); }},
        {"duplicate DT_SCE_MODULE_INFO", [](auto& r) { r.push_back(r[0]); }},
        {"module info record has id 1, expected 0",
         [](auto& r) { r[0].second = record(1, 0, 0); }},
        {"needed module record with id 0: 0 or a duplicate",
         [](auto& r) { r[4].second = record(0, 0, 0); }},
        {"needed module record with id 2: 0 or a duplicate",
         [](auto& r) { r[4].second = record(2, 0, 0); }},
        {"not numbered 1..n in DT_NEEDED order", [](auto& r) { r[3].second = record(3, 0, 0); }},
        {"not numbered 1..n in DT_NEEDED order", [](auto& r) { r.emplace_back(dt_needed, 0); }},
        {"duplicate library id 1",
         [](auto& r) { r.emplace_back(dt_sce_import_lib, record(1, 1, 0)); }},
        {"library attribute record 0x1000100000009 has bits 47..32 set",
         [](auto& r) { r[8].second = record(1, 1, 9); }},
        {"unpaired attribute record for library 5",
         [](auto& r) { r.emplace_back(dt_sce_import_lib_attr, record(5, 0, 9)); }},
        {"unpaired attribute record for library 1", [](auto& r) { r.push_back(r[8]); }},
        {"unpaired attribute record for library 1",  // libBar is imported, not exported
         [](auto& r) { r.emplace_back(dt_sce_export_lib_attr, record(1, 0, 1)); }},
        {"duplicate DT_SONAME", [](auto& r) { r.emplace_back(dt_soname, 0); }},
    };
    for (const Case& c : cases) {
        Synthetic s;
        c.edit(s.records);
        expect_load_error(s.build(), c.message);
    }
}

TEST(ParseDynamic, RejectsBadSymbols) {
    struct Case {
        std::string_view name;
        std::uint8_t info;  // binding << 4 | type
        std::uint16_t shndx;
        std::string_view message;
    };
    const Case cases[] = {
        {"foo", 0x20, 0, "symbol 6 'foo': plain name other than module_start or module_stop"},
        {"", 0x12, 0, "plain name other than"},
        {"module_stop", 0x10, 0, "plain name that is not a weak, untyped, undefined symbol"},
        {"module_stop", 0x22, 0, "plain name that is not a weak, untyped, undefined symbol"},
        {"module_stop", 0x20, 1, "plain name that is not a weak, untyped, undefined symbol"},
        {"AAAAAAAAAAA#A", 0x12, 0, "name is not NID#library#module"},
        {"AAAAAAAAAAA#A#B#C", 0x12, 0, "name is not NID#library#module"},
        {"AAAAAAAAAA#A#B", 0x12, 0, "malformed NID"},
        {"AAAAAAAAAA_#A#B", 0x12, 0, "malformed NID"},
        {"AAAAAAAAAAA#AA#B", 0x12, 0, "library or module id is not one base64 digit"},
        {"AAAAAAAAAAA#A#", 0x12, 0, "library or module id is not one base64 digit"},
        {"AAAAAAAAAAA#C#B", 0x12, 0, "no imported library with id 2"},
        {"AAAAAAAAAAA#B#A", 0x12, 1, "no exported library with id 1"},
        {"AAAAAAAAAAA#A#A", 0x12, 0, "no needed module with id 0"},
        {"AAAAAAAAAAA#A#D", 0x12, 0, "no needed module with id 3"},
        {"AAAAAAAAAAA#A#B", 0x12, 1, "no own module with id 1"},
        {"AAAAAAAAAAA#A#C", 0x12, 0, "library libFoo is used with two modules"},
        {"AAAAAAAAAAA#A#B", 0x10, 0, "import of symbol type 0"},
        {"AAAAAAAAAAA#A#B", 0x16, 0, "import of symbol type 6"},
    };
    for (const Case& c : cases) {
        Synthetic s;
        s.add_symbol(s.str(c.name), c.info, c.shndx, 0);
        expect_load_error(s.build(), c.message);
    }
    Synthetic other_module;  // libBar from module libBar is fine
    other_module.add_symbol(other_module.str("AAAAAAAAAAA#B#C"), 0x12, 0, 0);
    Elf elf = other_module.build();
    EXPECT_EQ(parse_dynamic(elf.bytes, elf.phdrs).symbols[6].decoded.module, "libBar");
}

TEST(ParseDynamic, RejectsBadSegments) {
    auto with_phdrs = [](const std::function<void(std::vector<Phdr>&)>& edit) {
        Elf elf = Synthetic{}.build();
        edit(elf.phdrs);
        return elf;
    };
    expect_load_error(with_phdrs([](auto& p) { p[2].filesz -= 16; }), "no DT_NULL");
    expect_load_error(with_phdrs([](auto& p) { p[2].offset = 0x2000; }), "PT_DYNAMIC lies outside");
    expect_load_error(with_phdrs([](auto& p) { p.push_back(p[2]); }), "more than one PT_DYNAMIC");
    expect_load_error(with_phdrs([](auto& p) { p.pop_back(); }), "no PT_SCE_DYNLIBDATA");
    Elf truncated = Synthetic{}.build();
    truncated.bytes.pop_back();
    expect_load_error(truncated, "truncated data: PT_SCE_DYNLIBDATA");
}

// Corrupt table bytes must give a LoadError or a valid result, never a crash (run under ASan).
TEST(ParseDynamic, SurvivesCorruptBytes) {
    Elf good = Synthetic{}.build();
    for (std::size_t i = dynlib_offset; i < good.bytes.size(); ++i) {
        for (std::byte value : {std::byte{0x00}, std::byte{0x80}, std::byte{0xff}}) {
            Bytes bytes = good.bytes;
            bytes[i] = value;
            try {
                parse_dynamic(bytes, good.phdrs);
            } catch (const LoadError&) {
            }
        }
    }
}

// Relocation tests use the Synthetic ELF above, mapped with map_elf.

constexpr std::uintptr_t reloc_base = 0x31000000;  // clear of the game tests' default_base
constexpr std::uintptr_t fake_func = 0x7e00'0000'1000, fake_object = 0x7e00'0000'2000;

// `elf` with its ELF header and program headers written into the start of the code segment.
Bytes to_file(const Elf& elf) {
    Bytes bytes = elf.bytes;
    std::memcpy(bytes.data(), "\x7f" "ELF\x02\x01", 6);
    put<std::uint16_t>(bytes, 0x10, et_sce_dynamic);
    put<std::uint16_t>(bytes, 0x12, em_x86_64);
    put<std::uint64_t>(bytes, 0x20, elf_header_size);  // e_phoff
    put<std::uint16_t>(bytes, 0x36, phdr_size);
    put<std::uint16_t>(bytes, 0x38, static_cast<std::uint16_t>(elf.phdrs.size()));
    for (std::size_t i = 0; i < elf.phdrs.size(); ++i) {
        const Phdr& ph = elf.phdrs[i];
        std::size_t at = elf_header_size + i * phdr_size;
        put(bytes, at, ph.type);
        put(bytes, at + 0x04, ph.flags);
        put(bytes, at + 0x08, ph.offset);
        put(bytes, at + 0x10, ph.vaddr);
        put(bytes, at + 0x20, ph.filesz);
        put(bytes, at + 0x28, ph.memsz);
        put(bytes, at + 0x30, ph.align);
    }
    return bytes;
}

// Resolves the function import of Synthetic to fake_func and the object import to fake_object,
// and records every call in `calls`.
Resolver fake_resolver(std::vector<Import>& calls) {
    return [&calls](const Import& imp) {
        calls.push_back(imp);
        return imp.type == stt_func ? fake_func : fake_object;
    };
}

std::uint64_t slot(const Image& image, std::uint64_t link) {
    std::uint64_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(image.runtime(link)), sizeof value);
    return value;
}

TEST(Relocate, AppliesEachType) {
    Synthetic s;
    Synthetic::add_rela(s.rela, 0x4020, r_x86_64_glob_dat, 5, 0);    // imported object
    Synthetic::add_rela(s.rela, 0x4028, r_x86_64_64, 5, 0x10);
    Synthetic::add_rela(s.rela, 0x4030, r_x86_64_64, 4, 8);          // defined, exported
    Synthetic::add_rela(s.rela, 0x4038, r_x86_64_glob_dat, 3, 0);    // defined section symbol
    Synthetic::add_rela(s.rela, 0x4040, r_x86_64_glob_dat, 2, 0);    // module_start
    Synthetic::add_rela(s.rela, 0x4048, r_x86_64_64, 2, 4);
    Synthetic::add_rela(s.rela, 0x5ff8, r_x86_64_relative, 0, -0x10);  // last slot, in .bss
    Synthetic::add_rela(s.rela, 0x800, r_x86_64_relative, 0, 0x4000);  // code, DT_TEXTREL is set
    Synthetic::add_rela(s.jmprel, 0x4050, r_x86_64_jump_slot, 2, 0);
    s.add_symbol(0, 0x03, 0, 0);  // 6: unnamed local section symbol, undefined, as in the PRXs
    Synthetic::add_rela(s.rela, 0x4058, r_x86_64_dtpmod64, 6, 0);
    Synthetic::add_rela(s.rela, 0x4060, r_x86_64_dtpmod64, 1, 0);  // the symbol is ignored
    Image image = map_elf(to_file(s.build()), reloc_base);
    std::vector<Import> calls;
    relocate(image, fake_resolver(calls), 3);

    EXPECT_EQ(slot(image, 0x4010), reloc_base + 0x40);
    EXPECT_EQ(slot(image, 0x4018), fake_func - 8);
    EXPECT_EQ(slot(image, 0x4008), fake_func);
    EXPECT_EQ(slot(image, 0x4020), fake_object);
    EXPECT_EQ(slot(image, 0x4028), fake_object + 0x10);
    EXPECT_EQ(slot(image, 0x4030), reloc_base + 0x4108);
    EXPECT_EQ(slot(image, 0x4038), reloc_base);
    EXPECT_EQ(slot(image, 0x4040), 0U);
    EXPECT_EQ(slot(image, 0x4048), 4U);
    EXPECT_EQ(slot(image, 0x4050), 0U);
    EXPECT_EQ(slot(image, 0x5ff8), reloc_base - 0x10);
    EXPECT_EQ(slot(image, 0x800), reloc_base + 0x4000);
    EXPECT_EQ(slot(image, 0x4058), 3U);  // the module id
    EXPECT_EQ(slot(image, 0x4060), 3U);

    // DT_SCE_RELA in table order, then DT_SCE_JMPREL. Defined and weak symbols need no resolver.
    ASSERT_EQ(calls.size(), 4U);
    EXPECT_EQ(calls[0].nid, "AAAAAAAAAAA");  // R_X86_64_64 at 0x4018
    EXPECT_EQ(calls[0].library, "libFoo");
    EXPECT_EQ(calls[0].module, "libFoo");
    EXPECT_EQ(calls[0].type, stt_func);
    EXPECT_EQ(calls[1].nid, "CCCCCCCCC+-");
    EXPECT_EQ(calls[1].type, stt_object);
    EXPECT_EQ(calls[2].nid, "CCCCCCCCC+-");
    EXPECT_EQ(calls[3].nid, "AAAAAAAAAAA");  // R_X86_64_JUMP_SLOT at 0x4008
}

// Maps `s` with `edit` applied to its dynamic entries, relocates it with `resolve` and expects a
// LoadError containing `message`. Without `resolve`, returns the calls fake_resolver recorded.
std::vector<Import> expect_relocate_error(const Synthetic& s, std::string_view message,
                                          const std::function<void(std::vector<Dyn>&)>& edit = {},
                                          Resolver resolve = {}) {
    std::vector<Import> calls;
    Image image = map_elf(to_file(s.build(edit)), reloc_base);
    try {
        relocate(image, resolve ? resolve : fake_resolver(calls), eboot_module_id);
        ADD_FAILURE() << "no error, expected: " << message;
    } catch (const LoadError& e) {
        EXPECT_NE(std::string_view(e.what()).find(message), std::string_view::npos)
            << "got: " << e.what() << "\nexpected: " << message;
    }
    return calls;
}

// Synthetic's own entries resolve import 1 before an appended entry runs: R_X86_64_64 at 0x4018
// ahead of a DT_SCE_RELA entry, and also R_X86_64_JUMP_SLOT at 0x4008 ahead of a DT_SCE_JMPREL one.
// A rejected entry must not reach the resolver, even when it names import 1 too.
void expect_default_imports_only(const std::vector<Import>& calls, std::size_t count,
                                 std::string_view message) {
    ASSERT_EQ(calls.size(), count) << message;
    for (const Import& imp : calls) {
        EXPECT_EQ(imp.nid, "AAAAAAAAAAA") << message;
    }
}

TEST(Relocate, RejectsBadRelocations) {
    struct Case {
        std::uint64_t offset;
        std::uint32_t type, sym;
        std::string_view message;
    };
    const Case cases[] = {
        {0x4050, 37, 0, "relocation type 37 at 0x4050: unsupported type"},
        {0x4050, 17, 1, "relocation type 17 at 0x4050: unsupported type"},  // DTPOFF64
        {0x4050, r_x86_64_64, 0, "relocation type 1 at 0x4050: undefined symbol 0 ''"},
        {0x5ffc, r_x86_64_relative, 0, "type 8 at 0x5ffc: target outside the PT_LOAD segments"},
        {0x2000, r_x86_64_relative, 0, "type 8 at 0x2000: target outside the PT_LOAD segments"},
        {0x6000, r_x86_64_glob_dat, 1, "type 6 at 0x6000: target outside the PT_LOAD segments"},
        {~std::uint64_t{3}, r_x86_64_relative, 0, "target outside the PT_LOAD segments"},
    };
    for (const Case& c : cases) {
        Synthetic s;
        Synthetic::add_rela(s.rela, c.offset, c.type, c.sym, 0);
        expect_default_imports_only(expect_relocate_error(s, c.message), 1, c.message);
    }
    Synthetic code;
    Synthetic::add_rela(code.jmprel, 0xff8, r_x86_64_jump_slot, 1, 0);
    expect_default_imports_only(
        expect_relocate_error(code,
                              "type 7 at 0xff8: target in a read-only segment without DT_TEXTREL",
                              [](std::vector<Dyn>& dyn) {
                                  std::erase_if(dyn, [](const Dyn& d) { return d.first == 0x16; });
                              }),
        2, "read-only target");
    expect_relocate_error(Synthetic{}, "type 1 at 0x4018: import AAAAAAAAAAA#A#B resolved to 0", {},
                          [](const Import&) { return std::uintptr_t{0}; });
}

TEST(Relocate, RefusesAFinalizedImage) {
    Image image = map_elf(to_file(Synthetic{}.build()), reloc_base);
    image.finalize_protection();
    std::vector<Import> calls;
    EXPECT_THROW(relocate(image, fake_resolver(calls), eboot_module_id), std::logic_error);
    EXPECT_TRUE(calls.empty());
}

TEST(ParseDynamicGame, ReadsTheEboot) {
    BB_REQUIRE_GAME(game);
    auto elf = self_to_elf(bb::common::read_file(game / "eboot.bin"));
    auto phdrs = read_phdrs(elf, read_elf_header(elf));
    DynamicInfo info = parse_dynamic(elf, phdrs);

    // Counts as tools/elfimports and tools/common/ps4elf.py report them for eboot.elf.
    EXPECT_EQ(info.symbols.size(), 703U);
    auto undefined = [&](std::uint8_t type) {
        return std::count_if(info.symbols.begin(), info.symbols.end(), [&](const Symbol& s) {
            return s.shndx == 0 && s.type == type && s.name.find('#') != std::string::npos;
        });
    };
    EXPECT_EQ(undefined(2), 681);  // STT_FUNC imports
    EXPECT_EQ(undefined(1), 20);   // STT_OBJECT imports
    auto count = [](const std::vector<Rela>& relas, std::uint32_t type) {
        return std::count_if(relas.begin(), relas.end(),
                             [&](const Rela& r) { return r.type == type; });
    };
    EXPECT_EQ(info.plt_relocs.size(), 661U);
    EXPECT_EQ(count(info.plt_relocs, r_x86_64_jump_slot), 661);
    EXPECT_EQ(info.relocs.size(), 234339U);
    EXPECT_EQ(count(info.relocs, r_x86_64_relative), 231478);
    EXPECT_EQ(count(info.relocs, r_x86_64_64), 2838);
    EXPECT_EQ(count(info.relocs, r_x86_64_glob_dat), 23);

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
    EXPECT_EQ(std::count_if(info.libraries.begin(), info.libraries.end(), unused), 4);
    auto imports = [&](std::string_view library, std::uint8_t type) {
        return std::count_if(info.symbols.begin(), info.symbols.end(), [&](const Symbol& s) {
            return s.kind == Symbol::Kind::import && s.decoded.library == library &&
                   s.decoded.type == type;
        });
    };
    auto kind = [&](Symbol::Kind k) {
        return std::count_if(info.symbols.begin(), info.symbols.end(),
                             [&](const Symbol& s) { return s.kind == k; });
    };
    EXPECT_EQ(kind(Symbol::Kind::import), 701);
    EXPECT_EQ(kind(Symbol::Kind::none), 2);
    EXPECT_EQ(imports("libc", stt_func), 197);
    EXPECT_EQ(imports("libc", stt_object), 19);
    EXPECT_EQ(imports("libkernel", stt_func), 79);
    EXPECT_EQ(imports("libkernel", stt_object), 1);
    EXPECT_EQ(imports("libScePosix", stt_func), 32);
    EXPECT_EQ(imports("libSceAppContent", stt_func), 3);
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
    Image image = map_eboot(game);
    std::map<std::string, std::uintptr_t> fakes;  // a distinct address per NID#library
    relocate(image, [&](const Import& imp) {
        std::uintptr_t next = 0x7e00'0000'0000 + 0x20 * (fakes.size() + 1);
        return fakes.try_emplace(imp.nid + '#' + imp.library, next).first->second;
    }, eboot_module_id);
    EXPECT_EQ(fakes.size(), 686U);  // of the 701 imports, as tools/common/ps4elf.py counts them

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

}  // namespace

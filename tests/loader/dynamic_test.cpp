#include "loader/dynamic.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/file.hpp"
#include "loader/bytes.hpp"
#include "loader/elf.hpp"
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

// A minimal PS4 ELF: a PT_LOAD and a PT_SCE_DYNLIBDATA segment at the end of the file with the
// tables and the dynamic table. It needs modules libFoo (id 1) and libBar (id 2), imports
// library libFoo (id 0) from libFoo, declares library libBar (id 1) without importing from it,
// and exports library libSelf (id 0).
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
                   {0x16, 0}};  // DT_TEXTREL
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
    EXPECT_FALSE(foo.attr);
    const Library& bar = info.libraries[1];
    EXPECT_EQ(bar.id, 1);
    EXPECT_EQ(bar.attr, 9U);
    const Library& own = info.libraries[2];
    EXPECT_EQ(own.name, "libSelf");
    EXPECT_TRUE(own.exported);
    EXPECT_EQ(own.attr, 1U);

    ASSERT_EQ(info.symbols.size(), 6U);
    const Symbol& import = info.symbols[1];
    EXPECT_EQ(import.name, "AAAAAAAAAAA#A#B");
    EXPECT_EQ(import.type, 2);
    EXPECT_EQ(import.bind, 1);
    EXPECT_EQ(import.shndx, 0);
    EXPECT_EQ(info.symbols[2].name, "module_start");
    EXPECT_EQ(info.symbols[2].bind, 2);
    EXPECT_EQ(info.pltgot, 0x4000U);
    EXPECT_TRUE(info.textrel);

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

TEST(ParseDynamic, RejectsBadSegments) {
    auto with_phdrs = [](const std::function<void(std::vector<Phdr>&)>& edit) {
        Elf elf = Synthetic{}.build();
        edit(elf.phdrs);
        return elf;
    };
    expect_load_error(with_phdrs([](auto& p) { p[1].filesz -= 16; }), "no DT_NULL");
    expect_load_error(with_phdrs([](auto& p) { p[1].offset = 0x2000; }), "PT_DYNAMIC lies outside");
    expect_load_error(with_phdrs([](auto& p) { p.push_back(p[1]); }), "more than one PT_DYNAMIC");
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
    EXPECT_EQ(info.needed.size(), 42U);
    auto exported = [](const Library& lib) { return lib.exported; };
    EXPECT_EQ(std::count_if(info.libraries.begin(), info.libraries.end(), exported), 0);
    EXPECT_EQ(info.libraries.size(), 43U);
}

}  // namespace

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
constexpr std::size_t dynlib_offset = 0x3000;  // file offset of PT_SCE_DYNLIBDATA

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
// tables and the dynamic table.
struct Synthetic {
    Bytes strtab{std::byte{0}};
    Bytes symtab, jmprel, rela;

    Synthetic() {
        add_symbol(0, 0, 0, 0);
        add_symbol(str("AAAAAAAAAAA#A#B"), 0x12, 0, 0);  // global function, undefined
        add_symbol(str("module_start"), 0x20, 0, 0);     // weak, untyped
        add_rela(jmprel, 0x4008, r_x86_64_jump_slot, 1, 0);
        add_rela(rela, 0x4010, r_x86_64_relative, 0, 0x40);
        add_rela(rela, 0x4018, r_x86_64_64, 1, -8);
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

    ASSERT_EQ(info.symbols.size(), 3U);
    EXPECT_EQ(info.symbols[0].name, "");
    const Symbol& import = info.symbols[1];
    EXPECT_EQ(import.name, "AAAAAAAAAAA#A#B");
    EXPECT_EQ(import.type, 2);
    EXPECT_EQ(import.bind, 1);
    EXPECT_EQ(import.shndx, 0);
    EXPECT_EQ(info.symbols[2].name, "module_start");
    EXPECT_EQ(info.symbols[2].bind, 2);

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
        {"unterminated string", dt_sce_strsz, 29},  // cuts off the NUL of "module_start"
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
    Synthetic::add_rela(bad_index.jmprel, 0x4020, r_x86_64_jump_slot, 3, 0);
    expect_load_error(bad_index.build(), "PLT relocations entry 1: symbol index 3 but only 3");
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
}

}  // namespace

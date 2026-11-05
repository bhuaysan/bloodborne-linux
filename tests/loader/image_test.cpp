#include "loader/image.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "loader/bytes.hpp"
#include "loader/elf.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::loader;
using Bytes = std::vector<std::byte>;

constexpr std::uintptr_t test_base = 0x30000000;  // clear of default_base, used by game tests
constexpr std::uint64_t code_size = 0x1800, data_link = 0x4000, data_file = 0x100,
                        data_mem = 0x3000, dynlib_offset = 0x4000;
constexpr std::uint32_t pf_rx = 5, pf_rw = 6;

template <class T>
void put(Bytes& out, std::size_t offset, T value) {
    if (out.size() < offset + sizeof value) {
        out.resize(offset + sizeof value);
    }
    std::memcpy(out.data() + offset, &value, sizeof value);
}

std::byte pattern(std::size_t i) {
    return static_cast<std::byte>(i * 13 + 1);
}

// Unwind tables in the code segment (docs/formats/eh-frame.md): .eh_frame at eh_frame_link with
// a "zR" and a "zPLR" CIE, an FDE (pcrel|sdata4) per function, a dead FDE (pc_begin 0) and the
// terminator, then .eh_frame_hdr at eh_hdr_link, whose search table lists the live FDEs.
struct Function {
    std::uint64_t start, size;
    bool lsda;  // uses the "zPLR" CIE
};
constexpr Function functions[] = {{0x10, 0x20, false}, {0x40, 0xd, true}, {0x60, 0x30, false}};
constexpr std::uint64_t eh_frame_link = 0x1000, eh_hdr_link = 0x1200,
                        eh_hdr_size = 12 + 8 * (std::size(functions) + 1);

Bytes raw(std::initializer_list<std::uint8_t> values) {
    Bytes out;
    for (std::uint8_t v : values) {
        out.push_back(std::byte{v});
    }
    return out;
}

// The byte at link address `link` of the code segment, which starts at file offset 0x1000.
std::byte& code_at(Bytes& elf, std::uint64_t link) {
    return elf.at(0x1000 + link);
}

void add_unwind_tables(Bytes& elf) {
    std::uint64_t pos = eh_frame_link;
    auto record = [&](Bytes body) {
        body.resize((body.size() + 3) / 4 * 4);  // DW_CFA_nop padding
        std::uint64_t at = pos;
        put(elf, 0x1000 + at, static_cast<std::uint32_t>(body.size()));
        std::memcpy(&code_at(elf, at + 4), body.data(), body.size());
        pos += 4 + body.size();
        return at;
    };
    // id 0, version 1, augmentation, code align 1, data align -8, return address r16, then the
    // augmentation data: length, personality (indirect pcrel sdata4), LSDA and FDE encodings.
    std::uint64_t cie_r = record(raw({0, 0, 0, 0, 1, 'z', 'R', 0, 1, 0x78, 0x10, 1, 0x1b}));
    std::uint64_t cie_plr = record(raw({0, 0, 0, 0, 1, 'z', 'P', 'L', 'R', 0, 1, 0x78, 0x10, 7,
                                        0x9b, 0, 0, 0, 0, 0x1b, 0x1b}));
    auto fde = [&](std::uint64_t cie, std::uint64_t start, std::uint64_t size, bool lsda) {
        Bytes body(lsda ? 17 : 13);
        put(body, 0, static_cast<std::uint32_t>(pos + 4 - cie));
        put(body, 4, static_cast<std::uint32_t>(start - (pos + 8)));
        put(body, 8, static_cast<std::uint32_t>(size));
        body[12] = std::byte{lsda ? std::uint8_t{4} : std::uint8_t{0}};  // augmentation length
        return record(body);
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> table;  // (function, FDE)
    for (const Function& f : functions) {
        table.emplace_back(f.start, fde(f.lsda ? cie_plr : cie_r, f.start, f.size, f.lsda));
    }
    fde(cie_r, pos + 8, 0x40, false);  // dead: raw pc_begin 0
    put<std::uint32_t>(elf, 0x1000 + pos, 0);

    std::memcpy(&code_at(elf, eh_hdr_link), raw({1, 0x1b, 0x03, 0x3b}).data(), 4);
    put(elf, 0x1000 + eh_hdr_link + 4, static_cast<std::int32_t>(eh_frame_link - eh_hdr_link - 4));
    put(elf, 0x1000 + eh_hdr_link + 8, static_cast<std::uint32_t>(table.size()));
    for (std::size_t i = 0; i < table.size(); ++i) {
        std::uint64_t at = 0x1000 + eh_hdr_link + 12 + 8 * i;
        put(elf, at, static_cast<std::int32_t>(table[i].first - eh_hdr_link));
        put(elf, at + 4, static_cast<std::int32_t>(table[i].second - eh_hdr_link));
    }
}

// The link address of the FDE that search table entry `i` points to.
std::uint64_t fde_link(Bytes& elf, std::size_t i) {
    std::int32_t rel;
    std::memcpy(&rel, &code_at(elf, eh_hdr_link + 16 + 8 * i), 4);
    return eh_hdr_link + rel;
}

// Replaces the FDE of entry 0 (function 0x10, size 0x20) by one at 0x1100 with 8-byte pointers
// and switches its "zR" CIE to pointer encoding `enc`.
void use_eight_byte_fde(Bytes& elf, std::uint8_t enc) {
    code_at(elf, eh_frame_link + 16) = std::byte{enc};
    put<std::uint32_t>(elf, 0x1000 + 0x1100, 24);  // CIE pointer, pc_begin, pc_range, aug length
    put<std::uint32_t>(elf, 0x1000 + 0x1104, 0x1104 - eh_frame_link);
    put<std::int64_t>(elf, 0x1000 + 0x1108, std::int64_t{0x10} - 0x1108);
    put<std::uint64_t>(elf, 0x1000 + 0x1110, 0x20);
    code_at(elf, 0x1118) = std::byte{0};
    put<std::int32_t>(elf, 0x1000 + eh_hdr_link + 16, -0x100);  // 0x1100, from eh_hdr_link
}

// A PS4 ELF with a code segment at link address 0, a data segment at data_link whose memory
// extends past its file data (.bss), the smallest dynamic table parse_dynamic accepts, and
// PT_TLS and PT_SCE_PROCPARAM inside the data segment, and unwind tables in the code segment.
// `edit` changes the program headers before they are written.
Bytes build_elf(void (*edit)(std::vector<Phdr>&) = nullptr) {
    Bytes elf(dynlib_offset);
    std::memcpy(elf.data(), "\x7f" "ELF\x02\x01", 6);
    put<std::uint16_t>(elf, 0x10, et_sce_dynexec);
    put<std::uint16_t>(elf, 0x12, em_x86_64);
    put<std::uint64_t>(elf, 0x18, 0x10);  // e_entry
    put<std::uint64_t>(elf, 0x20, 0x40);  // e_phoff
    put<std::uint16_t>(elf, 0x36, 0x38);  // e_phentsize
    for (std::size_t i = 0; i < code_size; ++i) {
        elf[0x1000 + i] = pattern(i);
    }
    for (std::size_t i = 0; i < data_file; ++i) {
        elf[0x3000 + i] = pattern(i + 7);
    }
    add_unwind_tables(elf);

    // DynamicInfo needs only the module info record and the (here empty) tables.
    std::size_t dyn = dynlib_offset + 8;
    elf.resize(dyn);
    std::memcpy(elf.data() + dynlib_offset, "\0self\0\0", 8);
    const std::pair<std::uint64_t, std::uint64_t> entries[] = {
        {0x6100000D, 0x0101ULL << 32 | 1},  // DT_SCE_MODULE_INFO "self"
        {0x61000035, 0}, {0x61000037, 6},   // string table
        {0x61000039, 0}, {0x6100003F, 0}, {0x6100003B, 24},  // symbols
        {0x61000029, 0}, {0x6100002D, 0}, {0x6100002B, 7},   // PLT relocations
        {0x6100002F, 0}, {0x61000031, 0}, {0x61000033, 24},  // relocations
        {0x61000027, data_link}, {0, 0}};                     // DT_SCE_PLTGOT, DT_NULL
    for (auto [tag, value] : entries) {
        put(elf, elf.size(), tag);
        put(elf, elf.size(), value);
    }

    std::vector<Phdr> phdrs = {
        {pt_load, pf_rx, 0x1000, 0, code_size, code_size, 0x4000},
        {pt_load, pf_rw, 0x3000, data_link, data_file, data_mem, 0x4000},
        {pt_sce_dynlibdata, 4, dynlib_offset, 0, elf.size() - dynlib_offset, 0, 0x10},
        {pt_dynamic, 6, dyn, 0, elf.size() - dyn, elf.size() - dyn, 8},
        {pt_tls, 4, 0x3040, data_link + 0x40, 0x10, 0x800, 0x20},
        {pt_sce_procparam, 4, 0x3000, data_link, 0x40, 0x40, 8},
        {pt_gnu_eh_frame, 4, 0x1000 + eh_hdr_link, eh_hdr_link, eh_hdr_size, eh_hdr_size, 4},
    };
    if (edit) {
        edit(phdrs);
    }
    put<std::uint16_t>(elf, 0x38, static_cast<std::uint16_t>(phdrs.size()));
    for (std::size_t i = 0; i < phdrs.size(); ++i) {
        const Phdr& ph = phdrs[i];
        std::size_t at = 0x40 + i * phdr_size;
        put(elf, at, ph.type);
        put(elf, at + 0x04, ph.flags);
        put(elf, at + 0x08, ph.offset);
        put(elf, at + 0x10, ph.vaddr);
        put(elf, at + 0x20, ph.filesz);
        put(elf, at + 0x28, ph.memsz);
        put(elf, at + 0x30, ph.align);
    }
    return elf;
}

// The permissions ("r-xp", ...) of the mapping that holds `addr`, from /proc/self/maps.
std::string protection(std::uintptr_t addr) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::uintptr_t start, end;
        char dash;
        std::string perms;
        in >> std::hex >> start >> dash >> end >> perms;
        if (start <= addr && addr < end) {
            return perms;
        }
    }
    return "unmapped";
}

// Expects `f` to throw an `E` whose message contains `message`.
template <class E, class F>
void expect_error(F f, std::string_view message) {
    try {
        f();
        ADD_FAILURE() << "no error, expected: " << message;
    } catch (const E& e) {
        EXPECT_NE(std::string_view(e.what()).find(message), std::string_view::npos) << e.what();
    }
}

TEST(MapElf, CopiesTheFileAndZeroesTheRest) {
    Bytes elf = build_elf();
    Image image = map_elf(elf, test_base);
    EXPECT_EQ(image.base, test_base);
    EXPECT_EQ(image.entry, 0x10U);
    ASSERT_EQ(image.segments.size(), 2U);
    EXPECT_EQ(image.segments[1].link_addr, data_link);
    EXPECT_EQ(image.segments[1].mem_size, data_mem);
    EXPECT_EQ(image.segments[1].file_size, data_file);
    EXPECT_EQ(image.segments[0].prot, PROT_READ | PROT_EXEC);
    EXPECT_EQ(image.segments[1].prot, PROT_READ | PROT_WRITE);
    EXPECT_EQ(image.dyn.self.name, "self");
    EXPECT_EQ(image.dyn.pltgot, data_link);

    EXPECT_EQ(std::memcmp(reinterpret_cast<void*>(image.runtime(0)), elf.data() + 0x1000,
                          code_size),
              0);
    auto* data = reinterpret_cast<const std::byte*>(image.runtime(data_link));
    EXPECT_EQ(std::memcmp(data, elf.data() + 0x3000, data_file), 0);
    for (std::uint64_t i = data_file; i < data_mem; ++i) {
        ASSERT_EQ(data[i], std::byte{0}) << "at " << i;
    }
}

TEST(MapElf, LeavesTheSegmentsWritable) {
    Image image = map_elf(build_elf(), test_base);
    EXPECT_EQ(protection(test_base), "rw-p");  // until relocated and finalized
    EXPECT_EQ(protection(test_base + 0x1fff), "rw-p");
    EXPECT_EQ(protection(test_base + 0x2000), "---p");  // gap between the segments
    EXPECT_EQ(protection(test_base + data_link + data_mem - 1), "rw-p");
    EXPECT_EQ(protection(test_base + data_link + data_mem), "unmapped");
}

TEST(Image, FinalizesProtections) {
    Image image = map_elf(build_elf(), test_base);
    EXPECT_FALSE(image.finalized());
    image.finalize_protection();
    EXPECT_TRUE(image.finalized());
    EXPECT_EQ(protection(test_base), "r-xp");
    EXPECT_EQ(protection(test_base + 0x1fff), "r-xp");  // the rest of the last code page
    EXPECT_EQ(protection(test_base + 0x2000), "---p");
    EXPECT_EQ(protection(test_base + data_link), "rw-p");
    EXPECT_EQ(protection(test_base + data_link + data_mem - 1), "rw-p");
    EXPECT_EQ(protection(test_base + data_link + data_mem), "unmapped");
    image.finalize_protection();  // again, no change
    EXPECT_EQ(protection(test_base), "r-xp");

    Image moved = std::move(image);
    EXPECT_TRUE(moved.finalized());
    Image read_only = map_elf(build_elf([](auto& p) { p[1].flags = 4; }), test_base + 0x100000);
    read_only.finalize_protection();
    EXPECT_EQ(protection(test_base + 0x100000 + data_link), "r--p");
}

TEST(MapElf, UnmapsOnDestructionOnly) {
    {
        Image image = map_elf(build_elf(), test_base);
        Image moved = std::move(image);
        { Image gone = std::move(moved); }
        EXPECT_EQ(protection(test_base), "unmapped");
    }
    Image image = map_elf(build_elf(), test_base);  // the range is free again
    Image other = map_elf(build_elf(), test_base + 0x100000);
    image = std::move(other);
    EXPECT_EQ(protection(test_base), "unmapped");
    EXPECT_EQ(protection(test_base + 0x100000), "rw-p");
    EXPECT_EQ(image.runtime(0), test_base + 0x100000);
}

TEST(MapElf, RefusesATakenRange) {
    Image image = map_elf(build_elf(), test_base);
    expect_error<std::system_error>([] { map_elf(build_elf(), test_base); },
                                    "cannot reserve 0x30000000..0x30007000");
    void* page = mmap(reinterpret_cast<void*>(test_base + 0x106000), 0x1000, PROT_READ,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    ASSERT_NE(page, MAP_FAILED);
    expect_error<std::system_error>([] { map_elf(build_elf(), test_base + 0x100000); },
                                    "cannot reserve 0x30100000..0x30107000");
    munmap(page, 0x1000);
}

TEST(MapElf, RefusesAMisalignedBase) {
    expect_error<LoadError>([] { map_elf(build_elf(), test_base + 0x1000); },
                            "base 0x30001000 is not aligned to 0x4000");
}

TEST(MapElf, RefusesBadSegments) {
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[1].filesz = data_mem + 1; }), test_base); },
        "p_filesz 0x3001 > p_memsz 0x3000");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[1].vaddr = 0x1000; }), test_base); },
        "share a page");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[1].vaddr = 0xffff'ffff'ffff'f000; }), test_base); },
        "is too large");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[1].align = 0x3000; }), test_base); },
        "is not a power of two");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[1].offset = 0x1'0000'0000; }), test_base); },
        "PT_LOAD file data");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[0].type = p[1].type = 0; }), test_base); },
        "no PT_LOAD");
}

TEST(MapElf, ReleasesTheRangeWhenTheDynamicDataIsBad) {
    // parse_dynamic runs after the range is reserved and the segments are copied.
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[2].filesz = 0x10; }), test_base); },
        "PT_DYNAMIC lies outside PT_SCE_DYNLIBDATA");
    EXPECT_EQ(protection(test_base), "unmapped");
    Image image = map_elf(build_elf(), test_base);  // the range is free again
    EXPECT_EQ(protection(test_base), "rw-p");
}

TEST(MapElf, ReadsTlsAndProcParam) {
    Image image = map_elf(build_elf(), test_base);
    ASSERT_TRUE(image.tls);
    EXPECT_EQ(image.tls->link_addr, data_link + 0x40);
    EXPECT_EQ(image.tls->file_size, 0x10U);
    EXPECT_EQ(image.tls->mem_size, 0x800U);
    EXPECT_EQ(image.tls->align, 0x20U);
    EXPECT_EQ(image.proc_param, data_link);

    Image none = map_elf(build_elf([](auto& p) { p.resize(4); }), test_base + 0x100000);
    EXPECT_FALSE(none.tls);
    EXPECT_FALSE(none.proc_param);
}

TEST(MapElf, RefusesBadTlsAndProcParam) {
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p.push_back(p[4]); }), test_base); },
        "more than one PT_TLS program header");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p.push_back(p[5]); }), test_base); },
        "more than one PT_SCE_PROCPARAM program header");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[4].filesz = 0x801; }), test_base); },
        "PT_TLS: p_filesz 0x801 > p_memsz 0x800");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[4].align = 0x18; }), test_base); },
        "PT_TLS: p_align 0x18 is not a power of two");
    EXPECT_EQ(protection(test_base), "unmapped");  // checked before the range is reserved
}

TEST(ReadTls, ReadsAlignZeroAsOne) {
    std::vector<Phdr> phdrs = {{pt_load, pf_rw, 0, 0x4000, 0x100, 0x1000, 0x4000},
                               {pt_tls, 4, 0, 0x4000, 0, 0x10, 0}};
    auto tls = read_tls(phdrs);
    ASSERT_TRUE(tls);
    EXPECT_EQ(tls->align, 1U);
    phdrs[1].align = 1;
    EXPECT_EQ(read_tls(phdrs)->align, 1U);
}

// Only the initialization image has to lie inside a PT_LOAD, not the whole TLS block.
TEST(ReadTls, NeedsTheImageInsideAPtLoad) {
    std::vector<Phdr> phdrs = {{pt_load, pf_rw, 0, 0x4000, 0x100, 0x1000, 0x4000},
                               {pt_tls, 4, 0, 0x4f00, 0x100, 0x2000, 0x10}};
    EXPECT_TRUE(read_tls(phdrs));
    phdrs[1].filesz = 0x101;
    expect_error<LoadError>([&] { read_tls(phdrs); },
                            "PT_TLS initialization image at 0x4f00 + 0x101 lies outside");
    phdrs[1].vaddr = 0x3ff0;
    phdrs[1].filesz = 0x20;
    expect_error<LoadError>([&] { read_tls(phdrs); }, "lies outside the PT_LOAD segments");
    phdrs[0].type = pt_dynamic;
    phdrs[1].vaddr = 0x4000;
    expect_error<LoadError>([&] { read_tls(phdrs); }, "lies outside the PT_LOAD segments");
}

TEST(ReadProcParam, NeedsItInsideAPtLoad) {
    std::vector<Phdr> phdrs = {{pt_load, pf_rw, 0, 0x4000, 0x100, 0x1000, 0x4000},
                               {pt_sce_procparam, 4, 0, 0x4fc0, 0x40, 0x40, 8}};
    EXPECT_EQ(read_proc_param(phdrs), 0x4fc0U);
    phdrs[1].memsz = 0x41;
    expect_error<LoadError>([&] { read_proc_param(phdrs); },
                            "PT_SCE_PROCPARAM at 0x4fc0 + 0x41 lies outside");
    EXPECT_FALSE(read_proc_param(std::span(phdrs).first(1)));
    EXPECT_FALSE(read_tls(phdrs));
}

TEST(Image, ChecksLinkAddresses) {
    Image image = map_elf(build_elf(), test_base);
    EXPECT_EQ(image.runtime(0), test_base);
    EXPECT_EQ(image.runtime(code_size - 1), test_base + code_size - 1);
    EXPECT_EQ(image.runtime(data_link + data_mem - 1), test_base + data_link + data_mem - 1);
    EXPECT_THROW(image.runtime(code_size), std::out_of_range);
    EXPECT_THROW(image.runtime(data_link - 1), std::out_of_range);
    EXPECT_THROW(image.runtime(data_link + data_mem), std::out_of_range);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(image.fn<void()>(0x10)), test_base + 0x10);
    expect_error<std::out_of_range>([&] { image.fn<void()>(data_link); },
                                    "link address 0x4000 lies outside the executable segments");
}

TEST(Image, FindsFunctionSizes) {
    Image image = map_elf(build_elf(), test_base);
    ASSERT_TRUE(image.unwind);
    EXPECT_EQ(image.unwind->link_addr, eh_hdr_link);
    EXPECT_EQ(image.unwind->fde_count, std::size(functions));
    for (const Function& f : functions) {
        EXPECT_EQ(image.function_size(f.start), f.size) << f.start;
    }
    std::uint64_t dead = eh_frame_link + 0x78;  // where the dead FDE "starts"
    EXPECT_EQ(protection(test_base), "rw-p");
    image.finalize_protection();
    for (std::uint64_t miss : {0x0UL, 0x11UL, 0x3fUL, 0x50UL, 0x61UL, dead, eh_hdr_link,
                               0x1'0000'0000UL, ~0x0UL}) {
        EXPECT_FALSE(image.function_size(miss)) << miss;
    }
    EXPECT_EQ(image.function_size(0x60), 0x30U);  // still readable once finalized

    Image none = map_elf(build_elf([](auto& p) { p.pop_back(); }), test_base + 0x100000);
    EXPECT_FALSE(none.unwind);
    EXPECT_FALSE(none.function_size(0x10));

    for (std::uint8_t enc : {0x10, 0x14, 0x1c}) {  // pcrel with absptr, udata8 and sdata8
        Bytes elf = build_elf();
        use_eight_byte_fde(elf, enc);
        Image wide = map_elf(elf, test_base + 0x200000);
        EXPECT_EQ(wide.function_size(0x10), 0x20U) << enc;
    }
}

// Maps the ELF with `edit` applied to its bytes and expects map_elf or, when `lookup` is set,
// function_size(lookup) to fail with `message`.
void expect_unwind_error(void (*edit)(Bytes&), std::string_view message,
                         std::optional<std::uint64_t> lookup = std::nullopt) {
    Bytes elf = build_elf();
    edit(elf);
    if (!lookup) {
        expect_error<LoadError>([&] { map_elf(elf, test_base); }, message);
        return;
    }
    Image image = map_elf(elf, test_base);
    expect_error<LoadError>([&] { image.function_size(*lookup); }, message);
}

TEST(Image, RefusesBadSearchTables) {
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p.push_back(p[6]); }), test_base); },
        "more than one PT_GNU_EH_FRAME");
    expect_error<LoadError>(
        [] {
            map_elf(build_elf([](auto& p) { p[6].vaddr = data_link + data_file - 8; }), test_base);
        },
        "PT_GNU_EH_FRAME at 0x40f8 + 0x2c lies outside the file data");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[0].flags = pf_x; }), test_base); },
        "PT_GNU_EH_FRAME lies in a segment that is not readable");
    expect_error<LoadError>(
        [] { map_elf(build_elf([](auto& p) { p[6].filesz = 8; }), test_base); },
        "PT_GNU_EH_FRAME of 0x8 bytes is too small");
    expect_unwind_error([](Bytes& e) { code_at(e, eh_hdr_link + 3) = std::byte{0x1b}; },
                        "unsupported .eh_frame_hdr encodings 01 1b 03 1b");
    expect_unwind_error([](Bytes& e) { code_at(e, eh_hdr_link + 8) = std::byte{5}; },
                        "search table of 5 entries exceeds PT_GNU_EH_FRAME");
    // Entry 2 gets the start of entry 1.
    expect_unwind_error(
        [](Bytes& e) { code_at(e, eh_hdr_link + 28) = code_at(e, eh_hdr_link + 20); },
        "search table is not sorted at entry 2");
    EXPECT_EQ(protection(test_base), "unmapped");  // checked before the range is reserved
}

TEST(Image, RefusesBadFdes) {
    // Entry 0 (function 0x10) points at the "zR" CIE instead of its FDE, then past the segment.
    constexpr std::size_t entry0_fde = 0x1000 + eh_hdr_link + 16;
    expect_unwind_error([](Bytes& e) { put<std::int32_t>(e, entry0_fde, -0x200); },
                        "no supported FDE at 0x1000 for 0x10", 0x10);
    expect_unwind_error([](Bytes& e) { put<std::int32_t>(e, entry0_fde, 0x1000); },
                        "FDE at 0x2200 + 0x4 lies outside its segment's file data", 0x10);
    expect_unwind_error([](Bytes& e) { code_at(e, fde_link(e, 0) + 8) = std::byte{0}; },
                        "starts at 0x", 0x10);
    expect_unwind_error([](Bytes& e) { code_at(e, eh_frame_link + 16) = std::byte{0x03}; },
                        "unsupported FDE pointer encoding 0x3", 0x60);
    expect_unwind_error([](Bytes& e) { code_at(e, eh_frame_link + 10) = std::byte{'X'}; },
                        "unsupported CIE augmentation \"zX\" at 0x1000", 0x10);
    expect_unwind_error([](Bytes& e) { code_at(e, eh_frame_link + 0x26) = std::byte{0x01}; },
                        "unsupported personality encoding at 0x1014", 0x40);
    expect_unwind_error([](Bytes& e) { code_at(e, eh_frame_link) = std::byte{8}; },
                        "CIE at 0x1000 overruns its length", 0x10);
    expect_unwind_error([](Bytes& e) { put<std::uint32_t>(e, 0x1000 + eh_frame_link, 0x1000); },
                        "CIE at 0x1000 + 0x1004 lies outside its segment's file data", 0x10);
    expect_unwind_error(
        [](Bytes& e) { put<std::uint32_t>(e, 0x1000 + fde_link(e, 0), 0xffffffff); },
        "no supported FDE at 0x1030 for 0x10", 0x10);
    // The FDE of 0x10 holds two 4-byte pointers: too short for 8-byte ones.
    expect_unwind_error([](Bytes& e) { code_at(e, eh_frame_link + 16) = std::byte{0x1c}; },
                        "FDE at 0x1030 is too short", 0x10);
    // pcrel|udata4 doesn't sign-extend, so the backward offset to 0x10 lands above 4 GiB.
    expect_unwind_error([](Bytes& e) { code_at(e, eh_frame_link + 16) = std::byte{0x13}; },
                        "FDE at 0x1030 starts at 0x100000010, the search table says 0x10", 0x10);
}

TEST(MapEbootGame, FindsFunctionSizes) {
    BB_REQUIRE_GAME(game);
    Image image = map_eboot(game);
    ASSERT_TRUE(image.unwind);
    EXPECT_EQ(image.unwind->link_addr, 0x4F41510U);
    EXPECT_EQ(image.unwind->fde_count, 162959U);
    // Sizes from tools/noreturn's fde_ranges over eboot.elf: the first and last table entries,
    // two in between and two functions shorter than a hook patch (mov al, 1; ret and ret).
    const std::pair<std::uint64_t, std::uint64_t> sizes[] = {
        {0xA0, 0x4F},        {0xF0, 0x77}, {0x190A950, 0xC3},
        {0x2BBE370, 0x135}, {0x34F0, 3},  {0x6600, 1}};
    for (auto [link, size] : sizes) {
        EXPECT_EQ(image.function_size(link), size) << link;
    }
    for (std::uint64_t miss : {0xA1UL, 0x2BBE371UL, 0x4F41510UL, 0x50DC000UL}) {
        EXPECT_FALSE(image.function_size(miss)) << miss;
    }
}

TEST(MapEboot, RefusesAnotherBuild) {
    auto dir = std::filesystem::path(testing::TempDir()) / "bb-map-eboot-test";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "eboot.bin") << "not the pinned eboot";
    expect_error<LoadError>([&] { map_eboot(dir, test_base); }, "eboot.bin has SHA-256 ");
    std::filesystem::remove_all(dir);
    expect_error<std::system_error>([&] { map_eboot(dir, test_base); }, "eboot.bin");
    EXPECT_EQ(protection(test_base), "unmapped");
}

TEST(MapEbootGame, MapsTheEbootAtTheDefaultBase) {
    BB_REQUIRE_GAME(game);
    Image image = map_eboot(game);
    EXPECT_EQ(image.base, default_base);
    EXPECT_EQ(image.entry, 0xA0U);
    ASSERT_EQ(image.segments.size(), 2U);
    const Segment& code = image.segments[0];
    EXPECT_EQ(code.link_addr, 0U);
    EXPECT_EQ(code.mem_size, 0x50D96DCU);
    EXPECT_EQ(code.file_size, 0x50D96DCU);
    EXPECT_EQ(code.prot, PROT_READ | PROT_EXEC);
    const Segment& data = image.segments[1];
    EXPECT_EQ(data.link_addr, 0x50DC000U);
    EXPECT_EQ(data.mem_size, 0x5F70F4U);
    EXPECT_EQ(data.file_size, 0x309B38U);
    EXPECT_EQ(data.prot, PROT_READ | PROT_WRITE);
    ASSERT_TRUE(image.tls);
    EXPECT_EQ(image.tls->link_addr, 0x50DC040U);
    EXPECT_EQ(image.tls->file_size, 0U);
    EXPECT_EQ(image.tls->mem_size, 0x750U);
    EXPECT_EQ(image.tls->align, 0x10U);
    EXPECT_EQ(image.proc_param, 0x50DC000U);

    image.finalize_protection();
    EXPECT_EQ(protection(default_base), "r-xp");
    EXPECT_EQ(protection(default_base + 0x50D96DB), "r-xp");
    EXPECT_EQ(protection(default_base + 0x50DB000), "---p");  // gap before the data segment
    EXPECT_EQ(protection(default_base + 0x50DC000), "rw-p");
    EXPECT_EQ(protection(default_base + 0x56D30F3), "rw-p");
    EXPECT_EQ(protection(default_base + 0x56D4000), "unmapped");
}

}  // namespace

#include "loader/image.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "loader/bytes.hpp"
#include "loader/elf.hpp"
#include "loader/load_set.hpp"
#include "support/elf_builder.hpp"

namespace {

using namespace bb::loader;
using bb::test::Bytes;
using bb::test::protection;
using bb::test::put;

constexpr std::uintptr_t test_base = 0x30000000;  // clear of default_base
constexpr std::uint64_t code_size = 0x1800, data_link = 0x4000, data_file = 0x100,
                        data_mem = 0x3000, dynlib_offset = 0x4000;
constexpr std::uint32_t pf_rx = 5, pf_rw = 6;

std::byte pattern(std::size_t i) {
    return static_cast<std::byte>(i * 13 + 1);
}

// Unwind tables in the code segment: .eh_frame at eh_frame_link with a "zR" and a "zPLR" CIE, an
// FDE per function, a dead FDE (pc_begin 0) and the terminator, then .eh_frame_hdr at
// eh_hdr_link, whose search table lists the live FDEs.
constexpr bb::test::UnwindFunction functions[] = {
    {0x10, 0x20, false}, {0x40, 0xd, true}, {0x60, 0x30, false}};
constexpr std::uint64_t eh_frame_link = 0x1000, eh_hdr_link = 0x1200,
                        eh_hdr_size = 12 + 8 * (std::size(functions) + 1);

// The byte at link address `link` of the code segment, which starts at file offset 0x1000.
std::byte& code_at(Bytes& elf, std::uint64_t link) {
    return elf.at(0x1000 + link);
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
    for (std::size_t i = 0; i < code_size; ++i) {
        elf[0x1000 + i] = pattern(i);
    }
    for (std::size_t i = 0; i < data_file; ++i) {
        elf[0x3000 + i] = pattern(i + 7);
    }
    bb::test::add_unwind_tables(elf, {0x1000, eh_frame_link, eh_hdr_link, true}, functions);
    std::size_t dyn = bb::test::append_minimal_dynamic(elf, dynlib_offset, data_link);

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
    bb::test::write_headers(elf, phdrs, 0x10);
    return elf;
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
    for (const bb::test::UnwindFunction& f : functions) {
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

TEST(MapEboot, RefusesAnotherBuild) {
    auto dir = std::filesystem::path(testing::TempDir()) / "bb-map-eboot-test";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "eboot.bin") << "not the pinned eboot";
    expect_error<LoadError>([&] { map_eboot(dir, test_base); }, "eboot.bin has SHA-256 ");
    std::filesystem::remove_all(dir);
    expect_error<std::system_error>([&] { map_eboot(dir, test_base); }, "eboot.bin");
    EXPECT_EQ(protection(test_base), "unmapped");
}

TEST(MapPrx, RefusesAnotherBuildAndUnpinnedNames) {
    auto dir = std::filesystem::path(testing::TempDir()) / "bb-map-prx-test";
    std::filesystem::create_directories(dir / "sce_module");
    std::ofstream(dir / "sce_module" / "libc.prx") << "not the pinned libc";
    expect_error<LoadError>([&] { map_prx(dir, "libc.prx", test_base); }, "libc.prx has SHA-256 ");
    expect_error<LoadError>([&] { map_prx(dir, "libSceFace.prx", test_base); },
                            "libSceFace.prx is not a pinned PRX");
    std::filesystem::remove_all(dir);
    EXPECT_EQ(protection(test_base), "unmapped");
}

}  // namespace

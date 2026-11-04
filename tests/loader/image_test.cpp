#include "loader/image.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// A PS4 ELF with a code segment at link address 0, a data segment at data_link whose memory
// extends past its file data (.bss), the smallest dynamic table parse_dynamic accepts, and
// PT_TLS and PT_SCE_PROCPARAM inside the data segment.
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

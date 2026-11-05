#include "harness/harness.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "hle/hle.hpp"
#include "loader/elf.hpp"
#include "loader/image.hpp"
#include "loader/relocate.hpp"

namespace {

using namespace bb::loader;
using bb::harness::Arena;
using bb::harness::Harness;
using bb::harness::Result;
using Bytes = std::vector<std::byte>;

constexpr std::uintptr_t test_base = 0x32000000;  // clear of the other tests' bases
// Link addresses: the code segment at 0 (file offset 0x1000), the data segment at 0x4000 (file
// offset 0x2000) with the GOT slots of the two imports and a global.
constexpr std::uint64_t got_a = 0x4000, got_b = 0x4008, global = 0x4100;
constexpr std::uint64_t read_write_at = 0x10, calls_at = 0x40, float_bits_at = 0x60;

template <class T>
void put(Bytes& out, std::size_t offset, T value) {
    if (out.size() < offset + sizeof value) {
        out.resize(offset + sizeof value);
    }
    std::memcpy(out.data() + offset, &value, sizeof value);
}

void code(Bytes& elf, std::uint64_t link, std::initializer_list<std::uint8_t> bytes) {
    std::ranges::transform(bytes, elf.begin() + 0x1000 + static_cast<std::ptrdiff_t>(link),
                           [](std::uint8_t b) { return std::byte{b}; });
}

// The rel32 field at `field` of an instruction that ends at `next` and refers to `target`.
void rel32(Bytes& elf, std::uint64_t field, std::uint64_t next, std::uint64_t target) {
    put(elf, 0x1000 + field, static_cast<std::int32_t>(target - next));
}

// A PS4 ELF with three hand-assembled functions and two function imports of libTest.
Bytes build_elf() {
    Bytes elf(0x3000);
    // read_write(int* p): v = p[0]; global = v + 1; p[1] = 7; return 2 * v
    code(elf, read_write_at, {0x8b, 0x07, 0x8d, 0x48, 0x01, 0x89, 0x0d, 0, 0, 0, 0,  //
                              0xc7, 0x47, 0x04, 7, 0, 0, 0, 0x01, 0xc0, 0xc3});
    rel32(elf, 0x17, 0x1b, global);
    // calls(a, b): harnessImpA(a, b); harnessImpB(a + 8, rsp)
    code(elf, calls_at, {0x53, 0x48, 0x89, 0xfb, 0xff, 0x15, 0, 0, 0, 0,          //
                         0x48, 0x8d, 0x7b, 0x08, 0x48, 0x89, 0xe6, 0xff, 0x15, 0,  //
                         0, 0, 0, 0x5b, 0xc3});
    rel32(elf, 0x46, 0x4a, got_a);
    rel32(elf, 0x53, 0x57, got_b);
    // float_bits(uint32_t bits): movd xmm0, edi; ret
    code(elf, float_bits_at, {0x66, 0x0f, 0x6e, 0xc7, 0xc3});

    // PT_SCE_DYNLIBDATA: the string table, the symbols, the PLT relocations, then the dynamic
    // table. Library A (id 0) of module B (id 1) is libTest of libTestModule.
    Bytes blob(1);
    auto str = [&](std::string_view s) {
        const std::uint64_t at = blob.size();
        std::ranges::transform(s, std::back_inserter(blob), [](char c) { return std::byte(c); });
        blob.push_back(std::byte{0});
        return at;
    };
    const std::uint64_t self = str("self"), file = str("libTest.prx"),
                        module = str("libTestModule"), library = str("libTest"),
                        imp_a = str("harnessImpA#A#B"), imp_b = str("harnessImpB#A#B");
    const std::uint64_t strsz = blob.size(), symtab = (strsz + 7) & ~7ULL;
    blob.resize(symtab + 24);  // the null symbol
    for (std::uint64_t name : {imp_a, imp_b}) {
        const std::size_t at = blob.size();
        put(blob, at, static_cast<std::uint32_t>(name));
        put<std::uint8_t>(blob, at + 4, 0x12);  // STB_GLOBAL, STT_FUNC
        put<std::uint64_t>(blob, at + 16, 0);   // up to st_size
    }
    const std::uint64_t jmprel = blob.size();
    for (std::uint64_t sym : {1, 2}) {
        put(blob, blob.size(), sym == 1 ? got_a : got_b);
        put(blob, blob.size(), sym << 32 | r_x86_64_jump_slot);
        put<std::int64_t>(blob, blob.size(), 0);  // r_addend
    }
    const std::uint64_t dynamic = blob.size();
    const std::pair<std::uint64_t, std::uint64_t> entries[] = {
        {1, file},                                            // DT_NEEDED
        {0x6100000F, 1ULL << 48 | 0x0101ULL << 32 | module},  // DT_SCE_NEEDED_MODULE
        {0x61000015, 1ULL << 32 | library},                   // DT_SCE_IMPORT_LIB
        {0x6100000D, 0x0101ULL << 32 | self},                 // DT_SCE_MODULE_INFO
        {0x61000035, 0}, {0x61000037, strsz},                 // string table
        {0x61000039, symtab}, {0x6100003F, 72}, {0x6100003B, 24},  // symbols
        {0x61000029, jmprel}, {0x6100002D, 48}, {0x6100002B, 7},   // PLT relocations
        {0x6100002F, 0}, {0x61000031, 0}, {0x61000033, 24},        // relocations
        {0x61000027, got_a}, {0, 0}};                              // DT_SCE_PLTGOT, DT_NULL
    for (auto [tag, value] : entries) {
        put(blob, blob.size(), tag);
        put(blob, blob.size(), value);
    }
    elf.insert(elf.end(), blob.begin(), blob.end());

    std::memcpy(elf.data(), "\x7f" "ELF\x02\x01", 6);
    put<std::uint16_t>(elf, 0x10, et_sce_dynexec);
    put<std::uint16_t>(elf, 0x12, em_x86_64);
    put<std::uint64_t>(elf, 0x20, elf_header_size);  // e_phoff
    put<std::uint16_t>(elf, 0x36, phdr_size);
    const Phdr phdrs[] = {
        {pt_load, pf_r | pf_x, 0x1000, 0, 0x1000, 0x1000, 0x4000},
        {pt_load, pf_r | 2, 0x2000, 0x4000, 0x1000, 0x2000, 0x4000},  // PF_W: data and .bss
        {pt_sce_dynlibdata, pf_r, 0x3000, 0, blob.size(), 0, 0x10},
        {pt_dynamic, pf_r, 0x3000 + dynamic, 0, blob.size() - dynamic, blob.size() - dynamic, 8},
    };
    put<std::uint16_t>(elf, 0x38, static_cast<std::uint16_t>(std::size(phdrs)));
    for (std::size_t i = 0; i < std::size(phdrs); ++i) {
        const Phdr& ph = phdrs[i];
        const std::size_t at = elf_header_size + i * phdr_size;
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

Image* loaded = nullptr;  // the image of the running test, for the replacements

Image load() {
    Image image = map_elf(build_elf(), test_base);
    relocate(image, bb::hle::resolve);
    image.finalize_protection();
    return image;
}

// The synthetic image, loaded as the harness loads the eboot, and a harness on it.
class HarnessTest : public testing::Test {
protected:
    HarnessTest() { loaded = &image; }
    ~HarnessTest() override { loaded = nullptr; }

    Image image = load();
    Harness harness{image};
};

void expect_report(const Result& r, std::string_view text) {
    EXPECT_FALSE(r);
    EXPECT_NE(r.report().find(text), std::string::npos) << r.report();
}

int* global_int() {
    return reinterpret_cast<int*>(loaded->runtime(global));
}

// 0x10
int read_write(int* p) {
    const int v = p[0];
    *global_int() = v + 1;
    p[1] = 7;
    return 2 * v;
}

int wrong_return(int* p) {
    return read_write(p) + 1;
}

int wrong_global(int* p) {
    const int r = read_write(p);
    ++*global_int();
    return r;
}

int wrong_arena(int* p) {
    const int r = read_write(p);
    p[1] = 8;
    return r;
}

std::tuple<int*> two_ints(Arena& a) {
    int* p = a.zeroed<int>(2);
    p[0] = 20;
    return {p};
}

TEST_F(HarnessTest, PassesAnEquivalentReplacement) {
    const Result r = harness.compare<int(int*)>(read_write_at, &read_write, two_ints);
    EXPECT_TRUE(r) << r.report();
    EXPECT_EQ(*global_int(), 21);
    harness.restore();
    EXPECT_EQ(*global_int(), 0);
}

TEST_F(HarnessTest, CatchesWrongResults) {
    expect_report(harness.compare<int(int*)>(read_write_at, &wrong_return, two_ints),
                  "return value: original 0x28, replacement 0x29");
    expect_report(harness.compare<int(int*)>(read_write_at, &wrong_global, two_ints),
                  "memory: image 0x4100..0x4101 differs: original 15, replacement 16");
    expect_report(harness.compare<int(int*)>(read_write_at, &wrong_arena, two_ints),
                  "memory: arena +0x4..+0x5 differs: original 07, replacement 08");
}

// A replacement that writes into the data block too, so it must be in the snapshot.
int writes_data_block(int* p) {
    reinterpret_cast<std::byte*>(bb::hle::import_address("libTest", "harnessData"))[0x10] =
        std::byte{1};
    return read_write(p);
}

TEST(HarnessDataTest, ComparesDataBlocks) {
    bb::hle::resolve({"harnessData", "libTest", "libTestModule", bb::loader::stt_object});
    Image image = load();
    loaded = &image;
    Harness harness{image};
    expect_report(harness.compare<int(int*)>(read_write_at, &writes_data_block, two_ints),
                  "memory: data block libTest harnessData +0x10..+0x11 differs: original 00, "
                  "replacement 01");
    loaded = nullptr;
}

using TwoArgs = void(std::uint64_t, std::uint64_t);

void call_import(std::string_view nid, std::uint64_t a, std::uint64_t b) {
    reinterpret_cast<TwoArgs*>(bb::hle::import_address("libTest", nid))(a, b);
}

// 0x40
void calls(std::uint64_t a, std::uint64_t b) {
    call_import("harnessImpA", a, b);
    std::uint64_t local = 0;  // its address is a stack pointer as the original's rsp is
    call_import("harnessImpB", a + 8, reinterpret_cast<std::uint64_t>(&local));
}

void swapped(std::uint64_t a, std::uint64_t b) {
    call_import("harnessImpB", a + 8, b);
    call_import("harnessImpA", a, b);
}

void second_arg_plus_one(std::uint64_t a, std::uint64_t b) {
    calls(a, b + 1);
}

// a in the arena, b the global in the image.
std::tuple<std::uint64_t, std::uint64_t> arena_and_global(Arena& a) {
    return {reinterpret_cast<std::uint64_t>(a.zeroed<std::uint64_t>(2)), loaded->runtime(global)};
}

TEST_F(HarnessTest, ComparesStubCallsWithNormalizedPointers) {
    harness.declare_arity("libTest", "harnessImpA", 2);
    harness.declare_arity("libTest", "harnessImpB", 2);
    const Result r = harness.compare<TwoArgs>(calls_at, &calls, arena_and_global);
    EXPECT_TRUE(r) << r.report();
    ASSERT_EQ(harness.log().size(), 2U);
    EXPECT_EQ(harness.log()[0].stub->nid, "harnessImpA");
    EXPECT_EQ(harness.log()[0].args[1], image.runtime(global));
    EXPECT_EQ(harness.log()[1].args[0], harness.log()[0].args[0] + 8);
    harness.restore();
    EXPECT_TRUE(harness.log().empty());
}

TEST_F(HarnessTest, RefusesASecondHarness) {
    EXPECT_THROW(Harness second{image}, std::logic_error);
    // The first harness still records the stub calls.
    EXPECT_TRUE(harness.compare<TwoArgs>(calls_at, &calls, arena_and_global));
    EXPECT_EQ(harness.log().size(), 2U);
}

TEST_F(HarnessTest, CatchesADifferentImportOrder) {
    const Result r = harness.compare<TwoArgs>(calls_at, &swapped, arena_and_global);
    expect_report(r, "stub log: differs at call 1 (original 2 calls, replacement 2)\n"
                     "  original 1: libTest harnessImpA (?) 0x");
    expect_report(r, "  replacement 1: libTest harnessImpB (?) 0x");
}

TEST_F(HarnessTest, ComparesArgumentsUpToTheDeclaredArity) {
    for (int arity : {0, 1}) {
        harness.declare_arity("libTest", "harnessImpA", arity);
        const Result r = harness.compare<TwoArgs>(calls_at, &second_arg_plus_one, arena_and_global);
        EXPECT_TRUE(r) << arity << ": " << r.report();
    }
    harness.declare_arity("libTest", "harnessImpA", 2);
    expect_report(harness.compare<TwoArgs>(calls_at, &second_arg_plus_one, arena_and_global),
                  "stub log: differs at call 1 (original 2 calls, replacement 2)");
    EXPECT_THROW(harness.declare_arity("libTest", "harnessImpA", 7), std::invalid_argument);
    EXPECT_THROW(harness.declare_arity("libTest", "harnessImpA", -1), std::invalid_argument);
}

// 0x60
float float_bits(std::uint32_t bits) {
    return std::bit_cast<float>(bits);
}

float positive_zero(std::uint32_t) {
    return 0.0f;
}

TEST_F(HarnessTest, ComparesFloatsBitwise) {
    auto minus_zero = [](Arena&) { return std::tuple{std::uint32_t{0x80000000}}; };
    const Result r = harness.compare<float(std::uint32_t)>(float_bits_at, &float_bits, minus_zero);
    EXPECT_TRUE(r) << r.report();
    expect_report(harness.compare<float(std::uint32_t)>(float_bits_at, &positive_zero, minus_zero),
                  "return value: original -0 (bits 0x80000000), replacement 0 (bits 0x0)");
}

TEST_F(HarnessTest, GameFnCallsTheSetImage) {
    bb::game::set_image(&image);
    EXPECT_EQ(&bb::game::image(), &image);
    int p[2] = {20, 0};
    EXPECT_EQ(bb::game::fn<int(int*)>(read_write_at)(p), 40);
    EXPECT_EQ(p[1], 7);
    EXPECT_THROW(bb::game::fn<int(int*)>(got_a), std::out_of_range);  // data, not code
    bb::game::set_image(nullptr);
}

TEST(GameDeathTest, ImageAbortsWhenUnset) {
    bb::game::set_image(nullptr);
    EXPECT_DEATH(bb::game::image(),
                 "game: no image is loaded \\(bb::game::set_image was not called\\)");
}

TEST_F(HarnessTest, ArenaLayoutIsDeterministic) {
    Arena& a = harness.arena();
    char* s = a.str("abc");
    std::byte* b = a.bytes(std::as_bytes(std::span{"xy", 2}));
    EXPECT_EQ(reinterpret_cast<std::byte*>(s), a.data());
    EXPECT_STREQ(s, "abc");
    EXPECT_EQ(b, a.data() + 16);
    harness.restore();
    EXPECT_EQ(a.str("abc"), s);
    EXPECT_THROW(a.alloc(Arena::size), std::length_error);
    EXPECT_STREQ(a.str({}), "");
    EXPECT_NE(a.bytes({}), nullptr);
    // 8 * 2^61 wraps to 0 without the check.
    EXPECT_THROW(a.zeroed<std::uint64_t>(SIZE_MAX / 8 + 1), std::length_error);
    EXPECT_THROW(a.zeroed<std::uint64_t>(Arena::size / 8 + 1), std::length_error);
}

// Ten differing arena ranges after p: a 12-byte run at +0x100, then single bytes.
int many_ranges(int* p) {
    const int r = read_write(p);
    auto* arena = reinterpret_cast<unsigned char*>(p);
    std::memset(arena + 0x100, 0xff, 12);
    for (std::size_t i = 1; i < 10; ++i) {
        arena[0x100 + 0x10 * i] = 0xff;
    }
    return r;
}

TEST_F(HarnessTest, CapsTheListedRanges) {
    const Result r = harness.compare<int(int*)>(read_write_at, &many_ranges, two_ints);
    expect_report(r, "memory: arena +0x100..+0x10c differs: original 00 00 00 00 00 00 00 00 "
                     "..., replacement ff ff ff ff ff ff ff ff ...\n");
    expect_report(r, "memory: arena +0x170..+0x171 differs");
    EXPECT_EQ(r.report().find("+0x180"), std::string::npos) << r.report();
    expect_report(r, "memory: 10 differing ranges in all\n");
    EXPECT_EQ(std::ranges::count(r.report(), '\n'), 9) << r.report();
}

}  // namespace

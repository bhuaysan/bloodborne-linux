#include "harness/harness.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

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
// offset 0x2000) with a global.
constexpr std::uint64_t global = 0x4100, read_write_at = 0x10, float_bits_at = 0x60;

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

// A PS4 ELF with two hand-assembled leaf functions and no imports.
Bytes build_elf() {
    Bytes elf(0x3000);
    // read_write(int* p): v = p[0]; global = v + 1; p[1] = 7; return 2 * v
    code(elf, read_write_at, {0x8b, 0x07, 0x8d, 0x48, 0x01, 0x89, 0x0d, 0, 0, 0, 0,  //
                              0xc7, 0x47, 0x04, 7, 0, 0, 0, 0x01, 0xc0, 0xc3});
    rel32(elf, 0x17, 0x1b, global);
    // float_bits(uint32_t bits): movd xmm0, edi; ret
    code(elf, float_bits_at, {0x66, 0x0f, 0x6e, 0xc7, 0xc3});

    // PT_SCE_DYNLIBDATA: the module name, then the smallest dynamic table parse_dynamic accepts.
    // The empty symbol table starts at offset 0 and holds only the null symbol.
    Bytes blob(8);
    std::memcpy(blob.data(), "\0self\0\0", 8);
    const std::pair<std::uint64_t, std::uint64_t> entries[] = {
        {0x6100000D, 0x0101ULL << 32 | 1},                   // DT_SCE_MODULE_INFO "self"
        {0x61000035, 0}, {0x61000037, 6},                    // string table
        {0x61000039, 0}, {0x6100003F, 0}, {0x6100003B, 24},  // symbols
        {0x61000029, 0}, {0x6100002D, 0}, {0x6100002B, 7},   // PLT relocations
        {0x6100002F, 0}, {0x61000031, 0}, {0x61000033, 24},  // relocations
        {0x61000027, 0}, {0, 0}};                            // DT_SCE_PLTGOT, DT_NULL
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
        {pt_dynamic, pf_r, 0x3000 + 8, 0, blob.size() - 8, blob.size() - 8, 8},
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

}  // namespace

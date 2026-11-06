// The descriptor source pass on synthetic programs (docs/design/graphics.md, "Resources and
// memory"), encoded by hand from the Sea Islands ISA manual.

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/cfg.hpp"
#include "gpu/gcn/descriptors.hpp"
#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"

namespace {

using namespace bb::test::gcn;
using Path = std::vector<std::uint32_t>;

constexpr std::uint32_t s_endpgm = sopp(1);

std::vector<std::uint32_t> join(std::vector<std::vector<std::uint32_t>> parts) {
    std::vector<std::uint32_t> code;
    for (const auto& part : parts) {
        code.insert(code.end(), part.begin(), part.end());
    }
    return code;
}

// The paths of the sources found in a program with 8 user SGPRs.
std::vector<Path> buffer_sources(const std::vector<std::uint32_t>& code) {
    const auto program = bb::gcn::decode(code);
    std::vector<Path> paths;
    for (const auto& source : bb::gcn::find_buffer_sources(program, bb::gcn::build_cfg(program), 8)) {
        paths.push_back(source.path);
    }
    return paths;
}

// The sources of the descriptors that s_buffer_load_dword and MUBUF use: a user-data slot, a load
// through a pointer in user data, loads through loaded pointers, and a copy with s_mov_b64. Each
// source is listed once.
TEST(GcnDescriptors, SourcesFollowLoadsAndCopies) {
    constexpr unsigned load_x4 = 2, load_x8 = 3, buffer_load = 8;
    const auto buffer_use = [](unsigned v_sharp) { return smrd(buffer_load, 40, v_sharp, true, 0); };
    EXPECT_EQ(buffer_sources(join({
                  {buffer_use(0), smrd(buffer_load, 40, 0, true, 1)},       // slot 0, twice
                  {smrd(load_x4, 12, 2, true, 4), buffer_use(12)},          // [s2, +4]
                  {smrd(load_x8, 16, 12, true, 2), buffer_use(16)},         // through the V#'s dwords
                  {sop1(4, 24, 2), smrd(load_x4, 28, 24, true, 8), buffer_use(28)},  // copied
                  mubuf(12, 3, 1, 4, 128, 0, buf::offen),  // buffer_load_dword v3, v1, s[4:7], 0
                  {s_endpgm}})),
              (std::vector<Path>{{0}, {2, 4}, {2, 4, 2}, {2, 8}, {4}}));
}

// A write forgets the source, and so does a join where the paths disagree.
TEST(GcnDescriptors, SourcesAreForgotten) {
    EXPECT_EQ(buffer_sources(join({{sop1(3, 1, 128)},  // s_mov_b32 s1, 0 breaks the pair s[0:1]
                                   {smrd(2, 12, 0, true, 4), smrd(8, 40, 12, true, 0)},
                                   {s_endpgm}})),
              std::vector<Path>{});
    EXPECT_EQ(buffer_sources(join({{sopp(5, 1)},  // s_cbranch_scc1 over the load
                                   {smrd(2, 8, 2, true, 1)},
                                   {smrd(8, 40, 8, true, 0), s_endpgm}})),
              std::vector<Path>{});
}

// A V# with swizzle_enable (dword 1 bit 31), add_tid_enable (dword 3 bit 23, bit 119 of the
// descriptor, Table 8.5) or heap (dword 3 bit 26, bit 122) is a GcnError until those modes are
// implemented; the other bits of those dwords, such as the cache swizzle (bit 62) and the stride,
// are not: the positive case sets every one of them.
TEST(GcnDescriptors, SwizzledDescriptorsAreRejected) {
    using bb::gcn::GcnError;
    using Vsharp = std::array<std::uint32_t, 4>;
    EXPECT_NO_THROW(bb::gcn::check_buffer_descriptor({0x1000, 0x7fff'ffff, 64, 0xfb7f'ffff}));
    try {
        bb::gcn::check_buffer_descriptor({0x1000, 0x8000'0010, 64, 0});
        FAIL() << "no error for swizzle_enable";
    } catch (const GcnError& e) {
        EXPECT_STREQ(e.what(),
                     "V# with swizzle_enable is not supported yet at offset 0x4 (word 0x80000010)");
    }
    try {
        bb::gcn::check_buffer_descriptor({0x1000, 0x10, 64, 0x0080'0000});
        FAIL() << "no error for add_tid_enable";
    } catch (const GcnError& e) {
        EXPECT_STREQ(e.what(),
                     "V# with add_tid_enable is not supported yet at offset 0xc (word 0x00800000)");
    }
    try {
        bb::gcn::check_buffer_descriptor({0x1000, 0x10, 64, 0x0400'0000});
        FAIL() << "no error for heap";
    } catch (const GcnError& e) {
        EXPECT_STREQ(e.what(),
                     "V# with heap is not supported yet at offset 0xc (word 0x04000000)");
    }
    EXPECT_THROW(bb::gcn::check_buffer_descriptor(Vsharp{0, 1u << 31, 0, 1u << 23}), GcnError);
}

}  // namespace

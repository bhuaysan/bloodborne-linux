// The descriptor source pass on synthetic programs (docs/design/graphics.md, "Resources and
// memory"), encoded by hand from the Sea Islands ISA manual.

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

}  // namespace

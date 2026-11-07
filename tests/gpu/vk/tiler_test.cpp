// bbvk's compute detiler and tiler: the shaders under spirv-val.

#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include "gpu/gcn/spirv_val.hpp"
#include "gpu/vk/tiler.hpp"

namespace {

using namespace bb::vk;

TEST(TilingShader, Validates) {
    if (!bb::test::gcn::have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    for (std::uint32_t bytes = 1; bytes <= 16; bytes *= 2) {
        for (const bool to_linear : {false, true}) {
            EXPECT_EQ(bb::test::gcn::validate(tiling_shader(bytes, to_linear)), "")
                << bytes << " bytes, to_linear " << to_linear;
        }
    }
    EXPECT_THROW(tiling_shader(3, true), std::invalid_argument);
    EXPECT_THROW(tiling_shader(32, true), std::invalid_argument);
}

}  // namespace

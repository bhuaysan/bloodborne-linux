// The rectangle list's geometry stage must be a valid Vulkan 1.3 module for any set of outputs.
#include <gtest/gtest.h>

#include "gpu/gcn/rect_list.hpp"
#include "gpu/gcn/spirv_val.hpp"

namespace {

using bb::test::gcn::have_spirv_val;
using bb::test::gcn::validate;

TEST(RectListGeometry, IsAValidModuleWithAndWithoutParameters) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "no spirv-val";
    }
    for (const std::vector<std::uint32_t>& locations :
         {std::vector<std::uint32_t>{}, {0}, {0, 1, 5}}) {
        const auto module = bb::gcn::rect_list_geometry(locations);
        EXPECT_EQ(validate(module), "") << locations.size() << " outputs";
    }
}

}  // namespace

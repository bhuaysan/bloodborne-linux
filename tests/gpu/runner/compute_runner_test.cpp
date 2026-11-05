// The compute runner on lavapipe: picking the device, and skipping without it.

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "gpu/runner/compute_runner.hpp"

namespace {

using namespace bb::test::vk;

class ComputeRunnerTest : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = ComputeRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
    }

    std::unique_ptr<ComputeRunner> runner_;
};

TEST_F(ComputeRunnerTest, ReportsDevice) {
    const DeviceInfo& info = runner_->info();
    std::cout << info.describe();
    EXPECT_NE(info.name.find("llvmpipe"), std::string::npos) << info.name;
    EXPECT_TRUE(std::has_single_bit(info.subgroup_size));
    EXPECT_LE(info.min_subgroup_size, info.subgroup_size);
    EXPECT_LE(info.subgroup_size, info.max_subgroup_size);
    EXPECT_LE(info.max_subgroup_size, 64u);  // a wider subgroup is not supported
}

// With no driver to load, creating the runner fails with a reason instead of throwing, so the
// tests above skip.
TEST(ComputeRunner, NoDriverGivesReason) {
    const auto saved = [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        return value ? std::optional<std::string>(value) : std::nullopt;
    };
    const auto driver_files = saved("VK_DRIVER_FILES");
    const auto icd_filenames = saved("VK_ICD_FILENAMES");
    const std::string none = (std::filesystem::temp_directory_path() / "bb-no-icd.json").string();
    ::setenv("VK_DRIVER_FILES", none.c_str(), 1);
    ::setenv("VK_ICD_FILENAMES", none.c_str(), 1);
    std::string why;
    const auto runner = ComputeRunner::create(why);
    const auto restore = [](const char* name, const std::optional<std::string>& value) {
        value ? ::setenv(name, value->c_str(), 1) : ::unsetenv(name);
    };
    restore("VK_DRIVER_FILES", driver_files);
    restore("VK_ICD_FILENAMES", icd_filenames);
    EXPECT_EQ(runner, nullptr);
    EXPECT_EQ(why.rfind("no Vulkan instance", 0), 0u) << why;
}

}  // namespace

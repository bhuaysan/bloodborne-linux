// The compute runner on lavapipe: picking the device, and skipping without it.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
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
// tests above skip. The fixture points the loader at a manifest that doesn't exist and puts the
// environment back afterwards.
class ComputeRunnerNoDriver : public testing::Test {
protected:
    static constexpr const char* kNames[] = {
        "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_DRIVERS_SELECT"};

    void SetUp() override {
        for (std::size_t i = 0; i < std::size(kNames); ++i) {
            const char* value = std::getenv(kNames[i]);
            saved_[i] = value ? std::optional<std::string>(value) : std::nullopt;
        }
        // The manifest path lies in a directory this test just created, so no other process can
        // have put a driver manifest there for the loader to parse and load.
        std::string dir_template =
            (std::filesystem::temp_directory_path() / "bb-no-icd-XXXXXX").string();
        ASSERT_NE(::mkdtemp(dir_template.data()), nullptr) << "cannot create a temp directory";
        dir_ = dir_template;
        const std::string none = (dir_ / "none.json").string();
        ASSERT_FALSE(std::filesystem::exists(none)) << none;
        ::setenv("VK_DRIVER_FILES", none.c_str(), 1);
        ::setenv("VK_ICD_FILENAMES", none.c_str(), 1);
    }

    void TearDown() override {
        for (std::size_t i = 0; i < std::size(kNames); ++i) {
            saved_[i] ? ::setenv(kNames[i], saved_[i]->c_str(), 1) : ::unsetenv(kNames[i]);
        }
        if (!dir_.empty()) {
            std::filesystem::remove(dir_);
        }
    }

    std::optional<std::string> saved_[std::size(kNames)];
    std::filesystem::path dir_;
};

// The runner's own filter loads only lavapipe, so the reason names lavapipe.
TEST_F(ComputeRunnerNoDriver, OwnFilterNamesLavapipe) {
    ::unsetenv("VK_LOADER_DRIVERS_SELECT");
    std::string why;
    const auto runner = ComputeRunner::create(why);
    EXPECT_EQ(runner, nullptr);
    EXPECT_EQ(why.rfind("no lavapipe driver", 0), 0u) << why;
    EXPECT_NE(why.find("VK_LOADER_DRIVERS_SELECT=*lvp*"), std::string::npos) << why;
}

// A filter from the environment may select other drivers, so the reason names the instance.
TEST_F(ComputeRunnerNoDriver, EnvironmentFilterNamesInstance) {
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*", 1);
    std::string why;
    const auto runner = ComputeRunner::create(why);
    EXPECT_EQ(runner, nullptr);
    EXPECT_EQ(why.rfind("no Vulkan instance", 0), 0u) << why;
}

}  // namespace

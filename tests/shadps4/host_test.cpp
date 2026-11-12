// The fixed host headers and functions that src/shadps4 builds against (src/shadps4_host).

#include <cstdlib>
#include <filesystem>

#include <gtest/gtest.h>

#include "common/alignment.h"
#include "common/clock.hpp"
#include "common/path_util.h"
#include "core/libraries/kernel/time.h"

namespace {

TEST(ShadPs4Host, Alignment) {
    EXPECT_EQ(Common::AlignUp(0u, 16), 0u);
    EXPECT_EQ(Common::AlignUp(1u, 16), 16u);
    EXPECT_EQ(Common::AlignUp(16u, 16), 16u);
    EXPECT_EQ(Common::AlignUp(17u, 12), 24u);
    EXPECT_EQ(Common::AlignDown(17u, 12), 12u);
    EXPECT_EQ(Common::AlignDown(24u, 12), 24u);
    EXPECT_EQ(Common::AlignUpPow2(0x1001ull, 0x1000), 0x2000ull);
    EXPECT_EQ(Common::AlignUpPow2(0x1000ull, 0x1000), 0x1000ull);
    EXPECT_EQ(Common::AlignDownPow2(0x1fffull, 0x1000), 0x1000ull);
    EXPECT_EQ(Common::AlignDownPow2(0xffffffffffffffffull, 0x4000), 0xffffffffffffc000ull);
}

// The TSC counts nanoseconds of the process clock that libkernel's sceKernelReadTsc reads, so GPU
// timestamps and the game's TSC share one origin.
TEST(ShadPs4Host, TscIsTheProcessClock) {
    EXPECT_EQ(Libraries::Kernel::sceKernelGetTscFrequency(), 1'000'000'000u);
    const u64 before = bb::common::process_nsec();
    const u64 tsc = Libraries::Kernel::sceKernelReadTsc();
    const u64 after = bb::common::process_nsec();
    EXPECT_LE(before, tsc);
    EXPECT_LE(tsc, after);
}

// shadPS4's user directories land in the runtime's XDG directories. GetUserPath reads the
// environment once, and nothing else in this binary calls it.
TEST(ShadPs4Host, UserPathsAreXdgDirectories) {
    ::setenv("XDG_DATA_HOME", "/nonexistent-bb-test/data", 1);
    ::setenv("XDG_CACHE_HOME", "/nonexistent-bb-test/cache", 1);
    ::setenv("XDG_STATE_HOME", "/nonexistent-bb-test/state", 1);
    ::setenv("XDG_CONFIG_HOME", "/nonexistent-bb-test/config", 1);
    using Common::FS::GetUserPath;
    using Common::FS::PathType;
    const std::filesystem::path base{"/nonexistent-bb-test"};
    EXPECT_EQ(GetUserPath(PathType::UserDir), base / "data/bloodborne-linux");
    EXPECT_EQ(GetUserPath(PathType::LogDir), base / "state/bloodborne-linux/logs");
    EXPECT_EQ(GetUserPath(PathType::CacheDir), base / "cache/bloodborne-linux/pipeline");
    EXPECT_FALSE(std::filesystem::exists(base));
}

// Any other directory ends the process: nothing in the port may write to an unplanned place.
TEST(ShadPs4HostDeathTest, UnplannedUserPathIsFatal) {
    EXPECT_DEATH(static_cast<void>(Common::FS::GetUserPath(Common::FS::PathType::GameDataDir)),
                 "GetUserPath");
}

}  // namespace

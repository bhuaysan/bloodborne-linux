#include "hle/system/save_data.hpp"

#include <stdlib.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "common/runtime_dirs.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/file.hpp"
#include "loader/dynamic.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::hle::system;

// Guest open flags (docs/formats/kernel-abi.md): O_WRONLY | O_CREAT | O_TRUNC.
constexpr std::int32_t guest_create = 0x1 | 0x200 | 0x400;

class SaveDataTest : public ::testing::Test {
protected:
    void SetUp() override {
        char pattern[] = "/tmp/bb-save-XXXXXX";
        ASSERT_NE(::mkdtemp(pattern), nullptr);
        tmp_ = pattern;
        ::setenv("XDG_DATA_HOME", (tmp_ / "data").c_str(), 1);
        reset_save_data();
    }
    void TearDown() override {
        reset_save_data();
        ::unsetenv("XDG_DATA_HOME");
        fs::remove_all(tmp_);
    }

    // The save root of `profile`, set as the current one.
    fs::path use_profile(const char* profile) {
        reset_save_data();
        const auto dirs = bb::common::RuntimeDirs::from_environment(profile, {});
        const fs::path root = save_data_root(dirs);
        set_save_data_root(root);
        return root;
    }

    static std::int32_t mount(const char* dir, std::uint32_t mode, SaveMountOutput* out) {
        std::array<char, 32> name{};
        std::strncpy(name.data(), dir, name.size() - 1);
        const SaveMountInput in{.user_id = 1, .pad0 = 0, .short_name = nullptr,
                                .dir_name = name.data(), .label = "", .blocks = 0x140,
                                .mode = mode};
        return save_data_mount(&in, out);
    }

    fs::path tmp_;
};

// The game's save path: mount, on "not found" mount again with the create bit (0x2842481).
TEST_F(SaveDataTest, MountCreatesTheDirectoryInTheProfile) {
    const fs::path root = use_profile("alice");
    EXPECT_EQ(root, tmp_ / "data" / "bloodborne-linux" / "profiles" / "alice" / "savedata");
    SaveMountOutput out{};
    EXPECT_EQ(mount("SPRJ0000", 2, &out), save_not_found);
    EXPECT_FALSE(fs::exists(root / "SPRJ0000"));
    EXPECT_EQ(mount("SPRJ0000", 2 | save_mode_create, &out), save_ok);
    EXPECT_STREQ(out.mount_point, "/savedata0");
    EXPECT_TRUE(fs::is_directory(root / "SPRJ0000"));
    EXPECT_EQ(save_data_unmount(&out), save_ok);
    // Load mode 1 finds it now.
    EXPECT_EQ(mount("SPRJ0000", 1, &out), save_ok);
    EXPECT_EQ(save_data_unmount(&out), save_ok);
    EXPECT_EQ(save_data_unmount(&out), save_failed);
}

TEST_F(SaveDataTest, AFileWrittenThroughTheMountPointLandsInTheDirectory) {
    const fs::path root = use_profile("alice");
    SaveMountOutput out{};
    ASSERT_EQ(mount("SPRJ0003", 2 | save_mode_create, &out), save_ok);
    const std::string path = std::string(out.mount_point) + "/userdata0000";
    const std::int32_t fd = bb::hle::kernel::sceKernelOpen(path.c_str(), guest_create, 0600);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(bb::hle::kernel::sceKernelWrite(fd, "save", 4), 4);
    EXPECT_EQ(bb::hle::kernel::sceKernelClose(fd), 0);
    ASSERT_EQ(save_data_unmount(&out), save_ok);

    std::ifstream file(root / "SPRJ0003" / "userdata0000", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "save");
    // After the unmount the guest path is gone.
    EXPECT_LT(bb::hle::kernel::sceKernelOpen(path.c_str(), 0, 0), 0);
}

TEST_F(SaveDataTest, TwoProfilesStayApart) {
    const fs::path alice = use_profile("alice");
    SaveMountOutput out{};
    ASSERT_EQ(mount("SPRJ0000", 2 | save_mode_create, &out), save_ok);
    ASSERT_EQ(save_data_unmount(&out), save_ok);

    const fs::path bob = use_profile("bob");
    EXPECT_NE(alice, bob);
    EXPECT_EQ(mount("SPRJ0000", 1, &out), save_not_found);
    EXPECT_TRUE(fs::is_directory(alice / "SPRJ0000"));
    EXPECT_FALSE(fs::exists(bob / "SPRJ0000"));
}

TEST_F(SaveDataTest, MountRefusesBadNamesAndDoubleMounts) {
    use_profile("alice");
    SaveMountOutput out{};
    EXPECT_EQ(mount("../escape", 6, &out), save_failed);
    EXPECT_EQ(mount("a/b", 6, &out), save_failed);
    EXPECT_EQ(mount("", 6, &out), save_failed);
    ASSERT_EQ(mount("SPRJ0000", 6, &out), save_ok);
    SaveMountOutput second{};
    EXPECT_EQ(mount("SPRJ0000", 6, &second), save_failed);
    ASSERT_EQ(mount("SPRJ0001", 6, &second), save_ok);
    EXPECT_STREQ(second.mount_point, "/savedata1");
    EXPECT_EQ(save_data_unmount(&out), save_ok);
    EXPECT_EQ(save_data_unmount(&second), save_ok);
}

TEST_F(SaveDataTest, WithoutARootEverythingFails) {
    SaveMountOutput out{};
    EXPECT_EQ(mount("SPRJ0000", 6, &out), save_failed);
}

TEST_F(SaveDataTest, SearchAndErase) {
    use_profile("alice");
    std::array<char, 32> name{};
    std::strcpy(name.data(), "SPRJ0002");
    char entries[1][32]{};
    const SaveSearchCondition cond{.user_id = 1, .pad0 = 0, .unused = nullptr,
                                   .dir_name = name.data(), .unknown_18 = 0, .unknown_1c = 0};
    SaveSearchResult result{.hits = 7, .pad0 = 0, .entries = entries, .capacity = 1};
    EXPECT_EQ(save_data_search(&cond, &result), save_ok);
    EXPECT_EQ(result.hits, 0U);  // the erase task then reports "no save" (game result 6)

    SaveMountOutput out{};
    ASSERT_EQ(mount("SPRJ0002", 6, &out), save_ok);
    const SaveEraseInput erase{.user_id = 1, .pad0 = 0, .unused = nullptr,
                               .dir_name = name.data()};
    EXPECT_EQ(save_data_erase(&erase), save_failed);  // still mounted
    ASSERT_EQ(save_data_unmount(&out), save_ok);
    EXPECT_EQ(save_data_search(&cond, &result), save_ok);
    EXPECT_EQ(result.hits, 1U);
    EXPECT_STREQ(entries[0], "SPRJ0002");
    EXPECT_EQ(save_data_erase(&erase), save_ok);
    EXPECT_EQ(save_data_erase(&erase), save_not_found);
    EXPECT_EQ(save_data_search(&cond, &result), save_ok);
    EXPECT_EQ(result.hits, 0U);
}

TEST(SaveDataNids, ResolveToTheImplementations) {
    struct Entry {
        const char* library;
        const char* nid;
        void* impl;
    };
    const Entry entries[] = {
        {"libSceSaveData", "ZkZhskCPXFw", reinterpret_cast<void*>(&save_data_initialize)},
        {"libSceSaveData", "yKDy8S5yLA0", reinterpret_cast<void*>(&save_data_terminate)},
        {"libSceSaveData", "32HQAQdwM2o", reinterpret_cast<void*>(&save_data_mount)},
        {"libSceSaveData", "BMR4F-Uek3E", reinterpret_cast<void*>(&save_data_unmount)},
        {"libSceSaveData", "dyIhnXq-0SM", reinterpret_cast<void*>(&save_data_search)},
        {"libSceSaveData", "S1GkePI17zQ", reinterpret_cast<void*>(&save_data_erase)},
    };
    for (const Entry& e : entries) {
        EXPECT_EQ(bb::hle::resolve({e.nid, e.library, e.library, bb::loader::stt_func}),
                  reinterpret_cast<std::uintptr_t>(e.impl))
            << e.nid;
    }
}

}  // namespace

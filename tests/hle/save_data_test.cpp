#include "hle/system/save_data.hpp"

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

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
#include "hle/kernel/vmem.hpp"
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
        const SaveMountInput in{.user_id = 1, .pad0 = 0, .name_prefix = nullptr,
                                .dir_name = name.data(), .descriptor_text = "", .blocks = 0x140,
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
    const char data[4] = {1, 2, 3, 4};
    ASSERT_EQ(save_store_write(1, data, sizeof data, 0), save_ok);

    const fs::path bob = use_profile("bob");
    EXPECT_NE(alice, bob);
    EXPECT_EQ(mount("SPRJ0000", 1, &out), save_not_found);
    std::array<char, 4> read{1, 1, 1, 1};
    EXPECT_EQ(save_store_read(1, read.data(), read.size(), 0), save_ok);
    EXPECT_EQ(read, (std::array<char, 4>{}));
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
    char byte = 0;
    EXPECT_EQ(save_store_read(1, &byte, 1, 0), save_failed);
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

// The per-user byte store: unwritten bytes read as zeros, so a fresh store has no header and
// the game reports "no save yet" (0x2841056).
TEST_F(SaveDataTest, TheByteStoreReadsBackWhatWasWritten) {
    use_profile("alice");
    EXPECT_EQ(save_store_declare(1, 0x100000, "record"), save_ok);
    std::array<char, 8> read{};
    read.fill(9);
    EXPECT_EQ(save_store_read(1, read.data(), read.size(), 0x10), save_ok);
    EXPECT_EQ(read, (std::array<char, 8>{}));
    EXPECT_EQ(save_store_write(1, "abcd", 4, 0x12), save_ok);
    EXPECT_EQ(save_store_read(1, read.data(), read.size(), 0x10), save_ok);
    EXPECT_EQ(std::string(read.data(), read.size()), std::string("\0\0abcd\0\0", 8));
    EXPECT_EQ(save_store_write(1, "x", 1, 0x400000), save_failed);
    EXPECT_EQ(save_store_read(1, nullptr, 1, 0), save_failed);
}

// The store is read with pread straight into the guest buffer, which the write tracking may have
// write-protected (docs/design/renderer.md, "System calls into tracked memory"): the buffer is
// announced first. A stand-in for the tracking lifts the protection there; without the
// announcement the read would fault on the protected page.
TEST_F(SaveDataTest, TheByteStoreAnnouncesItsBufferBeforeReading) {
    class Unprotect final : public bb::hle::kernel::vmem::MappingObserver {
    public:
        void mapped(std::uintptr_t, std::uint64_t, std::uint64_t) override {}
        void unmapped(std::uintptr_t, std::uint64_t, std::uint64_t) override {}
        void released(std::uint64_t, std::uint64_t) override {}
        void host_writing(std::uintptr_t address, std::uint64_t length) override {
            ++calls;
            EXPECT_EQ(address, reinterpret_cast<std::uintptr_t>(page) + 0x20);
            EXPECT_EQ(length, 8U);
            ::mprotect(page, ::getpagesize(), PROT_READ | PROT_WRITE);
        }
        void* page = nullptr;
        int calls = 0;
    } observer;
    use_profile("alice");
    EXPECT_EQ(save_store_declare(1, 0x100000, "record"), save_ok);
    EXPECT_EQ(save_store_write(1, "abcd", 4, 0x12), save_ok);
    observer.page =
        ::mmap(nullptr, ::getpagesize(), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(observer.page, MAP_FAILED);
    ASSERT_TRUE(bb::hle::kernel::vmem::add_mapping_observer(&observer));
    char* const buffer = static_cast<char*>(observer.page) + 0x20;
    EXPECT_EQ(save_store_read(1, buffer, 8, 0x10), save_ok);
    bb::hle::kernel::vmem::remove_mapping_observer(&observer);
    EXPECT_EQ(observer.calls, 1);
    EXPECT_EQ(std::string(buffer, 8), std::string("\0\0abcd\0\0", 8));
    ::munmap(observer.page, ::getpagesize());
}

TEST_F(SaveDataTest, TheDialogNeverOpens) {
    EXPECT_EQ(save_dialog_initialize(), 0);
    EXPECT_NE(save_dialog_open(nullptr), 0);
    EXPECT_NE(save_dialog_status(), 2);
    EXPECT_EQ(save_dialog_close(), 0);
}

TEST_F(SaveDataTest, TextAndBlobNeedAMount) {
    use_profile("alice");
    SaveMountOutput out{};
    EXPECT_EQ(save_data_set_text(&out, 1, "title", 0x80), save_failed);
    ASSERT_EQ(mount("SPRJ0000", 6, &out), save_ok);
    EXPECT_EQ(save_data_set_text(&out, 1, "title", 0x80), save_ok);
    const std::array<std::uint64_t, 5> blob{};
    EXPECT_EQ(save_data_set_blob(&out, blob.data()), save_ok);
    EXPECT_EQ(save_data_unmount(&out), save_ok);
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
        {"libSceSaveData", "85zul--eGXs", reinterpret_cast<void*>(&save_data_set_text)},
        {"libSceSaveData", "c88Yy54Mx0w", reinterpret_cast<void*>(&save_data_set_blob)},
        {"libSceSaveData", "7Bt5pBC-Aco", reinterpret_cast<void*>(&save_store_read)},
        {"libSceSaveData", "h3YURzXGSVQ", reinterpret_cast<void*>(&save_store_write)},
        {"libSceSaveData", "v7AAAMo0Lz4", reinterpret_cast<void*>(&save_store_declare)},
        {"libSceSaveDataDialog", "s9e3+YpRnzw", reinterpret_cast<void*>(&save_dialog_initialize)},
        {"libSceSaveDataDialog", "4tPhsP6FpDI", reinterpret_cast<void*>(&save_dialog_open)},
        {"libSceSaveDataDialog", "KK3Bdg1RWK0", reinterpret_cast<void*>(&save_dialog_status)},
        {"libSceSaveDataDialog", "YuH2FA7azqQ", reinterpret_cast<void*>(&save_dialog_close)},
    };
    for (const Entry& e : entries) {
        EXPECT_EQ(bb::hle::resolve({e.nid, e.library, e.library, bb::loader::stt_func}),
                  reinterpret_cast<std::uintptr_t>(e.impl))
            << e.nid;
    }
}

}  // namespace
